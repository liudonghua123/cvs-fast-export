/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  SPDX-License-Identifier: GPL-2.0+
 */
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef THREADS
#include <pthread.h>
#endif /* THREADS */

#include "cvs.h"
#include "gram.h"
#include "lex.h"

/*
 * CVS master analysis.  Grinds out a cvs_repo list represnting
 * the entire CVS history of a collection.
 */

typedef struct _rev_filename {
    struct _rev_filename	*next;
    const char			*file;
} rev_filename;

typedef struct _rev_file {
    const char *name;
    const char *rectified;
} rev_file;
/*
 * Ugh...least painful way to make some stuff that isn't thread-local
 * visible.
 */
static rev_filename         *fn_head = NULL, **fn_tail = &fn_head, *fn;
/* Slabs to be sorted in path_deep_compare order */
static rev_file             *sorted_files;
static cvs_master           *cvs_masters;
static rev_master           *rev_masters;
static volatile size_t      fn_i = 0, fn_n;
static volatile cvstime_t   skew_vulnerable;
static volatile size_t      total_revisions, load_current_file;
static volatile generator_t *generators;
static volatile int         err;

static int total_files, striplen;
static int verbose;

#ifdef THREADS
static pthread_mutex_t revlist_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t enqueue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t *workers;
#endif /* THREADS */

typedef struct _analysis {
    cvstime_t skew_vulnerable;
    unsigned int total_revisions;
    generator_t generator;
} analysis_t;

static cvs_master *
sort_cvs_masters(cvs_master *list);

static void
debug_cvs_masters(cvs_master *list);

static char *
rectify_name(const char *raw, char *rectified, size_t rectlen)
/* from master name to the name humans thought of the file by */
{
    unsigned len;
    const char *s, *snext;
    char *p;

    p = rectified;
    s = raw + striplen;
    while (*s) {
	for (snext = s; *snext; snext++)
	    if (*snext == '/') {
	        ++snext;
		/* assert(*snext != '\0'); */
	        break;
	    }
	len = snext - s;
	/* special processing for final components */
	if (*snext == '\0') {
	    /* trim trailing ,v */
	    if (len > 2 && s[len - 2] == ',' && s[len - 1] == 'v')
	        len -= 2;
	} else { /* s[len-1] == '/' */
	    /* drop some path components */
	    if (len == sizeof "Attic" && memcmp(s, "Attic/", len) == 0)
	        goto skip;
	    if (len == sizeof "RCS" && memcmp(s, "RCS/", len) == 0)
		goto skip;
	}
	/* copy the path component */
	if (p + len >= rectified + rectlen)
	    fatal_error("File name %s\n too long\n", raw);
	memcpy(p, s, len);
	p += len;
    skip:
	s = snext;
    }
    *p = '\0';
    len = p - rectified;

    return rectified;
}

static const char*
atom_rectify_name(const char *raw)
{
    char rectified[PATH_MAX];
    return atom(rectify_name(raw, rectified, sizeof(rectified)));
}

static void
rev_list_file(rev_file *file, analysis_t *out, cvs_master *cm, rev_master *rm) 
{
    struct stat	buf;
    yyscan_t scanner;
    FILE *in;
    cvs_file *cvs;

    in = fopen(file->name, "r");
    if (!in) {
	perror(file->name);
	++err;
	return;
    }
    if (stat(file->name, &buf) == -1) {
	fatal_system_error("%s", file->name);
    }

    cvs = xcalloc(1, sizeof(cvs_file), __func__);
    cvs->gen.master_name = file->name;
    cvs->gen.expand = EXPANDKB;
    cvs->export_name = file->rectified;
    cvs->mode = buf.st_mode;
    cvs->verbose = verbose;

    yylex_init(&scanner);
    yyset_in(in, scanner);
    yyparse(scanner, cvs);
    yylex_destroy(scanner);

    fclose(in);
    if (cvs_master_digest(cvs, cm, rm) == NULL) {
	warn("warning - master file %s has no revision number - ignore file\n", file->name);
	cvs->gen.master_name = NULL;	/* blank out data of previous file */
    } else {
	out->total_revisions = cvs->nversions;
	out->skew_vulnerable = cvs->skew_vulnerable;
    }
    out->generator = cvs->gen;
    cvs_file_free(cvs);
}

