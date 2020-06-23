Submitting patches
------------------

Submit a pull request against <https://github.com/coreos/rpm-ostree>.

Please look at `git log` and match the commit log style.

All contributions with user-visible changes should include a
patch to the [CHANGELOG.md](CHANGELOG.md) under the
"Unreleased" header. Non-trivial development-related changes
are also welcome there.

Running the test suite
----------------------

There is `make check` as well as `make vmcheck`. See also what
the [Jenkinsfile](.cci.jenkinsfile) file does as well as the
[HACKING.md](HACKING.md) document.

Coding style
------------

See the [OSTree CONTRIBUTING](https://ostree.readthedocs.io/en/latest/CONTRIBUTING/)
coding style.
