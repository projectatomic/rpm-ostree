/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <systemd/sd-journal.h>
#include "rpmostree-output.h"
#include "rpmostree-util.h"
#include "rpmostree-rust.h"
#include "rpmostree-bwrap.h"
#include <err.h>
#include <fcntl.h>
#include <systemd/sd-journal.h>
#include "libglnx.h"

#include "rpmostree-scripts.h"
#include "rpmostree-rpm-util.h"

#define RPMOSTREE_MESSAGE_PREPOST SD_ID128_MAKE(42,d3,72,22,dc,a2,4a,3b,9d,30,ce,d4,bb,bc,ac,d2)
#define RPMOSTREE_MESSAGE_FILETRIGGER SD_ID128_MAKE(ef,dd,0e,4e,79,ca,45,d3,88,76,ac,45,e1,28,23,68)

/* This bit is currently private in librpm */
enum rpmscriptFlags_e {
  RPMSCRIPT_FLAG_NONE		= 0,
  RPMSCRIPT_FLAG_EXPAND	= (1 << 0), /* macro expansion */
  RPMSCRIPT_FLAG_QFORMAT	= (1 << 1), /* header queryformat expansion */
};

typedef struct {
    const char *desc;
    rpmsenseFlags sense;
    rpmTagVal tag;
    rpmTagVal progtag;
    rpmTagVal flagtag;
} KnownRpmScriptKind;

/* The RPM interpreter for built-in lua */
static const char lua_builtin[] = "<lua>";

#if 0
static const KnownRpmScriptKind ignored_scripts[] = {
  /* Ignore all of the *un variants since we never uninstall
   * anything in the RPM sense.
   */
  { "%preun", 0,
    RPMTAG_PREUN, RPMTAG_PREUNPROG, RPMTAG_PREUNFLAGS },
  { "%postun", 0,
    RPMTAG_POSTUN, RPMTAG_POSTUNPROG, RPMTAG_POSTUNFLAGS },
  { "%triggerun", RPMSENSE_TRIGGERUN,
    RPMTAG_TRIGGERUN, 0, 0 },
  { "%triggerpostun", RPMSENSE_TRIGGERPOSTUN,
    RPMTAG_TRIGGERPOSTUN, 0, 0 },

  /* https://github.com/projectatomic/rpm-ostree/issues/1216 */
  { "%verifyscript", 0,
    RPMTAG_VERIFYSCRIPT, RPMTAG_VERIFYSCRIPTPROG, RPMTAG_VERIFYSCRIPTFLAGS},

  /* In practice, %pretrans are hack-arounds for broken upgrades.
   * Again, since we always assemble a new root, there's no point
   * to running them.
   * http://lists.rpm.org/pipermail/rpm-ecosystem/2016-August/000391.html
   */
  { "%pretrans", 0,
    RPMTAG_PRETRANS, RPMTAG_PRETRANSPROG, RPMTAG_PRETRANSFLAGS },
};
#endif

/* Supported script types */
static const KnownRpmScriptKind pre_script =
  { "%prein", 0, RPMTAG_PREIN, RPMTAG_PREINPROG, RPMTAG_PREINFLAGS };
static const KnownRpmScriptKind post_script =
  { "%post", 0, RPMTAG_POSTIN, RPMTAG_POSTINPROG, RPMTAG_POSTINFLAGS };
static const KnownRpmScriptKind posttrans_script =
  { "%posttrans", 0, RPMTAG_POSTTRANS, RPMTAG_POSTTRANSPROG, RPMTAG_POSTTRANSFLAGS };

static const KnownRpmScriptKind unsupported_scripts[] = {
  { "%triggerprein", RPMSENSE_TRIGGERPREIN,
    RPMTAG_TRIGGERPREIN, 0, 0 },
  { "%triggerin", RPMSENSE_TRIGGERIN,
    RPMTAG_TRIGGERIN, 0, 0 },
};

typedef struct {
  const char *pkgname_script;
  const char *interp;
  const char *replacement;
} RpmOstreeLuaReplacement;

static const char glibc_langpacks_script[] =
  "set -euo pipefail\n"
  "tmpl=/usr/lib/locale/locale-archive.tmpl\n"
  "if test -s \"${tmpl}\"; then\n"
  "  cp -a \"${tmpl}\"{,.new} && mv \"${tmpl}\"{.new,}\n"
  "  exec /usr/sbin/build-locale-archive --install-langs \"%{_install_langs}\"\n"
  "fi\n";

