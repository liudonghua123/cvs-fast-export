/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  SPDX-License-Identifier: GPL-2.0+
 *
 * A note on the "child commit emitted before parent exists" error:
 *
 * This code is somewhat complex because the natural order of operations
 * generated by the file-traversal operations in the rest of the code is
 * not even remotely like the canonical order generated by git-fast-export.
 * We want to emulate the latter in order to make regression-testing and
 * comparisons with other tools as easy as possible.
 *
 * Until 2020 there used to be support for a fast mode that did what's
 * natural given the way the analysis engine works - all blobs were
 * shipped first and then commits, avoiding the necessity to write
 * tempfiles and then copy blob data around on disk. When this export
 * code was first written in 2013, fast mode was good for about a 2x
 * throughput increase.  Fast mode was abandoned because SSDs have since
 * reduced the cost of that disk shuffle, and having two different output
 * code paths became more work than it was worth.
 *
 * This decision is recorded here because it allows the theoretical
 * possibility of a CVS collection that cannot be successfully lifted
 * without the old-fast mode code. Suspect this if you ever get a
 * "child commit emitted before parent exists" error that can't be
 * resolved with the -t 0 option.
 *
 * The problem is the shuffle done to put the generated gitspace
 * commits in time order is only correctness-preserving if time order
 * is never the reverse of topological (parent-child) order; otherwise
 * a commit could be shipped before its parent.  This can happen
 * sporadically in multithreaded mode due to a race condition in
 * accumulating CVS changes with identical timestamps.  One of the
 * test cases, t9601, exhibits this behavior if -t 0 is not forced.
 *
 * No case of order inversion has been observed in the wild with
 * cliques having *different* timestamps.  Unfortunately, CVS
 * timestamps are generated client-side, not server side, so
 * sufficiently bad skew between client-machine clocks could produce
 * such an inversion.  A hacker on machine A committed a change at time T,
 * then a hacker on machine B committed a change at time T+n seconds
 * with the A change as parent, but machine B's clock is skew by more
 * than -n seconds so the timestamps are in the wrong order and the
 * parent-child order gets messed up when the generated commits are
 * time-sorted.
 *
 * If this ever happens, it will probably be on a project history from
 * before the general deployment of NTP in the early 1990s, which
 * narrowed expected time skew to on the close order of 1 second.
 * That's not a large time window after the first release of CVS in
 * 1990, but some CVS projects inherited older and less precise
 * timestamps from RCS and SCCS - those fossils have been seen in the
 * wild, notably in the histories of GCC and Emacs.
 *
 * If you see this error with -t 0, report it as a bug.  It might mean
 * we have to bite the bullet and implement a real toposort here, or it 
 * might just mean some timestamps in your masters need to be patched
 * by a few seconds.
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ftw.h>
#include <time.h>

#include "cvs.h"
#include "revdir.h"
/*
 * If a program has ever invoked pthreads, the GNU C library does extra
 * checking during stdio operations even if the program no longer has
 * active subthreads.  Foil this with a GNU extension.  Doing this nearly
 * doubled throughput on the benchmark repositories.
 *
 * The unlocked_stdio(3) manual pages claim that fputs_unlocked) and
 * fclose_unlocked exist, but they don't actually seem to.
 */
#ifdef __GLIBC__
#define fread	fread_unlocked
#define fwrite	fwrite_unlocked
#define putchar	putchar_unlocked
#define fputc   fputc_unlocked
#define feof    feof_unlocked
#endif /* __GLIBC__ */

static serial_t *markmap;
static serial_t mark;
static volatile int seqno;
static char blobdir[PATH_MAX];

static export_stats_t export_stats;

static int seqno_next(void)
/* Returns next sequence number, starting with 1 */
{
    ++seqno;

    if (seqno >= MAX_SERIAL_T)
	fatal_error("snapshot sequence number too large, widen serial_t");

    return seqno;
}

/*
 * GNU CVS default ignores.  We omit from this things that CVS ignores
 * by default but which are highly unlikely to turn up outside an
 * actual CVS repository and should be conspicuous if they do: RCS
 * SCCS CVS CVS.adm RCSLOG cvslog.*
 */
