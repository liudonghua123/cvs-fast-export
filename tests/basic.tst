#!/usr/bin/env python
## basic test for CVS master parsing

import testlifter

repo = testlifter.CVSRepository("basic.repo")
repo.init()
repo.module("basic")
co = repo.checkout("basic", "basic.checkout")

co.write("README", "The quick brown fox jumped over the lazy dog.\n")
co.add("README")
co.commit("This is a sample commit")

repo.cleanup()

# end
