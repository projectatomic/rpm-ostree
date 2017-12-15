/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "rpmostreed-deployment-utils.h"
#include "rpmostree-origin.h"
#include "rpmostree-util.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostreed-utils.h"
#include "rpmostreed-errors.h"

#include <libglnx.h>

/* Get a currently unique (for this host) identifier for the
 * deployment; TODO - adding the deployment timestamp would make it
 * persistently unique, needs API in libostree.
 */
char *
rpmostreed_deployment_generate_id (OstreeDeployment *deployment)
{
  g_return_val_if_fail (OSTREE_IS_DEPLOYMENT (deployment), NULL);
  return g_strdup_printf ("%s-%s.%u",
                          ostree_deployment_get_osname (deployment),
                          ostree_deployment_get_csum (deployment),
                          ostree_deployment_get_deployserial (deployment));
}

OstreeDeployment *
rpmostreed_deployment_get_for_id (OstreeSysroot *sysroot,
                                  const gchar *deploy_id)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  for (guint i = 0; i < deployments->len; i++)
    {
      g_autofree gchar *id = rpmostreed_deployment_generate_id (deployments->pdata[i]);
      if (g_strcmp0 (deploy_id, id) == 0)
        return g_object_ref (deployments->pdata[i]);
    }

  return NULL;
}


/* rpmostreed_deployment_get_for_index:
 *
 * sysroot: A #OstreeSysroot instance
 * index: string index being parsed
 * error: a #GError instance
 *
 * Get a deployment based on a string index,
 * the string is parsed and checked. Then
 * the deployment at the parsed index will be
 * returned.
 *
 * returns: NULL if an error is being made
 */
OstreeDeployment *
rpmostreed_deployment_get_for_index (OstreeSysroot *sysroot,
                                     const gchar   *index,
                                     GError       **error)
{
  g_autoptr(GError) local_error = NULL;
  int deployment_index = -1;
  for (int counter = 0; counter < strlen(index); counter++)
    {
      if (!g_ascii_isdigit (index[counter]))
        {
          local_error = g_error_new (RPM_OSTREED_ERROR,
                                     RPM_OSTREED_ERROR_FAILED,
                                     "Invalid deployment index %s, must be a number and >= 0",
                                     index);
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
    }
  deployment_index = atoi (index);

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployment_index >= deployments->len)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Out of range deployment index %d, expected < %d",
                                 deployment_index, deployments->len);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  return g_object_ref (deployments->pdata[deployment_index]);
}

static gboolean
rpmostreed_deployment_gpg_results (OstreeRepo  *repo,
                                   const gchar *origin_refspec,
                                   const gchar *checksum,
                                   GVariant   **out_results,
                                   gboolean    *out_enabled,
                                   GError     **error)
{
  GLNX_AUTO_PREFIX_ERROR ("GPG verification error", error);

  g_autofree gchar *remote = NULL;
  if (!ostree_parse_refspec (origin_refspec, &remote, NULL, error))
    return FALSE;

  gboolean gpg_verify = FALSE;
  if (remote)
    {
      if (!ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, error))
        return FALSE;
    }

  if (!gpg_verify)
    {
      /* Note early return; no need to verify signatures! */
      *out_enabled = FALSE;
      *out_results = NULL;
      return TRUE;
    }

  g_autoptr(GError) local_error = NULL;
  g_autoptr(OstreeGpgVerifyResult) verify_result =
    ostree_repo_verify_commit_for_remote (repo, checksum, remote, NULL, &local_error);
  if (!verify_result)
    {
      /* Somehow, we have a deployment which has gpg-verify=true, but *doesn't* have a valid
       * signature. Let's not just bomb out here. We need to return this in the variant so
       * that `status` can show "(unsigned)". */
      *out_enabled = TRUE;
      *out_results = NULL;
      return TRUE;
    }

  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));

  guint n_sigs = ostree_gpg_verify_result_count_all (verify_result);
  for (guint i = 0; i < n_sigs; i++)
    g_variant_builder_add (&builder, "v", ostree_gpg_verify_result_get_all (verify_result, i));

  *out_results = g_variant_ref_sink (g_variant_builder_end (&builder));
  *out_enabled = TRUE;
  return TRUE;
}

GVariant *
rpmostreed_deployment_generate_blank_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);

  return g_variant_dict_end (&dict);
}

static void
variant_add_metadata_attribute (GVariantDict *dict,
                                const gchar  *attribute,
                                const gchar  *new_attribute,
                                GVariant     *commit)
{
  g_autofree gchar *attribute_string_value = NULL;
  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);

  if (metadata != NULL)
    {
      g_variant_lookup (metadata, attribute, "s", &attribute_string_value);
      if (attribute_string_value != NULL)
        g_variant_dict_insert (dict, new_attribute ?: attribute, "s", attribute_string_value);
    }
}

