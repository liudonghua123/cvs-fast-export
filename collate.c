/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  SPDX-License-Identifier: GPL-2.0+
 */
#include "cvs.h"
#include "revdir.h"
/*
 * These functions analyze a CVS revlist into a changeset DAG.
 *
 * collate_to_changesets() is the main function.
 */

/*
 * Pack the dead flag into the commit pointer so we can avoid dereferencing
 * in the inner loop. Also keep the dir near the packed pointer
 * as it is used in the inner loop.
 */
typedef struct _revision {
    /* packed commit pointer and dead flag */
    uintptr_t packed;
    const master_dir *dir;
} revision_t;

/*
 * Once set, dir doesn't change, so have an initial pack that sets dir
 * and a later pack that doesn't
 */
#define REVISION_T_PACK(rev, commit) (rev).packed = ((uintptr_t)(commit) | ((commit) ? ((commit)->dead) : 0))
#define REVISION_T_PACK_INIT(rev, commit) do {	\
	REVISION_T_PACK(rev, commit);		\
	(rev).dir = (commit)->master->dir;	\
    } while (0)
#define REVISION_T_DEAD(rev) (((rev).packed) & 1)
#define COMMIT_MASK (~(uintptr_t)0 ^ 1)
#define REVISION_T_COMMIT(rev) (cvs_commit *)(((rev).packed) & (COMMIT_MASK))

/*
 * Be aware using these macros that they bind to whatever revisions array
 * is in scope
 */
#define DEAD(index) (REVISION_T_DEAD(revisions[(index)]))
#define REVISIONS(index) (REVISION_T_COMMIT(revisions[(index)]))
#define DIR(index) (revisions[(index)].dir)

static rev_ref *
rev_find_head(head_list *rl, const char *name)
/* find a named branch head in a revlist - used on both CVS and gitspace sides */
{
    rev_ref	*h;

    for (h = rl->heads; h; h = h->next)
	if (h->ref_name == name)
	    return h;
    return NULL;
}

static rev_ref *
rev_ref_find_name(rev_ref *h, const char *name)
/* find a revision reference by name */
{
    for (; h; h = h->next)
	if (h->ref_name == name)
	    return h;
    return NULL;
}

static bool
parents_in_revlist(const char *child_name, rev_ref *rev_list,
		   cvs_master *source, size_t nsource)
/*
 * See whether all the parents of child_name are in rev_list
 * If child_name has no parents (e.g. master branch) then this is
 * trivally true.
 *
 * Parent branch names are determined by examining every cvs master.  See the
 * general note on branch matching under collate_changesets().
 */
{
    cvs_master *cm;
    for (cm = source; cm < source + nsource; cm++) {
	rev_ref *head = rev_find_head(cm, child_name);
	if (head) {
	    if (head->parent && !rev_ref_find_name(rev_list, head->parent->ref_name))
		    return false;
	}
    }
    return true;
}

static rev_ref *
rev_ref_tsort(rev_ref *git_branches, cvs_master *masters, size_t nmasters)
/* Sort a list of git space branches so parents come before children */
{
    rev_ref *sorted_git_branches = NULL;
    rev_ref **sorted_tail = &sorted_git_branches;
    rev_ref *r, **prev;

    while (git_branches) {
	/* search the remaining input list */
	for (prev = &git_branches; (r = *prev); prev = &(*prev)->next) {
            /*
	     * Find a branch where we've already sorted its parents.
	     * Toposorting with this relation will put the (parentless) trunk first,
	     * and child branches after their respective parent branches.
	     */
	    if (parents_in_revlist(r->ref_name, sorted_git_branches, masters, nmasters)) {
		break;
	    }
	}
	if (!r) {
	    announce("internal error - branch cycle\n");
	    return NULL;
	}
        /*
         * Remove the found branch from the input list and
         * append it to the output list
         */
	*prev = r->next;
	*sorted_tail = r;
	r->next = NULL;
	sorted_tail = &r->next;
    }
    return sorted_git_branches;
}

static int
cvs_commit_date_compare(const void *av, const void *bv)
{
    const cvs_commit	*a = REVISION_T_COMMIT(*(revision_t *) av);
    const cvs_commit	*b = REVISION_T_COMMIT(*(revision_t *) bv);
    int			t;

    /*
     * NULL entries sort last
     */
    if (!a && !b)
	return 0;
    else if (!a)
	return 1;
    else if (!b)
	return -1;
    /*
     * tailed entries sort next
     */
    if (a->tailed != b->tailed)
	return(int)a->tailed - (int)b->tailed;
    /*
     * Newest entries sort first
     */
    t = -time_compare(a->date, b->date);
    if (t)
	return t;
    /*
     * Ensure total order by ordering based on commit address
     */
    if ((uintptr_t) a > (uintptr_t) b)
	return -1;
    if ((uintptr_t) a < (uintptr_t) b)
	return 1;
    return 0;
}

