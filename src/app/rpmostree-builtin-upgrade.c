/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_preview;
static gboolean opt_check;
static gboolean opt_upgrade_unchanged_exit_77;
static gboolean opt_cache_only;
static gboolean opt_download_only;
static char *opt_automatic;
static gboolean opt_lock_finalization;

/* "check-diff" is deprecated, replaced by "preview" */
static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_preview, "Check for upgrades and print package diff only", NULL },
  { "preview", 0, 0, G_OPTION_ARG_NONE, &opt_preview, "Just preview package differences", NULL },
  { "check", 0, 0, G_OPTION_ARG_NONE, &opt_check, "Just check if an upgrade is available", NULL },
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Do not download latest ostree and RPM data", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Just download latest ostree and RPM data, don't deploy", NULL },
  { "upgrade-unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_upgrade_unchanged_exit_77, "If no upgrade is available, exit 77", NULL },
  { "trigger-automatic-update-policy", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_automatic, "For automated use only; triggered by automatic timer", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  _cleanup_peer_ GPid peer_pid = 0;
  const char *const *install_pkgs = NULL;
  const char *const *uninstall_pkgs = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       &peer_pid, NULL,
                                       error))
    return FALSE;

  if (opt_reboot && opt_preview)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --preview");
      return FALSE;
    }

  if (opt_reboot && opt_check)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --check");
      return FALSE;
    }

  if (opt_preview && (install_pkgs != NULL || uninstall_pkgs != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --preview and --install/--uninstall");
      return FALSE;
    }

  /* If both --check and --preview were passed, --preview overrides. */
  if (opt_preview)
    opt_check = FALSE;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  /* Print a notice if stage updates are enabled and the user
   * has requested an update manually - in that case we've
   * already doing basically all of `upgrade` here automatically in
   * the background.
   */
  if (!opt_automatic)
    {
      const char *policy = rpmostree_sysroot_get_automatic_update_policy (sysroot_proxy);
      if (policy && g_str_equal (policy, "stage"))
        g_print ("note: automatic updates (%s) are enabled\n", policy);
    }

  g_autoptr(RpmOstreeDeployment) starting_default_deployment =
    rpmostree_get_default_deployment (sysroot_proxy, cancellable, error);
  if (!starting_default_deployment)
    return FALSE;

  const gboolean check_or_preview = (opt_check || opt_preview);
  if (opt_automatic || check_or_preview)
    {
      GVariantDict dict;
      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert (&dict, "mode", "s", check_or_preview ? "check" : "auto");
      g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
      /* override default of TRUE if we're handling --check/--preview for backcompat,
       * or we're *are* handling --trigger-automatic-update-policy, but on a tty */
      if (check_or_preview || glnx_stdout_is_tty ())
        g_variant_dict_insert (&dict, "output-to-self", "b", FALSE);
      g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

      gboolean auto_updates_enabled;
      if (!rpmostree_os_call_automatic_update_trigger_sync (os_proxy,
                                                            options,
                                                            &auto_updates_enabled,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        return FALSE;

      if (!auto_updates_enabled)
        {
          /* print something for the benefit of the journal */
          g_print ("Automatic updates are not enabled; exiting...\n");
          return TRUE; /* Note early return */
        }
    }
  else
    {
      GVariantDict dict;
      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
      g_variant_dict_insert (&dict, "allow-downgrade", "b", opt_allow_downgrade);
      g_variant_dict_insert (&dict, "cache-only", "b", opt_cache_only);
      g_variant_dict_insert (&dict, "download-only", "b", opt_download_only);
      g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
      g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
      g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

      /* Use newer D-Bus API only if we have to. */
      if (install_pkgs || uninstall_pkgs)
        {
          if (!rpmostree_update_deployment (os_proxy,
                                            NULL, /* refspec */
                                            NULL, /* revision */
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
          if (!rpmostree_os_call_upgrade_sync (os_proxy,
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

  if (check_or_preview)
    {
      g_autoptr(GVariant) cached_update = NULL;
      if (rpmostree_os_get_has_cached_update_rpm_diff (os_proxy))
        cached_update = rpmostree_os_dup_cached_update (os_proxy);

      if (!cached_update)
        {
          g_print ("No updates available.\n");
          invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
        }
      else
        {
          /* preview --> verbose (i.e. we want the diff) */
          if (!rpmostree_print_cached_update (cached_update, opt_preview, FALSE,
                                              cancellable, error))
            return FALSE;
        }
    }
  else if (!opt_reboot)
    {
      gboolean changed;
      if (!rpmostree_print_diff_from_deployment (starting_default_deployment, &changed,
                                                 cancellable, error))
        return FALSE;

      if (!changed)
        {
          if (opt_upgrade_unchanged_exit_77)
            invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
        }
      else
        g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}
