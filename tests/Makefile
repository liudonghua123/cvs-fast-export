# Test makefile for cvs-fast-export

# Set OPTS to pass options to the cvs-fast-export instances.

PATH := ${CURDIR}/..:${CURDIR}:${PATH}
SHELL = sh	# Do not introduce bashisms!
CVS = cvs
DIFF = diff -u -a
CVS_FAST_EXPORT = ../cvs-fast-export $(OPTS)

FAILLOG=/tmp/cvsfastexport-make.log

check:
	@rm -f $(FAILLOG)
	@make --no-print-directory tap; if [ -s $(FAILLOG) ]; then ( echo "Tests failed:"; cat $(FAILLOG); exit 1); fi; 

.SUFFIXES: .tst .repo .testrepo .checkout .dot .fi ,v

.tst.repo:
	python $<
.repo.checkout:
	$(CVS) -d :local:${CURDIR}/$*.repo -Q checkout module && mv module $*.checkout
.repo.dot:
	find $</module -name '*,v' | $(CVS_FAST_EXPORT) -g >$*.dot
.repo.fi:
	find $</module -name '*,v' | $(CVS_FAST_EXPORT) >$*.dot
.testrepo.checkout:
	$(CVS) -d :local:${CURDIR}/$*.testrepo -Q checkout module && mv module $*.checkout
.testrepo.dot:
	find $</module -name '*,v' | $(CVS_FAST_EXPORT) -g >$*.dot
.testrepo.fi:
	find $</module -name '*,v' | $(CVS_FAST_EXPORT) >$*.fi
,v.dot:
	$(CVS_FAST_EXPORT) -g $< >$*.dot

rebuild: s_rebuild m_rebuild r_rebuild i_rebuild t_rebuild # z_rebuild

testlist:
	@grep '^##' *.tst *.py *.sh

# USER may not actually be defined inside CI; in that case we assume root.
neutralize.map:
	@echo "$${USER:-root} = foo <foo> -0500" >neutralize.map

TESTLOADS := $(shell ls -1 *.tst | sed '/.tst/s///')
TESTOPTS = -T -A neutralize.map --reposurgeon
s_rebuild: neutralize.map
	@-for file in $(TESTLOADS); do \
	    echo "Remaking $${file}.chk"; \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) >$${file}.chk 2>&1; \
	done;
REGRESS_LOADS := $(shell ls -1 *.tst | sed '/.tst/s///')
REGRESS_TARGETS=$(REGRESS_LOADS:%=regress-%)
$(REGRESS_TARGETS): regress-%: %.tst
	@(legend=$$( ( grep -a '##' $< || echo ' ## (no description)' ) | sed 's/## //' ); \
	$(MAKE) --quiet $(patsubst regress-%,%,$@).repo; \
	find $(patsubst regress-%,%,$@).repo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${legend}" $(patsubst regress-%,%,$@).chk) || echo "$@" >>$(FAILLOG)

MASTERS := $(shell ls -1 *,v | sed '/,v/s///')
MASTER_TARGETS=$(MASTERS:%=master-%)
m_rebuild:
	@-for file in $(MASTERS); do \
	    echo "Remaking $${file}.chk"; \
	    $(CVS_FAST_EXPORT) $${file},v >$${file}.chk 2>&1; \
	done;
$(MASTER_TARGETS): master-%: %,v
	@(legend=$$(sed <$< -n -e '/^comment[	 ]*@# \(.*\)@;/s//\1/p'); \
	$(CVS_FAST_EXPORT) $(patsubst master-%,%,$@),v 2>&1 | tapdiffer "$${legend}" $(patsubst master-%,%,$@).chk;) || echo "$@" >>$(FAILLOG)

INCREMENTAL=twobranch
INCREMENTAL_TARGETS=$(INCREMENTAL:%=incremental-%)
THRESHOLD=104000
i_rebuild: neutralize.map
	@-for file in $(INCREMENTAL); do \
	    echo "Remaking $${file}.inc-chk"; \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -A neutralize.map -i $(THRESHOLD) >$${file}.inc-chk 2>&1; \
	done;
$(INCREMENTAL_TARGETS): incremental-%:
	@(legend=$$( ( grep -a '##' $(patsubst incremental-%,%,$@).tst  || echo ' ## (no description)') | sed 's/## //' ); \
	$(MAKE) --quiet $(patsubst incremental-%,%,$@).repo; \
	find $(patsubst incremental-%,%,$@).repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -i $(THRESHOLD) -A neutralize.map 2>&1 | tapdiffer "$${legend}" $(patsubst incremental-%,%,$@).inc-chk) || echo "$@" >>$(FAILLOG)

REDUCED=oldhead
REDUCED_TARGETS=$(REDUCED:%=reduced-%)
r_rebuild: neutralize.map
	@-for file in $(REDUCED); do \
	    echo "Remaking $${file}.chk"; \
	    find $${file}.testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) >$${file}.chk 2>&1; \
	done;