static const RpmOstreeLuaReplacement lua_replacements[] = {
  /* This code only applies to Fedora <= 29, see:
   * https://src.fedoraproject.org/rpms/fedora-release/c/617b1bed348d92cba04443f8cce1486639a114f0?branch=master
   * Previously, the release packages were implemented Lua for
   * unnecessary reasons.  This doesn't fully generalize obviously arbitrarly
   * release packages, but anyone doing an exampleos-release package and that
   * wants to use rpm-ostree can just do `ln` in shell script in their package
   * too.
   */
  { "fedora-release-atomichost.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "ln -sf os.release.d/os-release-atomichost /usr/lib/os-release\n"
  },
  { "fedora-release-coreos.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "ln -sf os.release.d/os-release-coreos /usr/lib/os-release\n"
  },
  { "fedora-release-workstation.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "ln -sf os.release.d/os-release-workstation /usr/lib/os-release\n"
    "preset=/usr/lib/os.release.d/presets/80-workstation.preset\n"
    "if [ -f \"${preset}\" ]; then ln -sr ${preset} /usr/lib/systemd/system-preset && systemctl preset -q sshd.socket sshd.service cups.socket cups.path cups.service; fi\n"
  },
  { "fedora-release.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "if ! test -L /usr/lib/os-release; then ln -s os.release.d/os-release-fedora /usr/lib/os-release; fi\n"
  },
  /* Upstream bug for replacing lua with shell: https://bugzilla.redhat.com/show_bug.cgi?id=1367585
   * Further note that the current glibc code triggers a chain of bugs
   * in rofiles-fuse: https://github.com/ostreedev/ostree/pull/1470
   * Basically it does writes via mmap() and also to an unlink()ed file
   * and this creates a decoherence between the size reported by fstat()
   * vs the real size.  So here we break the hardlink for the template file
   * which glibc truncates, and down below we disable rofiles-fuse.   The glibc
   * locale code is (hopefully!) unlikely to go out mutating other files, so we'll
   * live with this hack for now.
   **/
  { "glibc-all-langpacks.posttrans",
    "/usr/bin/sh",
    glibc_langpacks_script
  },
  { "glibc-common.post",
    "/usr/bin/sh",
    glibc_langpacks_script
  },
  /* See https://src.fedoraproject.org/rpms/glibc/pull-request/12
   * Code originally introduced in https://src.fedoraproject.org/rpms/glibc/c/34927af202deb7d97dbb211a3cb13b1c53b496d3?branch=master
   * Most of that script is about working around bugs from a traditional RPM in-place update,
   * but rpm-ostree always starts from a clean filesystem checkout.  We just need step (4)
   * which is updating the iconv cache.
   */
  { "glibc.post",
    "/usr/bin/bash",
    "libdir=" LIBDIR "\n"
    "exec iconvconfig -o ${libdir}/gconv/gconv-modules.cache --nostdlib ${libdir}/gconv"
  },
  /* Just for the tests */
  { "rpmostree-lua-override-test.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "echo %{_install_langs} >/usr/share/rpmostree-lua-override-test\n"
  },
  { "rpmostree-lua-override-test-expand.post",
    "/usr/bin/sh",
    "set -euo pipefail\n"
    "echo %{_install_langs} >/usr/share/rpmostree-lua-override-test-expand\n"
  }
};

typedef struct {
  const char *pkgname_script;
  const char *release_suffix;
  const char *interp;
  const char *replacement;
} RpmOstreeScriptReplacement;

static const RpmOstreeScriptReplacement script_replacements[] = {
  /* Only neuter the rhel7 version; the Fedora one is fixed.
   * https://src.fedoraproject.org/rpms/pam/pull-request/3
   */
  { "pam.post", ".el7", NULL, NULL },
};

static gboolean
fail_if_interp_is_lua (const char *interp,
                       const char *pkg_name,
                       const char *script_desc,
                       GError    **error)
{
  if (g_strcmp0 (interp, lua_builtin) == 0)
    return glnx_throw (error, "Package '%s' has (currently) unsupported %s script in '%s'",
                       pkg_name, lua_builtin, script_desc);

  return TRUE;
}

static RpmOstreeScriptAction
lookup_script_action (const char *pkg_name,
                      const char *scriptdesc)
{
  const char *pkg_script = glnx_strjoina (pkg_name, ".", scriptdesc+1);
  const struct RpmOstreePackageScriptHandler *handler = rpmostree_script_gperf_lookup (pkg_script, strlen (pkg_script));
  if (!handler)
    return RPMOSTREE_SCRIPT_ACTION_DEFAULT;
  return handler->action;
}

gboolean
rpmostree_script_txn_validate (DnfPackage    *package,
                               Header         hdr,
                               GCancellable  *cancellable,
                               GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (unsupported_scripts); i++)
    {
      const char *desc = unsupported_scripts[i].desc;
      rpmTagVal tagval = unsupported_scripts[i].tag;
      rpmTagVal progtagval = unsupported_scripts[i].progtag;

      if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
        continue;

      RpmOstreeScriptAction action = lookup_script_action (dnf_package_get_name (package), desc);
      switch (action)
        {
        case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
          return glnx_throw (error, "Package '%s' has (currently) unsupported script of type '%s'",
                             dnf_package_get_name (package), desc);
        case RPMOSTREE_SCRIPT_ACTION_IGNORE:
          continue;
        }
    }

  return TRUE;
}

struct ChildSetupData {
  /* note fds are *not* owned */
  gboolean all_fds_initialized;
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
  int useradd_fd;
};

static void
script_child_setup (gpointer opaque)
{
  struct ChildSetupData *data = opaque;

  /* make it really obvious for new users that we expect all fds to be initialized or -1 */
  if (!data || !data->all_fds_initialized)
    return;

  if (data->stdin_fd >= 0 && dup2 (data->stdin_fd, STDIN_FILENO) < 0)
    err (1, "dup2(stdin)");
  if (data->stdout_fd >= 0 && dup2 (data->stdout_fd, STDOUT_FILENO) < 0)
    err (1, "dup2(stdout)");
  if (data->stderr_fd >= 0 && dup2 (data->stderr_fd, STDERR_FILENO) < 0)
    err (1, "dup2(stderr)");
  if (data->useradd_fd >= 0 && fcntl (data->useradd_fd, F_SETFD, 0) < 0)
    err (1, "fcntl(useradd_fd)");
}