static cvs_commit *
cvs_commit_latest(cvs_commit **commits, int ncommit)
/* find newest live commit in a set */
{
    cvs_commit *max = NULL, **c;
    for (c = commits; c < commits + ncommit; c++)
	if ((*c) && !(*c)->dead)
	    if (!max || time_compare((*c)->date, max->date) > 0)
		max = (*c);
    return max;
}

static int
cvs_commit_date_sort(revision_t *commits, int ncommit)
/* sort CVS commits by date */
{
    qsort(commits, ncommit, sizeof(revision_t), cvs_commit_date_compare);
    /*
     * Trim off NULL entries
     */
    while (ncommit && !REVISION_T_COMMIT(commits[ncommit-1]))
	ncommit--;
    return ncommit;
}

static bool
cvs_commit_time_close(const cvstime_t a, const cvstime_t b)
/* are two timestamps within the commit-coalescence window of each other? */
{
    long	diff = (long)a - (long)b;
    if (diff < 0) diff = -diff;
    if (diff < commit_time_window)
	return true;
    return false;
}

typedef enum {no, yes, maybe} tribool;

static tribool
cvs_commitid_match(const cvs_commit *a, const cvs_commit *b)
{
    if (trust_commitids)
    {
	/*
	 * Versions of GNU CVS after 1.12 (2004) place a commitid in
	 * each commit to track patch sets. Use it if present
	 */
	if (a->commitid && b->commitid)
	    return (a->commitid == b->commitid ? yes : no);
	if (a->commitid || b->commitid)
	    return no;
    }

    return maybe;
}

static bool
cvs_commit_match(const cvs_commit *a, const cvs_commit *b)
/* are two CVS commits eligible to be coalesced into a changeset? */
{
    tribool idcheck = cvs_commitid_match(a, b);

    switch (idcheck) {
    case yes:
	return true;
    case no:
	return false;
    case maybe:
	break;
    }

    if (!cvs_commit_time_close(a->date, b->date))
	return false;
    if (a->log != b->log)
	return false;
    if (a->author != b->author)
	return false;
    return true;
}

/*
 * These statics are part of an optimization to reduce allocation calls
 * by only doing one when more memory needs to be grabbed than the
 * previous commit build used.
 */
static const cvs_commit **files = NULL;
static int	        sfiles = 0;

static void
git_commit_cleanup(void)
/* clean up after rev list collate */
{
    if (files) {
	free(files);
	files = NULL;
	sfiles = 0;
    }
}

static git_commit *
git_commit_build(revision_t *revisions, const cvs_commit *leader, const int nrevisions)
/* build a changeset commit from a clique of CVS revisions */
{
    size_t     n;
    git_commit *commit;

    commit = xmalloc(sizeof(git_commit), "creating commit");

    commit->parent = NULL;
    commit->date = leader->date;
    commit->commitid = leader->commitid;
    commit->log = leader->log;
    commit->author = leader->author;
    commit->tail = commit->tailed = false;
    commit->dead = false;
    commit->refcount = commit->serial = 0;

    revdir_pack_init();
    for (n = 0; n < nrevisions; n++) {
	if (REVISIONS(n) && !(DEAD(n))) {
	    revdir_pack_add(REVISIONS(n), DIR(n));
	}
    }
    revdir_pack_end(&commit->revdir);

#ifdef ORDERDEBUG
    debugmsg("commit_build: %p\n", commit);

#if !defined STREAMDIR
    for (n = 0; n < nfile; n++)
	if (REVISIONS(n))
	    debugmsg("%s\n", REVISIONS(n)->master->name);
#endif
    fputs("After packing:\n", LOGFILE);
    revdir_iter *i = revdir_iter_alloc(&commit->revdir);
    cvs_commit *c;
    while((c = revdir_iter_next(i)))
	debugmsg("   file name: %s\n", c->master->name);

#endif /* ORDERDEBUG */

    return commit;
}

static git_commit *
git_commit_locate_date(const rev_ref *branch, const cvstime_t date)
/* on branch, locate a commit within fuzz-time distance of date */
{
    git_commit	*commit;

    /* PUNNING: see the big comment in cvs.h */
    for (commit = (git_commit *)branch->commit; commit; commit = commit->parent)
    {
	if (time_compare(commit->date, date) <= 0)
	    return commit;
    }
    return NULL;
}

