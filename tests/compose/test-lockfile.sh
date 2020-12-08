#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
build_rpm test-pkg-common
build_rpm test-pkg requires test-pkg-common
build_rpm another-test-pkg

# The test suite writes to pwd, but we need repos together with the manifests
# Also we need to disable gpgcheck
echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
treefile_append "packages" '["test-pkg", "another-test-pkg"]'

runcompose --ex-write-lockfile-to="$PWD/versions.lock"
rpm-ostree --repo=${repo} db list ${treeref} > test-pkg-list.txt
assert_file_has_content test-pkg-list.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content test-pkg-list.txt 'another-test-pkg-1.0-1.x86_64'
echo "ok compose"

assert_has_file "versions.lock"
assert_jq "versions.lock" \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg"].evra = "1.0-1.x86_64"' \
  '.metadata.rpmmd_repos|length > 0' \
  '.metadata.generated'
echo "ok lockfile created"

# Read lockfile back (should be a no-op)
build_rpm test-pkg-common version 2.0
build_rpm test-pkg version 2.0 requires test-pkg-common
build_rpm another-test-pkg version 2.0
runcompose --ex-lockfile="$PWD/versions.lock" |& tee out.txt

rpm-ostree --repo=${repo} db list ${treeref} > test-pkg-list.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-1.0-1.x86_64'
echo "ok lockfile read"

# now add an override and check that not specifying a digest is allowed
cat > override.lock <<EOF
{
  "packages": {
    "another-test-pkg": {
      "evra": "2.0-1.x86_64"
    }
  }
}
EOF

runcompose \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-lockfile="$PWD/override.lock" \
  --ex-write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_file_has_content out.txt 'another-test-pkg-2.0-1.x86_64'
assert_jq versions.lock.new \
  '.packages["test-pkg"].evra = "1.0-1.x86_64"' \
  '.packages["test-pkg-common"].evra = "1.0-1.x86_64"' \
  '.packages["another-test-pkg"].evra = "2.0-1.x86_64"'
echo "ok override"

# sanity-check that we can remove packages in relaxed mode
treefile_remove "packages" '"another-test-pkg"'
runcompose \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-lockfile="$PWD/override.lock" \
  --ex-write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'test-pkg-1.0-1.x86_64'
assert_file_has_content out.txt 'test-pkg-common-1.0-1.x86_64'
assert_not_file_has_content out.txt 'another-test-pkg'
echo "ok relaxed mode can remove pkg"

# check that we prefer locked packages over unlocked ones to minimize the diff
build_rpm dodo requires dodo-dep
build_rpm dodo-dep-a provides dodo-dep
build_rpm dodo-dep-b provides dodo-dep
cat > override.lock <<EOF
{
  "packages": {
    "dodo-dep-b": {
      "evra": "1.0-1.x86_64"
    }
  }
}
EOF
treefile_append "packages" '["dodo"]'
runcompose \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-lockfile="$PWD/override.lock" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'dodo-1.0-1.x86_64'
assert_file_has_content out.txt 'dodo-dep-b-1.0-1.x86_64'
assert_not_file_has_content out.txt 'dodo-dep-a-1.0-1.x86_64'
echo "ok relaxed mode prefer locked pkg"
treefile_remove "packages" '"dodo"'

# test strict mode

# sanity-check that refeeding the output lockfile as input satisfies strict mode
mv versions.lock{.new,}
runcompose \
  --ex-lockfile-strict \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_streq \
  "$(jq .packages versions.lock | sha256sum)" \
  "$(jq .packages versions.lock.new | sha256sum)"
echo "ok strict mode sanity check"

# check that trying to install a pkg that's not in the lockfiles fails
build_rpm unlocked-pkg
treefile_append "packages" '["unlocked-pkg"]'
if runcompose \
    --ex-lockfile-strict \
    --ex-lockfile="$PWD/versions.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Packages not found: unlocked-pkg'
echo "ok strict mode no unlocked pkgs"

# check that a locked pkg with unlocked deps causes an error
build_rpm unlocked-pkg version 2.0 requires unlocked-pkg-dep
build_rpm unlocked-pkg-dep version 2.0
# notice we add unlocked-pkg, but not unlocked-pkg-dep
cat > override.lock <<EOF
{
  "packages": {
    "unlocked-pkg": {
      "evra": "2.0-1.x86_64"
    }
  }
}
EOF
if runcompose \
    --ex-lockfile-strict \
    --ex-lockfile="$PWD/versions.lock" \
    --ex-lockfile="$PWD/override.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt 'Could not depsolve transaction'
assert_file_has_content err.txt 'unlocked-pkg-dep-2.0-1.x86_64 is filtered out by exclude filtering'
treefile_remove "packages" '"unlocked-pkg"'
echo "ok strict mode no unlocked pkg deps"

# check that a locked pkg which isn't actually in the repos causes an error
cat > override.lock <<EOF
{
  "packages": {
    "unmatched-pkg": {
      "evra": "1.0-1.x86_64"
    }
  }
}
EOF
if runcompose \
    --ex-lockfile-strict \
    --ex-lockfile="$PWD/versions.lock" \
    --ex-lockfile="$PWD/override.lock" \
    --dry-run "${treefile}" &>err.txt; then
  fatal "compose unexpectedly succeeded"
fi
assert_file_has_content err.txt "Couldn't find locked package 'unmatched-pkg-1.0-1.x86_64'"
echo "ok strict mode locked pkg missing from rpmmd"

# test lockfile-repos, i.e. check that a pkg in a lockfile repo with higher
# NEVRA isn't picked unless if it's not in the lockfile

# some file shuffling to get a separate yumrepo-locked/ which has foobar-2.0
build_rpm foobar
mv yumrepo yumrepo.bak
build_rpm foobar version 2.0
mv yumrepo yumrepo-locked
mv yumrepo.bak yumrepo
sed -e 's/test-repo/test-lockfile-repo/g' < yumrepo.repo > yumrepo-locked.repo
sed -e 's/yumrepo/yumrepo-locked/g' < yumrepo-locked.repo > yumrepo-locked.repo.new
mv yumrepo-locked.repo.new yumrepo-locked.repo
ln "$PWD/yumrepo-locked.repo" config/yumrepo-locked.repo
treefile_append "packages" '["foobar"]'

# try first as a regular repo, to make sure it's functional
treefile_append "repos" '["test-lockfile-repo"]'
runcompose \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'foobar-2.0-1.x86_64'

# ok, now as a lockfile repo
treefile_remove "repos" '"test-lockfile-repo"'
treefile_append "lockfile-repos" '["test-lockfile-repo"]'
runcompose \
  --ex-lockfile="$PWD/versions.lock" \
  --ex-write-lockfile-to="$PWD/versions.lock.new" \
  --dry-run "${treefile}" |& tee out.txt
assert_file_has_content out.txt 'foobar-1.0-1.x86_64'
treefile_remove "packages" '"foobar"'
echo "ok lockfile-repos"