/* Print the output of a script, with each line prefixed with
 * the script identifier (e.g. foo.post: bla bla bla).
 */
static gboolean
dump_buffered_output (const char *prefix,
                      GLnxTmpfile *tmpf,
                      GError     **error)
{
  /* The tmpf won't be initialized in the journal case */
  if (!tmpf->initialized)
    return TRUE;
  if (lseek (tmpf->fd, 0, SEEK_SET) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  g_autoptr(FILE) buf = fdopen (tmpf->fd, "r");
  if (!buf)
    return glnx_throw_errno_prefix (error, "fdopen");
  tmpf->fd = -1;  /* Ownership of fd was transferred */

  while (TRUE)
    {
      size_t len = 0;
      ssize_t bytes_read = 0;
      g_autofree char *line = NULL;
      errno = 0;
      if ((bytes_read = getline (&line, &len, buf)) == -1)
        {
          if (errno != 0)
            return glnx_throw_errno_prefix (error, "getline");
          else
            break;
        }
      printf ("%s: %s", prefix, line);
      if (bytes_read > 0 && line[bytes_read-1] != '\n')
        fputc ('\n', stdout);
    }

  return TRUE;
}

/* Since it doesn't make sense to fatally error if printing output fails, catch
 * any errors there and print.
 */
static void
dump_buffered_output_noerr (const char *prefix,
                            GLnxTmpfile *tmpf)
{
  g_autoptr(GError) local_error = NULL;
  if (!dump_buffered_output (prefix, tmpf, &local_error))
    g_printerr ("While writing output: %s\n", local_error->message);
}

/* Lowest level script handler in this file; create a bwrap instance and run it
 * synchronously.
 */
static gboolean
run_script_in_bwrap_container (int rootfs_fd,
                               GLnxTmpDir *var_lib_rpm_statedir,
                               gboolean    enable_fuse,
                               const char *name,
                               const char *scriptdesc,
                               const char *interp,
                               const char *script,
                               const char *script_arg,
                               int         stdin_fd,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  const char *pkg_script = glnx_strjoina (name, ".", scriptdesc+1);
  const char *postscript_name = glnx_strjoina ("/", pkg_script);
  const char *postscript_path_container = glnx_strjoina ("/usr", postscript_name);
  const char *postscript_path_host = postscript_path_container + 1;
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;
  g_auto(GLnxTmpfile) useradd_tmpf = {0, };
  gboolean created_var_lib_rpmstate = FALSE;
  gboolean created_run_ostree_booted = FALSE;
  glnx_autofd int stdout_fd = -1;
  glnx_autofd int stderr_fd = -1;
  struct stat stbuf;
  glnx_autofd int useradd_pipe_fd = -1;

  /* TODO - Create a pipe and send this to bwrap so it's inside the
   * tmpfs.  Note the +1 on the path to skip the leading /.
   */
  if (!glnx_file_replace_contents_at (rootfs_fd, postscript_path_host,
                                      (guint8*)script, -1,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      NULL, error))
    {
      g_prefix_error (error, "Writing script to %s: ", postscript_path_host);
      goto out;
    }

  /* And similarly for /var/lib/rpm-state */
  if (var_lib_rpm_statedir)
    {
      if (mkdirat (rootfs_fd, "var/lib/rpm-state", 0755) < 0)
        {
          if (errno == EEXIST)
            ;
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else
        created_var_lib_rpmstate = TRUE;
    }

  /* ⚠⚠⚠ If you change this, also update scripts/bwrap-script-shell.sh ⚠⚠⚠ */

  /* We just did a ro bind mount over /var above. However we want a writable
   * var/tmp, so we need to tmpfs mount on top of it. See also
   * https://github.com/projectatomic/bubblewrap/issues/182
   * Similarly for /var/lib/rpm-state.
   *
   * See above for why we special case glibc.
   */
  const gboolean is_glibc_locales = strcmp (pkg_script, "glibc-all-langpacks.posttrans") == 0 ||
    strcmp (pkg_script, "glibc-common.post") == 0;
  RpmOstreeBwrapMutability mutability =
    (is_glibc_locales || !enable_fuse) ? RPMOSTREE_BWRAP_MUTATE_FREELY : RPMOSTREE_BWRAP_MUTATE_ROFILES;
  bwrap = rpmostree_bwrap_new (rootfs_fd,
                               mutability,
                               error);
  if (!bwrap)
    goto out;
  /* Scripts can see a /var with compat links like alternatives */
  rpmostree_bwrap_var_tmp_tmpfs (bwrap);

  if (glnx_fstatat (rootfs_fd, "usr/lib/opt", &stbuf, AT_SYMLINK_NOFOLLOW, NULL) && S_ISDIR(stbuf.st_mode))
    rpmostree_bwrap_append_bwrap_argv (bwrap, "--symlink", "usr/lib/opt", "/opt", NULL);

  /* Add ostree-booted API; some scriptlets may work differently on OSTree systems; e.g.
   * akmods. Just create it manually; /run is usually tmpfs, but scriptlets shouldn't be
   * adding stuff there anyway. */
  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "run", 0755, cancellable, error))
    return FALSE;
  rpmostree_bwrap_bind_readwrite (bwrap, "./run", "/run");
  int fd =
    openat (rootfs_fd, "run/ostree-booted", O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC, 0640);
  if (fd == -1)
    return glnx_throw_errno_prefix (error, "touch(run/ostree-booted)");
  (void) close (fd);
  created_run_ostree_booted = TRUE;

  if (var_lib_rpm_statedir)
    rpmostree_bwrap_bind_readwrite (bwrap, var_lib_rpm_statedir->path, "/var/lib/rpm-state");

  const gboolean debugging_script = g_strcmp0 (g_getenv ("RPMOSTREE_SCRIPT_DEBUG"), pkg_script) == 0;

  /* https://github.com/systemd/systemd/pull/7631 AKA
   * "systemctl,verbs: Introduce SYSTEMD_OFFLINE environment variable"
   * https://github.com/systemd/systemd/commit/f38951a62837a00a0b1ff42d007e9396b347742d
   */
  rpmostree_bwrap_setenv (bwrap, "SYSTEMD_OFFLINE", "1");

  GLnxTmpfile buffered_output = { 0, };
  const char *id = glnx_strjoina ("rpm-ostree(", pkg_script, ")");
  if (debugging_script)
    {
      rpmostree_bwrap_append_child_argv (bwrap, "/usr/bin/bash", NULL);
      rpmostree_bwrap_set_inherit_stdin (bwrap);
    }
  else
    {
      struct ChildSetupData data = { .stdin_fd = stdin_fd,
                                     .stdout_fd = -1,
                                     .stderr_fd = -1,
                                     .useradd_fd = -1 };

      /* Only try to log to the journal if we're already set up that way (normally
       * rpm-ostreed for host system management). Otherwise we might be in a Docker
       * container, or directly on a host system being executed unprivileged
       * via `ex container`, and in these cases we want to output to stdout, which
       * is where other output will go.
       */
      if (rpmostree_stdout_is_journal ())
        {
          data.stdout_fd = stdout_fd = sd_journal_stream_fd (id, LOG_INFO, 0);
          if (stdout_fd < 0)
            {
              glnx_throw_errno_prefix (error, "While creating stdout stream fd");
              goto out;
            }

          data.stderr_fd = stderr_fd = sd_journal_stream_fd (id, LOG_ERR, 0);
          if (stderr_fd < 0)
            {
              glnx_throw_errno_prefix (error, "While creating stderr stream fd");
              goto out;
            }
        }
      else
        {
          /* In the non-journal case we buffer so we can prefix output */
          if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &buffered_output, error))
            return FALSE;
          data.stdout_fd = data.stderr_fd = buffered_output.fd;
        }

      /* Special API to pass useradd/groupadd invocations back up to the parent
       * outside the root.
       */
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &useradd_tmpf, error))
        return FALSE;
      data.useradd_fd = useradd_tmpf.fd;
      { g_autofree char *useradd_env = g_strdup_printf ("%d", data.useradd_fd);
        rpmostree_bwrap_setenv (bwrap, "RPMOSTREE_USERADD_FD", useradd_env);
      }

      data.all_fds_initialized = TRUE;
      rpmostree_bwrap_set_child_setup (bwrap, script_child_setup, &data);

      const char *script_trace = g_getenv ("RPMOSTREE_SCRIPT_TRACE");
      if (script_trace)
        {
          int trace_argc = 0;
          char **trace_argv = NULL;
          if (!g_shell_parse_argv (script_trace, &trace_argc, &trace_argv, error))
            return glnx_prefix_error (error, "Parsing '%s'", script_trace);
          rpmostree_bwrap_append_child_argva (bwrap, trace_argc, trace_argv);
          g_strfreev (trace_argv);
        }

      rpmostree_bwrap_append_child_argv (bwrap,
                                         interp,
                                         postscript_path_container,
                                         script_arg,
                                         NULL);
    }


  if (!rpmostree_bwrap_run (bwrap, cancellable, error))
    {
      dump_buffered_output_noerr (pkg_script, &buffered_output);
      /* If errors go to the journal, help the user/admin find them there */
      if (error && rpmostree_stdout_is_journal ())
        {
          g_assert (*error);
          g_autofree char *errmsg = (*error)->message;
          (*error)->message =
            g_strdup_printf ("%s; run `journalctl -t '%s'` for more information", errmsg, id);
        }
      goto out;
    }
  else
    dump_buffered_output_noerr (pkg_script, &buffered_output);

  {
    // Transfer ownership
    int fd = useradd_tmpf.fd;
    useradd_tmpf.initialized = FALSE;
    if (!ror_sysusers_process_useradd_invocations (rootfs_fd, fd, error))
      return FALSE;
  }

  ret = TRUE;
 out:
  glnx_tmpfile_clear (&buffered_output);
  (void) unlinkat (rootfs_fd, postscript_path_host, 0);
  if (created_var_lib_rpmstate)
    (void) unlinkat (rootfs_fd, "var/lib/rpm-state", AT_REMOVEDIR);
  if (created_run_ostree_booted)
    (void) unlinkat (rootfs_fd, "run/ostree-booted", 0);
  return ret;
}

