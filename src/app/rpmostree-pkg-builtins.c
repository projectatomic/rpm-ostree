/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

#include <gio/gunixfdlist.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_dry_run;
static gboolean opt_idempotent;
static gchar **opt_install;
static gchar **opt_uninstall;
static gboolean opt_cache_only;
static gboolean opt_download_only;
static gboolean opt_allow_inactive;
static gboolean opt_uninstall_all;
static gboolean opt_unchanged_exit_77;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction", NULL },
  { "allow-inactive", 0, 0, G_OPTION_ARG_NONE, &opt_allow_inactive, "Allow inactive package requests", NULL },
  { "idempotent", 0, 0, G_OPTION_ARG_NONE, &opt_idempotent, "Do nothing if package already (un)installed", NULL },
  { "unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_unchanged_exit_77, "If no overlays were changed, exit 77", NULL },
  { NULL }
};

static GOptionEntry uninstall_option_entry[] = {
  { "install", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_install, "Overlay additional package", "PKG" },
  { "all", 0, 0, G_OPTION_ARG_NONE, &opt_uninstall_all, "Remove all overlayed additional packages", NULL },
  { NULL }
};

static GOptionEntry install_option_entry[] = {
  { "uninstall", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_uninstall, "Remove overlayed additional package", "PKG" },
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Do not download latest ostree and RPM data", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Just download latest ostree and RPM data, don't deploy", NULL },
  { NULL }
};

static gboolean
pkg_change (RpmOstreeCommandInvocation *invocation,
            RPMOSTreeSysroot *sysroot_proxy,
            const char *const* packages_to_add,
            const char *const* packages_to_remove,
            GCancellable  *cancellable,
            GError       **error)
{
  const char *const strv_empty[] = { NULL };

  if (!packages_to_add)
    packages_to_add = strv_empty;
  if (!packages_to_remove)
    packages_to_remove = strv_empty;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  g_autoptr(RpmOstreeDeployment) starting_default_deployment =
    rpmostree_get_default_deployment (sysroot_proxy, cancellable, error);
  if (!starting_default_deployment)
    return FALSE;

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "cache-only", "b", opt_cache_only);
  g_variant_dict_insert (&dict, "download-only", "b", opt_download_only);
  g_variant_dict_insert (&dict, "no-pull-base", "b", TRUE);
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run);
  g_variant_dict_insert (&dict, "allow-inactive", "b", opt_allow_inactive);
  g_variant_dict_insert (&dict, "no-layering", "b", opt_uninstall_all);
  g_variant_dict_insert (&dict, "idempotent-layering", "b", opt_idempotent);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  gboolean met_local_pkg = FALSE;
  for (const char *const* it = packages_to_add; it && *it; it++)
    met_local_pkg = met_local_pkg || g_str_has_suffix (*it, ".rpm");

  /* Use newer D-Bus API only if we have to. */
  g_autofree char *transaction_address = NULL;
  if (met_local_pkg)
    {
      if (!rpmostree_update_deployment (os_proxy,
                                        NULL, /* refspec */
                                        NULL, /* revision */
                                        packages_to_add,
                                        packages_to_remove,
                                        NULL, /* override replace */
                                        NULL, /* override remove */
                                        NULL, /* override reset */
                                        NULL, /* local_repo_remote */
                                        options,
                                        &transaction_address,
                                        cancellable,
                                        error))
        return FALSE;
    }
  else
    {
      if (!rpmostree_os_call_pkg_change_sync (os_proxy,
                                              options,
                                              packages_to_add,
                                              packages_to_remove,
                                              NULL,
                                              &transaction_address,
                                              NULL,
                                              cancellable,
                                              error))
        return FALSE;
    }

  return rpmostree_transaction_client_run (invocation, sysroot_proxy, os_proxy,
                                           options, opt_unchanged_exit_77,
                                           transaction_address,
                                           starting_default_deployment,
                                           cancellable, error);
}

gboolean
rpmostree_builtin_install (int            argc,
                           char         **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable  *cancellable,
                           GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, install_option_entry, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return pkg_change (invocation, sysroot_proxy,
                     (const char *const*)argv,
                     (const char *const*)opt_uninstall,
                     cancellable, error);
}

gboolean
rpmostree_builtin_uninstall (int            argc,
                             char         **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable  *cancellable,
                             GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, uninstall_option_entry, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid, NULL,
                                       error))
    return FALSE;

  if (argc < 2 && !opt_uninstall_all)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  /* If we don't also have to install pkgs, perform uninstalls offline; users don't expect
   * the "auto-update" behaviour here. */
  if (!opt_install)
    opt_cache_only = TRUE;

  return pkg_change (invocation, sysroot_proxy,
                     (const char *const*)opt_install,
                     (const char *const*)argv,
                     cancellable, error);
}
