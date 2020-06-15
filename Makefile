# Makefile for cvs-fast-export
#
# Build requirements:  Read the "buildprep" script.
# You may be able to run that to install all dependencies.
#
# The C compiler must support anonymous unions (GNU, clang, C11).
#
# If you get a link error complaining that clock_gettime() can't be
# found, uncomment the line that includes -lrt in the link flags.
# You'll need this if your glibc version is < 2.17; that is for RHEL 6
# or older and Debian Wheezy or older.  Alas, we can't unconditionally
# include -lrt because Darwin.
#
# You will see some meaningless failures with git 1.7.1 and older.
#
# Note: the reason for the .adoc extensions on README/NEWS/TODO is so they'll
# display nicely in GitLab's repository-browsing interface.

VERSION=$(shell sed -n <NEWS.adoc '/::/s/^\([0-9][^:]*\).*/\1/p' | head -1)

.PATH: $(.PARSEDIR)
prefix?=/usr/local
target=$(DESTDIR)$(prefix)
parsedir:=$(.PARSEDIR)
srcdir=$(dir $(abspath $(firstword $(MAKEFILE_LIST))))$(parsedir)
VPATH=$(srcdir)
mandir?=$(DESTDIR)$(prefix)/share/man

INSTALL = install
TAR ?= tar
BISON ?= bison
FLEX ?= flex
A2X ?= a2x

GCC_WARNINGS1=-Wall -Wpointer-arith -Wstrict-prototypes
GCC_WARNINGS2=-Wmissing-prototypes -Wmissing-declarations
GCC_WARNINGS3=-Wno-unused-function -Wno-unused-label -Wno-format-zero-length
GCC_WARNINGS=$(GCC_WARNINGS1) $(GCC_WARNINGS2) $(GCC_WARNINGS3)
CFLAGS += $(GCC_WARNINGS)
CPPFLAGS += -I. -I$(srcdir)
#LIBS=-lrt
CPPFLAGS += -DVERSION=\"$(VERSION)\"

# Enable this for multithreading.
CFLAGS += -pthread
CPPFLAGS += -DTHREADS

# Optimizing for speed. Un-comment for testing Linux build
#CFLAGS += -march=native

# To enable debugging of the Yacc grammar, uncomment the following line
#CPPFLAGS += -DYYDEBUG=1
# To enable verbose debugging of the analysis stage, uncomment the following line
#CPPFLAGS += -DCVSDEBUG=1
# To enable debugging of blob export, uncomment the following line
#CPPFLAGS += -DFDEBUG=1
# To enable assertions of red black trees, uncomment the following line
#CPPFLAGS += -DRBDEBUG=1
# To enable debugging of order instability issues
#CPPFLAGS += -DORDERDEBUG=1
# To enable debugging of gitspace backlinks, uncomment the following line
#CPPFLAGS += -DGITSPACEDEBUG=1

# Condition in various optimization hacks.  You almost certainly
# don't want to turn any of these off; the condition symbols are
# present more as documentation of the program structure than
# anything else
CPPFLAGS += -DREDBLACK # Use red-black trees for faster symbol lookup
CPPFLAGS += -DUSE_MMAP # Use mmap for reading CVS masters
CPPFLAGS += -DLINESTATS # Keep track of which lines have @ string delimiters
CPPFLAGS += -DTREEPACK # Reduce memory usage, particularly on large repos

# First line works for GNU C.  
# Replace with the next if your compiler doesn't support C99 restrict qualifier
CPPFLAGS+=-Drestrict=__restrict__
#CPPFLAGS+=-Drestrict=""

# To enable profiling, uncomment the following line
# Note: the profiler gets confused if you don't also turn off -O flags.
# CFLAGS += -pg
# Warning: Using -O3 has been seen to cause core dumps on repositories with
# very long revision names - some bounds check gets optimized away. Don't do that.
# CFLAGS += -O2
# If your toolchain supports link time optimization this is a cheap speedup
# CFLAGS += -flto
CFLAGS += -g
# Test coverage flags
# CFLAGS += -ftest-coverage -fprofile-arcs
CFLAGS += $(EXTRA_CFLAGS)

#YFLAGS= --report=all
LFLAGS=

OBJS=gram.o lex.o rbtree.o main.o import.o dump.o cvsnumber.o \
	cvsutil.o revdir.o revlist.o atom.o revcvs.o generate.o export.o \
	nodehash.o tags.o authormap.o graph.o utils.o collate.o hash.o

all: cvs-fast-export man html

cvs-fast-export: $(OBJS)
	$(CC) $(CFLAGS) $(TARGET_ARCH) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

$(OBJS): cvs.h cvstypes.h
revcvs.o cvsutils.o rbtree.o: rbtree.h
atom.o nodehash.o revcvs.o revdir.o: hash.h
revdir.o: treepack.c dirpack.c revdir.c
dump.o export.o graph.o main.o collate.o revdir.o: revdir.h

gram.h gram.c: gram.y
	$(BISON)  $(YFLAGS) --defines=gram.h --output-file=gram.c $(srcdir)/gram.y
lex.h lex.c: lex.l
	$(FLEX) $(LFLAGS) --header-file=lex.h --outfile=lex.c $(srcdir)/lex.l

