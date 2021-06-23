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

#pragma once

#include <ostree.h>

#include "rpmostree-core.h"
#include "rpmostree-cxxrs.h"

G_BEGIN_DECLS

gboolean
rpmostree_composeutil_checksum (HyGoal             goal,
                                OstreeRepo        *repo,
                                const rpmostreecxx::Treefile &tf,
                                JsonObject        *treefile,
                                char             **out_checksum,
                                GError           **error);

gboolean
rpmostree_composeutil_legacy_prep_dev (int         rootfs_dfd,
                                       GError    **error);

RpmOstreeTreespec *
rpmostree_composeutil_get_treespec (RpmOstreeContext  *ctx,
                                    rpmostreecxx::Treefile &treefile_rs,
                                    JsonObject  *treedata,
                                    GError     **error);
gboolean
rpmostree_composeutil_read_json_metadata (JsonNode    *root,
                                          GHashTable  *metadata,
                                          GError     **error);
gboolean
rpmostree_composeutil_read_json_metadata_from_file (const char *path,
                                                    GHashTable *metadata,
                                                    GError    **error);

GVariant *
rpmostree_composeutil_finalize_metadata (GHashTable *metadata,
                                         int         rootfs_dfd,
                                         GError    **error);

gboolean
rpmostree_composeutil_write_composejson (OstreeRepo  *repo,
                                         const char *path,
                                         const OstreeRepoTransactionStats *stats,
                                         const char *new_revision,
                                         GVariant   *new_commit,
                                         const char *new_ref,
                                         GCancellable *cancellable,
                                         GError    **error);

G_END_DECLS
