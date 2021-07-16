/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gio/gio.h>
#include <glib-unix.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <exception>

#include "rpmostree-util.h"
#include "rpmostree-builtins.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-polkit-agent.h"
#include "rpmostreemain.h"

#include "libglnx.h"

static RpmOstreeCommand commands[] = {
  { "compose", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
               RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT),
    "Commands to compose a tree",
    rpmostree_builtin_compose },
  { "cleanup", static_cast<RpmOstreeBuiltinFlags>(0),
    "Clear cached/pending data",
    rpmostree_builtin_cleanup },
  { "db", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD),
    "Commands to query the RPM database",
    rpmostree_builtin_db },
  { "deploy", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS),
    "Deploy a specific commit",
    rpmostree_builtin_deploy },
  { "rebase", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS),
    "Switch to a different tree",
    rpmostree_builtin_rebase },
  { "rollback", static_cast<RpmOstreeBuiltinFlags>(0),
    "Revert to the previously booted tree",
    rpmostree_builtin_rollback },
  { "status", static_cast<RpmOstreeBuiltinFlags>(0),
    "Get the version of the booted system",
    rpmostree_builtin_status },
  { "upgrade", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS),
    "Perform a system upgrade",
    rpmostree_builtin_upgrade },
  { "update", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS | RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    "Alias for upgrade",
    rpmostree_builtin_upgrade },
  { "reload", static_cast<RpmOstreeBuiltinFlags>(0),
    "Reload configuration",
    rpmostree_builtin_reload },
  { "usroverlay", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT),
    "Apply a transient overlayfs to /usr",
    rpmostree_builtin_usroverlay },
  /* Let's be "cognitively" compatible with `ostree admin unlock` */
  { "unlock", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL,
    rpmostree_builtin_usroverlay },
  { "cancel", static_cast<RpmOstreeBuiltinFlags>(0),
    "Cancel an active transaction",
    rpmostree_builtin_cancel },
  { "initramfs", static_cast<RpmOstreeBuiltinFlags>(0),
    "Enable or disable local initramfs regeneration",
    rpmostree_builtin_initramfs },
  { "install", static_cast<RpmOstreeBuiltinFlags>(0),
    "Overlay additional packages",
    rpmostree_builtin_install },
  { "uninstall", static_cast<RpmOstreeBuiltinFlags>(0),
    "Remove overlayed additional packages",
    rpmostree_builtin_uninstall },
  { "override", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD),
    "Manage base package overrides", rpmostree_builtin_override },
  { "reset", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS),
    "Remove all mutations",
    rpmostree_builtin_reset },
  { "refresh-md", static_cast<RpmOstreeBuiltinFlags>(0),
    "Generate rpm repo metadata",
    rpmostree_builtin_refresh_md },
  { "kargs", static_cast<RpmOstreeBuiltinFlags>(0),
    "Query or modify kernel arguments",
    rpmostree_builtin_kargs },
  /* Rust-implemented commands; they're here so that they show up in `rpm-ostree
   * --help` alongside the other commands, but the command itself is fully
   *  handled Rust side. */
  /*
  { "my-rust-command", static_cast<RpmOstreeBuiltinFlags>(0),
    "Cool thing my command does", NULL },
  */
  /* Legacy aliases */
  { "pkg-add", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_install },
  { "pkg-remove", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_uninstall },
  { "rpm", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
           RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_db },
  /* Compat with dnf */
  { "remove", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_uninstall },
  { "makecache", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_refresh_md },
  /* Hidden */
  { "ex", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
          RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    "Experimental commands that may change or be removed in the future",
    rpmostree_builtin_ex },
  { "testutils", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
                 RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_testutils },
  { "shlib-backend", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
                 RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_shlib_backend },
  { "start-daemon", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
                    RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
                    RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_start_daemon },
  { "finalize-deployment", static_cast<RpmOstreeBuiltinFlags>(RPM_OSTREE_BUILTIN_FLAG_HIDDEN),
    NULL, rpmostree_builtin_finalize_deployment },
  { NULL }
};

