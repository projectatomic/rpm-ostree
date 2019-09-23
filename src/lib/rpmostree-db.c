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

#include "rpmostree-db.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-private.h"
#include "rpmostree-package-priv.h"
#include "rpmostree-refsack.h"

/**
 * SECTION:librpmostree-dbquery
 * @title: Query RPM database
 * @short_description: Access the RPM database in commits
 *
 * These APIs provide queryable access to the RPM database inside an
 * OSTree repository.
 */

/**
 * rpm_ostree_db_query_all:
 * @repo: An OSTree repository
 * @ref: A branch name or commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Return all of the RPM packages present in the @ref branch or commit
 * in @repo.
 *
 * Returns: (transfer container) (element-type RpmOstreePackage): A query result, or %NULL on error
 */
GPtrArray *
rpm_ostree_db_query_all (OstreeRepo                *repo,
                         const char                *ref,
                         GCancellable              *cancellable,
                         GError                   **error)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  if (!_rpm_ostree_package_list_for_commit (repo, ref, FALSE, &pkglist, cancellable, error))
    return NULL;
  return g_steal_pointer (&pkglist);
}

/**
 * rpm_ostree_db_diff:
 * @repo: An OSTree repository
 * @orig_ref: Original ref (branch or commit)
 * @new_ref: New ref (branch or commit)
 * @out_removed: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for removed packages
 * @out_added: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for added packages
 * @out_modified_old: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for modified old packages
 * @out_modified_new: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for modified new packages
 *
 * Compute the RPM package delta between two commits.
 *
 * If there are multiple packages with the same name, they are dealt
 * with as follow:
 *   - if there are N pkgs of the same name in @orig_ref, and 0 pkgs of the same name in
 *     @new_ref, then there will be N entries in @out_removed (and vice-versa for
 *     @new_ref/@out_added)
 *   - if there are N pkgs of the same name in @orig_ref, and M pkgs of the same name in
 *     @new_ref, then there will be M entries in @out_modified_new, where all M entries will
 *     be paired with the same arbitrary pkg coming from one of the N entries.
 */
gboolean
rpm_ostree_db_diff (OstreeRepo               *repo,
                    const char               *orig_ref,
                    const char               *new_ref,
                    GPtrArray               **out_removed,
                    GPtrArray               **out_added,
                    GPtrArray               **out_modified_old,
                    GPtrArray               **out_modified_new,
                    GCancellable             *cancellable,
                    GError                  **error)
{
  return rpm_ostree_db_diff_ext (repo,
                                 orig_ref,
                                 new_ref,
                                 0,
                                 out_removed,
                                 out_added,
                                 out_modified_old,
                                 out_modified_new,
                                 cancellable,
                                 error);
}

/**
 * rpm_ostree_db_diff_ext:
 * @repo: An OSTree repository
 * @orig_ref: Original ref (branch or commit)
 * @new_ref: New ref (branch or commit)
 * @flags: Flags controlling diff behaviour
 * @out_removed: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for removed packages
 * @out_added: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for added packages
 * @out_modified_old: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for modified old packages
 * @out_modified_new: (out) (transfer container) (element-type RpmOstreePackage) (allow-none): Return location for modified new packages
 *
 * This function is identical to rpm_ostree_db_diff_ext(), but supports a @flags argument to
 * further control behaviour. At least one of the @out parameters must not be NULL.
 *
 * Since: 2017.12
 */
gboolean
rpm_ostree_db_diff_ext (OstreeRepo               *repo,
                        const char               *orig_ref,
                        const char               *new_ref,
                        RpmOstreeDbDiffExtFlags   flags,
                        GPtrArray               **out_removed,
                        GPtrArray               **out_added,
                        GPtrArray               **out_modified_old,
                        GPtrArray               **out_modified_new,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  g_return_val_if_fail (out_removed || out_added ||
                        out_modified_old || out_modified_new, FALSE);

  const gboolean allow_noent = ((flags & RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT) > 0);

  g_autoptr(GPtrArray) orig_pkglist = NULL;
  if (!_rpm_ostree_package_list_for_commit (repo, orig_ref, allow_noent, &orig_pkglist,
                                            cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) new_pkglist = NULL;
  if (orig_pkglist)
    {
      if (!_rpm_ostree_package_list_for_commit (repo, new_ref, allow_noent, &new_pkglist,
                                                cancellable, error))
        return FALSE;
    }

  if (!orig_pkglist || !new_pkglist)
    {
      /* it's the only way we could've gotten this far */
      g_assert (allow_noent);
      if (out_removed)
        *out_removed = NULL;
      if (out_added)
        *out_added = NULL;
      if (out_modified_old)
        *out_modified_old = NULL;
      if (out_modified_new)
        *out_modified_new = NULL;
      return TRUE;
    }

  return _rpm_ostree_package_diff_lists (orig_pkglist, new_pkglist, out_removed, out_added,
                                         out_modified_old, out_modified_new, NULL);
}
