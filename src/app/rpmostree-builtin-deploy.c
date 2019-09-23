/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_preview;
static gboolean opt_cache_only;
static gboolean opt_download_only;
static gboolean opt_lock_finalization;
static gboolean opt_disallow_downgrade;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  /* XXX As much as I dislike the inconsistency with "rpm-ostree upgrade",
   *     calling this option --check-diff doesn't really make sense here.
   *     A --preview option would work for both commands if we wanted to
   *     deprecate --check-diff. */
  { "preview", 0, 0, G_OPTION_ARG_NONE, &opt_preview, "Just preview package differences", NULL },
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Do not download latest ostree and RPM data", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Just download latest ostree and RPM data, don't deploy", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { "disallow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_disallow_downgrade, "Forbid deployment of chronologically older trees", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_deploy (int            argc,
                          char         **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable  *cancellable,
                          GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  const char * const packages[] = { NULL };
  const char *revision;
  _cleanup_peer_ GPid peer_pid = 0;
  const char *const *install_pkgs = NULL;
  const char *const *uninstall_pkgs = NULL;

  context = g_option_context_new ("REVISION");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "REVISION must be specified", error);
      return FALSE;
    }

  if (opt_preview && (install_pkgs != NULL || uninstall_pkgs != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --preview and --install/--uninstall");
      return FALSE;
    }

  revision = argv[1];

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  g_autoptr(RpmOstreeDeployment) starting_default_deployment =
    rpmostree_get_default_deployment (sysroot_proxy, cancellable, error);
  if (!starting_default_deployment)
    return FALSE;

  if (opt_preview)
    {
      if (!rpmostree_os_call_download_deploy_rpm_diff_sync (os_proxy,
                                                            revision,
                                                            packages,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        return FALSE;
    }
  else
    {
      GVariantDict dict;
      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
      g_variant_dict_insert (&dict, "allow-downgrade", "b", !opt_disallow_downgrade);
      g_variant_dict_insert (&dict, "cache-only", "b", opt_cache_only);
      g_variant_dict_insert (&dict, "download-only", "b", opt_download_only);
      g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
      g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
      g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

      /* Use newer D-Bus API only if we have to so we maintain coverage. */
      if (install_pkgs || uninstall_pkgs)
        {
          if (!rpmostree_update_deployment (os_proxy,
                                            NULL, /* refspec */
                                            revision,
                                            install_pkgs,
                                            uninstall_pkgs,
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
          if (!rpmostree_os_call_deploy_sync (os_proxy,
                                              revision,
                                              options,
                                              NULL,
                                              &transaction_address,
                                              NULL,
                                              cancellable,
                                              error))
            return FALSE;
        }
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  if (opt_preview)
    {
      g_autoptr(GVariant) result = NULL;
      g_autoptr(GVariant) details = NULL;

      if (!rpmostree_os_call_get_cached_deploy_rpm_diff_sync (os_proxy,
                                                              revision,
                                                              packages,
                                                              &result,
                                                              &details,
                                                              cancellable,
                                                              error))
        return FALSE;

      if (g_variant_n_children (result) == 0)
        {
          invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
          return TRUE;
        }

      rpmostree_print_package_diffs (result);
    }
  else if (!opt_reboot)
    {
      gboolean changed;
      if (!rpmostree_print_diff_from_deployment (starting_default_deployment, &changed,
                                                 cancellable, error))
        return FALSE;

      if (!changed)
        invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
      else
        g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}