/* Medium level script entrypoint; we already validated it exists and isn't
 * ignored. Here we mostly compute arguments/input, then proceed into the lower
 * level bwrap execution.
 */
static gboolean
impl_run_rpm_script (const KnownRpmScriptKind *rpmscript,
                     DnfPackage    *pkg,
                     Header         hdr,
                     int            rootfs_fd,
                     GLnxTmpDir    *var_lib_rpm_statedir,
                     gboolean       enable_fuse,
                     GCancellable  *cancellable,
                     GError       **error)
{
  struct rpmtd_s td;
  g_autofree char **args = NULL;
  if (headerGet (hdr, rpmscript->progtag, &td, (HEADERGET_ALLOC|HEADERGET_ARGV)))
    args = td.data;

  const rpmFlags flags = headerGetNumber (hdr, rpmscript->flagtag);
  const char *script;
  const char *interp = (args && args[0]) ? args[0] : "/bin/sh";
  const char *pkg_scriptid = glnx_strjoina (dnf_package_get_name (pkg), ".", rpmscript->desc + 1);
  gboolean expand = (flags & RPMSCRIPT_FLAG_EXPAND) > 0;
  if (g_str_equal (interp, lua_builtin))
    {
      /* This is a lua script; look for a built-in override/replacement */
      gboolean found_replacement = FALSE;
      for (guint i = 0; i < G_N_ELEMENTS (lua_replacements); i++)
        {
          const RpmOstreeLuaReplacement *repl = &lua_replacements[i];
          if (!g_str_equal (repl->pkgname_script, pkg_scriptid))
            continue;
          found_replacement = TRUE;
          interp = repl->interp;
          script = repl->replacement;
          break;
        }
      if (!found_replacement)
        {
          /* No override found, throw an error and return */
          g_assert (!fail_if_interp_is_lua (interp, dnf_package_get_name (pkg), rpmscript->desc, error));
          return FALSE;
        }

      /* Hack around RHEL7's glibc-locales, which uses rpm-expand in the Lua script */
      if (strcmp (pkg_scriptid, "glibc-common.post") == 0)
        expand = TRUE;
    }
  else
    {
      script = headerGetString (hdr, rpmscript->tag);

      for (guint i = 0; i < G_N_ELEMENTS (script_replacements); i++)
        {
          const RpmOstreeScriptReplacement *repl = &script_replacements[i];
          if (!g_str_equal (repl->pkgname_script, pkg_scriptid))
            continue;
          if (repl->release_suffix &&
              !g_str_has_suffix (dnf_package_get_release (pkg), repl->release_suffix))
            continue;
          /* Is this completely suppressing the script?  If so, we're done */
          if (!repl->interp)
            return TRUE;
          interp = repl->interp;
          script = repl->replacement;
          break;
        }
    }
  g_autofree char *script_owned = NULL;
  g_assert (script);
  if (expand)
    script = script_owned = rpmExpand (script, NULL);

  /* http://ftp.rpm.org/max-rpm/s1-rpm-inside-scripts.html#S2-RPM-INSIDE-ERASE-TIME-SCRIPTS */
  const char *script_arg = NULL;
  switch (dnf_package_get_action (pkg))
    {
      /* XXX: we're not running *un scripts for removals yet, though it'd look like:
         case DNF_STATE_ACTION_REMOVE:
         case DNF_STATE_ACTION_OBSOLETE:
         script_arg = obsoleted_by_update ? "1" : "0";
         break;
      */
    case DNF_STATE_ACTION_INSTALL:
      script_arg = "1";
      break;
    case DNF_STATE_ACTION_UPDATE:
      script_arg = "2";
      break;
    case DNF_STATE_ACTION_DOWNGRADE:
      script_arg = "2";
      break;
    default:
      /* we shouldn't have been asked to perform for any other kind of action */
      g_assert_not_reached ();
      break;
    }

  guint64 start_time_ms = g_get_monotonic_time () / 1000;
  if (!run_script_in_bwrap_container (rootfs_fd, var_lib_rpm_statedir, enable_fuse,
                                      dnf_package_get_name (pkg),
                                      rpmscript->desc, interp, script, script_arg,
                                      -1, cancellable, error))
    return glnx_prefix_error (error, "Running %s for %s", rpmscript->desc, dnf_package_get_name (pkg));
  guint64 end_time_ms = g_get_monotonic_time () / 1000;
  guint64 elapsed_ms = end_time_ms - start_time_ms;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PREPOST),
                   "MESSAGE=Executed %s for %s in %ums", rpmscript->desc, dnf_package_get_name (pkg), elapsed_ms,
                   "SCRIPT_TYPE=%s", rpmscript->desc,
                   "PKG=%s", dnf_package_get_name (pkg),
                   "EXEC_TIME_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                   NULL);

  return TRUE;
}