#define CVS_IGNORES "# CVS default ignores begin\ntags\nTAGS\n.make.state\n.nse_depinfo\n*~\n\\#*\n.#*\n,*\n_$*\n*$\n*.old\n*.bak\n*.BAK\n*.orig\n*.rej\n.del-*\n*.a\n*.olb\n*.o\n*.obj\n*.so\n*.exe\n*.Z\n*.elc\n*.ln\ncore\n# CVS default ignores end\n"

static char *blobfile(const char *basename,
		      const int serial,
		      const bool create, char *path)
/* Random-access location of the blob corresponding to the specified serial */
{
    int m;

#ifdef FDEBUG
    (void)fprintf(stderr, "-> blobfile(%d, %d)...\n", serial, create);
#endif /* FDEBUG */
    (void)snprintf(path, PATH_MAX, "%s", blobdir);
    /*
     * FANOUT should be chosen to be the largest directory size that does not
     * cause slow secondary allocations.  It's something near 256 on ext4
     * (we think...)
     */
#define FANOUT	256
    for (m = serial;;)
    {
	int digit = m % FANOUT;
	if ((m = (m - digit) / FANOUT) == 0) {
	    (void)snprintf(path + strlen(path), PATH_MAX - strlen(path),
			   "/=%x", digit);
#ifdef FDEBUG
	    (void)fprintf(stderr, "path: %s\n", path);
#endif /* FDEBUG */
	    break;
	}
	else
	{
	    (void)snprintf(path + strlen(path), PATH_MAX - strlen(path),
			   "/%x", digit);
	    /* coverity[toctou] */
#ifdef FDEBUG
	    (void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
	    if (create && access(path, R_OK) != 0) {
#ifdef FDEBUG
		(void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
		errno = 0;
		if (mkdir(path,S_IRWXU | S_IRWXG) != 0 && errno != EEXIST)
		    fatal_error("blob subdir creation of %s failed: %s (%d)\n", path, strerror(errno), errno);
	    }
	}
    }
#undef FANOUT
#ifdef FDEBUG
    (void)fprintf(stderr, "<- ...returned path for %s %d = %s\n",
		  basename, serial, path);
#endif /* FDEBUG */
    return path;
}

static void export_blob(node_t *node, 
			void *buf, const size_t len,

			export_options_t *opts)
/* output the blob, or save where it will be available for random access */
{
    /*
     * This is the only place we do interpretation of ignores.
     *
     * Fortunately, git's ignore-pattern glob syntax was designed to
     * be upward-compatible from CVS's.  The major differences are that
     * git treats a # as a comment leader and Git doesn't interpret
     * spaces as pattern separators.
     *
     * For the record, GNU CVS does its ignore pattern matching with a
     * local implementation of POSIX fnmatch(3) with 0 flags. Patterns
     * match against the pathnames of files relative to the repository
     * root.  Available wildcards are * and ?;, both [ ] and [^ ] are
     * available and ! is an alternate for ^. Unlike POSIX fnmatch(3),
     * the dash for character ranges *is* supported in the GNU CVS
     * code.  The path segment separator / is not treated specially in
     * any way.  The zero flags mean that \ acts to escape wildcard
     * characters, and there are no restrictions on matching a period.
     *
     * It is unclear what effect, if any, a .cvsignore in a subdirectory
     * is supposed to have, if any.
     */
    bool is_ignore = strcmp(node->commit->master->name, ".cvsignore") == 0;
    size_t extralen = 0;

    export_stats.snapsize += len;

    char *cbuf = (char *)buf;
    size_t clen = len;
    if (is_ignore) {
	if (!noignores)
	    extralen = sizeof(CVS_IGNORES) - 1;
	/*
	 * Two incredibly obscure features of CVS:
	 * 1. Spaces in include files are supposed to be
	 *    treated equivalently to newlines,  Yes, really.
	 * 2. A leading "!\n" says to ignore the defaults.
	 */
	for (char *cp = cbuf; cp < cbuf + len; cp++)
	    if (*cp == ' ')
		*cp = '\n';
	if (strlen(cbuf) >= 2 && cbuf[0] == '!' && cbuf[1] == '\n') {
	    extralen = 0;
	    cbuf += 2;
	    clen -= 2;
	}
    }

    node->commit->serial = seqno_next();

    /*
     * FIXME: Someday, avoid this I/O when incremental-dumping.  For
     * some unknown reason the obvious test opts->fromtime <
     * node->commit->date fails - emits too few blobs - but only
     * if the -T option is not used. See test/badincr.sh
     */
    char path[PATH_MAX];
    FILE *wfp;
    blobfile(node->commit->master->name, node->commit->serial, true, path);
    wfp = fopen(path, "w");

    if (wfp == NULL)
	fatal_error("blobfile open of %s: %s (%d)", 
		    path, strerror(errno), errno);
    else {
	fprintf(wfp, "data %lu\n", (unsigned long)(len + extralen));
	if (extralen > 0)
	    fwrite(CVS_IGNORES, extralen, sizeof(char), wfp);
	for (char *cp = cbuf; cp < cbuf + len; cp++) {
	    // CVS .cvsignores don't have hash-led comments. so if we
	    // see one it needs to be escaped.
	    if (is_ignore && cp[0] == '#' && (cp == cbuf || cp[-1] == '\n'))
	        fputc('\\', wfp);
	    fputc(*cp, wfp);
	}
	fputc('\n', wfp);
	(void)fclose(wfp);
    }
}

static int unlink_cb(const char *fpath, 
		     const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);

    if (rv)
        perror(fpath);

    return rv;
}

static void cleanup(const export_options_t *opts)
{
    nftw(blobdir, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];
    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_string_return_content] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
#ifndef __CYGWIN__
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);
#else
		// Cygdwin doesn't have %s for strftime
    int x = sprintf(outbuf, "%li", *timep);
    strftime(outbuf + x, sizeof(outbuf) - x, " %z", tm);