static int
strcommonendingwith(const char *a, const char *b, char endc)
/* return the length of the common prefix of strings a and b ending with endc */
{
    int c = 0;
    int d = 0;
    while (*a == *b) {
	if (!*a) {
	    break;
 	}
	a++;
	b++;
	c++;
	if (*a == endc) {
	    d = c + 1;
	}
    }
    return d;
}

static void *worker(void *arg)
/* consume masters off the queue */
{
    analysis_t out = {0, 0};
    for (;;)
    {
	/* pop a master off the queue, terminating if none left */
#ifdef THREADS
	if (threads > 1)
	    pthread_mutex_lock(&enqueue_mutex);
#endif /* THREADS */
	size_t i = fn_i++;
#ifdef THREADS
	if (threads > 1)
	    pthread_mutex_unlock(&enqueue_mutex);
#endif /* THREADS */
	if (i >= fn_n)
	    return(NULL);

	/* process it */
	rev_list_file(&sorted_files[i], &out, &cvs_masters[i], &rev_masters[i]);

	/* pass it to the next stage */
#ifdef THREADS
	if (threads > 1)
	    pthread_mutex_lock(&revlist_mutex);
#endif /* THREADS */
	if ((generators[i] = out.generator).master_name != NULL) {
	    progress_jump(++load_current_file);
	    total_revisions += out.total_revisions;
	    if (out.skew_vulnerable > skew_vulnerable)
		skew_vulnerable = out.skew_vulnerable;
	}
#ifdef THREADS
	if (threads > 1)
	    pthread_mutex_unlock(&revlist_mutex);
#endif /* THREADS */
    }
}

/*
 * Compare/order filenames, such that files in subdirectories
 * sort earlier than files in the parent
 *
 * Also sorts in the same order that git fast-export does
 * As it says, 'Handle files below a directory first, in case they are
 * all deleted and the directory changes to a file or symlink.'
 * Because this doesn't have to handle renames, just sort lexicographically
 *
 *    a/x < b/y < a < b
 */
int
path_deep_compare(const void *a, const void *b)
{
    const char *af = (const char *)a;
    const char *bf = (const char *)b;
    const char *aslash;
    const char *bslash;
    int compar;

    /* short circuit */
    if (af == bf)
        return 0;

    compar = strcmp(af, bf);

    /*
     * strcmp() will suffice, except for this case:
     *
     *   p/p/b/x/x < p/p/a
     *
     * In the comments below,
     *   ? is a string without slashes
     *  ?? is a string that may contain slashes
     */

    if (compar == 0)
        return 0;		/* ?? = ?? */

    aslash = strrchr(af, '/');
    bslash = strrchr(bf, '/');

    if (!aslash && !bslash)
	return compar;		/*    ? ~ ?    */
    if (!aslash) return +1;	/*    ? > ??/? */
    if (!bslash) return -1;     /* ??/? < ?    */


    /*
     * If the final slashes are at the same position, then either
     * both paths are leaves of the same directory, or they
     * are totally different paths. Both cases are satisfied by
     * normal lexicographic sorting:
     */
    if (aslash - af == bslash - bf)
	return compar;		/* ??/? ~ ??/? */


    /*
     * Must find the case where the two paths share a common
     * prefix (p/p).
     */
    if (aslash - af < bslash - bf) {
	if (bf[aslash - af] == '/' && memcmp(af, bf, aslash - af) == 0) {
	    return +1;		/* p/p/??? > p/p/???/? */
	}
    } else {
	if (af[bslash - bf] == '/' && memcmp(af, bf, bslash - bf) == 0) {
	    return -1;		/* ???/???/? ~ ???/??? */
	}
    }
    return compar;
}

static int 
file_compare(const void *f1, const void *f2)
{
    rev_file r1 = *(rev_file *)f1;
    rev_file r2 = *(rev_file *)f2;
    return path_deep_compare(r1.rectified, r2.rectified);
}

void analyze_masters(int argc, const char *argv[],
			  import_options_t *analyzer, 
			  forest_t *forest)
