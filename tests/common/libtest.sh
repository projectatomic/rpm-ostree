# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

# Have we already been sourced?
if test -n "${LIBTEST_SH:-}"; then
  # would be good to know when it happens
  echo "INFO: Skipping subsequent sourcing of libtest.sh"
  return
fi
LIBTEST_SH=1

self="$(realpath $0)"
if test -z "${SRCDIR:-}" && test -n "${topsrcdir:-}"; then
    SRCDIR=${topsrcdir}/tests
    commondir=${SRCDIR}/common
fi
commondir=${commondir:-${KOLA_EXT_DATA}}
. ${commondir}/libtest-core.sh

for bin in jq; do
    if ! command -v $bin >/dev/null; then
        (echo ${bin} is required to execute tests 1>&2; exit 1)
    fi
done

_cleanup_tmpdir () {
    if test -z "${TEST_SKIP_CLEANUP:-}"; then
	if test -f ${test_tmpdir}/.test; then
           rm ${test_tmpdir} -rf
	fi
    else
	echo "Skipping cleanup of ${test_tmpdir}"
    fi
}

# Create a tmpdir if we're running as a local test (i.e. through `make check`)
# or as a `vmcheck` test, which also needs some scratch space on the host.
if { test -n "${UNINSTALLEDTESTS:-}" || \
     test -n "${VMTESTS:-}" || \
     test -n "${COMPOSETESTS:-}"; } && ! test -f "$PWD/.test"; then
   # Use --tmpdir to keep it in /tmp. This also keeps paths short; this is
   # important if we want to create UNIX sockets under there.
   test_tmpdir=$(mktemp -d test.XXXXXX --tmpdir)
   touch ${test_tmpdir}/.test
   trap _cleanup_tmpdir EXIT SIGINT
   cd ${test_tmpdir}
fi
if test -n "${UNINSTALLEDTESTS:-}"; then
    export PATH=${builddir}:${PATH}
fi

test_tmpdir=$(pwd)
echo "Using tmpdir ${test_tmpdir}"

export G_DEBUG=fatal-warnings

# Don't flag deployments as immutable so that test harnesses can
# easily clean up.
export OSTREE_SYSROOT_DEBUG=mutable-deployments

# See comment in ot-builtin-commit.c and https://github.com/ostreedev/ostree/issues/758
# Also keep this in sync with the bits in libostreetest.c
case $(stat -f --printf '%T' /) in
    overlayfs)
        echo "overlayfs found; enabling OSTREE_NO_XATTRS"
        ostree --version
        export OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},no-xattrs"
        export OSTREE_NO_XATTRS=1 ;;
    *) echo "Not using overlayfs" ;;
esac

if test -n "${OT_TESTS_DEBUG:-}"; then
    set -x
fi

check_root_test ()
{
    if test "$(id -u)" != "0"; then
       skip "This test requires uid 0"
    fi
    if ! capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin'; then
        skip "No CAP_SYS_ADMIN in bounding set"
    fi
}

# https://github.com/ostreedev/ostree/commit/47b4dd1b38e422254afa67756873957c25aeab6d
# Unfortunately, introspection uses dlopen(), which doesn't quite
# work when the DSO is compiled with ASAN but the outer executable
# isn't.
skip_one_with_asan () {
    if test -n "${BUILDOPT_ASAN:-}"; then
        echo "ok # SKIP - built with ASAN"
        return 0
    else
        return 1
    fi
}

get_obj_path() {
  repo=$1; shift
  csum=$1; shift
  objtype=$1; shift
  echo "${repo}/objects/${csum:0:2}/${csum:2}.${objtype}"
}

uinfo_cmd() {
    ${SRCDIR}/utils/updateinfo --repo "${test_tmpdir}/yumrepo" "$@"
}

# builds a new RPM and adds it to the testdir's repo
# $1 - name
# $2+ - optional, treated as directive/value pairs
build_rpm() {
    local name=$1; shift
    # Unset, not zero https://github.com/projectatomic/rpm-ostree/issues/349
    local epoch=""
    local version=1.0
    local release=1
    local arch=x86_64

    mkdir -p $test_tmpdir/yumrepo/{specs,packages}
    local spec=$test_tmpdir/yumrepo/specs/$name.spec

    # write out the header
    cat > $spec << EOF
Name: $name
Summary: %{name}
License: GPLv2+
EOF

    local build= install= files= pretrans= pre= post= posttrans= post_args=
    local verifyscript= uinfo=
    local transfiletriggerin= transfiletriggerin_patterns=
    local transfiletriggerin2= transfiletriggerin2_patterns=
    local transfiletriggerun= transfiletriggerun_patterns=
    while [ $# -ne 0 ]; do
        local section=$1; shift
        local arg=$1; shift
        case $section in
        requires)
            echo "Requires: $arg" >> $spec;;
        recommends)
            echo "Recommends: $arg" >> $spec;;
        provides)
            echo "Provides: $arg" >> $spec;;
        conflicts)
            echo "Conflicts: $arg" >> $spec;;
        post_args)
            post_args="$arg";;
        version|release|epoch|arch|build|install|files|pretrans|pre|post|posttrans|verifyscript|uinfo)
            declare $section="$arg";;
        transfiletriggerin)
            transfiletriggerin_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerin2)
            transfiletriggerin2_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerun)
            transfiletriggerun_patterns="$arg";
            declare $section="$1"; shift;;
        *)
            assert_not_reached "unhandled section $section";;
        esac
    done

    cat >> $spec << EOF