#endif
    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

struct fileop {
    char op;
    mode_t mode;
    cvs_commit *rev;
    const char *path;
};

/*
 * The magic number 100000 avoids generating forced UTC times that might be
 * negative in some timezone, while producing a sequence easy to read.
 */
#define display_date(c, m, f)	(f ? (100000 + (m) * commit_time_window * 2) : ((c)->date + RCS_EPOCH))

#ifdef ORDERDEBUG
static void dump_file(const cvs_commit *cvs_commit, FILE *fp)
{
    char buf[CVS_MAX_REV_LEN + 1];
    fprintf(fp, "   file name: %s %s\n", cvs_commit->master->name, 
	    cvs_number_string(cvs_commit->number, buf, sizeof(buf)));
}

static void dump_commit(const git_commit *commit, FILE *fp)
{
    revdir_iter *it = revdir_iter_alloc(&commit->revdir);
    cvs_commit *c;
    fprintf(fp, "commit %p seq %d mark %d nfiles: %d\n",
	    commit, commit->serial, markmap[commit->serial],
	    revdir_nfiles(&commit->revdir));
    while ((c = revdir_iter_next(it)))
	dump_file(c, fp);
}
#endif /* ORDERDEBUG */


static struct fileop *
next_op_slot(struct fileop **operations, struct fileop *op, int *noperations)
/* move to next operations slot, expand if necessary */
{
#define OP_CHUNK	32
    if (++op == (*operations) + (*noperations)) {
	(*noperations) += OP_CHUNK;
	*operations = xrealloc(*operations, sizeof(struct fileop) * (*noperations), __func__);
	// realloc can move operations
	op = (*operations) + (*noperations) - OP_CHUNK;
    }
    return op;
}

static void
build_modify_op(cvs_commit *c, struct fileop *op)
/* fill out a modify fileop from a cvs commit */
{
    op->rev = c;
    op->path = c->master->fileop_name;
    op->op = 'M';
    if (c->master->mode & 0100)
	op->mode = 0755;
    else
	op->mode = 0644;
}

static void
append_revpair(cvs_commit *c, const export_options_t *opts,
	       char **revpairs, size_t *revpairsize)
