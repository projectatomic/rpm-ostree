AC_PREREQ([2.63])
dnl To do a release, increment this number and the spec file, rename the CHANGELOG header,
dnl and create a new `[Unreleased]` one. Then commit the changes and submit it as a PR
dnl to merge via prow.  After that, do `git pull origin && git reset --hard origin/master`.
dnl Then draft release notes from the CHANGELOG.md and generate the shortlog using
dnl <https://github.com/cgwalters/homegit/blob/master/bin/git-shortlog-with-prs>.
dnl Then use <https://github.com/cgwalters/git-evtag> to create a signed tag with the
dnl release notes as its content.
dnl Then git push origin v201X.XX.
dnl Then create the xz tarball from `make -f Makefile.dist-packaging dist-snapshot`.
dnl Then create a GitHub release for the new release tag and attach the tarball.
m4_define([year_version], [2020])
m4_define([release_version], [3])
m4_define([package_version], [year_version.release_version])
AC_INIT([rpm-ostree], [package_version], [atomic-devel@projectatomic.io])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([buildutil])
AC_CONFIG_AUX_DIR([build-aux])
dnl Versioning information
AC_SUBST([YEAR_VERSION], [year_version])
AC_SUBST([RELEASE_VERSION], [release_version])
AC_SUBST([PACKAGE_VERSION], [package_version])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz subdir-objects])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
dnl we don't today use C++ in rpm-ostree, only in libdnf,
dnl but this has the side effect of setting CXXFLAGS with -g
dnl if not set, which we definitely want; cmake doesn't do that.
AC_PROG_CXX
AM_PROG_CC_C_O

# dbus system dir
AC_MSG_CHECKING(for dbus system services directory)
if test "$enable_prefix_only" = "yes"; then
  dbusservicedir='${datadir}/dbus-1/system-services'