Version: $version
Release: $release
${epoch:+Epoch: $epoch}
BuildArch: $arch

%description
%{summary}

# by default, we create a /usr/bin/$name script which just outputs $name
%build
echo -e "#!/bin/sh\necho $name-$version-$release.$arch" > $name
chmod a+x $name
$build

${pretrans:+%pretrans}
$pretrans

${pre:+%pre}
$pre

${post:+%post} ${post_args}
$post

${posttrans:+%posttrans}
$posttrans

${transfiletriggerin:+%transfiletriggerin -- ${transfiletriggerin_patterns}}
$transfiletriggerin

${transfiletriggerin2:+%transfiletriggerin -- ${transfiletriggerin2_patterns}}
$transfiletriggerin2

${transfiletriggerun:+%transfiletriggerun -- ${transfiletriggerun_patterns}}
$transfiletriggerun

${verifyscript:+%verifyscript}
$verifyscript

%install
mkdir -p %{buildroot}/usr/bin
install $name %{buildroot}/usr/bin
$install

%clean
rm -rf %{buildroot}

%files
/usr/bin/$name
$files
EOF

    # because it'd be overkill to set up mock for this, let's just fool
    # rpmbuild using setarch
    local buildarch=$arch
    if [ "$arch" == "noarch" ]; then
        buildarch=$(uname -m)
    fi

    (cd $test_tmpdir/yumrepo/specs &&
     setarch $buildarch rpmbuild --target $arch -ba $name.spec \
        --define "_topdir $PWD" \
        --define "_sourcedir $PWD" \
        --define "_specdir $PWD" \
        --define "_builddir $PWD/.build" \
        --define "_srcrpmdir $PWD" \
        --define "_rpmdir $test_tmpdir/yumrepo/packages" \
        --define "_buildrootdir $PWD")
    # use --keep-all-metadata to retain previous updateinfo
    (cd $test_tmpdir/yumrepo &&
     createrepo_c --no-database --update --keep-all-metadata .)
    # convenience function to avoid follow-up add-pkg
    if [ -n "$uinfo" ]; then
        uinfo_cmd add-pkg $uinfo $name 0 $version $release $arch
    fi
    if test '!' -f $test_tmpdir/yumrepo.repo; then
        cat > $test_tmpdir/yumrepo.repo.tmp << EOF
[test-repo]
name=test-repo
baseurl=file:///$PWD/yumrepo
EOF
        mv $test_tmpdir/yumrepo.repo{.tmp,}
    fi
}

# build an SELinux package ready to be installed -- really, we just support file
# context entries for now, though it's enough to test policy changes
# $1 - package name
# $2+ - pairs of file path regex and context types
build_selinux_rpm() {
    local name=$1; shift

    local module_dir=$test_tmpdir/policies/$name
    mkdir -p $module_dir
    local module_te=$module_dir/$name.te
    local module_fc=$module_dir/$name.fc

    # also declare a type associated with the app; any non-trivial SELinux
    # package will have some type enforcement rules that will require policy
    # recompilation
    cat > $module_te <<EOF
policy_module(${name}, 1.0.0)
type ${name}_t;
EOF

    echo -n "" > $module_fc

    while [ $# -ne 0 ]; do
        local fc_regex=$1; shift
        local fc_type=$1; shift
        local fc_label="gen_context(system_u:object_r:$fc_type,s0)"
        echo "$fc_regex -- $fc_label" >> $module_fc
    done

    make -C $module_dir -f /usr/share/selinux/devel/Makefile $name.pp

    # We point the spec file directly at our pp. This is a bit underhanded, but
    # it's cleaner than copying it in and using e.g. Source0 or something.
    local pp=$(realpath $module_dir/$name.pp)
    local install_dir=/usr/share/selinux/packages
    build_rpm $name install "mkdir -p %{buildroot}${install_dir}
                             install ${pp} %{buildroot}${install_dir}" \
                    post "semodule -n -i ${install_dir}/${name}.pp" \
                    files "${install_dir}/${name}.pp"
}

# build a module
# $1 - module name
# $2+ - optional, treated as directive/value pairs
build_module() {
    # warning: this function is at the boundary of comfortable bash scripting
    local name=$1; shift
    local stream=latest
    local version=$(date +%Y%m%d%H%M%S)
    local context=$(uuidgen | cut -d- -f1)
    local arch=x86_64

    declare -A profiles
    declare -a rpms
    local requires=
    while [ $# -ne 0 ]; do
        local section=$1; shift
        local arg=$1; shift
        case $section in
        stream|version|context|arch|requires)
            declare $section="$arg";;
        rpm)
            rpms+=($arg);;
        profile)
            profiles[${arg%:*}]=${arg#*:};;
        *)
            assert_not_reached "unhandled section $section";;
        esac
    done

    local rendered_profiles="{"
    for profile in "${!profiles[@]}"; do
        rendered_profiles+="$profile: {rpms: [${profiles[$profile]}]},"
    done
    rendered_profiles+="}"

    # write out the module YAML
    # see https://docs.fedoraproject.org/en-US/modularity/building-modules/fedora/defining-modules/
    mkdir -p $test_tmpdir/yumrepo/modules
    local md=$test_tmpdir/yumrepo/modules/$name:$stream:$version:$context:$arch.modulemd.yaml
    cat >> $md << EOF
