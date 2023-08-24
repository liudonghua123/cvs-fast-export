%{
/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  SPDX-License-Identifier: GPL-2.0+
 */

#include "cvs.h"
#include "gram.h"
#include "lex.h"

extern void yyerror(yyscan_t, cvs_file *, const char *);

extern YY_DECL;	/* FIXME: once the Bison bug requiring this is fixed */
%}

/*
 * Properly, the first declaration in %parse-params should have the
 * type yyscan_t, but this runs into the problem that this type is both
 * declared in lex.h and needed in gram.y - whch lex.h needs.  We
 * used to kluge around this by declaring typedef void *yyscan_t
 * in CVS, but this caused other problems including complaints
 * from compilers like clang that barf on duplicate typedefs.
 */
%define api.pure full
%lex-param {yyscan_t scanner}
%lex-param {cvs_file *cvsfile}
%parse-param {void *scanner}
%parse-param {cvs_file *cvsfile}

%union {
    int		i;
    cvstime_t	date;
    char	*s; 		/* on heap */
    const char	*atom;
    cvs_text	text;
    cvs_number	number;
    cvs_symbol	*symbol;
    cvs_version	*version;
    cvs_version	**vlist;
    cvs_patch	*patch;
    cvs_patch	**patches;
    cvs_branch	*branch;
    cvs_file	*file;
}

/*
 * There's a good description of the CVS master format at:
 * http://www.opensource.apple.com/source/cvs/cvs-19/cvs/doc/RCSFILES?txt
 */

%token		HEAD BRANCH ACCESS SYMBOLS LOCKS COMMENT DATE
%token		BRANCHES DELTATYPE NEXT COMMITID EXPAND
%token		GROUP KOPT OWNER PERMISSIONS FILENAME MERGEPOINT HARDLINKS USERNAME
%token		DESC LOG TEXT STRICT AUTHOR STATE
%token		SEMI COLON IGNORED
%token		BRAINDAMAGED_NUMBER
%token		LOGIN
%token <atom>	TOKEN
%token <s>	DATA
%token <text>	TEXT_DATA
%token <number>	NUMBER

%type <text>	text
%type <s>	log
%type <symbol>	accesslist logins
%type <symbol>	symbollist symbol symbols
%type <version>	revision
%type <vlist>	revisions
%type <date>	date
%type <branch>	branches numbers
%type <atom>	revtrailer commitid
%type <atom>	name
%type <atom>	author state
%type <atom>	deltatype
%type <atom>	group
%type <atom>	owner
%type <atom>	permissions
%type <atom>	filename
%type <number>	mergepoint
%type <number>	next opt_number
%type <patch>	patch
%type <patches>	patches


%%
file		: headers revisions desc patches
		  {
		    /* The description text (if any) is only used
		     * for empty log messages in the 'patch' production
                     */
		     free((void *)cvsfile->description);
		     cvsfile->description = NULL;
		  }
		;
headers		: header headers
		|
		;
header		: HEAD opt_number SEMI
		  { cvsfile->head = atom_cvs_number($2); }
		| BRANCH NUMBER SEMI
		  { cvsfile->branch = atom_cvs_number($2); }
		| BRANCH SEMI
		  { warn("ignoring empty branch\n"); }
		| accesslist
		| symbollist
		  { cvsfile->symbols = $1; }
		| LOCKS locks SEMI lock_type
		| COMMENT DATA SEMI
		  { free($2); }
		| EXPAND DATA SEMI
		  { cvsfile->gen.expand = expand_override($2); }
		| EXPAND TOKEN SEMI
		  { cvsfile->gen.expand = expand_override($2); }
		;
locks		: locks lock
		|
		;
lock		: TOKEN COLON NUMBER
		;
lock_type	: STRICT SEMI
		|
		;
accesslist	: ACCESS logins SEMI
		  {
		    /********************************************************************
		     *	From OPTIONS in rcs(1) man page
		     *
		     *   -alogins
		     *	     Append  the login names appearing in the comma-separated list logins
		     *	     to the access list of the RCS file.
		     *
		     * The logins in the access list seems to be ignored by all RCS operations.
		     * Nevertheless it is appropriate to allow an access list with logins.
		     * Some RCS files have them.  Without this patch you get a syntax error
		     * if you have logins in the access list.	JW 20151120
		     *******************************************************************/
		    $$ = $2;
		  }
		;
logins		: logins LOGIN
		  { $$ = NULL;		/* ignore LOGIN */ }
		|
		  { $$ = NULL;		/* empty access list */ }
		;
symbollist	: SYMBOLS symbols SEMI
		  { $$ = $2; }
		;
symbols		: symbols symbol
		  { $2->next = $1; $$ = $2; }
		| symbols fscked_symbol
		  { $$ = $1; }
		|
		  { $$ = NULL; }
		;
symbol		: name COLON NUMBER
		  {
		  	$$ = xcalloc (1, sizeof (cvs_symbol), "making symbol");
			$$->symbol_name = $1;
			$$->number = atom_cvs_number($3);
		  }
		;