/* main entry point; collect and parse CVS masters */
{
    char	    name[PATH_MAX];
    const char      *last = NULL;
    char	    *file;
    size_t	    i, j = 1;
    int		    c;
#ifdef THREADS
    pthread_attr_t  attr;

    /* Initialize and reinforce default thread non-detached attribute */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    atom_dir_init();
#endif /* THREADS */

    striplen = analyzer->striplen;

    forest->textsize = forest->filecount = 0;
    progress_begin("Reading file list...", NO_MAX);
    for (;;)
    {
	struct stat stb;
	if (argc < 2) {
	    /* coverity[tainted_data] Safe, never handed to exec */
	    if (fgets(name, sizeof(name), stdin) == NULL)
		break;
	    int l = strlen(name);
	    if (name[l-1] == '\n')
		name[l-1] = '\0';
	    file = name;
	} else {
	  file = (char *)argv[j++];
	    if (!file)
		break;
	}

	if (stat(file, &stb) != 0)
	    continue;
	else if (S_ISDIR(stb.st_mode) != 0)
	    continue;
	else if (!analyzer->promiscuous)
	{
	    char *end = file + strlen(file);
	    if (end - file < 2 || end[-1] != 'v' || end[-2] != ',')
		continue;
	    if (strstr(file, "CVSROOT") != NULL) {
	        forest->cvsroot = true;
		continue;
	    }
	}
	forest->textsize += stb.st_size;

	fn = xcalloc(1, sizeof(rev_filename), "filename gathering");
	*fn_tail = fn;
	fn_tail = (rev_filename **)&fn->next;
	if (striplen > 0 && last != NULL) {
	    c = strcommonendingwith(file, last, '/');
	    if (c < striplen)
		striplen = c;
	} else if (striplen < 0) {
	    striplen = 0;
	    for (i = 0; i < strlen(file); i++)
		if (file[i] == '/')
		    striplen = i + 1;
	}
	fn->file = atom(file);
	last = fn->file;
	total_files++;
	if (progress && total_files % 100 == 0)
	    progress_jump(total_files);
    }
    forest->filecount = total_files;

    generators = xcalloc(sizeof(generator_t), total_files, "Generators");
    sorted_files = xmalloc(sizeof(rev_file) * total_files, "sorted_files");
    cvs_masters = xcalloc(total_files, sizeof(cvs_master), "cvs_masters");
    rev_masters = xmalloc(sizeof(rev_master) * total_files, "rev_masters");
    fn_n = total_files;
    i = 0;
    rev_filename *tn;
    for (fn = fn_head; fn; fn = tn) {
	tn = fn->next;
	sorted_files[i].name = fn->file;
	sorted_files[i++].rectified = atom_rectify_name(fn->file);
	free(fn);
    }
    /*
     * Sort list of files in path_deep_compare order of output name.
     * cvs_masters and rev_masters will be mainteined in this order.
     * This causes commits to come out in correct pack order.
     * It also causes operations to come out in correct fileop_sort order.
     * Note some output names are different to input names.
     * e.g. .cvsignore becomes .gitignore
     */
    qsort(sorted_files, total_files, sizeof(rev_file), file_compare);
	
    progress_end("done, %.3fKB in %d files",
		 (forest->textsize/1024.0), forest->filecount);

    /* things that must be visible to inner functions */
    load_current_file = 0;
    verbose = analyzer->verbose;

    /*
     * Analyze the files for CVS revision structure.
     *
     * The result of this analysis is a rev_list, each element of
     * which corresponds to a CVS master and points at a list of named
     * CVS branch heads (rev_refs), each one of which points at a list
     * of CVS commit structures (cvs_commit).
     */
#ifdef THREADS
    if (threads > 1)
	snprintf(name, sizeof(name), 
		 "Analyzing masters with %d threads...", threads);
    else
#endif /* THREADS */
	strcpy(name, "Analyzing masters...");
    progress_begin(name, total_files);
#ifdef THREADS
    if (threads > 1)
    {
	int i;

	workers = (pthread_t *)xcalloc(threads, sizeof(pthread_t), __func__);
	for (i = 0; i < threads; i++)
	    pthread_create(&workers[i], &attr, worker, NULL);

        /* Wait for all the threads to die off. */
	for (i = 0; i < threads; i++)
          pthread_join(workers[i], NULL);
        
	pthread_mutex_destroy(&enqueue_mutex);
	pthread_mutex_destroy(&revlist_mutex);
    }
    else
#endif /* THREADS */
	worker(NULL);

    progress_end("done, %d revisions", (int)total_revisions);
    free(sorted_files);

    forest->errcount = err;
    forest->total_revisions = total_revisions;
    forest->skew_vulnerable = skew_vulnerable;
    forest->cvs = cvs_masters;
    forest->generators = (generator_t *)generators;
}

/* end */
