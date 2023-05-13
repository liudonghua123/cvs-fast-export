# Test makefile for cvs-fast-export

# Set OPTS to pass options to the cvs-fast-export instances.

PATH := ${CURDIR}/..:${CURDIR}:${PATH}
SHELL = sh	# Do not introduce bashisms!
CVS = cvs
DIFF = diff -u -a
CVS_FAST_EXPORT = ../cvs-fast-export $(OPTS)
PYTHON = python3

.PHOMY: CHECK FULLCHECK

# Ordinary tests, use pregenerated repos for speed.
check:
	@$(MAKE) --quiet tap | ./tapview

# Full test with rebuild of genert
fullcheck:
	@$(MAKE) --quiet clean tap | ./tapview

.SUFFIXES: .tst .repo .testrepo .checkout .dot .fi ,v

.tst.repo:
	$(PYTHON) $<
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

test: s_regress m_regress r_regress i_regress t_regress c_regress sporadic

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
s_regress: neutralize.map
	@echo "# Dump regressions"
	@-for file in $(TESTLOADS); do \
	    legend=$$( ( grep -a '##' $${file}.tst || echo ' ## (no description)' ) | sed 's/## //' ); \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${file}: $${legend}" $${file}.chk; \
	done
TEST_TARGETS = $(TESTLOADS)

MASTERS := $(shell ls -1 *,v | sed '/,v/s///')
m_rebuild:
	@-for file in $(MASTERS); do \
	    echo "Remaking $${file}.chk"; \
	    $(CVS_FAST_EXPORT) $${file},v >$${file}.chk 2>&1; \
	done;
m_regress:
	@echo "# Master-parsing regressions"
	@-for file in $(MASTERS); do \
	    legend=$$(sed <$${file},v -n -e '/^comment[	 ]*@# \(.*\)@;/s//\1/p'); \
	    $(CVS_FAST_EXPORT) $${file},v 2>&1 | tapdiffer "$${file}: $${legend}" $${file}.chk; \
	done
TEST_TARGETS += $(MASTERS)

INCREMENTAL=twobranch
THRESHOLD=104000
i_rebuild: neutralize.map
	@-for file in $(INCREMENTAL); do \
	    echo "Remaking $${file}.inc-chk"; \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -A neutralize.map -i $(THRESHOLD) >$${file}.inc-chk 2>&1; \
	done;
i_regress: neutralize.map
	@echo "# Incremental-dump regressions"
	@-for file in $(INCREMENTAL); do \
	    legend=$$( ( grep -a '##' $${file}.tst  || echo ' ## (no description)') | sed 's/## //' ); \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -i $(THRESHOLD) -A neutralize.map 2>&1 | tapdiffer "$${file} $${legend}" $${file}.inc-chk; \
	done
TEST_TARGETS += $(INCREMENTAL)

REDUCED=oldhead issue-57
r_rebuild: neutralize.map
	@-for file in $(REDUCED); do \
	    echo "Remaking $${file}.chk"; \
	    find $${file}.testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) >$${file}.chk 2>&1; \
	done;
r_regress: neutralize.map
	@echo "# Repo regressions"
	@-for repo in $(REDUCED); do \
	    legend=$$( ( grep -a '##' $${repo}.testrepo/README  || echo ' ## (no description)') | sed 's/## //' ); \
	    find $${repo}.testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${repo}: $${legend}" $${repo}.chk; \
	done
TEST_TARGETS += $(REDUCED)

PYTESTS=t9601 t9602 t9603 t9604 t9605
PATHSTRIP = sed -e '/\/.*tests/s//tests/'
t_regress:
	@echo "# Pathological cases"
	@for pytest in $(PYTESTS); do \
		legend=$$( ( grep -a '##' $${pytest}.py  || echo ' ## (no description)') | sed 's/## //' ); \
		$(PYTHON) $${pytest}.py 2>&1 | $(PATHSTRIP) | tapdiffer "$${pytest}: $${legend}" $${pytest}.err; \
	done
t_rebuild:
	@for pytest in $(PYTESTS); do \
		echo "Remaking $${pytest}.err "; \
		$(PYTHON) $${pytest}.py 2>&1 | $(PATHSTRIP) >$${pytest}.err; \
	done
TEST_TARGETS += $(PYTESTS)

# Omitted:
# branchy.repo - because of illegal tag
# twotag.repo - because of inconsistent tagging
# QED.testrepo - produces a branch cycle fatal error
# t9601.testrepo - content mismatch expected
# t9602.testrepo - manifest mismatch expected
CT = at.repo basic.repo daughterbranch.repo exec.repo \
	hack1.repo hack2.repo hack3.repo longrev.repo postbranch.repo \
	tagbug.repo twobranch.repo
CD =  oldhead.testrepo t9603.testrepo t9604.testrepo t9605.testrepo \
	vendor.testrepo
c_regress:
	@echo "# Conversion checks"
	@for ct in $(CT); do $(MAKE) --quiet $$ct; cvsconvert -q -n -t $$ct; done
	@for cd in $(CD); do cvsconvert -q -n -t $$cd; done
TEST_TARGETS += $(CD) $(CT)

UNSTRIPPED = $(shell ls issue22.txt,v t9601.testrepo/module/* at.repo/module/*)
z_regress:
	@echo "# Reductions"
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
SPORADIC = incremental.sh
sporadic:
	@echo "# Sporadic tests"
	@for x in $(SPORADIC); do sh $${x}; done
TEST_TARGETS += $(SPORADIC)

clean:
	rm -fr neutralize.map *.checkout *.repo *.pyc *.dot *.git *.git.fi

# Use this to scope out problems with test counting. 
detailcount:
	@echo "Dump regressions: $(words $(TESTLOADS))"
	@echo "Master-parsing regressions: $(words $(MASTERS))"
	@echo "Incremental-dump regressions: $(words $(INCREMENTAL))"
	@echo "Repo regressions: $(words $(REDUCED))"
	@echo "Pathological cases: $(words $(PYTESTS))"
	@echo "Conversion checks: $(words $(CD) $(CT))"
	@echo "Sporadic tests: $(words $(SPORADIC))"
	@echo "Total tests: $(words $(TEST_TARGETS))"


# Issue a straight TAP report, no parallelism or filtering
tap: count test
	@echo "# Tests complete"
count:
	@echo 1..$(words $(TEST_TARGETS))