static gboolean opt_version;
static gboolean opt_force_peer;
static char *opt_sysroot;
static gchar **opt_install;
static gchar **opt_uninstall;

static GOptionEntry global_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry daemon_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer-to-peer connection instead of using the system message bus", NULL },
  { NULL }
};

static GOptionEntry pkg_entries[] = {
  { "install", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_install, "Overlay additional package", "PKG" },
  { "uninstall", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_uninstall, "Remove overlayed additional package", "PKG" },
  { NULL }
};

static GOptionContext *
option_context_new_with_commands (RpmOstreeCommandInvocation *invocation,
                                  RpmOstreeCommand *commands)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("COMMAND");
  g_autoptr(GString) summary = g_string_new (NULL);

  if (invocation)
    {
      if (invocation->command->description != NULL)
        g_string_append_printf (summary, "%s\n\n",
                                invocation->command->description);

      g_string_append_printf (summary, "Builtin \"%s\" Commands:",
                              invocation->command->name);
    }
  else /* top level */
    g_string_append (summary, "Builtin Commands:");

  for (RpmOstreeCommand *command = commands; command->name != NULL; command++)
    {
      gboolean hidden = (command->flags & RPM_OSTREE_BUILTIN_FLAG_HIDDEN) > 0;
      if (!hidden)
        {
          g_string_append_printf (summary, "\n  %-17s", command->name);
          if (command->description != NULL)
            g_string_append_printf (summary, "%s", command->description);
        }
    }

  g_option_context_set_summary (context, summary->str);
  return util::move_nullify (context);
}

namespace rpmostreecxx {

void 
client_require_root(void)
{
  if (getuid () != 0 && getenv ("RPMOSTREE_SUPPRESS_REQUIRES_ROOT_CHECK") == NULL)
    throw std::runtime_error("This command requires root privileges");
}

} /* namespace */