gram.o: gram.c lex.h gram.h
import.o: import.c lex.h gram.h
lex.o: lex.c gram.h

.SUFFIXES: .html .adoc .txt .1

# Requires asciidoc
# To debug asciidoc problems, you may need to run "xmllint --nonet --noout --valid"
# on the intermediate XML.
.adoc.1:
	$(A2X) --doctype manpage --format manpage $<
.adoc.html:
	$(A2X) --doctype manpage --format xhtml -D . $<
	rm -f docbook-xsl.css

reporting-bugs.html: reporting-bugs.adoc
	asciidoc reporting-bugs.adoc

man: cvs-fast-export.1 cvssync.1 cvsconvert.1

html: cvs-fast-export.html cvssync.html cvsconvert.html reporting-bugs.html

clean:
	rm -f $(OBJS) gram.h gram.c lex.h lex.c cvs-fast-export
	rm -f *.1 *.html docbook-xsl.css gram.output gmon.out
	rm -f MANIFEST index.html *.tar.gz
	rm -f *.gcno *.gcda


# Warning: The regression tests will fail spuriously if your CVS lacks the
# MirOS patches.  These are carried by Debian Linux and derivatives; you can
# check by Looking for "MirDebian" in the output of cvs --version.
check: cvs-fast-export
	-make EXTRA=-q cppcheck pylint
	-shellcheck -f gcc buildprep tests/visualize tests/setpython tests/gitwash tests/incremental.sh
	$(MAKE) -C tests -s -f $(srcdir)tests/Makefile

install: install-bin install-man
install-bin: cvs-fast-export cvssync cvsconvert
	$(INSTALL) -d "$(target)/bin"
	$(INSTALL) $^ "$(target)/bin"
install-man: man
	$(INSTALL) -d "$(mandir)/man1"
	$(INSTALL) -m 644 cvs-fast-export.1 "$(mandir)/man1"
	$(INSTALL) -m 644 cvssync.1 "$(mandir)/man1"
	$(INSTALL) -m 644 cvsconvert.1 "$(mandir)/man1"
uninstall: uninstall-man uninstall-bin
uninstall-man:
	cd $(mandir)/man1/ && rm -f cvs-fast-export.1 cvssync.1 cvsconvert.1
uninstall-bin:
	cd $(target)/bin && rm -f cvs-fast-export cvssync cvsconvert

PROFILE_REPO = ~/software/groff-conversion/groff-mirror/groff
gmon.out: cvs-fast-export
	find $(PROFILE_REPO) -name '*,v' | cvs-fast-export -k -p >/dev/null
PROFILE: gmon.out
	gprof cvs-fast-export >PROFILE

version:
	@echo $(VERSION)

# Weird suppressions are required because of strange tricks in Bison and Flex.
CSUPPRESSIONS = -U__UNUSED__ -UYYPARSE_PARAM -UYYTYPE_INT16 -UYYTYPE_INT8 \
	-UYYTYPE_UINT16 -UYYTYPE_UINT8 -UYY_USER_INIT -UYY_READ_BUF_SIZE \
	-UYY_NO_INPUT -UECHO -UYY_START_STACK_INCR -UYY_FATAL_ERROR \
	-U_SC_NPROCESSORS_ONLN -Ushort -Usize_t -Uyytext_ptr \
	-Uyyoverflow -U__cplusplus -U__APPLE__ -DCLOCK_REALTIME=0
cppcheck:
	cppcheck -I. --template $(CC) --enable=all $(CSUPPRESSIONS) --suppress=unusedStructMember --suppress=unusedFunction --suppress=unreadVariable --suppress=uselessAssignmentPtrArg --suppress=missingIncludeSystem $(EXTRA) --inline-suppr *.[ch]

PYLINTOPTS = --rcfile=/dev/null --reports=n \
	--msg-template="{path}:{line}: [{msg_id}({symbol}), {obj}] {msg}" \
	--dummy-variables-rgx='^_'
PYSUPPRESSIONS = --disable="C0103,C0111,C0301,C0325,C0326,C0410,C0411,C0413,E1305,R0201,R0205,R0801,R0903,R0912,R0913,R0914,W0142,R1705,R1718,R1721,R1724,W0221,W0621"
pylint:
	@pylint $(PYLINTOPTS) $(PYSUPPRESSIONS) cvssync cvsconvert cvsreduce tests/*.py

# Because we don't want copies of the test repositories in the distribution.
distclean: clean
	cd tests; $(MAKE) --quiet clean

SOURCES = Makefile *.[ch] *.[yl] cvssync cvsconvert cvsreduce buildprep
DOCS = control *.adoc cfe-logo.png
ALL =  $(SOURCES) $(DOCS) tests
cvs-fast-export-$(VERSION).tar.gz: $(ALL)
	$(TAR) --transform='s:^:cvs-fast-export-$(VERSION)/:' --show-transformed-names -cvzf cvs-fast-export-$(VERSION).tar.gz $(ALL)

dist: distclean cvs-fast-export-$(VERSION).tar.gz

release: cvs-fast-export-$(VERSION).tar.gz html
	shipper version=$(VERSION) | sh -e -x

refresh: html
	shipper -N -w version=$(VERSION) | sh -e -x