/* High level script entrypoint; check a package to see whether a script exists,
 * execute it if it exists (and it's not ignored).
 */
static gboolean
run_script (const KnownRpmScriptKind *rpmscript,
            DnfPackage               *pkg,
            Header                    hdr,
            int                       rootfs_fd,
            GLnxTmpDir               *var_lib_rpm_statedir,
            gboolean                  enable_fuse,
            gboolean                 *out_did_run,
            GCancellable             *cancellable,
            GError                  **error)
{
  rpmTagVal tagval = rpmscript->tag;
  rpmTagVal progtagval = rpmscript->progtag;

  *out_did_run = FALSE;

  if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
    return TRUE;

  const char *script = headerGetString (hdr, tagval);
  if (!script)
    return TRUE;

  const char *desc = rpmscript->desc;
  RpmOstreeScriptAction action = lookup_script_action (dnf_package_get_name (pkg), desc);
  switch (action)
    {
    case RPMOSTREE_SCRIPT_ACTION_IGNORE:
      return TRUE; /* Note early return */
    case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
      break; /* Continue below */
    }

  *out_did_run = TRUE;
  return impl_run_rpm_script (rpmscript, pkg, hdr, rootfs_fd, var_lib_rpm_statedir,
                              enable_fuse, cancellable, error);
}

