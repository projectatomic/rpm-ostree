#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
# And suppress depcheck since we didn't install via RPM
export COSA_SUPPRESS_DEPCHECK=1
ls -al /usr/bin/rpm-ostree
rpm-ostree --version
cd $(mktemp -d)
cosa init https://github.com/coreos/fedora-coreos-config/
rsync -rlv /cosa/component-install/ overrides/rootfs/
cosa fetch
cosa build

# vmcheck tests
export COSA_DIR=$(pwd)
cd /cosa/component-source-tests
env JOBS=2 ./tests/vmcheck.sh
cd ${COSA_DIR}

# kola tests, just ours for now
cosa kola run 'ext.rpm-ostree.*'
