/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "libglnx.h"

GPtrArray *rpmostree_bwrap_base_argv_new_for_rootfs (int rootfs, GError **error);

void rpmostree_ptrarray_append_strdup (GPtrArray *argv_array, ...) G_GNUC_NULL_TERMINATED;

gboolean rpmostree_run_sync_fchdir_setup (char **argv_array, GSpawnFlags flags,
                                          int rootfs_fd, GError **error);

gboolean rpmostree_bwrap_bootstrap_if_in_mock (GError **error);
gboolean rpmostree_bwrap_selftest (GError **error);
