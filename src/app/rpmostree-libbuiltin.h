/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#include "rpmostree-util.h"

#include "rpm-ostreed-generated.h"

G_BEGIN_DECLS

#define TERM_ESCAPE_SEQUENCE(type,seq)                   \
    static inline const char* get_##type (void) {        \
      if (glnx_stdout_is_tty ())                         \
        return seq;                                      \
      return "";                                         \
    }

TERM_ESCAPE_SEQUENCE(red_start,  "\x1b[31m")
TERM_ESCAPE_SEQUENCE(red_end,    "\x1b[22m")
TERM_ESCAPE_SEQUENCE(bold_start, "\x1b[1m")
TERM_ESCAPE_SEQUENCE(bold_end,   "\x1b[0m")

#undef TERM_ESCAPE_SEQUENCE

void
rpmostree_print_kv_no_newline (const char *key,
                               guint       maxkeylen,
                               const char *value);

void
rpmostree_print_kv (const char *key,
                    guint       maxkeylen,
                    const char *value);

void
rpmostree_usage_error (GOptionContext  *context,
                       const char      *message,
                       GError         **error);

typedef struct RpmOstreeDeployment RpmOstreeDeployment;

RpmOstreeDeployment*
rpmostree_get_default_deployment (RPMOSTreeSysroot *sysroot_proxy,
                                  GCancellable  *cancellable,
                                  GError       **error);

void
rpmostree_deployment_free (RpmOstreeDeployment *deployment);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeDeployment, rpmostree_deployment_free);

gboolean
rpmostree_print_diff_from_deployment (RpmOstreeDeployment *deployment,
                                      gboolean      *out_changed,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean
rpmostree_print_treepkg_diff_from_sysroot_path (const gchar   *sysroot_path,
                                                RpmOstreeDiffPrintFormat format,
                                                guint          max_key_len,
                                                GCancellable  *cancellable,
                                                GError       **error);

void
rpmostree_print_timestamp_version (const char  *version_string,
                                   const char  *timestamp_string,
                                   guint        max_key_len);

G_END_DECLS