static git_commit *
git_commit_locate_one(const rev_ref *branch, const cvs_commit *part)
/* seek a gitspace commit on branch incorporating cvs_commit */
{
    git_commit	*commit;

    if (!branch)
	return NULL;

    /* PUNNING: see the big comment in cvs.h */
    for (commit = (git_commit *)branch->commit;
	 commit;
	 commit = commit->parent)
    {
	/* PUNNING: see the big comment in cvs.h */
	if (cvs_commit_match((cvs_commit *)commit, part))
	    return commit;
    }
    return NULL;
}

static git_commit *
git_commit_locate_any(const rev_ref *branch, const cvs_commit *part)
/* seek a gitspace commit on *any* branch incorporating cvs_commit */
{
    git_commit	*commit;

    if (!branch)
	return NULL;
    commit = git_commit_locate_any(branch->next, part);
    if (commit)
	return commit;
    return git_commit_locate_one(branch, part);
}

static git_commit *
git_commit_locate(const rev_ref *branch, const cvs_commit *cm)
{
    git_commit	*commit;

    /*
     * Check the presumed trunk first
     */
    commit = git_commit_locate_one(branch, cm);
    if (commit)
	return commit;
    /*
     * Now look through all branches
     */
    while (branch->parent)
	branch = branch->parent;
    return git_commit_locate_any(branch, cm);
}

static rev_ref *
git_branch_of_commit(const git_repo *gl, const cvs_commit *commit)
/* return the gitspace branch head that owns a specified CVS commit */
{
    rev_ref	*h;
    cvs_commit	*c;

    for (h = gl->heads; h; h = h->next)
    {
	if (h->tail)
	    continue;
	for (c = h->commit; c; c = c->parent) {
	    if (cvs_commit_match(c, commit))
		return h;
	    if (c->tail)
		break;
	}
    }
    return NULL;
}

static cvstime_t
cvs_commit_first_date(cvs_commit *commit)
/* return time of first commit along entire history */
{
    while (commit->parent)
	commit = commit->parent;
    return commit->date;
}

static void
collate_branches(rev_ref **branches, int nbranch,
		  rev_ref *branch, git_repo *gl)
