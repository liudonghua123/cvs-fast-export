/*
 * Manage a map from short CVS-syle names to DVCS-style name/email pairs.
 *
 *  SPDX-License-Identifier: GPL-2.0+
 */

#include "cvs.h"
#include "hash.h"

#define AUTHOR_HASH 1021

static cvs_author	*author_buckets[AUTHOR_HASH];

static unsigned
author_hash(const char *name)
{
    HASH_INIT(h);
    HASH_MIX(h, name);
    return h % AUTHOR_HASH;
}

cvs_author *
fullname(const char *name)
/* return the fullname structure corresponding to a specified shortname */
{
    cvs_author	**bucket = &author_buckets[author_hash(name)];
    cvs_author	*a;

    for (a = *bucket; a; a = a->next)
	if (a->name == name)
	    return a;
    return NULL;
}

void
free_author_map(void)
/* discard author-map information */
{
    int	h;

    for (h = 0; h < AUTHOR_HASH; h++) {
	cvs_author	**bucket = &author_buckets[h];
	cvs_author	*a;

	while ((a = *bucket)) {
	    *bucket = a->next;
	    free(a);
	}
    }
}

bool
load_author_map(const char *filename)
/* load author-map information from a file */
{
    char    line[10240];
    char    *equal;
    char    *angle;
    char    *email;
    const char *name;
    char    *full;
    FILE    *f;
    int	    lineno = 0;
    cvs_author	*a, **bucket;
    
    f = fopen(filename, "r");
    if (!f) {
	announce("%s: authormap open failed, %s\n", filename, strerror(errno));
	return false;
    }
    while (fgets(line, sizeof(line) - 1, f)) {
	lineno++;
	if (line[0] == '#')
	    continue;
	equal = strchr(line, '=');
	if (!equal) {
	    announce("%s:%d: missing '='\n", filename, lineno);
	    fclose(f);
	    return false;
	}
	full = equal + 1;
	while (equal > line && equal[-1] == ' ')
	    equal--;
	*equal = '\0';
	name = atom(line);
	if (fullname(name)) {
	    announce("%s:%d: duplicate username '%s' ignored\n",
		     filename, lineno, name);
	    fclose(f);
	    return 0;
	}
	a = xcalloc(1, sizeof(cvs_author), "authormap creation");
	a->name = name;
	angle = strchr(full, '<');
	if (!angle) {
	    announce("%s:%d: missing email address '%s'\n",
		     filename, lineno, name);
	    fclose(f);
	    free(a);
	    return false;
	}
	email = angle + 1;
	while (full < angle && full[0] == ' ')
	    full++;
        while (angle > full && angle[-1] == ' ')
	    angle--;
	*angle = '\0';
	a->full = atom(full);
	angle = strchr(email, '>');
	if (!angle) {
	    announce("%s:%d: malformed email address '%s\n",
		     filename, lineno, name);
	    fclose(f);
	    free(a);
	    return false;
	}
	*angle = '\0';
	a->email = atom(email);
	a->timezone = NULL;
	if (*++angle) {
	    while (isspace((unsigned char)*angle))
		angle++;
	    while (*angle != '\0') {
		char *end = angle + strlen(angle) - 1;
		if (isspace((unsigned char)*end))
		    *end = '\0';
		else
		    break;
	    }
	    a->timezone = atom(angle);
	}
	bucket = &author_buckets[author_hash(name)];
	a->next = *bucket;
	*bucket = a;
    }
    fclose(f);
    return true;
}

/* end */