else
  dbusservicedir='${datadir}/dbus-1/system-services'
  if test "$dbusservicedir" = ""; then
    AC_MSG_ERROR(Couldn't find dbus services directory. Try installing dbus-devel)
  fi
fi
AC_SUBST([dbusservicedir], [$dbusservicedir])
AC_MSG_RESULT(dbusservicedir)

dnl Keep this in sync with the version in ostree
AS_IF([echo "$CFLAGS" | grep -q -E -e '-Werror($| )'], [], [
CC_CHECK_FLAGS_APPEND([WARN_CFLAGS], [CFLAGS], [\
  -pipe \
  -Wall \
  -Werror=empty-body \
  -Werror=strict-prototypes \
  -Werror=missing-prototypes \
  -Werror=implicit-function-declaration \
  "-Werror=format=2 -Werror=format-security -Werror=format-nonliteral" \
  -Werror=pointer-arith -Werror=init-self \
  -Werror=missing-declarations \
  -Werror=return-type \
  -Werror=switch \
  -Werror=overflow \
  -Werror=int-conversion \
  -Werror=parenthesis \
  -Werror=undef \
  -Werror=incompatible-pointer-types \
  -Werror=misleading-indentation \
  -Werror=missing-include-dirs -Werror=aggregate-return \
  -Wstrict-aliasing=2 \
  -Werror=unused-result \
])])
AC_SUBST(WARN_CFLAGS)

AC_MSG_CHECKING([for -fsanitize=address in CFLAGS])
if echo $CFLAGS | grep -q -e -fsanitize=address; then
AC_MSG_RESULT([yes])
using_asan=yes
else
AC_MSG_RESULT([no])
fi
AM_CONDITIONAL(BUILDOPT_ASAN, [test x$using_asan = xyes])

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

RPM_OSTREE_FEATURES=""
AC_SUBST([RPM_OSTREE_FEATURES])

PKG_PROG_PKG_CONFIG

save_LIBS="$LIBS"
LIBS=
AC_SEARCH_LIBS([cap_init], [cap], [], [AC_MSG_ERROR([*** POSIX caps library not found])])
CAP_LIBS="$LIBS"
AC_SUBST(CAP_LIBS)
LIBS="$save_LIBS"

# Remember to update AM_CPPFLAGS in Makefile.am when bumping GIO req.
PKG_CHECK_MODULES(PKGDEP_GIO_UNIX, [gio-unix-2.0])
PKG_CHECK_MODULES(PKGDEP_RPMOSTREE, [gio-unix-2.0 >= 2.50.0 json-glib-1.0
				     ostree-1 >= 2020.1
				     libsystemd
				     polkit-gobject-1
				     rpm librepo libsolv
				     libarchive])

dnl -ldl: https://github.com/ostreedev/ostree/commit/1f832597fc83fda6cb8daf48c4495a9e1590774c
dnl -ldl: https://github.com/rust-lang/rust/issues/47714
dnl -lm: needed for rand crate in debug mode
dnl See also rpmostree_core_linkage_for_rust()
PKGDEP_RPMOSTREE_LIBS="$PKGDEP_RPMOSTREE_LIBS -Wl,--push-state,--no-as-needed,-ldl,-lm,--pop-state"

# We just keep rust-specific deps separate for better tracking
# The `libcurl` one is redundant since we already require it for libostree. `openssl`
# is required by libcurl anyway, but we need to link to it directly too because
# curl-rust uses it.
PKG_CHECK_MODULES(PKGDEP_RPMOSTREE_RS, [libcurl openssl])

dnl bundled libdnf
PKGDEP_RPMOSTREE_CFLAGS="-I $(pwd)/libdnf -I $(pwd)/libdnf-build $PKGDEP_RPMOSTREE_CFLAGS"
PKGDEP_RPMOSTREE_LIBS="-L$(pwd)/libdnf-build/libdnf -ldnf $PKGDEP_RPMOSTREE_LIBS"

dnl RHEL8.1 has old libarchive
AS_IF([pkg-config --atleast-version=3.3.3 libarchive],
  [AC_DEFINE([HAVE_LIBARCHIVE_ZSTD], 1, [Define if we have libarchive with zstd])])

dnl This is the current version in Fedora 25.
AS_IF([pkg-config --atleast-version=4.14.2 rpm], [], [AC_MSG_ERROR([librpm 4.14.2 required])])

dnl We don't *actually* use this ourself, but librepo does, and libdnf gets confused
dnl if librepo doesn't support it.
have_zchunk=no
AS_IF([pkg-config --exists zck],
  [have_zchunk=yes; AC_DEFINE([HAVE_ZCHUNK], 1, [Define if we have zchunk])])

AC_PATH_PROG([XSLTPROC], [xsltproc])

GLIB_TESTS
LIBGLNX_CONFIGURE

AC_CHECK_TOOL(GPERF, gperf)
AS_IF([test -z "$GPERF"],
  AC_MSG_ERROR([*** gperf not found])
)
dnl stolen from https://github.com/systemd/systemd/commit/c9f7b4d
GPERF_TEST="$(echo foo,bar | ${GPERF} -L ANSI-C)"
AC_COMPILE_IFELSE(
[AC_LANG_PROGRAM([
#include <string.h>
const char * in_word_set(const char *, size_t);
$GPERF_TEST]
)],
[GPERF_LEN_TYPE=size_t],
[AC_COMPILE_IFELSE(
[AC_LANG_PROGRAM([
#include <string.h>
const char * in_word_set(const char *, unsigned);
$GPERF_TEST]
)],
[GPERF_LEN_TYPE=unsigned],
[AC_MSG_ERROR([** unable to determine gperf len type])]
)]
)
AC_DEFINE_UNQUOTED([GPERF_LEN_TYPE], [$GPERF_LEN_TYPE], [gperf len type])

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.34.0])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
],[
AM_CONDITIONAL([ENABLE_GTK_DOC],[false])
])

AC_ARG_ENABLE(installed_tests,
              AS_HELP_STRING([--enable-installed-tests],
                             [Install test programs (default: no)]),,
              [enable_installed_tests=no])
AM_CONDITIONAL(BUILDOPT_INSTALL_TESTS, test x$enable_installed_tests = xyes)

AC_ARG_WITH(bubblewrap,
              AS_HELP_STRING([--with-bubblewrap],
                             [Path to bubblewrap binary (default: /usr/bin/bwrap)]),,
              [with_bubblewrap=/usr/bin/bwrap])
AC_DEFINE_UNQUOTED(WITH_BUBBLEWRAP_PATH, ["$with_bubblewrap"], [Define to bubblewrap path])

RPM_OSTREE_FEATURES="$RPM_OSTREE_FEATURES compose"

dnl PKG_CHECK_VAR added to pkg-config 0.28 (though it's already in the new pkgconf; this
dnl backport is more for el7)
m4_define_default(
    [PKG_CHECK_VAR],
    [AC_ARG_VAR([$1], [value of $3 for $2, overriding pkg-config])
     AS_IF([test -z "$$1"], [$1=`$PKG_CONFIG --variable="$3" "$2"`])
     AS_IF([test -n "$$1"], [$4], [$5])])

PKG_CHECK_VAR(BASH_COMPLETIONSDIR, [bash-completion], [completionsdir],,
  BASH_COMPLETIONSDIR="${datadir}/bash-completion/completions")
AC_SUBST(BASH_COMPLETIONSDIR)

AC_PATH_PROG([cargo], [cargo])
AS_IF([test -z "$cargo"], [AC_MSG_ERROR([cargo is required for --enable-rust])])
AC_PATH_PROG([rustc], [rustc])
AS_IF([test -z "$rustc"], [AC_MSG_ERROR([rustc is required for --enable-rust])])