/* collate a set of per-CVS-master branches into a gitspace DAG branch */
{
    int nlive;
    int n;
    git_commit *prev = NULL;
    git_commit *head = NULL, **tail = &head;
    revision_t *revisions = xmalloc(nbranch * sizeof(revision_t), "collating per-file branches");
    git_commit *commit;
    cvs_commit *latest;
    revision_t *p;
    time_t birth = 0;

    /*
     * It is expected that the array of input branches is all CVS branches
     * tagged with some single branch name. The job of this code is to
     * build the changeset sequence for the corresponding named git branch,
     * then graft it to its parent git branch.  Note that the main loop walks
     * backwards from each branch tip.
     */
    nlive = 0;
    for (n = 0; n < nbranch; n++) {
	/*
	 * Initialize revisions to head of each branch (that is, the
	 * most recent entry).
	 */
	cvs_commit *c = branches[n]->commit;
	REVISION_T_PACK_INIT(revisions[n], c);
	/*
	 * Compute number of CVS branches that are still live - that is,
	 * have remaining older CVS file commits for this branch. Non-live
	 * branches are reachable by parent-of links from the named head
	 * reference but we're past their branch point from a parent with
	 * a different name (also in our set of heads).
	 */
	if (!c)
	    continue;
	if (branches[n]->tail) {
	    c->tailed = true;
	    continue;
	}
	nlive++;

	/*
	 * This code updates our notion of the start date for the
	 * gitspace branch - that is, the date of the oldest CVS
	 * commit contributing to it.  Once we've walked all the CVS
	 * branches, 'start' should hold that oldest commit date.
	 */
	while (c && !c->tail) {
	    if (!birth || time_compare(c->date, birth) < 0)
		birth = c->date;
	    c = c->parent;
	}
	if (c && (!c->dead || c->date != c->parent->date)) {
	    if (!birth || time_compare(c->date, birth) < 0)
		birth = c->date;
	}
    }

    /*
     * This is a sanity check done just once for each gitspace
     * branch. If any of the commits at our CVS branch heads is older
     * than the git branch's imputed start date, something is badly
     * wrong.  In a sane universe with a synchronous clock this
     * shouldn't be possible, but the CVS universe is not sane and
     * attempts to do time ordering among branches can be confused by
     * clock skew on the CVS clients.
     */
    for (n = 0; n < nbranch; n++) {
	cvs_commit *c = REVISIONS(n);
	if (!c->tailed)
	    continue;
	if (!birth || time_compare(birth, c->date) >= 0)
	    continue;
	if (!c->dead) {
	    warn("warning - %s branch %s: tip commit older than imputed branch join\n",
		     c->master->name, branch->ref_name);
	    continue;
	}

	// cppcheck-suppress nullPointer
	REVISION_T_PACK(revisions[n], (cvs_commit *)NULL);
    }

    /*
     * Walk down CVS branches creating gitspace commits until each CVS
     * branch has collated with its parent.
     */
    while (nlive > 0 && nbranch > 0) {
	/*
	 * Gather the next set of CVS commits down the branch and
	 * figure out which (non-tailed) one of them is latest in
	 * time.  It will be the leader for the git commit build.
	 */
	for (n = 0, p = revisions, latest = NULL; n < nbranch; n++) {
	    /*
	     * Squeeze null commit pointers out of the current set.
	     */
	    cvs_commit *rev = REVISIONS(n);
	    if (!rev)
		continue;
	    *p++ = revisions[n];
	    if (rev->tailed)
		continue;
	    if (!latest || time_compare(latest->date, rev->date) < 0)
		latest = rev;
	}
	assert(latest != NULL);
	nbranch = p - revisions;

	/*
	 * Construct current commit from the set of CVS commits
	 * accumulated the last time around the loop.
	 * This is the point at which revisions needs to be sorted
	 * by master for rev dir packing to perform reasonably.
	 */
	commit = git_commit_build(revisions, latest, nbranch);

	/*
	 * Step down each CVS branch in parallel.  Our goal is to land on
	 * a clique of matching CVS commits that will  be made into a
	 * matching gitspace commit on the next time around the loop.
	 */
	nlive = 0;
	for (n = 0; n < nbranch; n++) {
	    cvs_commit *c = REVISIONS(n);
	    cvs_commit *to;
	    /* already got to parent branch? */
	    if (c->tailed)
		continue;
	    /* not affected? */
	    if (c != latest && !cvs_commit_match(c, latest)) {
		if (c->parent || !c->dead)
		    nlive++;
		continue;
	    }
#ifdef GITSPACEDEBUG
	    if (c->gitspace) {
		warn("CVS commit allocated to multiple git commits: ");
		dump_number_file(LOGFILE, c->master->name, c->number);
		warn("\n");
	    } else
#endif /* GITSPACEDEBUG */
		c->gitspace = commit;

	    to = c->parent;
	    /*
	     * CVS branch starts here?  If so, drop it out of
	     * the revision set and keep going.
	     */
	    if (!to)
		goto Kill;

	    if (c->tail) {
		/*
		 * Adding file independently added on another
		 * non-trunk branch.
		 */
		if (!to->parent && to->dead)
		    goto Kill;
		/*
		 * If the parent is at the beginning of trunk
		 * and it is younger than some events on our
		 * branch, we have old CVS adding file
		 * independently added on another branch.
		 */
		if (birth && time_compare(birth, to->date) < 0)
		    goto Kill;
		/*
		 * XXX: we still can't be sure that it's
		 * not a file added on trunk after parent
		 * branch had forked off it but before
		 * our branch's creation.
		 */
		to->tailed = true;
	    } else if (!to->dead) {
		nlive++;
	    } else {
		/*
		 * See if it's recent CVS adding a file
		 * independently added on another branch.
		 */
		if (!to->parent)
		    goto Kill;
		if (to->tail && to->date == to->parent->date)
		    goto Kill;
		nlive++;
	    }

	    /*
	     * Commit is either not tailed or passed all the special-case
	     * tests for tailed commits. Leave it in the set for the next
	     * changeset construction.
	     */
	    REVISION_T_PACK(revisions[n], to);
	    continue;
	Kill:
	    // cppcheck-suppress nullPointer
	    REVISION_T_PACK(revisions[n], (cvs_commit *)NULL);
	}

	*tail = commit;
	tail = &commit->parent;
	prev = commit;
    }

    /*
     * Gitspace branch construction is done. Now connect it to its
     * parent branch.  The CVS commits now referenced in the revisions
     * array are for the oldest commit on the branch (the last clique
     * to be collected in the previous phase).  This is not the brahch's
     * root commit, but the child of that root.
     */
    nbranch = cvs_commit_date_sort(revisions, nbranch);
    if (nbranch && branch->parent )
    {
	int	present;

	for (present = 0; present < nbranch; present++) {
	    if (!DEAD(present)) {
		/*
		 * Skip files which appear in the repository after
		 * the first commit along the branch
		 */
		if (prev && REVISIONS(present)->date > prev->date &&
		    REVISIONS(present)->date == cvs_commit_first_date(REVISIONS(present)))
		{
		    /* FIXME: what does this mean? */
		    warn("file %s appears after branch %s date\n",
			 REVISIONS(present)->master->name, branch->ref_name);
		    continue;
		}
		break;
	    }
	}
	if (present == nbranch)
	    /*
	     * Branch join looks normal, we can just go ahead and build
	     * the last commit.
	     */
	    *tail = NULL;
	else if ((*tail = git_commit_locate_one(branch->parent,
						REVISIONS(present))))
	{
	    if (prev && time_compare((*tail)->date, prev->date) > 0) {
		cvs_commit *first;
		warn("warning - branch point %s -> %s later than branch\n",
			 branch->ref_name, branch->parent->ref_name);
		warn("\ttrunk(%3d):  %s %s", n,
		     cvstime2rfc3339(REVISIONS(present)->date),
		     DEAD(present) ? "D" : " " );
		if (!DEAD(present))
		    dump_number_file(LOGFILE,
				     REVISIONS(present)->master->name,
				     REVISIONS(present)->number);
		fprintf(LOGFILE, "\n");
		/*
		 * The file part of the error message could be spurious for
		 * a multi-file commit, alas.  It wasn't any better back when
		 * both flavors of commit had dedicated 'file' members; the
		 * problem is that we can't actually know which CVS file
		 * commit is the right one for purposes of this message.
		 * (uniform warn messages jw 20151122)
		 */
		warn("\tbranch(%3d): %s  ", n,
		    cvstime2rfc3339(prev->date));
		revdir_iter *ri = revdir_iter_alloc(&prev->revdir);
		first = revdir_iter_next(ri);
		free(ri);
		dump_number_file(LOGFILE,
				  first->master->name,
				  first->number);
		fprintf(LOGFILE, "\n");
	    }
	} else if ((*tail = git_commit_locate_date(branch->parent,
						   REVISIONS(present)->date)))
	    warn("warning - branch point %s -> %s matched by date\n",
		     branch->ref_name, branch->parent->ref_name);
	else {
	    rev_ref	*lost;
	    warn("error - branch point %s -> %s not found.",
		branch->ref_name, branch->parent->ref_name);

	    if ((lost = git_branch_of_commit(gl, REVISIONS(present))))
		warn(" Possible match on %s.", lost->ref_name);
	    fprintf(LOGFILE, "\n");
	}
	if (*tail) {
	    if (prev)
		prev->tail = true;
	} else {
	    *tail = git_commit_build(revisions, REVISIONS(0), nbranch);
	    for (n = 0; n < nbranch; n++)
		if (REVISIONS(n)) {
#ifdef GITSPACEDEBUG
		    if (REVISIONS(n)->gitspace) {
			warn("CVS commit allocated to multiple git commits: ");
			dump_number_file(LOGFILE,
					 REVISIONS(n)->master->name,
					 REVISIONS(n)->number);
			warn("\n");
		    } else
#endif /* GITSPACEDEBUG */
			REVISIONS(n)->gitspace = *tail;
		}
	}
    }

    for (n = 0; n < nbranch; n++)
	if (REVISIONS(n))
	    REVISIONS(n)->tailed = false;

    free(revisions);
    /* PUNNING: see the big comment in cvs.h */
    branch->commit = (cvs_commit *)head;
}