/* append file information if requested */
{
    if (opts->revision_map || opts->reposurgeon || opts->embed_ids) {
	char fr[BUFSIZ];
	int xtr = opts->embed_ids ? 10 : 2;
	stringify_revision(c->master->name, " ", c->number, fr, sizeof fr);
	if (strlen(*revpairs) + strlen(fr) + xtr > *revpairsize) {
	    *revpairsize *= 2;
	    *revpairs = xrealloc(*revpairs, *revpairsize, "revpair allocation");
	}
	if (opts->embed_ids)
	    strcat(*revpairs, "CVS-ID: ");
	strcat(*revpairs, fr);
	strcat(*revpairs, "\n");
    }
}

static revdir_iter *commit_iter = NULL;
static revdir_iter *parent_iter = NULL;

static void
build_delete_op(cvs_commit *c, struct fileop *op)
{
    op->op = 'D';
    op->path = c->master->fileop_name;
}

static const char *
visualize_branch_name(const char *name)
{
    if (name == NULL) {
	warn("null branch name, probably from a damaged Attic file\n");
	return "null";
    } else
	return name;
}

static void
export_commit(git_commit *commit, const char *branch,
	      const bool report, const export_options_t *opts)
/* export a commit and the blobs it is the first to reference */
{
    const git_commit *parent = commit->parent;
    cvs_commit *cc;
    cvs_author *author;
    const char *full;
    const char *email;
    const char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    time_t ct;
    struct fileop *operations, *op, *op2;
    int noperations;
    serial_t here;
    static const char *s_gitignore;

    if (!s_gitignore) s_gitignore = atom(".gitignore");

    if (opts->reposurgeon || opts->revision_map || opts->embed_ids) {
	revpairs = xmalloc((revpairsize = 1024), "revpair allocation");
	revpairs[0] = '\0';
    }

    noperations = OP_CHUNK;
    op = operations = xmalloc(sizeof(struct fileop) * noperations, "fileop allocation");

    /* Perform a merge join between files in commit and files in parent commit
     * to determine modified (including new) and deleted files  between commits.
     * This works because files are sorted by path_deep_compare order
     * The merge join also preserves this order, removing the need to sort
     * operations once generated.
     */
    REVDIR_ITER_START(commit_iter, &commit->revdir);

    cc = revdir_iter_next(commit_iter);
    if (parent) {
	REVDIR_ITER_START(parent_iter, &parent->revdir);

	cvs_commit *pc = revdir_iter_next(parent_iter);
	while (cc && pc) {
	    /* If we're in the same packed directory then skip it */
	    if (revdir_iter_same_dir(commit_iter, parent_iter)) {
		pc = revdir_iter_next_dir(parent_iter);
		cc = revdir_iter_next_dir(commit_iter);
		continue;
	    }
	    if (cc == pc) {
		/* Child and parent the same, skip. Do this check second
		 * as we have already accessed cc and pc, so they'll be hot
                 * plus, it's a common case.
		 */
		pc = revdir_iter_next(parent_iter);
		cc = revdir_iter_next(commit_iter);
		continue;
	    }
	    if (pc->master == cc->master) {
		/* file exists in commit and parent, but different revisions, modify op */
		build_modify_op(cc, op);
		append_revpair(cc, opts, &revpairs, &revpairsize);
		op = next_op_slot(&operations, op, &noperations);
		pc = revdir_iter_next(parent_iter);
		cc = revdir_iter_next(commit_iter);
		continue;
	    }
	    /* masters are sorted in fileop order */
	    if (pc->master < cc->master) {
		/* parent but no child, delete op */
		build_delete_op(pc, op);
		op = next_op_slot(&operations, op, &noperations);
		pc = revdir_iter_next(parent_iter);
	    } else {
		/* child but no parent, modify op */
		build_modify_op(cc, op);
		append_revpair(cc, opts, &revpairs, &revpairsize);
		op = next_op_slot(&operations, op, &noperations);
		cc = revdir_iter_next(commit_iter);
	    }
	}
	for (; pc; pc = revdir_iter_next(parent_iter)) {
	    /* parent but no child, delete op */
	    build_delete_op(pc, op);
	    op = next_op_slot(&operations, op, &noperations);
	}
    }
    for (; cc; cc = revdir_iter_next(commit_iter)) {
	/* child but no parent, modify op */
	build_modify_op(cc, op);
	append_revpair(cc, opts, &revpairs, &revpairsize);
	op = next_op_slot(&operations, op, &noperations);
    }

    for (op2 = operations; op2 < op; op2++) {
	if (op2->op == 'M' && !op2->rev->emitted) {
	    markmap[op2->rev->serial] = ++mark;
	    if (report) {
		char path[PATH_MAX];
		char *fn = blobfile(op2->path, op2->rev->serial, false, path);
		FILE *rfp = fopen(fn, "r");
		if (rfp == NULL) {
		    warn("content for %s at %d is missing\n", op2->path, mark);
		} else {
		    char buf[BUFSIZ];
		    printf("blob\nmark :%d\n", (int)mark);

		    while (!feof(rfp)) {
			size_t len = fread(buf, 1, sizeof(buf), rfp);
			(void)fwrite(buf, 1, len, stdout);
		    }
		    (void) unlink(fn);
		    op2->rev->emitted = true;
		    (void)fclose(rfp);
		}
	    }
	}
    }

    author = fullname(commit->author);
    if (!author) {
	full = commit->author;
	email = commit->author;
	timezone = "UTC";
    } else {
	full = author->full;
	email = author->email;
	timezone = author->timezone ? author->timezone : "UTC";
    }

    if (report)
	printf("commit %s%s\n", opts->branch_prefix, visualize_branch_name(branch));
    commit->serial = ++seqno;
    here = markmap[commit->serial] = ++mark;
#ifdef ORDERDEBUG2
    /* can't move before mark is updated */
    dump_commit(commit, stderr);
#endif /* ORDERDEBUG2 */
    if (report) {
	static bool need_ignores = true;
	if (noignores)
	    need_ignores = false;
	const char *ts;
	printf("mark :%d\n", (int)mark);
	ct = display_date(commit, mark, opts->force_dates);
	ts = utc_offset_timestamp(&ct, timezone);
	//printf("author %s <%s> %s\n", full, email, ts);
	printf("committer %s <%s> %s\n", full, email, ts);
	if (!opts->embed_ids)
	    printf("data %zd\n%s", (ssize_t)strlen(commit->log), commit->log);
	else
	    printf("data %zd\n%s\n%s", strlen(commit->log) + strlen(revpairs) + 1,
		commit->log, revpairs);
	if (commit->parent) {
	    if (markmap[commit->parent->serial] == 0)
	    {
		cleanup(opts);
		/* should never happen */
		fatal_error("internal error: child commit emitted before parent exists");
	    }
	    else if (opts->fromtime < commit->parent->date)
		printf("from :%d\n", (int)markmap[commit->parent->serial]);
	}

	for (op2 = operations; op2 < op; op2++)
	{
	    assert(op2->op == 'M' || op2->op == 'D');
	    if (op2->op == 'M')
		printf("M 100%o :%d %s\n", 
		       op2->mode, 
		       (int)markmap[op2->rev->serial], 
		       op2->path);
	    if (op2->op == 'D')
		printf("D %s\n", op2->path);
	    /*
	     * If there's a .gitignore in the first commit, don't generate one.
	     * export_blob() will already have prepended them.
	     */
	    if (need_ignores && op2->path == s_gitignore)
		need_ignores = false;
	}
	if (need_ignores) {
	    need_ignores = false;
	    printf("M 100644 inline .gitignore\ndata %d\n%s\n",
		   (int)sizeof(CVS_IGNORES)-1, CVS_IGNORES);
	}
	if (revpairs != NULL && strlen(revpairs) > 0)
	{
	    if (opts->revision_map) {
		char *cp;
		for (cp = revpairs; *cp; cp++) {
		    if (*cp == '\n')
			fprintf(opts->revision_map, " :%d", (int)here);
		    fputc(*cp, opts->revision_map);
		}
	    }
	    if (opts->reposurgeon) {
		printf("property cvs-revisions %zd %s", (ssize_t)strlen(revpairs), revpairs);
	    }
	}
    }
    free(revpairs);
    free(operations);

    if (report)
	printf("\n");
#undef OP_CHUNK
}