gboolean
rpmostree_option_context_parse (GOptionContext *context,
                                const GOptionEntry *main_entries,
                                int *argc,
                                char ***argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable *cancellable,
                                const char *const* *out_install_pkgs,
                                const char *const* *out_uninstall_pkgs,
                                RPMOSTreeSysroot **out_sysroot_proxy,
                                GError **error)
{
  /* with --version there's no command, don't require a daemon for it */
  const RpmOstreeBuiltinFlags flags =
    invocation ? invocation->command->flags : RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD;
  gboolean use_daemon = ((flags & RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD) == 0);

  if (invocation && invocation->command->description != NULL)
    {
      /* The extra summary explanation is only provided for commands with description */
      const char* context_summary = g_option_context_get_summary (context);

      /* check whether the summary has been set earlier */
      if (context_summary == NULL)
       g_option_context_set_summary (context, invocation->command->description);
    }

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  if (use_daemon)
    g_option_context_add_main_entries (context, daemon_entries, NULL);

  if ((flags & RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS) > 0)
    g_option_context_add_main_entries (context, pkg_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  if (opt_version)
    {
      /* This should now be YAML, like `docker version`, so it's both nice to read
       * possible to parse.  The canonical implementation of this is in
       * ostree/ot-main.c.
       */
      g_auto(GStrv) features = g_strsplit (RPM_OSTREE_FEATURES, " ", -1);
      g_print ("%s:\n", PACKAGE_NAME);
      g_print (" Version: '%s'\n", PACKAGE_VERSION);
      if (strlen (RPM_OSTREE_GITREV) > 0)
        g_print (" Git: %s\n", RPM_OSTREE_GITREV);
      g_print (" Features:\n");
      for (char **iter = features; iter && *iter; iter++)
        g_print ("  - %s\n", *iter);
      auto featuresrs = rpmostreecxx::get_features();
      for (auto & s : featuresrs)
        {
          g_print ("  - %s\n", s.c_str());
        }
      exit (EXIT_SUCCESS);
    }

  if ((flags & RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT) > 0)
    rpmostreecxx::client_require_root();

  if (use_daemon)
    {
      /* More gracefully handle the case where
       * no --sysroot option was specified and we're not booted via ostree
       * https://github.com/projectatomic/rpm-ostree/issues/1537
       */
      if (!opt_sysroot)
        {
          if (!glnx_fstatat_allow_noent (AT_FDCWD, "/run/ostree-booted", NULL, 0, error))
            return FALSE;
          if (errno == ENOENT)
            {
              const char *containerenv = getenv ("container");
              if (containerenv != NULL)
                return glnx_throw (error, "This system was not booted via libostree; found $container=%s environment variable.\nrpm-ostree is designed to manage host systems, not containers.\n", containerenv);
              else
                return glnx_throw (error, "This system was not booted via libostree; cannot operate");
            }
        }

      /* root never needs to auth */
      if (getuid () != 0)
        /* ignore errors; we print out a warning if we fail to spawn pkttyagent */
        (void)rpmostree_polkit_agent_open ();

      if (!rpmostree_load_sysroot (opt_sysroot, cancellable,
                                   out_sysroot_proxy,
                                   error))
        return FALSE;
    }

  if (out_install_pkgs)
    *out_install_pkgs = (const char *const*)opt_install;
  if (out_uninstall_pkgs)
    *out_uninstall_pkgs = (const char *const*)opt_uninstall;

  return TRUE;
}

static RpmOstreeCommand *
lookup_command (const char *name)
{
  RpmOstreeCommand *command = commands;

  while (command->name)
    {
      if (g_strcmp0 (name, command->name) == 0)
        return command;
      command++;
    }
  return NULL;
}

static const char *
rpmostree_subcommand_parse (int *inout_argc,
                            char **inout_argv)
{
  const int argc = *inout_argc;
  const char *command_name = NULL;
  int in, out;

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (inout_argv[in][0] != '-')
        {
          if (command_name == NULL)
            {
              command_name = inout_argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (inout_argv[in], "--"))
        {
          break;
        }

      inout_argv[out] = inout_argv[in];
    }

  *inout_argc = out;
  return command_name;
}

gboolean
rpmostree_handle_subcommand (int argc, char **argv,
                             RpmOstreeCommand *subcommands,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  const char *subcommand_name =
    rpmostree_subcommand_parse (&argc, argv);

  RpmOstreeCommand *subcommand = subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_autoptr(GOptionContext) context =
        option_context_new_with_commands (invocation, subcommands);

      /* This will not return for some options (e.g. --version). */
      (void) rpmostree_option_context_parse (context, NULL,
                                             &argc, &argv,
                                             invocation,
                                             cancellable,
                                             NULL, NULL, NULL, NULL);
      if (subcommand_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No \"%s\" subcommand specified",
                       invocation->command->name);
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unknown \"%s\" subcommand \"%s\"",
                       invocation->command->name,
                       subcommand_name);
        }

      g_autofree char *help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      return FALSE;
    }

  g_autofree char *prgname =
    g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  /* We need a new sub-invocation with the new command, which also carries a new
   * exit code, but we'll proxy the latter. */
  RpmOstreeCommandInvocation sub_invocation = { .command = subcommand,
                                                .command_line = invocation->command_line,
                                                .exit_code = -1 };
  gboolean ret = subcommand->fn (argc, argv, &sub_invocation, cancellable, error);
  /* Proxy the exit code */
  invocation->exit_code = sub_invocation.exit_code;
  return ret;
}

static char*
rebuild_command_line (int    argc,
                      char **argv)
{
  /* be nice and quote args as needed instead of just g_strjoinv() */
  g_autoptr(GString) command = g_string_new (NULL);
  for (int i = 1; i < argc; i++)
    {
      auto quoted = rpmostreecxx::maybe_shell_quote (argv[i]);
      g_string_append (command, quoted.c_str());
      g_string_append_c (command, ' ');
    }
  return g_string_free (util::move_nullify (command), FALSE);
}

