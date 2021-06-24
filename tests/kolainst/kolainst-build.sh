#!/bin/bash
# This pre-generates RPMs for testing that will be provided
# to the kola tests as data/, so we don't need to rpmbuild.
set -euo pipefail
dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd $dn/../.. && pwd)
commondir=$(cd "$dn/../common" && pwd)
export topsrcdir commondir
. "${commondir}/libtest.sh"

rm rpm-repos -rf

test_tmpdir=$(mktemp -d)
mkdir ${test_tmpdir}/rpm-repos
repover=0

# Right now we build a few RPMs, with one repo version,
# but the idea is to extend this with more.
mkdir ${test_tmpdir}/rpm-repos/${repover}
# The obligatory `foo` and `bar` packages
build_rpm foo version 1.2 release 3
build_rpm bar
# And from here we lose our creativity and name things starting
# with `testpkg` and grow more content.
# This one has various files in /etc
build_rpm testpkg-etc \
  build 'echo "A config file for %{name}" > %{name}.conf' \
  install 'mkdir -p %{buildroot}/etc
           install %{name}.conf %{buildroot}/etc
           echo otherconf > %{buildroot}/etc/%{name}-other.conf
           mkdir -p %{buildroot}/etc/%{name}/
           echo subconfig-one > %{buildroot}/etc/%{name}/subconfig-one.conf
           echo subconfig-two > %{buildroot}/etc/%{name}/subconfig-two.conf
           mkdir -p %{buildroot}/etc/%{name}/subdir
           install -d %{buildroot}/etc/%{name}/subdir/subsubdir
           echo subconfig-three > %{buildroot}/etc/%{name}/subdir/subsubdir/subconfig-three.conf
           ln -s / %{buildroot}/etc/%{name}/subdir/link2root
           ln -s nosuchfile %{buildroot}/etc/%{name}/link2nowhere
           ln -s . %{buildroot}/etc/%{name}/subdir/link2self
           ln -s ../.. %{buildroot}/etc/%{name}/subdir/link2parent
           mkdir -p %{buildroot}/etc/opt
           echo file-in-opt-subdir > %{buildroot}/etc/opt/%{name}-opt.conf' \
  files "/etc/%{name}.conf
         /etc/%{name}-other.conf
         /etc/%{name}/*
         /etc/opt/%{name}*"
# A service that adds a user and has data in tmpfiles.d
build_rpm testdaemon \
             build "echo testdaemon-binary > %{name}" \
             install "mkdir -p %{buildroot}/{usr/bin,var/lib/%{name}}
                      install %{name} %{buildroot}/usr/bin" \
             pre "groupadd -r testdaemon-group
                  useradd -r testdaemon-user -g testdaemon-group -s /sbin/nologin" \
             files "/usr/bin/%{name}
                    /var/lib/%{name}"
# Will be useful for testing cancellation
build_rpm testpkg-post-infinite-loop \
             post "echo entering testpkg-post-infinite-loop 1>&2; while true; do sleep 1h; done"
# Test module
build_rpm foomodular
build_module foomodular \
  stream no-profile \
  rpm foomodular-0:1.0-1.x86_64
build_module foomodular \
  stream no-default-profile \
  profile myprof:foomodular \
  profile myotherprof:foomodular \
  rpm foomodular-0:1.0-1.x86_64
build_module foomodular \
  stream with-default-profile \
  profile default:foomodular \
  profile myotherprof:foomodular \
  rpm foomodular-0:1.0-1.x86_64
build_module_defaults foomodular \
  defprofile with-default-profile:default

mv ${test_tmpdir}/yumrepo/* ${test_tmpdir}/rpm-repos/${repover}

# Other repo versions here e.g.
# repover=1
# ...
# mv ${test_tmpdir}/yumrepo/* ${test_tmpdir}/rpm-repos/${repover}

# And finally; put in place. This is the marker for `make` to know we succeeded.
mv ${test_tmpdir}/rpm-repos rpm-repos