static int export_ncommit(const git_repo *rl)
/* return a count of converted commits */
{
    rev_ref	*h;
    git_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	/* PUNNING: see the big comment in cvs.h */ 
	for (c = (git_commit *)h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

struct commit_seq {
    git_commit *commit;
    rev_ref *head;
    bool isbase;
    bool realized;
};

static struct commit_seq *canonicalize(git_repo *rl)
/* copy/sort collated commits into git-fast-export order */
{
    /*
     * Dump in canonical (strict git-fast-export) order.
     *
     * Commits are in reverse order on per-branch lists.  The branches
     * have to ship in their current order, otherwise some marks may not 
     * be resolved.
     *
     * Dump them all into a common array because (a) we're going to
     * need to ship them back to front, and (b) we'd prefer to ship
     * them in canonical order by commit date rather than ordered by
     * branches.
     *
     * But there's a hitch; the branches themselves need to be dumped
     * in forward order, otherwise not all ancestor marks will be defined.
     * Since the branch commits need to be dumped in reverse, the easiest
     * way to arrange this is to reverse the branches in the array, fill
     * the array in forward order, and dump it forward order.
     */
    struct commit_seq *history, *hp;
    int n;
    int branchbase;
    rev_ref *h;
    git_commit *c;

    history = (struct commit_seq *)xcalloc(export_stats.export_total_commits, 
					   sizeof(struct commit_seq),
					   "export");
#ifdef ORDERDEBUG
    fputs("Export phase 1:\n", stderr);
#endif /* ORDERDEBUG */
    branchbase = 0;
    for (h = rl->heads; h; h = h->next) {
	if (!h->tail) {
	    int i = 0, branchlength = 0;
	    /* PUNNING: see the big comment in cvs.h */ 
	    for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent))
		branchlength++;
	    /* PUNNING: see the big comment in cvs.h */ 
	    for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent)) {
		/* copy commits in reverse order into this branch's span */
		n = branchbase + branchlength - (i + 1);
		history[n].commit = c;
		history[n].head = h;
		i++;
#ifdef ORDERDEBUG
		fprintf(stderr, "At n = %d, i = %d\n", n, i);
		dump_commit(c, stderr);
#endif /* ORDERDEBUG */
	    }
	    history[branchbase].isbase = true;
	    branchbase += branchlength;
	}
    }

    /*
     * Topological ordering is now correct.  Shuffle commits to make it as
     * consistent with time order as we can without changing the topology.  To
     * do this, we go to each commit in turn and move it as far towards the root
     * as we can without moving it past a commit that is (a) its parent, (b) on
     * a different branch, or (c) has an older datestamp.
     *
     * This is worse than O(n**2) in the number of commits, alas.
     */
    for (hp = history+1; hp < history + export_stats.export_total_commits; hp++) {
	struct commit_seq sc, *tp, *bp = hp;
#define is_parent_of(x, y) (((struct commit_seq *)x)->commit == ((struct commit_seq *)y)->commit->parent)
#define is_branchroot_of(x, y) ((x)->head == (y)->head && (x)->isbase)
#define is_older_than(x, y) (((struct commit_seq *)x)->commit->date < ((struct commit_seq *)y)->commit->date)
	/* back up as far as we can */
	while (!is_parent_of(bp-1, hp) && !is_branchroot_of(bp-1, hp) && !is_older_than(bp-1, hp))
	    bp--;
	if (bp < hp) {
	    /* shift commits up and put *hp where *bp was */
	    sc = *hp;
	    for (tp = hp; tp > bp; tp--) {
		tp[0] = tp[-1];
	    }
	    *bp = sc;
	}
    }

    return history;
}