$(REDUCED_TARGETS): reduced-%:
	@(legend=$$( ( grep -a '##' $(patsubst reduced-%,%,$@).testrepo/README  || echo ' ## (no description)') | sed 's/## //' ); \
	find $(patsubst reduced-%,%,$@).testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${legend}" $(patsubst reduced-%,%,$@).chk) || echo "$@" >>$(FAILLOG)

PYTESTS=t9601 t9602 t9603 t9604 t9605
PYTEST_TARGETS=$(PYTESTS:%=pytest-%)
PATHSTRIP = sed -e '/\/.*tests/s//tests/'
t_rebuild:
	@for pytest in $(PYTESTS); do \
		echo "Remaking $${pytest}.err "; \
		python $${pytest}.py 2>&1 | $(PATHSTRIP) >$${pytest}.err; \
	done
$(PYTEST_TARGETS): pytest-%:
	@(legend=$$( ( grep -a '##' $(patsubst pytest-%,%,$@).py  || echo ' ## (no description)') | sed 's/## //' ); \
	python $(patsubst pytest-%,%,$@).py 2>&1 | $(PATHSTRIP) | tapdiffer "$${legend}" $(patsubst pytest-%,%,$@).err) || echo "$@" >>$(FAILLOG)

# Omitted:
# branchy.repo - because of illegal tag
# twotag.repo - because of inconsistent tagging
# QED.testrepo - produces a branch cycle fatal error
# t9601.testrepo - content mismatch expected
# t9602.testrepo - manifest mismatch expected
CT = at.repo basic.repo daughterbranch.repo exec.repo expand.repo \
	hack[123].repo longrev.repo postbranch.repo tagbug.repo \
	twobranch.repo
CD =  oldhead.testrepo t9603.testrepo t9604.testrepo t9605.testrepo \
	vendor.testrepo
CT_TARGETS=$(CT:%=ct-%)
$(CT_TARGETS): ct-%:
	@($(MAKE) --quiet $(patsubst ct-%,%,$@); cvsconvert -q -n $(patsubst ct-%,%,$@) | tapdiffer "$(patsubst ct-%,%,$@) conversion check" /dev/null) || echo "$@" >>$(FAILLOG)
CD_TARGETS=$(CD:%=cd-%)
$(CD_TARGETS): cd-%:
	@(cvsconvert -q -n $(patsubst cd-%,%,$@) | tapdiffer "$(patsubst cd-%,%,$@) conversion check" /dev/null) || echo "$@" >>$(FAILLOG)

UNSTRIPPED = $(shell ls issue22.txt,v t9601.testrepo/module/* at.repo/module/*)
z_regress:
	@echo "== Reductions =="
	@for rtest in $(UNSTRIPPED); do \
		base=`basename $${rtest}`; \
		echo "  $${base}"; \
		cvsstrip <$${rtest} | $(DIFF) reductions/$${base}.reduced -; \
	done
z_rebuild:
	@for rtest in $(UNSTRIPPED); do \
		base=`basename $${rtest}`; \
		echo "Remaking $${base}.reduced "; \
		cvsstrip <$${rtest} >reductions/$${base}.reduced; \
	done
Z_TARGETS=$(UNSTRIPPED:%=z-%)
$(Z_TARGETS): z-%:
	@(python2 ../cvsstrip <$(patsubst z-%,%,$@) | tapdiffer "$(patsubst z-%,%,$@) reduction (python2)" reductions/$(shell basename $(patsubst z-%,%,$@)).reduced) || echo "$@" >>$(FAILLOG)
	@(python3 ../cvsstrip <$(patsubst z-%,%,$@) | tapdiffer "$(patsubst z-%,%,$@) reduction (python3)" reductions/$(shell basename $(patsubst z-%,%,$@)).reduced) || echo "$@" >>$(FAILLOG)

sporadic:
	@sh incremental.sh

clean:
	rm -fr neutralize.map *.checkout *.repo *.pyc *.dot *.git *.git.fi

# Here's how we actually run the tests with TAP:
TEST_TARGETS= $(CT_TARGETS) $(CD_TARGETS) $(REGRESS_TARGETS) $(MASTER_TARGETS) $(INCREMENTAL_TARGETS) $(PYTEST_TARGETS) \
	$(Z_TARGETS) sporadic

PREREQS=neutralize.map

# Issue a straight TAP report, no parallelism or filtering
tap: $(PREREQS) count $(TEST_TARGETS)
count:
	@echo 1..$(words $(TEST_TARGETS))

