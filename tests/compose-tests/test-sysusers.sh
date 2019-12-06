#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "sysusers"
cat >sysusers-base.yaml <<EOF
experimental:
  sysusers: true
EOF
cat >mergeyaml.py <<EOF
#!/usr/bin/python3
import json,yaml,sys
with open("${treefile}") as f:
  tf = json.load(f)
with open(sys.argv[1]) as f:
  for k,v in yaml.safe_load(f).items():
    tf[k] = v
with open("${treefile}", 'w') as f:
  json.dump(tf, f)
EOF
chmod a+x mergeyaml.py

./mergeyaml.py sysusers-base.yaml
# Run a compose without static assignments
if runcompose --unified-core 2>err.txt; then
    fatal "sysusers should have errored"
fi

assert_file_has_content_literal err.txt 'Non-static uid/gid assignments for files found'
echo "ok sysusers expected error"

cat >sysusers.yaml <<EOF
# Keep this in sync with https://github.com/coreos/fedora-coreos-config/pull/56
experimental:
  sysusers: true
  sysusers-users:
    dbus: 81  # Historical
    polkitd: 921
    chrony: 922
  sysusers-groups:
    utmp: 22 # Matches historical soft value
EOF
./mergeyaml.py sysusers.yaml
runcompose --unified-core

ostreels() {
    ostree --repo=${repobuild} ls ${treeref} "$@"
}

if ostreels /usr/lib/passwd 2>/dev/null; then
    fatal "Found /usr/lib/passwd"
fi

for x in {basic,rpmostree-auto,dbus}.conf; do
    ostree --repo=${repobuild} cat "${treeref}" /usr/lib/sysusers.d/$x > $x
done
assert_file_has_content dbus.conf '^u  *dbus  *81 '
assert_file_has_content basic.conf '^g  *utmp  *22 '
assert_file_has_content rpmostree-auto.conf '^u  *chrony  *922 '
assert_file_has_content rpmostree-auto.conf '^u  *polkitd  *921 '

echo "ok sysusers"