/* See comment in main.cxx */
namespace rpmostreecxx {

/* Initialize process global state, used by both Rust and C++ */
void
early_main (void)
{
  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  /* There's not really a "root dconf" right now; otherwise we might
   * try to spawn one via GSocketClient → GProxyResolver → GSettings.
   *
   * https://github.com/projectatomic/rpm-ostree/pull/312
   * https://bugzilla.gnome.org/show_bug.cgi?id=767183
   **/
  if (getuid () == 0)
    g_assert (g_setenv ("GSETTINGS_BACKEND", "memory", TRUE));

  setlocale (LC_ALL, "");

  /* We don't support /etc/dnf/dnf.conf, so tell libdnf to not look for it. The function
   * name here is misleading; it's not attached to a `DnfContext` object, but instead
   * controls a global var. And it's not just the `DnfContext` that uses it, but e.g.
   * `DnfSack` and Repo too. So just do this upfront. XXX: Clean up that API so it's always
   * attached to a context object. */
  dnf_context_set_config_file_path("");
}

static int
rpmostree_main_inner (const rust::Slice<const rust::Str> args)
{
  auto argv0 = std::string(args[0]);
  g_set_prgname (argv0.c_str());

  GCancellable *cancellable = g_cancellable_new ();

  int argc = args.size();
  /* For now we intentionally leak these because the command parsing
   * code can end up reordering the array which will totally break
   * us calling free().  The right solution is to replace this with Rust.
   */
  g_autoptr(GPtrArray) argv_p = g_ptr_array_new ();
  for (const auto & arg: args) {
    g_ptr_array_add (argv_p, g_strndup (arg.data(), arg.size()));
  }
  char **argv = (char**)argv_p->pdata;

  g_autofree char *command_line = rebuild_command_line (argc, argv);

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */
  const char *command_name = rpmostree_subcommand_parse (&argc, argv);

  RpmOstreeCommand *command = lookup_command (command_name);
  if (!command)
    {
      g_autoptr(GOptionContext) context =
        option_context_new_with_commands (NULL, commands);
      /* This will not return for some options (e.g. --version). */
      (void) rpmostree_option_context_parse (context, NULL, &argc, &argv,
                                             NULL, NULL, NULL, NULL, NULL,
                                             NULL);
      g_autofree char *help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      if (command_name == NULL)
        throw std::runtime_error ("No command specified");
      else
        throw std::runtime_error (std::string("Unknown command '") + command_name + "'");
    }

  g_autofree char *prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  RpmOstreeCommandInvocation invocation = 
    { .command = command,
      .command_line = command_line,
      .exit_code = -1 };
  /* Note this may also throw a C++ exception, which will
   * be caught by a higher level.
   */
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  if (!command->fn (argc, argv, &invocation, cancellable, error))
    {
      if (invocation.exit_code == -1)
        invocation.exit_code = EXIT_FAILURE;
      g_assert (error && *error);
      g_dbus_error_strip_remote_error (local_error);
      util::throw_gerror (local_error);
    }
  else
    {
      if (invocation.exit_code == -1)
        return EXIT_SUCCESS;
      else
        return invocation.exit_code;
    }
}

/* Never returns */
void
rpmostree_main (const rust::Slice<const rust::Str> args)
{
  int r = EXIT_FAILURE;
  try {
    r = rpmostree_main_inner (args);
  } catch (std::exception& e) {
    auto msg = std::string(e.what());
    main_print_error(msg);
  }
  
  /* Teardown any important process global state here */
  rpmostree_polkit_agent_close ();

  exit (r);
}

void
main_print_error (rust::Str msg)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  g_printerr ("%serror: %s%.*s\n", prefix, suffix, (int)msg.length(), msg.data());
}

} /* namespace */