static bool
git_commit_contains_revs(git_commit *g, cvs_commit **revs, size_t nrev)
/* Check whether the commit is made up of the supplied file list.
 * List mut be sorted in path_deep_compare order.
 */
{
    revdir_iter *it = revdir_iter_alloc(&g->revdir);
    size_t i = 0;
    cvs_commit *c = NULL;
    static bool n_init = false;
    static const cvs_number *n1 = NULL;
    static const cvs_number *n2 = NULL;

    if (!n_init) {
	n1 = atom_cvs_number(lex_number("1.1"));
	n2 = atom_cvs_number(lex_number("1.1.1.1"));
	n_init = true;
    }
    /* order of checks is important */
    while ((c = revdir_iter_next(it)) && i < nrev) {
	if (revs[i] != c) {
	    // seen repos where 1.1 and 1.1.1.1 are used interchangeably
	    if (revs[i]->master != c->master
		|| (revs[i]->number != n1 && revs[i]->number != n2)
		|| (c->number != n1 && c->number != n2)) {
		free(it);
		return false;
	    }
	}
	i++;
    }
    free(it);
    /* check we got to the end of both */
    return (i == nrev) && (!c);

}

static int
compare_cvs_commit(const void *a, const void *b)
{
    cvs_commit **ap = (cvs_commit **)a;
    cvs_commit **bp = (cvs_commit **)b;
    const char *af = (*ap)->master->name;
    const char *bf = (*bp)->master->name;

#ifdef ORDERDEBUG
    warn("Comparing %s with %s\n", af, bf);
#endif /* ORDERDEBUG */

    return path_deep_compare(af, bf);
}