void export_authors(forest_t *forest, export_options_t *opts)
/* dump a list of author IDs in the repository */
{
    const char **authors;
    int i, nauthors = 0;
    size_t alloc;
    authors = NULL;
    alloc = 0;
    export_stats.export_total_commits = export_ncommit(forest->git);
    struct commit_seq *hp, *history = canonicalize(forest->git);

    progress_begin("Finding authors...", NO_MAX);
    for (hp = history; hp < history + export_stats.export_total_commits; hp++) {
	for (i = 0; i < nauthors; i++) {
	    if (authors[i] == hp->commit->author)
		goto duplicate;
	}
	if (nauthors >= alloc) {
	    alloc += 1024;
	    authors = xrealloc(authors, sizeof(char*) * alloc, "author list");
	}
	authors[nauthors++] = hp->commit->author;
    duplicate:;
    }
    progress_end("done");

    for (i = 0; i < nauthors; i++)
	printf("%s\n", authors[i]);

    free(authors);
    free(history);
}

void export_commits(forest_t *forest, 
		    export_options_t *opts, export_stats_t *stats)
/* export a revision list as a git fast-import stream */
{
    tag_t *t;
    git_repo *rl = forest->git;
    generator_t *gp;
    int recount = 0;
    char *tmp = getenv("TMPDIR");
	
    if (tmp == NULL) 
	tmp = "/tmp";
    seqno = mark = 0;
    snprintf(blobdir, sizeof(blobdir), "%s/cvs-fast-export-XXXXXX", tmp);
    if (mkdtemp(blobdir) == NULL)
	fatal_error("temp dir creation failed\n");

    /* an attempt to optimize output throughput */
    setvbuf(stdout, NULL, _IOFBF, BUFSIZ);

    export_stats.export_total_commits = export_ncommit(rl);
    /* the +1 is because mark indices are 1-origin, slot 0 always empty */
    markmap = (serial_t *)xcalloc(sizeof(serial_t),
				  forest->total_revisions + export_stats.export_total_commits + 1,
				  "markmap allocation");

    progress_begin("Generating snapshots...", forest->filecount);
    for (gp = forest->generators; 
	 gp < forest->generators + forest->filecount;
	 gp++) {
	generate_files(gp, opts, export_blob);
	generator_free(gp);
	progress_jump(++recount);
    }
    progress_end("done");

    if (opts->reposurgeon)
        printf("#reposurgeon sourcetype %s\n",  forest->cvsroot ? "cvs" : "rcs");

    struct commit_seq *history, *hp;

    history = canonicalize(rl);

#ifdef ORDERDEBUG2
    fputs("Export phase 2:\n", stderr);
    for (hp = history; hp < history + export_stats.export_total_commits; hp++)
	dump_commit(hp->commit, stderr);
#endif /* ORDERDEBUG2 */

#ifdef ORDERDEBUG2
    fputs("Export phase 3:\n", stderr);
#endif /* ORDERDEBUG2 */
    for (hp = history; hp < history + export_stats.export_total_commits; hp++) {
	bool report = true;
	if (opts->fromtime > 0) {
	    if (opts->fromtime >= display_date(hp->commit, mark+1, opts->force_dates)) {
		report = false;
	    } else if (!hp->realized) {
		struct commit_seq *lp;
		if (hp->commit->parent != NULL && display_date(hp->commit->parent, markmap[hp->commit->parent->serial], opts->force_dates) < opts->fromtime)
		    (void)printf("from %s%s^0\n\n", opts->branch_prefix, hp->head->ref_name);
		for (lp = hp; lp < history + export_stats.export_total_commits; lp++) {
		    if (lp->head == hp->head) {
			lp->realized = true;
		    }
		}
	    }
	}
	progress_jump(hp - history);
	export_commit(hp->commit, hp->head->ref_name, report, opts);
	for (t = all_tags; t; t = t->next)
	    if (t->commit == hp->commit && display_date(hp->commit, markmap[hp->commit->serial], opts->force_dates) > opts->fromtime)
		printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, (int)markmap[hp->commit->serial]);
    }

    free(history);

#ifdef __UNUSED__
    // git fast-export no longer emits branch-tip resets.
    for (rev_ref *h = rl->heads; h; h = h->next) {
	if (display_date(h->commit, markmap[h->commit->serial], opts->force_dates) > opts->fromtime)
	    printf("reset %s%s\nfrom :%d\n\n",
		   opts->branch_prefix,
		   visualize_branch_name(h->ref_name),
		   (int)markmap[h->commit->serial]);
    }
#endif /* __UNUSED__ */
    free(markmap);

    progress_end("done");

    fputs("done\n", stdout);

    cleanup(opts);

    if (forest->skew_vulnerable > 0 && forest->filecount > 1 && !opts->force_dates) {
	time_t udate = forest->skew_vulnerable;
	warn("no commitids before %s.\n", cvstime2rfc3339(udate));
    }

    memcpy(stats, &export_stats, sizeof(export_stats_t));

}

/* end */