static gboolean
write_filename (FILE *f, GString *prefix,
                GError **error)
{
  if (fwrite_unlocked (prefix->str, 1, prefix->len, f) != prefix->len)
    return glnx_throw_errno_prefix (error, "fwrite");
  if (fputc_unlocked ('\n', f) == EOF)
    return glnx_throw_errno_prefix (error, "fputc");
  return TRUE;
}

/* Used for %transfiletriggerin - basically an implementation of `find -type f` that
 * writes the filenames to a tmpfile.
 */
static gboolean
write_subdir (int dfd, const char *path,
              GString *prefix,
              FILE *f,
              guint *inout_n_matched,
              GCancellable *cancellable,
              GError **error)
{
  /* This is an inlined copy of ot_dfd_iter_init_allow_noent()...if more users
   * appear we should probably push to libglnx.
   */
  g_assert (*path != '/');
  glnx_autofd int target_dfd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (target_dfd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendirat");
      /* Not early return */
      return TRUE;
    }
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_take_fd (&target_dfd, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      const size_t origlen = prefix->len;
      g_string_append_c (prefix, '/');
      g_string_append (prefix, dent->d_name);
      if (dent->d_type == DT_DIR)
        {
          if (!write_subdir (dfd_iter.fd, dent->d_name, prefix, f,
                             inout_n_matched,
                             cancellable, error))
            return FALSE;
        }
      else
        {
          if (!write_filename (f, prefix, error))
            return FALSE;
          (*inout_n_matched)++;
        }
      g_string_truncate (prefix, origlen);
    }

  return TRUE;
}

/* Given file trigger @pattern (really a subdirectory), traverse the
 * filesystem @rootfs_fd and write all matches as file names to @f.  Used
 * for %transfiletriggerin.
 */
static gboolean
find_and_write_matching_files (int rootfs_fd, const char *pattern,
                               FILE *f,
                               guint *out_n_matches,
                               GCancellable *cancellable,
                               GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finding matches", error);

  /* Fontconfig in fedora has /usr/local; we don't support RPM
   * touching /usr/local.  While I'm here, proactively
   * require /usr as a prefix too.
   */
  if (g_str_has_prefix (pattern, "usr/local") ||
      !g_str_has_prefix (pattern, "usr/"))
    return TRUE;

  /* The printed buffer does have a leading / */
  g_autoptr(GString) buf = g_string_new ("/");
  g_string_append (buf, pattern);
  /* Strip trailing '/' in the mutable copy we have here */
  while (buf->len > 0 && buf->str[buf->len-1] == '/')
    g_string_truncate (buf, buf->len - 1);

  guint n_pattern_matches = 0;
  if (!write_subdir (rootfs_fd, pattern, buf, f, &n_pattern_matches,
                     cancellable, error))
    return glnx_prefix_error (error, "pattern '%s'", pattern);
  *out_n_matches += n_pattern_matches;

  return TRUE;
}

/* Execute a supported script.  Note that @cancellable
 * does not currently kill a running script subprocess.
 */
gboolean
rpmostree_script_run_sync (DnfPackage    *pkg,
                           Header         hdr,
                           RpmOstreeScriptKind kind,
                           int            rootfs_fd,
                           GLnxTmpDir    *var_lib_rpm_statedir,
                           gboolean       enable_fuse,
                           guint         *out_n_run,
                           GCancellable  *cancellable,
                           GError       **error)
{
  const KnownRpmScriptKind *scriptkind;
  switch (kind)
    {
    case RPMOSTREE_SCRIPT_PREIN:
      scriptkind = &pre_script;
      break;
    case RPMOSTREE_SCRIPT_POSTIN:
      scriptkind = &post_script;
      break;
    case RPMOSTREE_SCRIPT_POSTTRANS:
      scriptkind = &posttrans_script;
      break;
    default:
      g_assert_not_reached ();
    }

  gboolean did_run = FALSE;
  if (!run_script (scriptkind, pkg, hdr, rootfs_fd,
                   var_lib_rpm_statedir, enable_fuse,
                   &did_run, cancellable, error))
    return FALSE;

  if (did_run)
    (*out_n_run)++;
  return TRUE;
}

/* File triggers, as used by e.g. glib2.spec and vagrant.spec in Fedora. More
 * info at <http://rpm.org/user_doc/file_triggers.html>.
 */
