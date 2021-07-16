#!/bin/bash
set -euo pipefail

if [ -n "${V:-}" ]; then
  set -x
fi

baseoutputdir=$1; shift
fixtures=$1; shift
testname=$1; shift

# this is used directly just by the basic test, but it also hosts the RPMs
export fixtures

successdir="${baseoutputdir}/success"

outputdir="${baseoutputdir}/running/${testname}"
rm -rf "${outputdir:?}"/*
mkdir -p "${outputdir}"

# keep original stdout around; this propagates to the terminal
exec 3>&1

# but redirect everything else to a log file
exec 1>"${outputdir}/output.log"
exec 2>&1

# seed output log with current date
date

if [ -n "${V:-}" ]; then
  setpriv --pdeathsig SIGKILL -- tail -f "${outputdir}/output.log" >&3 &
fi

echo "EXEC: ${testname}" >&3

# this will cause libtest.sh to allocate a tmpdir and cd to it
export COMPOSETESTS=1

# shellcheck source=../common/libtest.sh disable=2154
. "${commondir}/libtest.sh"

# use `git clone` rather than a symlink; we want our own copy so that we can
# modify it
git clone file://${fixtures}/config
ostree init --repo repo --mode=bare-user

if "${topsrcdir}/tests/compose/test-${testname}.sh"; then
  mkdir -p ${successdir} >&3
  mv ${outputdir} ${successdir}/${testname} >&3
  echo "PASS: ${testname}" >&3
else
  echo "FAIL: ${testname}" >&3
  if [ -z "${V:-}" ]; then
    tail -n20 "${outputdir}/output.log" | sed "s/^/   ${testname}: /g" >&3
  fi

  if [ -n "${COMPOSE_DEBUG:-}" ]; then
    echo "--- COMPOSE_DEBUG ---" >&3
    echo "Working directory: ${PWD}" >&3
    echo "Sleeping..." >&3
    sleep infinity
  fi
  exit 1
fi
