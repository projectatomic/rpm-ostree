/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2018 Jonathan Lebon <jonathan@jlebon.com>
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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_overlays;
static gboolean opt_overrides;
static gboolean opt_initramfs;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after transaction is complete", NULL },
  { "overlays", 'l', 0, G_OPTION_ARG_NONE, &opt_overlays, "Remove all overlayed packages", NULL },
  { "overrides", 'o', 0, G_OPTION_ARG_NONE, &opt_overrides, "Remove all overrides", NULL },
  { "initramfs", 'i', 0, G_OPTION_ARG_NONE, &opt_initramfs, "Stop regenerating initramfs", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_reset (int             argc,
                         char          **argv,
                         RpmOstreeCommandInvocation *invocation,
                         GCancellable   *cancellable,
                         GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
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
                                       &peer_pid,
                                       NULL,
                                       error))
    return FALSE;

  if (argc < 1 || argc > 2)
    {
      rpmostree_usage_error (context, "Too few or too many arguments", error);
      return FALSE;
    }

  /* default to resetting all if no specificiers */
  if (!opt_overlays && !opt_overrides && !opt_initramfs)
    opt_overlays = opt_overrides = opt_initramfs = TRUE;

  /* If we don't also have to install pkgs, do resets offline */
  gboolean cache_only = (install_pkgs == NULL);

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
  g_variant_dict_insert (&dict, "no-pull-base", "b", TRUE);
  g_variant_dict_insert (&dict, "no-layering", "b", opt_overlays);
  g_variant_dict_insert (&dict, "no-overrides", "b", opt_overrides);
  g_variant_dict_insert (&dict, "no-initramfs", "b", opt_initramfs);
  g_variant_dict_insert (&dict, "cache-only", "b", cache_only);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  if (!rpmostree_update_deployment (os_proxy, NULL, NULL, install_pkgs, uninstall_pkgs,
                                    NULL, NULL, NULL, NULL, options, &transaction_address,
                                    cancellable, error))
    return FALSE;

  return rpmostree_transaction_client_run (invocation, sysroot_proxy, os_proxy,
                                           options, FALSE,
                                           transaction_address,
                                           starting_default_deployment,
                                           cancellable, error);
}