gboolean
rpmostree_transfiletriggers_run_sync (Header        hdr,
                                      int           rootfs_fd,
                                      gboolean      enable_fuse,
                                      guint        *out_n_run,
                                      GCancellable *cancellable,
                                      GError      **error)
{
  const char *pkg_name = headerGetString (hdr, RPMTAG_NAME);
  g_assert (pkg_name);

  g_autofree char *error_prefix = g_strconcat ("Executing %transfiletriggerin for ", pkg_name, NULL);
  GLNX_AUTO_PREFIX_ERROR (error_prefix, error);

  RpmOstreeScriptAction action = lookup_script_action (pkg_name, "%transfiletriggerin");
  switch (action)
    {
    case RPMOSTREE_SCRIPT_ACTION_IGNORE:
      return TRUE; /* Note early return */
    case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
      break; /* Continue below */
    }

  headerGetFlags hgflags = HEADERGET_MINMEM;
  struct rpmtd_s tname, tscripts, tprogs, tflags, tscriptflags;
  struct rpmtd_s tindex;
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERNAME, &tname, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTS, &tscripts, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTPROG, &tprogs, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERFLAGS, &tflags, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTFLAGS, &tscriptflags, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERINDEX, &tindex, hgflags);
  g_debug ("pkg %s transtrigger count %u/%u/%u/%u/%u/%u\n",
           pkg_name, rpmtdCount (&tname), rpmtdCount (&tscripts),
           rpmtdCount (&tprogs), rpmtdCount (&tflags), rpmtdCount (&tscriptflags),
           rpmtdCount (&tindex));

  if (rpmtdCount (&tscripts) == 0)
    return TRUE;

  const guint n_scripts = rpmtdCount (&tscripts);
  const guint n_names = rpmtdCount (&tname);
  /* Given multiple matching patterns, RPM expands it into multiple copies.
   * The trigger index (AIUI) defines where to find the pattern (and flags) given a
   * script.
   *
   * Some librpm source references:
   *  - tagexts.c:triggercondsTagFor()
   *  - rpmscript.c:rpmScriptFromTriggerTag()
   */
  for (guint i = 0; i < n_scripts; i++)
    {
      rpmtdInit (&tname);
      rpmtdInit (&tflags);

      rpmFlags flags = 0;
      if (rpmtdSetIndex (&tscriptflags, i) >= 0)
        flags = rpmtdGetNumber (&tscriptflags);

      g_assert_cmpint (rpmtdSetIndex (&tprogs, i), ==, i);
      const char *interp = rpmtdGetString (&tprogs);
      if (!interp)
        interp = "/bin/sh";
      if (!fail_if_interp_is_lua (interp, pkg_name, "%transfiletriggerin", error))
        return FALSE;

      g_autofree char *script_owned = NULL;
      g_assert_cmpint (rpmtdSetIndex (&tscripts, i), ==, i);
      const char *script = rpmtdGetString (&tscripts);
      if (!script)
        continue;
      if (flags & RPMSCRIPT_FLAG_EXPAND)
        script = script_owned = rpmExpand (script, NULL);

      g_autoptr(GPtrArray) patterns = g_ptr_array_new_with_free_func (g_free);

      /* Iterate over the trigger "names" which are file patterns */
      for (guint j = 0; j < n_names; j++)
        {
          g_assert_cmpint (rpmtdSetIndex (&tindex, j), ==, j);
          guint32 tindex_num = *rpmtdGetUint32 (&tindex);

          if (tindex_num != i)
            continue;

          rpmFlags sense = 0;
          if (rpmtdSetIndex (&tflags, j) >= 0)
            sense = rpmtdGetNumber (&tflags);
          /* See if this is a triggerin (as opposed to triggerun, which we)
           * don't execute.
           */
          const gboolean trigger_in = (sense & RPMSENSE_TRIGGERIN) > 0;
          if (!trigger_in)
            continue;

          g_assert_cmpint (rpmtdSetIndex (&tname, j), ==, j);
          const char *pattern = rpmtdGetString (&tname);
          if (!pattern)
            continue;
          /* Skip leading `/`, since we use fd-relative access */
          pattern += strspn (pattern, "/");
          /* Silently ignore broken patterns for now */
          if (!*pattern)
            continue;

          g_ptr_array_add (patterns, g_strdup (pattern));
        }

      if (patterns->len == 0)
        continue;

      /* Build up the list of files matching the patterns. librpm uses a pipe and
       * doesn't do async writes, and hence is subject to deadlock. We could use
       * a pipe and do async, but an O_TMPFILE is easier for now. There
       * shouldn't be megabytes of data here, and the parallelism loss in
       * practice I think is going to be small.
       */
      g_auto(GLnxTmpfile) matching_files_tmpf = { 0, };
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &matching_files_tmpf, error))
        return FALSE;
      g_autoptr(FILE) tmpf_file = fdopen (matching_files_tmpf.fd, "w");
      if (!tmpf_file)
        return glnx_throw_errno_prefix (error, "fdopen");
      matching_files_tmpf.fd = -1; /* Transferred */

      g_autoptr(GString) patterns_joined = g_string_new ("");
      guint n_total_matched = 0;
      for (guint j = 0; j < patterns->len; j++)
        {
          guint n_matched = 0;
          const char *pattern = patterns->pdata[j];
          if (j > 0)
            g_string_append (patterns_joined, ", ");
          g_string_append (patterns_joined, pattern);
          if (!find_and_write_matching_files (rootfs_fd, pattern, tmpf_file, &n_matched,
                                              cancellable, error))
            return FALSE;
          if (n_matched == 0)
            {
              /* This is probably a bug...let's log it */
              sd_journal_print (LOG_INFO, "No files matched %%transfiletriggerin(%s) for %s", pattern, pkg_name);
            }
          n_total_matched += n_matched;
        }

      if (n_total_matched == 0)
        continue;

      if (!glnx_stdio_file_flush (tmpf_file, error))
        return FALSE;
      /* Now, point back to the beginning so the script reads it from the start
         as stdin */
      if (lseek (fileno (tmpf_file), 0, SEEK_SET) < 0)
        return glnx_throw_errno_prefix (error, "lseek");

      /* Run it, and log the result */
      guint64 start_time_ms = g_get_monotonic_time () / 1000;
      if (!run_script_in_bwrap_container (rootfs_fd, NULL, enable_fuse, pkg_name,
                                          "%transfiletriggerin", interp, script, NULL,
                                          fileno (tmpf_file), cancellable, error))
        return FALSE;
      guint64 end_time_ms = g_get_monotonic_time () / 1000;
      guint64 elapsed_ms = end_time_ms - start_time_ms;

      (*out_n_run)++;

      sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_FILETRIGGER),
                       "MESSAGE=Executed %%transfiletriggerin(%s) for %s in %ums; %u matched files",
                       pkg_name, patterns_joined->str, elapsed_ms, n_total_matched,
                       "SCRIPT_TYPE=%%transfiletriggerin"
                       "PKG=%s", pkg_name,
                       "PATTERNS=%s", patterns_joined->str,
                       "TRIGGER_N_MATCHES=%u", n_total_matched,
                       "EXEC_TIME_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                       NULL);
    }
  return TRUE;
}