dnl https://github.com/projectatomic/rpm-ostree/pull/1573
dnl We support 3 modes for bindgen:
dnl  - External /usr/bin/cbindgen (Fedora + CI)
dnl  - Downloading it from crates.io (CentOS + CI builds)
dnl  - Not running it at all, and using a pre-built rpmostree-rust.h (Koji)
AS_IF([test -d "${srcdir}"/rust/vendor],
      [cbindgen=dist],
      [AC_PATH_PROG([cbindgen], [cbindgen])])
AM_CONDITIONAL(HAVE_PREBUILT_CBINDGEN, [test "${cbindgen}" = "dist"])
AM_CONDITIONAL(HAVE_EXTERNAL_CBINDGEN, [test -n "${cbindgen}" && test "${cbindgen}" != dist])

AC_MSG_CHECKING(whether to build in debug mode)
debug_release=release
if $(echo $CFLAGS |grep -q -E "(-O0|-Og)"); then
  debug_release=debug
fi
AC_MSG_RESULT($debug_release)

dnl These bits based on gnome:librsvg/configure.ac
dnl By default, we build in release mode (but without LTO!)
AC_ARG_ENABLE(rust-debug,
  AC_HELP_STRING([--enable-rust-debug],
    [Build Rust code with debugging information [default=no]]),
    [rust_debug_release=$enableval],
    [rust_debug_release=$debug_release])
dnl Canonicalize yes/no to debug/release
AS_IF([test x$rust_debug_release = xno ], [rust_debug_release=release])
AS_IF([test x$rust_debug_release = xyes ], [rust_debug_release=debug])

RUST_TARGET_SUBDIR=${rust_debug_release}
AC_SUBST([RUST_TARGET_SUBDIR])
AM_CONDITIONAL(RUST_DEBUG, [test "x$rust_debug_release" = "xdebug"])
dnl Unconditional now.
RPM_OSTREE_FEATURES="$RPM_OSTREE_FEATURES rust"

dnl Only use this for package builds, as it will hackily edit Cargo.toml right now
AC_ARG_ENABLE(lto,
AC_HELP_STRING([--enable-lto],
               [Build code with Link Time Optimization [default=no]]))
dnl https://github.com/rust-lang/cargo/issues/4349
dnl https://bugzilla.mozilla.org/show_bug.cgi?id=1386371
if test "${enable_lto}" = yes; then
    if ! grep -q '^lto.*=true' rust/Cargo.toml 2>/dev/null; then
        echo '# Inserted by configure --enable-lto' >> rust/Cargo.toml
        echo "lto = true" >> rust/Cargo.toml
    fi
fi

dnl And propagate the release/debug type to cmake
cmake_args=-DCMAKE_BUILD_TYPE=RelWithDebugInfo
if test ${debug_release} = debug; then
  cmake_args="-DCMAKE_BUILD_TYPE=Debug"
fi
if test x${have_zchunk} = xno; then
  cmake_args="${cmake_args} -DWITH_ZCHUNK:BOOL=0"
fi
export cmake_args

dnl I picked /usr/libexec/rpm-ostree just because we need an
dnl arbitrary path - we don't actually install there.
dnl Use `WITH_GIR=0`; it's our private libdnf bundle and we don't need the bindings.
(set -euo pipefail; mkdir -p libdnf-build && cd libdnf-build &&
 set -x && 
 export CXXFLAGS &&
 cmake \
     -DCMAKE_INSTALL_PREFIX:PATH=/usr/libexec/rpm-ostree \
     -DINCLUDE_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree/include \
     -DLIB_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree \
     -DSYSCONF_INSTALL_DIR:PATH=/usr/libexec/rpm-ostree/etc \
     -DSHARE_INSTALL_PREFIX:PATH=/usr/libexec/rpm-ostree/share \
     -DBUILD_SHARED_LIBS:BOOL=ON \
     -DWITH_SWDB:BOOL=0 \
     -DWITH_BINDINGS:BOOL=0 \
     -DWITH_HTML:BOOL=0 \
     -DWITH_MAN:BOOL=0 \
     -DWITH_GIR:BOOL=0 \
     ${cmake_args} ../libdnf) || exit 1

AC_CONFIG_FILES([
Makefile
api-doc/Makefile
src/lib/rpm-ostree-1.pc
src/lib/rpmostree-version.h
])
AC_OUTPUT

echo "
    $PACKAGE $VERSION

    introspection:                           $found_introspection
    bubblewrap:                              $with_bubblewrap
    gtk-doc:                                 $enable_gtk_doc
    rust:                                    $rust_debug_release (lto: ${enable_lto:-no})
    cbindgen:                                ${cbindgen:-internal}
"
