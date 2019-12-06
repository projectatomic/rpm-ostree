/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#pragma once

#include <ostree.h>
#include "rpmostree-json-parsing.h"
#include "rpmostree-rust.h"

/* "public" for unit tests */
char *
rpmostree_postprocess_replace_nsswitch (const char *buf,
                                        GError    **error);

gboolean
rpmostree_treefile_postprocessing (int            rootfs_fd,
                                   RORTreefile   *treefile_rs,
                                   JsonObject    *treefile,
                                   const char    *next_version,
                                   gboolean       unified_core_mode,
                                   GCancellable  *cancellable,
                                   GError       **error);

gboolean
rpmostree_rootfs_symlink_emptydir_at (int rootfs_fd,
                                      const char *dest,
                                      const char *src,
                                      GError **error);

gboolean
rpmostree_rootfs_prepare_links (int           rootfs_fd,
                                GCancellable *cancellable,
                                GError       **error);

gboolean
rpmostree_cleanup_leftover_rpmdb_files (int            rootfs_fd,
                                        GCancellable  *cancellable,
                                        GError       **error);

gboolean
rpmostree_rootfs_postprocess_common (int           rootfs_fd,
                                     GCancellable *cancellable,
                                     GError       **error);

gboolean
rpmostree_postprocess_run_depmod (int           rootfs_fd,
                                  const char   *kver,
                                  gboolean      unified_core_mode,
                                  GCancellable *cancellable,
                                  GError      **error);

gboolean
rpmostree_postprocess_run_sysusers (int           rootfs_fd,
                                    gboolean      unified_core_mode,
                                    GCancellable *cancellable,
                                    GError      **error);

gboolean
rpmostree_prepare_rootfs_get_sepolicy (int            dfd,
                                       OstreeSePolicy **out_sepolicy,
                                       GCancellable  *cancellable,
                                       GError       **error);

gboolean
rpmostree_rootfs_fixup_selinux_store_root (int rootfs_dfd,
                                           GCancellable *cancellable,
                                           GError       **error);

gboolean
rpmostree_prepare_rootfs_for_commit (int            src_rootfs_dfd,
                                     int            target_rootfs_dfd,
                                     JsonObject    *treefile,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
rpmostree_postprocess_final (int            rootfs_dfd,
                             RORTreefile   *treefile_rs,
                             JsonObject    *treefile,
                             gboolean       unified_core_mode,
                             GCancellable  *cancellable,
                             GError       **error);

gboolean
rpmostree_compose_commit (int            rootfs_dfd,
                          OstreeRepo    *repo,
                          const char    *parent,
                          GVariant      *metadata,
                          const char    *gpg_keyid,
                          gboolean       enable_selinux,
                          OstreeRepoDevInoCache *devino_cache,
                          char         **out_new_revision,
                          GCancellable  *cancellable,
                          GError       **error);