/* Ensure that we can at least execute /usr/bin/true inside the new root.
 * See https://github.com/projectatomic/rpm-ostree/pull/888
 *
 * Currently at least on Fedora this will run through e.g. the dynamic linker
 * and hence some bits of glibc.
 *
 * We could consider doing more here, perhaps even starting systemd in a
 * volatile mode, but that could just as easily be a separate tool.
 */
gboolean
rpmostree_deployment_sanitycheck_true (int           rootfs_fd,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  /* Used by the test suite */
  if (getenv ("RPMOSTREE_SKIP_SANITYCHECK"))
    return TRUE;

  GLNX_AUTO_PREFIX_ERROR ("Sanity-checking final rootfs", error);
  g_autoptr(RpmOstreeBwrap) bwrap =
    rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_IMMUTABLE, error);

  if (!bwrap)
    return FALSE;
  rpmostree_bwrap_append_child_argv (bwrap, "/usr/bin/true", NULL);
  if (!rpmostree_bwrap_run (bwrap, cancellable, error))
    return FALSE;

  sd_journal_print (LOG_INFO, "sanitycheck(/usr/bin/true) successful");
  return TRUE;
}

static gboolean
verify_packages_in_sack (DnfSack      *sack,
                         GPtrArray    *pkgs,
                         GError      **error)
{
  if (!pkgs || pkgs->len == 0)
    return TRUE;

  for (guint i = 0; i < pkgs->len; i++)
    {
      DnfPackage *pkg = pkgs->pdata[i];
      const char *nevra = dnf_package_get_nevra (pkg);
      if (!rpmostree_sack_has_subject (sack, nevra))
        return glnx_throw (error, "Didn't find package '%s'", nevra);
    }

  return TRUE;
}

/* Check that we can load the rpmdb. See
 * https://github.com/projectatomic/rpm-ostree/issues/1566.
 *
 * This is split out of the one above for practical reasons: the check above runs right
 * after scripts are executed to give a nicer error if the scripts did `rm -rf`.
 */
gboolean
rpmostree_deployment_sanitycheck_rpmdb (int           rootfs_fd,
                                        /* just allow two args to avoid allocating */
                                        GPtrArray     *overlays,
                                        GPtrArray     *overrides,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Sanity-checking final rpmdb", error);

  g_autoptr(RpmOstreeRefSack) sack = rpmostree_get_refsack_for_root (rootfs_fd, ".", error);
  if (!sack)
    return FALSE;

  if ((overlays && overlays->len > 0) || (overrides && overrides->len > 0))
    {
      if (!verify_packages_in_sack (sack->sack, overlays, error) ||
          !verify_packages_in_sack (sack->sack, overrides, error))
        return FALSE;
    }
  else
    {
      /* OK, let's just sanity check that there are *some* packages in the rpmdb */
      hy_autoquery HyQuery query = hy_query_create (sack->sack);
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      g_autoptr(GPtrArray) pkgs = hy_query_run (query);
      if (pkgs->len == 0)
        return glnx_throw (error, "No packages found in rpmdb!");
    }

  sd_journal_print (LOG_INFO, "sanitycheck(rpmdb) successful");
  return TRUE;
}