static void
variant_add_commit_details (GVariantDict *dict,
                            const char *prefix,
                            GVariant     *commit)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autofree gchar *version_commit = NULL;
  guint64 timestamp = 0;

  timestamp = ostree_commit_get_timestamp (commit);
  metadata = g_variant_get_child_value (commit, 0);
  if (metadata != NULL)
    g_variant_lookup (metadata, "version", "s", &version_commit);

  if (version_commit != NULL)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "version"),
                           "s", version_commit);
  if (timestamp > 0)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "timestamp"),
                           "t", timestamp);
}

static void
variant_add_from_hash_table (GVariantDict *dict,
                             const char   *key,
                             GHashTable   *table)
{
  g_autofree char **values = (char**)g_hash_table_get_keys_as_array (table, NULL);
  g_variant_dict_insert (dict, key, "^as", values);
}

GVariant *
rpmostreed_deployment_generate_variant (OstreeSysroot *sysroot,
                                        OstreeDeployment *deployment,
                                        const char *booted_id,
                                        OstreeRepo *repo,
                                        GError **error)
{
  const gchar *osname = ostree_deployment_get_osname (deployment);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  gint serial = ostree_deployment_get_deployserial (deployment);

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 csum,
                                 &commit,
                                 error))
    return NULL;

  g_autofree gchar *id = rpmostreed_deployment_generate_id (deployment);
  g_autoptr(RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return NULL;

  const char *refspec = rpmostree_origin_get_refspec (origin);

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);

  g_variant_dict_insert (&dict, "id", "s", id);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "serial", "i", serial);
  g_variant_dict_insert (&dict, "checksum", "s", csum);

  gboolean is_layered = FALSE;
  g_autofree char *base_checksum = NULL;
  g_auto(GStrv) layered_pkgs = NULL;
  g_autoptr(GVariant) removed_base_pkgs = NULL;
  g_autoptr(GVariant) replaced_base_pkgs = NULL;
  if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered, &base_checksum,
                                              &layered_pkgs, &removed_base_pkgs,
                                              &replaced_base_pkgs, error))
    return NULL;

  g_autoptr(GVariant) base_commit = NULL;
  if (is_layered)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     base_checksum, &base_commit, error))
        return NULL;

      g_variant_dict_insert (&dict, "base-checksum", "s", base_checksum);
      variant_add_commit_details (&dict, "base-", base_commit);
      /* for layered commits, check if their base commit has end of life attribute */
      variant_add_metadata_attribute (&dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife", base_commit);

      /* See below for base commit metadata */
      g_autoptr(GVariant) layered_metadata = g_variant_get_child_value (commit, 0);
      g_variant_dict_insert (&dict, "layered-commit-meta", "@a{sv}", layered_metadata);
    }
  else
    {
      base_commit = g_variant_ref (commit);
      base_checksum = g_strdup (csum);
      variant_add_metadata_attribute (&dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife", commit);
    }

  /* We used to bridge individual keys, but that was annoying; just pass through all
   * of the commit metadata.
   */
  { g_autoptr(GVariant) base_meta = g_variant_get_child_value (commit, 0);
    g_variant_dict_insert (&dict, "base-commit-meta", "@a{sv}", base_meta);
  }

  gboolean gpg_enabled = FALSE;
  g_autoptr(GVariant) sigs = NULL;
  if (!rpmostreed_deployment_gpg_results (repo, refspec, base_checksum, &sigs, &gpg_enabled, error))
    return NULL;
  variant_add_commit_details (&dict, NULL, commit);

  g_autofree char *pending_base_commitrev = NULL;
  if (!ostree_repo_resolve_rev (repo, refspec, TRUE,
                                &pending_base_commitrev, error))
    return NULL;

  if (pending_base_commitrev && strcmp (pending_base_commitrev, base_checksum) != 0)
    {
      g_autoptr(GVariant) pending_base_commit = NULL;

      if (!ostree_repo_load_variant (repo,
                                     OSTREE_OBJECT_TYPE_COMMIT,
                                     pending_base_commitrev,
                                     &pending_base_commit,
                                     error))
        return NULL;

      g_variant_dict_insert (&dict, "pending-base-checksum", "s", pending_base_commitrev);
      variant_add_commit_details (&dict, "pending-base-", pending_base_commit);
    }

  g_autofree char *live_inprogress = NULL;
  g_autofree char *live_replaced = NULL;
  if (!rpmostree_syscore_deployment_get_live (sysroot, deployment, &live_inprogress,
                                              &live_replaced, error))
    return NULL;

  if (live_inprogress)
    g_variant_dict_insert (&dict, "live-inprogress", "s", live_inprogress);
  if (live_replaced)
    g_variant_dict_insert (&dict, "live-replaced", "s", live_replaced);

  g_variant_dict_insert (&dict, "origin", "s", refspec);

  variant_add_from_hash_table (&dict, "requested-packages",
                               rpmostree_origin_get_packages (origin));
  variant_add_from_hash_table (&dict, "requested-local-packages",
                               rpmostree_origin_get_local_packages (origin));
  variant_add_from_hash_table (&dict, "requested-base-removals",
                               rpmostree_origin_get_overrides_remove (origin));
  variant_add_from_hash_table (&dict, "requested-base-local-replacements",
                               rpmostree_origin_get_overrides_local_replace (origin));

  g_variant_dict_insert (&dict, "packages", "^as", layered_pkgs);
  g_variant_dict_insert_value (&dict, "base-removals", removed_base_pkgs);
  g_variant_dict_insert_value (&dict, "base-local-replacements", replaced_base_pkgs);

  if (sigs != NULL)
    g_variant_dict_insert_value (&dict, "signatures", sigs);
  g_variant_dict_insert (&dict, "gpg-enabled", "b", gpg_enabled);

  g_variant_dict_insert (&dict, "unlocked", "s",
                         ostree_deployment_unlocked_state_to_string (ostree_deployment_get_unlocked (deployment)));

  g_variant_dict_insert (&dict, "regenerate-initramfs", "b",
                         rpmostree_origin_get_regenerate_initramfs (origin));
  { const char *const* args = rpmostree_origin_get_initramfs_args (origin);
    if (args && *args)
      g_variant_dict_insert (&dict, "initramfs-args", "^as", args);
  }

  if (booted_id != NULL)
    g_variant_dict_insert (&dict, "booted", "b", g_strcmp0 (booted_id, id) == 0);

  return g_variant_dict_end (&dict);
}