/*
 * Locate position in git tree corresponding to specific tag
 */
static void
rev_tag_search(tag_t *tag, cvs_commit **revisions, git_repo *gl)
{
    /*
     * The cvs_commit->gitspace pointer gives the first git commit a
     * cvs commit appears in. (first in DAG pre-order) If we find the
     * newest revision C in the tag and then follow the backlink to G,
     * there is a good chance this will be the tag point.
     *
     * Consider, if any future git commits add files, then these files
     * would be newer than C, and hence not in the tag set.
     *
     * However, this argument doesn't work if a subsequent commit only
     * deletes files (or a set of commits has this net effect) So, we
     * first check whether G has a matching set of revisions to the
     * tag. If so, we're done.
     *
     * If not, we search the whole tree (pruning where possible)
     * for a matching set of revisions.
     *
     * If this doesn't work we create branch from G with a single
     * commit with the correct revisions.
     *
     * It is possible for multiple git commits to contain the same
     * set of cvs revisions.
     *
     * Tags can point to dead commits, we ignore these as they
     * don't get backlinks to git commits. This may get revisited later.
     */
    cvs_commit *c = cvs_commit_latest(revisions, tag->count);
    if (!c)	/* only dead revisions in the tag */
	return;
    if (c->gitspace == NULL) {
	char buf[CVS_MAX_REV_LEN + 1];
	warn("%s %s: %s points at commit with no gitspace link.\n",
	     c->master->name,
	     cvs_number_string(c->number, buf, sizeof(buf)),
	     tag->name);
	return;
    }

    qsort(revisions, tag->count, sizeof(cvs_commit *), compare_cvs_commit);
    if (git_commit_contains_revs(c->gitspace, revisions, tag->count)) {
	/* we've seen this set of revisions before, just link tag to it */
	tag->commit = c->gitspace;
	return;
    } else {
	/* Search to try and find a matching git commit.
	 * We can prune if we get to c->gitspace.
         * We can prune if we get to an older commit than c->gitspace.
         * We could also use tail-bits here to avoid checking the same
         * commit multiple times, but we haven't built them yet.
         * If we build them before tagging we would need to teach
         * this code how to write correct tail bits in the branches it
         * creates.
	 *
	 * This section can also find revisions in the branches
	 * we add below.
	 *
	 * Emacs has one place with 35 tags pointing to the same
	 * revision set, so this saves 34 branches.
	 */
	rev_ref    *h;
	git_commit *g;

	for (h = gl->heads; h; h = h->next) {
	    if (h->tail)
		continue;
	    /* PUNNING: See large comment in cvs.h */
	    for (g = (git_commit *)h->commit; g; g = g->parent) {
		if (g == c->gitspace)
		    break;
		if (time_compare(g->date, c->gitspace->date) < 0)
		    break;
		if (git_commit_contains_revs(g, revisions, tag->count)) {
		    tag->commit = g;
		    return;
		}
	    }
	}
    }
    /* Tagging mechanism for incomplete tags
     *
     * The tag doesn't point to a previously seen set of revisions.
     * The old code just did "tag->commit = c->gitspace;" at this point.
     *
     * Create a new branch with the tag name and join at the inferred
     * join point. The join point is the earliest one that makes
     * sense, but it may have happened later. However, if you check the
     * tag out you will get the correct set of files.
     * We have no way of knowing the correct author of a tag.
     */
    revision_t *revs = xmalloc(sizeof(revision_t) * tag->count, __func__);
    size_t i;
    for (i = 0; i < tag->count; i++)
	REVISION_T_PACK_INIT(revs[i], revisions[i]);
    git_commit *g = git_commit_build(revs, c, tag->count);
    free(revs);
    g->parent = c->gitspace;
    rev_ref *parent_branch = git_branch_of_commit(gl, c);
    rev_ref *tag_branch = xcalloc(1, sizeof(rev_ref), __func__);
    tag_branch->parent = parent_branch;
    /* type punning */
    tag_branch->commit = (cvs_commit *)g;
    tag_branch->ref_name = tag->name;
    tag_branch->depth = parent_branch->depth + 1;
    rev_ref *r;
    /* Add tag branch to end of list to maintain toposort */
    for (r = gl->heads; r->next; r = r->next)
	continue;
    r->next = tag_branch;
    g->author = atom("cvs-fast-export");
    size_t len = strlen(tag->name) + 42;
    char *log = xmalloc(len, __func__);
    snprintf(log, len, "Synthetic commit for incomplete tag %s\n", tag->name);
    g->log = atom(log);
    free(log);
}

