/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
#include "rpmostree-core.h"

G_BEGIN_DECLS

typedef struct RpmOstreeOrigin RpmOstreeOrigin;
RpmOstreeOrigin *rpmostree_origin_ref (RpmOstreeOrigin *origin);
void rpmostree_origin_unref (RpmOstreeOrigin *origin);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeOrigin, rpmostree_origin_unref)

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile *keyfile,
                                GError **error);

static inline
RpmOstreeOrigin *
rpmostree_origin_parse_deployment (OstreeDeployment *deployment,
                                   GError **error)
{
  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
      return NULL;
    }
  return rpmostree_origin_parse_keyfile (origin, error);
}

RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin);

void
rpmostree_origin_remove_transient_state (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin);

void
rpmostree_origin_classify_refspec (RpmOstreeOrigin      *origin,
                                   RpmOstreeRefspecType *out_type,
                                   const char          **out_refspecdata);

char *
rpmostree_origin_get_full_refspec (RpmOstreeOrigin *origin,
                                   RpmOstreeRefspecType *out_refspectype);

void
rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin,
                                         char           **custom_type,
                                         char           **custom_description);

GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_modules_enable (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_modules_install (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin);

GHashTable *
rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin);

gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin);

const char *const*
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin);

const char *
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin);

gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin);

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value);

GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin);

void
rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin,
                                            char **paths,
                                            gboolean *out_changed);

void
rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin,
                                              char **paths,
                                              gboolean *out_changed);

void
rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin,
                                                  gboolean *out_changed);

void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin,
                                           gboolean regenerate,
                                           char **args);

void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin,
                                      const char      *checksum,
                                      const char      *version);

gboolean
rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin);
void
rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, gboolean cliwrap);

gboolean
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin,
                             const char      *new_refspec,
                             GError         **error);
gboolean
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin,
                                    const char      *new_refspec,
                                    const char      *custom_origin_url,
                                    const char      *custom_origin_description,
                                    GError         **error);

gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin   *origin,
                               char             **packages,
                               gboolean           local,
                               gboolean           allow_existing,
                               gboolean          *out_changed,
                               GError           **error);

gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin  *origin,
                                  char            **packages,
                                  gboolean          allow_noent,
                                  gboolean         *out_changed,
                                  GError          **error);
gboolean
rpmostree_origin_remove_all_packages (RpmOstreeOrigin  *origin,
                                      gboolean         *out_changed,
                                      GError          **error);

gboolean
rpmostree_origin_add_modules (RpmOstreeOrigin  *origin,
                              char           **modules,
                              gboolean         enable_only,
                              gboolean        *out_changed,
                              GError         **error);

gboolean
rpmostree_origin_remove_modules (RpmOstreeOrigin  *origin,
                                 char           **modules,
                                 gboolean         enable_only,
                                 gboolean        *out_changed,
                                 GError         **error);

typedef enum {
  /* RPMOSTREE_ORIGIN_OVERRIDE_REPLACE, */
  RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL,
  RPMOSTREE_ORIGIN_OVERRIDE_REMOVE
} RpmOstreeOriginOverrideType;

gboolean
rpmostree_origin_add_overrides (RpmOstreeOrigin  *origin,
                                char            **packages,
                                RpmOstreeOriginOverrideType type,
                                GError          **error);

gboolean
rpmostree_origin_remove_override (RpmOstreeOrigin  *origin,
                                  const char       *package,
                                  RpmOstreeOriginOverrideType type);

gboolean
rpmostree_origin_remove_all_overrides (RpmOstreeOrigin  *origin,
                                       gboolean         *out_changed,
                                       GError          **error);

G_END_DECLS