static gboolean
add_all_commit_details_to_vardict (OstreeDeployment *deployment,
                                   OstreeRepo       *repo,
                                   const char       *refspec,  /* allow-none */
                                   const char       *checksum, /* allow-none */
                                   GVariant         *commit,   /* allow-none */
                                   GVariantDict     *dict,     /* allow-none */
                                   GError          **error)
{
  const gchar *osname = ostree_deployment_get_osname (deployment);

  g_autofree gchar *refspec_owned = NULL;
  if (!refspec)
    {
      g_autoptr(RpmOstreeOrigin) origin =
        rpmostree_origin_parse_deployment (deployment, error);
      if (!origin)
        return FALSE;
      refspec = refspec_owned = g_strdup (rpmostree_origin_get_refspec (origin));
    }

  g_assert (refspec);

  /* allow_noent=TRUE since the ref may have been deleted for a rebase */
  g_autofree gchar *checksum_owned = NULL;
  if (!checksum)
    {
      if (!ostree_repo_resolve_rev (repo, refspec, TRUE, &checksum_owned, error))
        return FALSE;

      /* in that case, use deployment csum */
      checksum = checksum_owned ?: ostree_deployment_get_csum (deployment);
    }

  g_autoptr(GVariant) commit_owned = NULL;
  if (!commit)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                     &commit_owned, error))
        return FALSE;
      commit = commit_owned;
    }

  gboolean gpg_enabled;
  g_autoptr(GVariant) sigs = NULL;
  if (!rpmostreed_deployment_gpg_results (repo, refspec, checksum,
                                          &sigs, &gpg_enabled, error))
    return FALSE;

  if (osname != NULL)
    g_variant_dict_insert (dict, "osname", "s", osname);
  g_variant_dict_insert (dict, "checksum", "s", checksum);
  variant_add_commit_details (dict, NULL, commit);
  g_variant_dict_insert (dict, "origin", "s", refspec);
  if (sigs != NULL)
    g_variant_dict_insert_value (dict, "signatures", sigs);
  g_variant_dict_insert (dict, "gpg-enabled", "b", gpg_enabled);
  return TRUE;
}

GVariant *
rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment,
                                                   OstreeRepo *repo,
                                                   const gchar *refspec,
                                                   GError **error)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  if (!add_all_commit_details_to_vardict (deployment, repo, refspec,
                                          NULL, NULL, &dict, error))
    return NULL;

  return g_variant_dict_end (&dict);
}