static void
rev_ref_set_parent(git_repo *gl, rev_ref *dest, cvs_master *source, size_t nmasters)
/* compute parent relationships among gitspace branches */
{
    cvs_master	*s;
    rev_ref	*p, *max;

    if (dest->depth)
	return;

    max = NULL;
    for (s = source; s < source + nmasters; s++) {
	rev_ref	*sh;
	sh = rev_find_head(s, dest->ref_name);
	if (!sh)
	    continue;
	if (!sh->parent)
	    continue;
	p = rev_find_head((head_list*)gl, sh->parent->ref_name);
	assert(p);
	rev_ref_set_parent(gl, p, source, nmasters);
	if (!max || p->depth > max->depth)
	    max = p;
    }
    dest->parent = max;	/* where the magic happens */
    if (max)
	dest->depth = max->depth + 1;
    else
	dest->depth = 1;
}

git_repo *
collate_to_changesets(cvs_master *masters, size_t nmasters, int verbose)
/* entry point - collate CVS revision lists to a gitspace DAG */
{
    size_t	head_count = 0;
    int		n; /* used only in progress messages */
    git_repo	*gl = xcalloc(1, sizeof(git_repo), "list collate");
    cvs_master	*cm;
    rev_ref	*lh, *h;
    tag_t	*t;
    rev_ref	**refs = xcalloc(nmasters, sizeof(rev_ref *), "list collate");

    /*
     * It is expected that the branch trees in all CVS masters have
     * equivalent sets of parent-child relationships, but not
     * necessarily that the branch nodes always occur in the same
     * order. Equivalently, it may not be the case that the branch IDs
     * of equivalent named branches in different masters are the
     * same. So the only way we can group CVS branches into cliques
     * that should be bundled into single gitspace branches is by the
     * labels at their tips.
     *
     * First, find all of the named heads across all of the incoming
     * CVS trees.  Use them to initialize named branch heads in the
     * output list.  Yes, this is currently very inefficient.
     */
    progress_begin("Make DAG branch heads...", nmasters);
    n = 0;
    for (cm = masters; cm < masters + nmasters; cm++) {
	for (lh = cm->heads; lh; lh = lh->next) {
	    h = rev_find_head((head_list *)gl, lh->ref_name);
	    if (!h) {
		head_count++;
		rev_list_add_head((head_list *)gl, NULL, lh->ref_name, lh->degree);
	    } else if (lh->degree > h->degree)
		h->degree = lh->degree;
	}
	if (++n % 100 == 0)
	    progress_jump(n);
    }
    progress_jump(n);
    progress_end(NULL);
    /*
     * Sort by degree so that finding branch points always works.
     * In later operations we always want to walk parent branches
     * before children, with trunk first.
     */
    progress_begin("Sorting...", nmasters);
    gl->heads = rev_ref_tsort(gl->heads, masters, nmasters);
    if (!gl->heads) {
	free(refs);
	/* coverity[leaked_storage] */
	return NULL;
    }
    progress_end(NULL);

    if (verbose) {
	/*
	 * Display the result of the branch toposort.
	 * The "master" branch should always be at the front
	 * of the list.
	 */
	debugmsg("Sorted branches are:\n");
	for (h = gl->heads; h; h = h->next)
	    debugmsg("head %s(%d)\n", h->ref_name, h->degree);
    }
    /*
     * Compute branch parent relationships.
     */
    progress_begin("Compute branch parent relationships...", head_count);
    for (h = gl->heads; h; h = h->next) {
	rev_ref_set_parent(gl, h, masters, nmasters);
	progress_step();
    }
    progress_end(NULL);

#ifdef ORDERDEBUG
    fputs("collate_to_changesets: before common branch collate:\n", stderr);
    for (cm = masters; cm < masters + nmasters; cm++) {
	for (lh = cm->heads; lh; lh = lh->next) {
	    cvs_commit *commit = lh->commit;
	    fputs("rev_ref: ", stderr);
	    dump_number_file(stderr, lh->ref_name, lh->number);
	    fputc('\n', stderr);
	    fprintf(stderr, "commit first file: %s\n", commit->master->name);
	}
    }
#endif /* ORDERDEBUG */

    /*
     * Collate common branches
     */

    progress_begin("Collate common branches...", head_count);
    revdir_pack_alloc(nmasters);
    for (h = gl->heads; h; h = h->next) {
	/*
	 * For this imputed gitspace branch, locate the corresponding
	 * set of CVS branches from every master.
	 */
	int nref = 0;
	for (cm = masters; cm < masters + nmasters; cm++) {
	    lh = rev_find_head(cm, h->ref_name);
	    if (lh) {
		refs[nref++] = lh;
	    }
	}
	if (nref)
	    /*
	     * Collate those branches into a single gitspace branch
	     * and add that to the output revlist on gl.
	     */
	    collate_branches(refs, nref, h, gl);
	progress_step();
    }
    progress_end(NULL);


#ifdef GITSPACEDEBUG
    /* Check every non-dead cvs commit has a backlink
     * and that every pair of linked commits match
     * according to cvs_commit_match.
     * (note this will check common parents multiple times)
     */
    for (cm = masters; cm < masters + nmasters; cm++) {
	for (lh = cm->heads; lh; lh = lh->next) {
	    cvs_commit *c = lh->commit;
	    for (; c; c = c->parent) {
		if (!c->gitspace) {
		    if (!c->dead) {
			fprintf(LOGFILE, "No gitspace: ");
			dump_number_file(LOGFILE, c->master->name, c->number);
			fprintf(LOGFILE, "\n");
		    }
		} else if (!cvs_commit_match(c, (cvs_commit *)c->gitspace)) {
		    fprintf(LOGFILE, "Gitspace doesn't match cvs: ");
		    dump_number_file(LOGFILE, c->master->name, c->number);
		    fprintf(LOGFILE, "\n");
		}
	    }
	}
    }
#endif /* GITSPACEDEBUG */

    /*
     * Find tag locations.  The goal is to associate each tag object
     * (which normally corresponds to a clique of named tags, one per master)
     * with the right gitspace commit.
     */
    progress_begin("Find tag locations...", tag_count);
    for (t = all_tags; t; t = t->next) {
	cvs_commit **commits = tagged(t);
	if (commits)
	    rev_tag_search(t, commits, gl);
	else
	    announce("internal error - lost tag %s\n", t->name);
	free(commits);
	progress_step();
    }
    revdir_pack_free();
    revdir_free_bufs();
    progress_end(NULL);

    /*
     * Compute 'tail' values.  These allow us to recognize branch joins
     * so we can write efficient traversals that walk branches without
     * wandering on to their parent branches.
     */
    progress_begin("Compute tail values...", NO_MAX);
    rev_list_set_tail((head_list *)gl);
    progress_end(NULL);

    free(refs);

    //progress_begin("Validate...", NO_MAX);
    //rev_list_validate(gl);
    //progress_end(NULL);

    git_commit_cleanup();

    return gl;
}

