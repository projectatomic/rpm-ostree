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

#include "rpmostree-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_skip_purge;
static char * opt_branch;
static char * opt_remote;
static char * opt_custom_origin_url;
static char * opt_custom_origin_description;
static gboolean opt_cache_only;
static gboolean opt_download_only;
static gboolean opt_experimental;
static gboolean opt_disallow_downgrade;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Rebase to branch BRANCH; use --remote to change remote as well", "BRANCH" },
  { "remote", 'm', 0, G_OPTION_ARG_STRING, &opt_remote, "Rebase to current branch name using REMOTE; may also be combined with --branch", "REMOTE" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "skip-purge", 0, 0, G_OPTION_ARG_NONE, &opt_skip_purge, "Keep previous refspec after rebase", NULL },
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Do not download latest ostree and RPM data", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Just download latest ostree and RPM data, don't deploy", NULL },
  { "custom-origin-description", 0, 0, G_OPTION_ARG_STRING, &opt_custom_origin_description, "Human-readable description of custom origin", NULL },
  { "custom-origin-url", 0, 0, G_OPTION_ARG_STRING, &opt_custom_origin_url, "Machine-readable description of custom origin", NULL },
  { "experimental", 0, 0, G_OPTION_ARG_NONE, &opt_experimental, "Enable experimental features", NULL },
  { "disallow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_disallow_downgrade, "Forbid deployment of chronologically older trees", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_rebase (int             argc,
                          char          **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable   *cancellable,
                          GError        **error)
{
  const char *new_provided_refspec;
  g_autofree char *new_refspec_owned = NULL;
  const char *revision = NULL;

  /* forced blank for now */
  const char *packages[] = { NULL };

  g_autoptr(GOptionContext) context = g_option_context_new ("REFSPEC [REVISION]");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
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

  if (argc > 3)
    {
      rpmostree_usage_error (context, "Too many arguments", error);
      return FALSE;
    }

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  if (argc < 2 && !(opt_branch || opt_remote))
    {
      return rpmostree_usage_error (context, "Must specify refspec, or -b branch or -r remote", error), FALSE;
    }
  else if (argc >= 2)
    {
      new_provided_refspec = argv[1];
      if (argc == 3)
        revision = argv[2];

      /* Canonicalize */
      new_provided_refspec = new_refspec_owned =
        rpmostree_refspec_canonicalize (new_provided_refspec, error);
      if (!new_provided_refspec)
        return FALSE;
    }
  else
    {
      if (opt_remote)
        {
          new_provided_refspec = new_refspec_owned =
            g_strconcat (RPMOSTREE_REFSPEC_OSTREE_PREFIX, opt_remote, ":", opt_branch ?: "", NULL);
        }
      else
        new_provided_refspec = opt_branch;
    }

  const char *remainder = NULL;
  RpmOstreeRefspecType refspectype;
  if (!rpmostree_refspec_classify (new_provided_refspec, &refspectype, &remainder, error))
    return FALSE;
  if (!opt_experimental && refspectype == RPMOSTREE_REFSPEC_TYPE_ROJIG)
    return glnx_throw (error, "rojig:// refspec requires --experimental");

  /* catch "ostree://" or "rojig://"; we'd error out much later in the daemon otherwise */
  if (strlen (remainder) == 0)
    return glnx_throw (error, "Refspec is empty");

  /* Check if remote refers to a local repo */
  g_autofree char *local_repo_remote = NULL;
  if (G_IN_SET (refspectype, RPMOSTREE_REFSPEC_TYPE_OSTREE,
                             RPMOSTREE_REFSPEC_TYPE_CHECKSUM))
    {
      if (*remainder == '/')
        {
          if (!opt_experimental)
            return glnx_throw (error, "Local repo rebase requires --experimental");
          const char *ref = strrchr (remainder, ':');
          if (!ref)
            return glnx_throw (error, "Missing ':' in LOCALPATH:REF rebase");
          local_repo_remote = g_strndup (remainder, ref - remainder);
          new_provided_refspec = ref + 1;
          /* just don't support "/path/to/repo:" for now */
          if (strlen (new_provided_refspec) == 0)
            return glnx_throw (error, "Missing REF in LOCALPATH:REF rebase");
        }
    }

  g_autoptr(RpmOstreeDeployment) starting_default_deployment =
    rpmostree_get_default_deployment (sysroot_proxy, cancellable, error);
  if (!starting_default_deployment)
    return FALSE;

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "allow-downgrade", "b", !opt_disallow_downgrade);
  g_variant_dict_insert (&dict, "cache-only", "b", opt_cache_only);
  g_variant_dict_insert (&dict, "download-only", "b", opt_download_only);
  g_variant_dict_insert (&dict, "skip-purge", "b", opt_skip_purge);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  if (opt_custom_origin_url)
    {
      if (!opt_custom_origin_description)
        return glnx_throw (error, "--custom-origin-description must be supplied with --custom-origin-url");
      g_variant_dict_insert (&dict, "custom-origin", "(ss)",
                             opt_custom_origin_url,
                             opt_custom_origin_description);
    }
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  /* Use newer D-Bus API only if we have to. */
  if (install_pkgs || uninstall_pkgs || local_repo_remote)
    {
      if (!rpmostree_update_deployment (os_proxy,
                                        new_provided_refspec,
                                        revision,
                                        install_pkgs,
                                        uninstall_pkgs,
                                        NULL, /* override replace */
                                        NULL, /* override remove */
                                        NULL, /* override reset */
                                        local_repo_remote,
                                        options,
                                        &transaction_address,
                                        cancellable,
                                        error))
        return FALSE;
    }
  else
    {
      /* the original Rebase() call takes the revision through the options */
      if (revision)
        {
          g_autoptr(GVariant) old_options = options;
          g_auto(GVariantDict) dict;
          g_variant_dict_init (&dict, old_options);
          g_variant_dict_insert (&dict, "revision", "s", revision);
          options = g_variant_ref_sink (g_variant_dict_end (&dict));
        }

      if (!rpmostree_os_call_rebase_sync (os_proxy,
                                          options,
                                          new_provided_refspec,
                                          packages,
                                          NULL,
                                          &transaction_address,
                                          NULL,
                                          cancellable,
                                          error))
        return FALSE;
    }

  return rpmostree_transaction_client_run (invocation, sysroot_proxy, os_proxy,
                                           options, FALSE,
                                           transaction_address,
                                           starting_default_deployment,
                                           cancellable, error);
}
