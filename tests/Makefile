# Test makefile for cvs-fast-export

# Set OPTS to pass options to the cvs-fast-export instances.

PATH := ${CURDIR}/..:${CURDIR}:${PATH}
SHELL = sh	# Do not introduce bashisms!
CVS = cvs
DIFF = diff -u -a
CVS_FAST_EXPORT = ../cvs-fast-export $(OPTS)
PYTHON = python3

check: test

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
	@echo "No diff output is good news."

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
	@echo "== Dump regressions =="
	@-for file in $(TESTLOADS); do \
	    legend=$$( ( grep -a '##' $${file}.tst || echo ' ## (no description)' ) | sed 's/## //' ); \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${legend}" $${file}.chk; \
	done

MASTERS := $(shell ls -1 *,v | sed '/,v/s///')
m_rebuild:
	@-for file in $(MASTERS); do \
	    echo "Remaking $${file}.chk"; \
	    $(CVS_FAST_EXPORT) $${file},v >$${file}.chk 2>&1; \
	done;
m_regress:
	@echo "== Master-parsing regressions =="
	@-for file in $(MASTERS); do \
	    legend=$$(sed <$${file},v -n -e '/^comment[	 ]*@# \(.*\)@;/s//\1/p'); \
	    $(CVS_FAST_EXPORT) $${file},v 2>&1 | tapdiffer "$${legend}" $${file}.chk; \
	done

INCREMENTAL=twobranch
THRESHOLD=104000
i_rebuild: neutralize.map
	@-for file in $(INCREMENTAL); do \
	    echo "Remaking $${file}.inc-chk"; \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -A neutralize.map -i $(THRESHOLD) >$${file}.inc-chk 2>&1; \
	done;
i_regress: neutralize.map
	@echo "== Incremental-dump regressions =="
	@-for file in $(INCREMENTAL); do \
	    legend=$$( ( grep -a '##' $${file}.tst  || echo ' ## (no description)') | sed 's/## //' ); \
	    $(MAKE) --quiet $${file}.repo; \
	    find $${file}.repo/module -name '*,v' | $(CVS_FAST_EXPORT) -T -i $(THRESHOLD) -A neutralize.map 2>&1 | tapdiffer "$${legend}" $${file}.inc-chk; \
	done

REDUCED=oldhead
r_rebuild: neutralize.map
	@-for file in $(REDUCED); do \
	    echo "Remaking $${file}.chk"; \
	    find $${file}.testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) >$${file}.chk 2>&1; \
	done;
r_regress: neutralize.map
	@echo "== Repo regressions =="
	@-for repo in $(REDUCED); do \
	    legend=$$( ( grep -a '##' $${repo}.testrepo/README  || echo ' ## (no description)') | sed 's/## //' ); \
	    find $${repo}.testrepo/module -name '*,v' | $(CVS_FAST_EXPORT) $(TESTOPTS) 2>&1 | tapdiffer "$${legend}" $${repo}.chk; \
	done

PYTESTS=t9601 t9602 t9603 t9604 t9605
PATHSTRIP = sed -e '/\/.*tests/s//tests/'
t_regress:
	@echo "== Pathological cases =="
	@for pytest in $(PYTESTS); do \
		legend=$$( ( grep -a '##' $${pytest}.py  || echo ' ## (no description)') | sed 's/## //' ); \
		$(PYTHON) $${pytest}.py 2>&1 | $(PATHSTRIP) | tapdiffer "$${legend}" $${pytest}.err; \
	done
t_rebuild:
	@for pytest in $(PYTESTS); do \
		echo "Remaking $${pytest}.err "; \
		$(PYTHON) $${pytest}.py 2>&1 | $(PATHSTRIP) >$${pytest}.err; \
	done

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
c_regress:
	@echo "== Conversion checks =="
	@for ct in $(CT); do $(MAKE) --quiet $$ct; cvsconvert -q -n $$ct; done
	@for cd in $(CD); do cvsconvert -q -n $$cd; done

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

sporadic:
	@echo "== Sporadic tests =="
	@sh incremental.sh

clean:
	rm -fr neutralize.map *.checkout *.repo *.pyc *.dot *.git *.git.fi