/*
 * Generate a list of files in uniq that aren't in common
 */
static cvs_commit_list *
rev_uniq_file(git_commit *uniq, git_commit const *common, int *nuniqp)
{
    int nuniq = 0;
    cvs_commit_list   *head = NULL, **tail = &head, *fl;
    cvs_commit *c;

    if (!uniq)
	return NULL;
    revdir_iter *ri = revdir_iter_alloc(&uniq->revdir);
    while ((c = revdir_iter_next(ri))) {
	if (c->gitspace != common) {
	    fl = xcalloc(1, sizeof(cvs_commit_list), "rev_uniq_file");
	    fl->file = c;
	    *tail = fl;
	    tail = &fl->next;
	    ++nuniq;
	}
    }
    free(ri);
    *nuniqp = nuniq;
    return head;
}

/*
 * Generate a diff between two gitspace commits. Either may be NULL
 */
rev_diff *
git_commit_diff(git_commit *old, git_commit *new)
{
    rev_diff	*diff = xcalloc(1, sizeof(rev_diff), __func__);

    diff->del = rev_uniq_file(old, new, &diff->ndel);
    diff->add = rev_uniq_file(new, old, &diff->nadd);
    return diff;
}

static void
cvs_commit_list_free(cvs_commit_list *fl)
{
    cvs_commit_list   *next;

    while (fl) {
	next = fl->next;
	free(fl);
	fl = next;
    }
}

void
rev_diff_free(rev_diff *d)
{
    cvs_commit_list_free(d->del);
    cvs_commit_list_free(d->add);
    free(d);
}

/* end */
