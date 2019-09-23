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

#include "string.h"

#include "rpmostree-libbuiltin.h"
#include "rpmostree.h"
#include "rpmostree-util.h"
#include "rpmostree-private.h"

#include "libglnx.h"

void
rpmostree_print_kv_no_newline (const char *key,
                               guint       maxkeylen,
                               const char *value)
{
  printf ("  %*s%s %s", maxkeylen, key, strlen (key) ? ":" : " ", value);
}

void
rpmostree_print_kv (const char *key,
                    guint       maxkeylen,
                    const char *value)
{
  rpmostree_print_kv_no_newline (key, maxkeylen, value);
  putc ('\n', stdout);
}

void
rpmostree_usage_error (GOptionContext  *context,
                       const char      *message,
                       GError         **error)
{
  g_return_if_fail (context != NULL);
  g_return_if_fail (message != NULL);

  g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);

  (void) glnx_throw (error, "usage error: %s", message);
}

/* This is just a simple struct to avoid reloading the sysroot multiple times in client
 * code, as well as to remember the previous default pkglist before it gets GC'ed in order
 * to only print differences: https://github.com/projectatomic/rpm-ostree/issues/945 */
struct RpmOstreeDeployment
{
  OstreeSysroot    *sysroot;
  OstreeRepo       *repo;
  OstreeDeployment *deployment;
  GPtrArray        *pkglist;
};

RpmOstreeDeployment*
rpmostree_get_default_deployment (RPMOSTreeSysroot *sysroot_proxy,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);

  g_autoptr(GFile) sysroot_file = g_file_new_for_path (sysroot_path);
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return NULL;

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return NULL;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  g_assert_cmpuint (deployments->len, >, 0);
  g_autoptr(OstreeDeployment) default_deployment = g_object_ref (deployments->pdata[0]);

  const char *csum = ostree_deployment_get_csum (default_deployment);
  g_autoptr(GPtrArray) pkglist = rpm_ostree_db_query_all (repo, csum, cancellable, error);
  if (!pkglist)
    return NULL;

  RpmOstreeDeployment *deployment = g_new (RpmOstreeDeployment, 1);
  deployment->sysroot = g_steal_pointer (&sysroot);
  deployment->repo = g_steal_pointer (&repo);
  deployment->deployment = g_steal_pointer (&default_deployment);
  deployment->pkglist = g_steal_pointer (&pkglist);
  return deployment;
}

void
rpmostree_deployment_free (RpmOstreeDeployment *deployment)
{
  g_clear_pointer (&deployment->pkglist, g_ptr_array_unref);
  g_clear_pointer (&deployment->deployment, g_object_unref);
  g_clear_pointer (&deployment->repo, g_object_unref);
  g_clear_pointer (&deployment->sysroot, g_object_unref);
  g_free (deployment);
}

gboolean
rpmostree_print_diff_from_deployment (RpmOstreeDeployment *deployment,
                                      gboolean      *out_changed,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  /* just reload the sysroot to make sure it's updated */
  if (!ostree_sysroot_load_if_changed (deployment->sysroot, out_changed,
                                       cancellable, error))
    return FALSE;

  if (!*out_changed)
    return TRUE;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (deployment->sysroot);
  g_assert_cmpuint (deployments->len, >, 1);

  OstreeDeployment *new_deployment = deployments->pdata[0];
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (deployment->sysroot);

  if (!booted_deployment || ostree_deployment_equal (booted_deployment, new_deployment))
    return TRUE; /* e.g. `cleanup -p` or `rollback` */

  if (ostree_deployment_equal (deployment->deployment, new_deployment))
    return TRUE; /* no change in default deployment */

  const char *csum = ostree_deployment_get_csum (new_deployment);
  g_autoptr(GPtrArray) pkglist = rpm_ostree_db_query_all (deployment->repo, csum,
                                                          cancellable, error);
  if (!pkglist)
    return FALSE;

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;

  /* need API here to print diff between old pkglist and new pkglist */
  g_assert (_rpm_ostree_package_diff_lists (deployment->pkglist, pkglist, &removed, &added,
                                            &modified_old, &modified_new, NULL));
  rpmostree_diff_print_formatted (RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, 0,
                                  removed, added, modified_old, modified_new);
  return TRUE;
}

gboolean
rpmostree_print_treepkg_diff_from_sysroot_path (const gchar   *sysroot_path,
                                                RpmOstreeDiffPrintFormat format,
                                                guint          max_key_len,
                                                GCancellable  *cancellable,
                                                GError       **error)
{
  g_autoptr(GFile) sysroot_file = g_file_new_for_path (sysroot_path);
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  g_assert_cmpuint (deployments->len, >, 1);

  OstreeDeployment *new_deployment = deployments->pdata[0];
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (!booted_deployment || ostree_deployment_equal (booted_deployment, new_deployment))
    return TRUE;

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  const char *from_rev = ostree_deployment_get_csum (booted_deployment);
  const char *to_rev = ostree_deployment_get_csum (new_deployment);

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;
  if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                           &removed, &added, &modified_old, &modified_new,
                           cancellable, error))
    return FALSE;

  rpmostree_diff_print_formatted (format, max_key_len,
                                  removed, added, modified_old, modified_new);
  return TRUE;
}

void
rpmostree_print_timestamp_version (const char  *version_string,
                                   const char  *timestamp_string,
                                   guint        max_key_len)
{
  if (!version_string)
    rpmostree_print_kv ("Timestamp", max_key_len, timestamp_string);
  else
    {
      g_autofree char *version_time
        = g_strdup_printf ("%s%s%s (%s)", get_bold_start (), version_string,
                           get_bold_end (), timestamp_string);
      rpmostree_print_kv ("Version", max_key_len, version_time);
    }
}
