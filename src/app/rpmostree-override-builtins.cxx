/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "rpmostree-override-builtins.h"
#include "rpmostree-libbuiltin.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_dry_run;
static gboolean opt_reset_all;
static const char *const *opt_remove_pkgs;
static const char *const *opt_replace_pkgs;
static const char *const *install_pkgs;
static const char *const *uninstall_pkgs;
static gboolean opt_lock_finalization;
static gboolean opt_ex_pin_from_repos;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { NULL }
};

static GOptionEntry reset_option_entries[] = {
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_reset_all, "Reset all active overrides", NULL },
  { NULL }
};

static GOptionEntry replace_option_entries[] = {
  { "remove", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_remove_pkgs, "Remove a package", "PKG" },
  { "ex-pin-from-repos", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_ex_pin_from_repos, "Allow fetching from repos", NULL},
  { NULL }
};

static GOptionEntry remove_option_entries[] = {
  { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_replace_pkgs, "Replace a package", "RPM" },
  { NULL }
};

static gboolean
handle_override (RPMOSTreeSysroot  *sysroot_proxy,
                 RpmOstreeCommandInvocation *invocation,
                 const char *const *override_remove,
                 const char *const *override_replace,
                 const char *const *override_reset,
                 GCancellable      *cancellable,
                 GError           **error)
{
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  RPMOSTreeOSExperimental *osexperimental_proxy = NULL;

  if(!rpmostree_load_os_proxies(sysroot_proxy, opt_osname, cancellable,
                                &os_proxy, &osexperimental_proxy, error))
    return FALSE;

  /* Perform uninstalls offline; users don't expect the "auto-update" behaviour here. But
   * note we might still need to fetch pkgs in the local replacement case (e.g. the
   * replacing pkg has an additional out-of-tree dep). */
  const gboolean cache_only = (override_replace == NULL && install_pkgs == NULL);

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "cache-only", "b", cache_only);
  g_variant_dict_insert (&dict, "no-pull-base", "b", TRUE);
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run);
  g_variant_dict_insert (&dict, "no-overrides", "b", opt_reset_all);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  g_autoptr(GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);
  g_autoptr(GPtrArray) override_replace_final = g_ptr_array_new();
  GUnixFDList fds;
  g_autoptr(GUnixFDList) list_fds;

  if (override_replace && opt_ex_pin_from_repos)
    {
      g_autoptr(GPtrArray) queries = g_ptr_array_new();

      for (const char *const* it = override_replace; it && *it; it++)
        {
          auto pkg = *it;

          if(rpmostreecxx::is_http_arg(pkg) || rpmostreecxx::is_rpm_arg(pkg))
            {
              g_ptr_array_add(override_replace_final, (gpointer)pkg);
              continue;
	    }
          else
            {
              g_ptr_array_add(queries, (gpointer)pkg);
            }
        }
      if (queries->len > 0)
        {
          g_ptr_array_add(queries, NULL);
          g_autofree char *refresh_transaction_address = NULL;

          if (!rpmostree_os_call_refresh_md_sync(os_proxy, options, &refresh_transaction_address,
                                                 cancellable, error))
            return FALSE;
          if (!rpmostree_transaction_get_response_sync(sysroot_proxy, (const char*)refresh_transaction_address,
                                                       cancellable, error))
            return FALSE;
          g_autoptr(GVariant) fd_idxs = NULL;
          if (!rpmostree_osexperimental_call_download_packages_sync (osexperimental_proxy,
                                                                     (const char *const*) queries->pdata,
                                                                     &fds, &fd_idxs, &list_fds,
                                                                     cancellable, error))
            return FALSE;

          const int nfds = g_unix_fd_list_get_length (list_fds);
          const guint n = g_variant_n_children (fd_idxs);
          gint *new_fds = g_new0 (gint, n+1);

          for (guint i = 0; i < n; i++)
            {
              g_autoptr(GVariant) hv = g_variant_get_child_value (fd_idxs, i);
              g_assert (hv);
              gint32 h = g_variant_get_handle (hv);
              g_assert (0 <= h && h < nfds);
              new_fds[i] = fds[h];
            }

          new_fds[n] = -1;
          override_replace = (const char *const*) new_fds;
        }
    }
  g_autofree char *transaction_address = NULL;
  if (!rpmostree_update_deployment (os_proxy,
                                    NULL, /* set-refspec */
                                    NULL, /* set-revision */
                                    install_pkgs,
                                    uninstall_pkgs,
                                    override_replace,
                                    override_remove,
                                    override_reset,
                                    NULL,
                                    options,
                                    &transaction_address,
                                    cancellable,
                                    error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  if (opt_dry_run)
    {
      g_print ("Exiting because of '--dry-run' option\n");
    }
  else if (!opt_reboot)
    {
      /* only print diff if a new deployment was laid down (e.g. reset --all may not) */
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        return TRUE;

      const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
      rpmostreecxx::print_treepkg_diff_from_sysroot_path (rust::Str(sysroot_path),
            RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, 0, cancellable);

      if (override_replace || override_remove)
        g_print ("Use \"rpm-ostree override reset\" to undo overrides\n");

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}

gboolean
rpmostree_override_builtin_replace (int argc, char **argv,
                                    RpmOstreeCommandInvocation *invocation,
                                    GCancellable *cancellable,
                                    GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, replace_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified",
                             error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation,
                          opt_remove_pkgs, (const char *const*)argv, NULL,
                          cancellable, error);
}

gboolean
rpmostree_override_builtin_remove (int argc, char **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable *cancellable,
                                   GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, remove_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified",
                             error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation,
                          (const char *const*)argv, opt_replace_pkgs, NULL,
                          cancellable, error);
}

gboolean
rpmostree_override_builtin_reset (int argc, char **argv,
                                  RpmOstreeCommandInvocation *invocation,
                                  GCancellable *cancellable,
                                  GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, reset_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2 && !opt_reset_all)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified",
                             error);
      return FALSE;
    }
  else if (opt_reset_all && argc >= 2)
    {
      rpmostree_usage_error (context, "Cannot specify PACKAGEs with --all",
                             error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation,
                          NULL, NULL, (const char *const*)argv,
                          cancellable, error);
}