document: modulemd
version: 2
data:
  name: $name
  stream: $stream
  version: $version
  context: $context
  arch: $arch
  summary: $name
  description: $name
  license: {module: [MIT]}
  dependencies:
  - requires: {platform: []${requires:+, ${requires}}}
  profiles: $rendered_profiles
  artifacts:
    rpms: [$(IFS=,; echo "${rpms[*]}")]
EOF

    # use --keep-all-metadata to retain previous updateinfo
    (cd $test_tmpdir/yumrepo &&
     createrepo_c --no-database --update --keep-all-metadata .)
}

# build a module defaults file
# $1 - module name
# $2+ - optional, treated as directive/value pairs
build_module_defaults() {
    local name=$1; shift
    declare -A defprofiles
    local defstream=
    while [ $# -ne 0 ]; do
        local section=$1; shift
        local arg=$1; shift
        case $section in
        defstream)
            defstream="$arg";;
        defprofile)
            defprofiles[${arg%:*}]=${arg#*:};;
        *)
            assert_not_reached "unhandled section $section";;
        esac
    done

    local rendered_dict="{"
    for stream in "${!defprofiles[@]}"; do
        rendered_dict+="$stream: [${defprofiles[$stream]}],"
    done
    rendered_dict+="}"

    # write out the module defaults YAML
    # see https://github.com/fedora-modularity/libmodulemd/blob/8577d670e88b9a0938428df80bfbdfe6b69698de/yaml_specs/modulemd_defaults_v1.yaml
    mkdir -p $test_tmpdir/yumrepo/modules
    local md=$test_tmpdir/yumrepo/modules/$name.modulemd-defaults.yaml
        cat >> $md << EOF
document: modulemd-defaults
version: 1
data:
  module: $name
  modified: $(date +%Y%m%d%H%M)
  ${defstream:+stream: ${defstream}}
  profiles: ${rendered_dict}
EOF

    # use --keep-all-metadata to retain previous updateinfo
    (cd $test_tmpdir/yumrepo &&
     createrepo_c --no-database --update --keep-all-metadata .)
}

files_are_hardlinked() {
    inode1=$(stat -c %i $1)
    inode2=$(stat -c %i $2)
    test -n "${inode1}" && test -n "${inode2}"
    [ "${inode1}" == "${inode2}" ]
}

assert_files_hardlinked() {
    if ! files_are_hardlinked "$1" "$2"; then
        fatal "Files '$1' and '$2' are not hardlinked"
    fi
}

# $1  - json file
# $2+ - assertions
assert_jq() {
    f=$1; shift
    for expression in "$@"; do
        if ! jq -e "${expression}" >/dev/null < $f; then
            jq . < $f | sed -e 's/^/# /' >&2
            echo 1>&2 "${expression} failed to match $f"
            exit 1
        fi
    done
}

# Takes a list of `jq` expressions, each of which should evaluate to a boolean,
# and asserts that they are true.
rpmostree_assert_status() {
    rpm-ostree status --json > status.json
    assert_jq status.json "$@"
    rm -f status.json
}

# This function below was taken and adapted from coreos-assembler. We
# should look into sharing this code more easily.

# Determine if current user has enough privileges for composes
_privileged=
has_compose_privileges() {
    if [ -z "${_privileged:-}" ]; then
        if [ -n "${FORCE_UNPRIVILEGED:-}" ]; then
            echo "Detected FORCE_UNPRIVILEGED; using virt"
            _privileged=0
        elif ! capsh --print | grep -q 'Bounding.*cap_sys_admin'; then
            echo "Missing CAP_SYS_ADMIN; using virt"
            _privileged=0
        elif [ "$(id -u)" != "0" ]; then
            echo "Not running as root; using virt"
            _privileged=0
        else
            _privileged=1
        fi
    fi
    [ ${_privileged} == 1 ]
}

# Given a version, enable the local rpm-md repo at that version.
# Currently the only supported version is `0`.
libtest_enable_repover() {
    local v=$1
    shift
    cat >/etc/yum.repos.d/libtest.repo <<EOF
[libtest]
name=libtest repo
baseurl=file://${KOLA_EXT_DATA}/rpm-repos/${v}
gpgcheck=0
enabled=1
EOF
}

# Use this at the top of installed tests which should
# run fully offline.  We also canonicalize to a single
# deployment.
libtest_prepare_offline() {
    rpm-ostree cleanup -pr
    rm -vrf /etc/yum.repos.d/*
}