fscked_symbol	: name COLON BRAINDAMAGED_NUMBER
		  {
		        warn("ignoring symbol %s (FreeBSD RELENG_2_1_0 braindamage?)\n", $1);
		  }
		;
name		: TOKEN
		;
revisions	: revisions revision
		  { *$1 = $2; $$ = &$2->next;}
		|
		  { $$ = &cvsfile->gen.versions; }
		;

revtrailer	: /* empty */
                  { $$ = NULL; }
                | revtrailer commitid
		  { $$ = $2; }
                | revtrailer ignored
		;


/* ignored items from CVS-NT (except hardlinks which is from late GNU CVS) */
ignored         : owner | group | deltatype | kopt | 
                  permissions | mergepoint | filename | hardlinks | username;

revision	: NUMBER date author state branches next revtrailer
		  {
		    $$ = xcalloc (1, sizeof (cvs_version),
				    "gram.y::revision");
		    $$->number = atom_cvs_number($1);
		    $$->date = $2;
		    $$->author = $3;
		    $$->state = $4;
		    $$->dead = !strcmp ($4, "dead");
		    $$->branches = $5;
		    $$->parent = atom_cvs_number($6);
		    $$->commitid = $7;
		    if ($$->commitid == NULL 
			        && cvsfile->skew_vulnerable < $$->date) {
			cvsfile->skew_vulnerable = $$->date;
			if (cvsfile->verbose) {
			    char jw_buf[33];
			    warn("skew_vulnerable in file %s rev %s set to %s\n",
				 cvsfile->export_name,
				 cvs_number_string($$->number,
						   jw_buf, sizeof(jw_buf)-1),
				 cvstime2rfc3339($$->date));
			}
		    }
		    hash_version(&cvsfile->gen.nodehash, $$);
		    ++cvsfile->nversions;			
		  }
		;
date		: DATE NUMBER SEMI
		  {
		    $$ = lex_date (&$2, scanner, cvsfile);
		  }
		;
author		: AUTHOR TOKEN SEMI
		  { $$ = $2; }
		;
state		: STATE TOKEN SEMI
		  { $$ = $2; }
		;
branches	: BRANCHES numbers SEMI
		  { $$ = $2; }
		;
numbers		: NUMBER numbers
		  {
			$$ = xcalloc (1, sizeof (cvs_branch),
				    "gram.y::numbers");
			$$->next = $2;
			$$->number = atom_cvs_number($1);
			hash_branch(&cvsfile->gen.nodehash, $$);
		  }
		|
		  { $$ = NULL; }
		;
next		: NEXT opt_number SEMI
		  { $$ = $2; }
		;
opt_number	: NUMBER
		  { $$ = $1; }
		|
		  { $$.c = 0; }
		;
commitid	: COMMITID TOKEN SEMI
		  { $$ = $2; }
		;
desc		: DESC DATA
		  { cvsfile->description = $2; }
		;
patches		: patches patch
		  { *$1 = $2; $$ = &$2->next; }
		|
		  { $$ = &cvsfile->gen.patches; }
		;
patch		: NUMBER log text
		  { $$ = xcalloc (1, sizeof (cvs_patch), "gram.y::patch");
		    $$->number = atom_cvs_number($1);
		    if (!strcmp($2, "Initial revision\n")) {
			    /* description is available because the
			     * desc production has already been reduced */
			    if (strlen(cvsfile->description) == 0)
				    $$->log = atom("*** empty log message ***\n");
			    else
				    $$->log = atom(cvsfile->description);
		    } else
			    $$->log = atom($2);
		    $$->text = $3;
		    hash_patch(&cvsfile->gen.nodehash, $$);
		    free($2);
		  }
		;
log		: LOG DATA
		  { $$ = $2; }
		;
text		: TEXT TEXT_DATA
		  { $$ = $2; }
		;
deltatype	: DELTATYPE TOKEN SEMI
		  { $$ = $2; }
		;
group		: GROUP IGNORED SEMI
		  { $$ = NULL; }
		;
kopt		: KOPT IGNORED SEMI
                | KOPT SEMI
		;
owner		: OWNER IGNORED SEMI
		  { $$ = NULL; }
		;
permissions	: PERMISSIONS IGNORED SEMI
		  { $$ = NULL; }
		;
filename	: FILENAME IGNORED SEMI
		  { $$ = NULL; }
		;
mergepoint	: MERGEPOINT NUMBER SEMI
		  { $$ = $2; }
		;
hardlinks	: HARDLINKS strings SEMI
		;

username	: USERNAME strings SEMI
		;

strings		: IGNORED strings
		| /* empty*/
		;
%%

void yyerror(yyscan_t scanner, cvs_file *cvs, const char *msg)
{
    progress_interrupt();
    fprintf(stderr, "%s:%d: cvs-fast-export %s on token %s\n",
	    cvs->export_name, yyget_lineno(scanner),
	    msg, yyget_text(scanner));
}
