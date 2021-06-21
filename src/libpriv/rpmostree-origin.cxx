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

#include "config.h"

#include "string.h"

#include "libglnx.h"
#include "rpmostree-origin.h"
#include "rpmostree-core.h"
#include "rpmostree-util.h"
#include "rpmostree-rpm-util.h"

struct RpmOstreeOrigin {
  guint refcount;

  /* this is the single source of truth */
  GKeyFile *kf;

  /* Branch name or pinned to commit*/
  RpmOstreeRefspecType refspec_type;
  char *cached_refspec;

  /* Version data that goes along with the refspec */
  char *cached_override_commit;

  char *cached_unconfigured_state;
  char **cached_initramfs_args;
  GHashTable *cached_initramfs_etc_files;       /* set of paths */
  GHashTable *cached_packages;                  /* set of reldeps */
  GHashTable *cached_modules_enable;            /* set of module specs to enable */
  GHashTable *cached_modules_install;           /* set of module specs to install */
  GHashTable *cached_local_packages;            /* NEVRA --> header sha256 */
  /* GHashTable *cached_overrides_replace;         XXX: NOT IMPLEMENTED YET */
  GHashTable *cached_overrides_local_replace;   /* NEVRA --> header sha256 */
  GHashTable *cached_overrides_remove;          /* set of pkgnames (no EVRA) */
};

static GKeyFile *
keyfile_dup (GKeyFile *kf)
{
  GKeyFile *ret = g_key_file_new ();
  gsize length = 0;
  g_autofree char *data = g_key_file_to_data (kf, &length, NULL);

  g_key_file_load_from_data (ret, data, length, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return ret;
}

/* take <nevra:sha256> entries from keyfile and inserts them into hash table */
static gboolean
parse_packages_strv (GKeyFile *kf,
                     const char *group,
                     const char *key,
                     gboolean    has_sha256,
                     GHashTable *ht,
                     GError    **error)
{
  g_auto(GStrv) packages = g_key_file_get_string_list (kf, group, key, NULL, NULL);

  for (char **it = packages; it && *it; it++)
    {
      if (has_sha256)
        {
          const char *nevra = *it;
          g_autofree char *sha256 = NULL;
          if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", nevra);
          g_hash_table_replace (ht, g_strdup (nevra), util::move_nullify (sha256));
        }
      else
        {
          g_hash_table_add (ht, util::move_nullify (*it));
        }
    }

  return TRUE;
}

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile         *origin,
                                GError          **error)
{
  g_autoptr(RpmOstreeOrigin) ret = NULL;

  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->kf = keyfile_dup (origin);

  ret->cached_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_enable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_modules_install = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_local_packages =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_local_replace =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_remove =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_initramfs_etc_files =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  ret->cached_unconfigured_state = g_key_file_get_string (ret->kf, "origin", "unconfigured-state", NULL);

  g_autofree char *refspec = g_key_file_get_string (ret->kf, "origin", "refspec", NULL);
  if (!refspec)
    {
      refspec = g_key_file_get_string (ret->kf, "origin", "baserefspec", NULL);
      if (!refspec)
        return (RpmOstreeOrigin *)glnx_null_throw (error, "No origin/refspec, or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree");
    }

  if (!rpmostree_refspec_classify (refspec, &ret->refspec_type, NULL, error))
    return FALSE;
  /* Note the lack of a prefix here so that code that just calls
   * rpmostree_origin_get_refspec() in the ostree:// case
   * sees it without the prefix for compatibility.
   */
  ret->cached_refspec = util::move_nullify (refspec);
  ret->cached_override_commit =
    g_key_file_get_string (ret->kf, "origin", "override-commit", NULL);

  if (!parse_packages_strv (ret->kf, "packages", "requested", FALSE,
                            ret->cached_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "packages", "requested-local", TRUE,
                            ret->cached_local_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "modules", "enable", FALSE,
                            ret->cached_modules_enable, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "modules", "install", FALSE,
                            ret->cached_modules_install, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "remove", FALSE,
                            ret->cached_overrides_remove, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "replace-local", TRUE,
                            ret->cached_overrides_local_replace, error))
    return FALSE;

  g_auto(GStrv) initramfs_etc_files =
    g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-etc", NULL, NULL);
  for (char **f = initramfs_etc_files; f && *f; f++)
    g_hash_table_add (ret->cached_initramfs_etc_files, util::move_nullify (*f));

  ret->cached_initramfs_args =
    g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-args", NULL, NULL);

  return util::move_nullify (ret);
}

RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin)
{
  g_autoptr(GError) local_error = NULL;
  RpmOstreeOrigin *ret = rpmostree_origin_parse_keyfile (origin->kf, &local_error);
  g_assert_no_error (local_error);
  return ret;
}

/* This is useful if the origin is meant to be used to generate a *new* deployment, as
 * opposed to simply gathering information about an existing one. In such cases, there are
 * some things that we do not generally want to apply to a new deployment. */
void
rpmostree_origin_remove_transient_state (RpmOstreeOrigin *origin)
{
  /* first libostree-known things */
  ostree_deployment_origin_remove_transient_state (origin->kf);

  /* this is already covered by the above, but the below also updates the cached value */
  rpmostree_origin_set_override_commit (origin, NULL, NULL);
}

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return origin->cached_refspec;
}

char *
rpmostree_origin_get_full_refspec (RpmOstreeOrigin *origin,
                                   RpmOstreeRefspecType *out_refspectype)
{
  if (out_refspectype)
    *out_refspectype = origin->refspec_type;
  return g_strdup (origin->cached_refspec);
}

void
rpmostree_origin_classify_refspec (RpmOstreeOrigin      *origin,
                                   RpmOstreeRefspecType *out_type,
                                   const char          **out_refspecdata)
{
  *out_type = origin->refspec_type;
  if (out_refspecdata)
    *out_refspecdata = origin->cached_refspec;
}

static char *
keyfile_get_nonempty_string (GKeyFile *kf, const char *section, const char *key)
{
  char *ret = g_key_file_get_string (kf, section, key, NULL);
  if (ret && !*ret)
    g_clear_pointer (&ret, g_free);
  return ret;
}

void
rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin,
                                         char           **custom_type,
                                         char           **custom_description)
{
  *custom_type = keyfile_get_nonempty_string (origin->kf, "origin", "custom-url");
  if (*custom_type)
    *custom_description = keyfile_get_nonempty_string (origin->kf, "origin", "custom-description");
}

GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_packages;
}

GHashTable *
rpmostree_origin_get_modules_enable (RpmOstreeOrigin *origin)
{
  return origin->cached_modules_enable;
}

GHashTable *
rpmostree_origin_get_modules_install (RpmOstreeOrigin *origin)
{
  return origin->cached_modules_install;
}

GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_local_packages;
}

GHashTable *
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_remove;
}

GHashTable *
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_local_replace;
}

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return origin->cached_override_commit;
}

GHashTable *
rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return origin->cached_initramfs_etc_files;
}

gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "regenerate-initramfs", NULL);
}

const char *const*
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (const char * const*)origin->cached_initramfs_args;
}

const char*
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return origin->cached_unconfigured_state;
}

/* Determines whether the origin hints at local assembly being required. In some
 * cases, no assembly might actually be required (e.g. if requested packages are
 * already in the base). IOW:
 *    FALSE --> definitely does not require local assembly
 *    TRUE  --> maybe requires assembly, need to investigate further by doing work
 */
gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return 
        rpmostree_origin_get_cliwrap (origin) || 
        rpmostree_origin_get_regenerate_initramfs (origin) ||
        (g_hash_table_size (origin->cached_initramfs_etc_files) > 0) ||
        (g_hash_table_size (origin->cached_packages) > 0) ||
        (g_hash_table_size (origin->cached_modules_install) > 0) ||
        /* Technically, alone it doesn't require require assembly, but it still
         * requires fetching repo metadata to validate (remember: modules are a
         * pure rpmmd concept). This means we may pay the cost of an unneeded
         * tree checkout, but it's not worth trying to optimize for it. */
        (g_hash_table_size (origin->cached_modules_enable) > 0) ||
        (g_hash_table_size (origin->cached_local_packages) > 0) ||
        (g_hash_table_size (origin->cached_overrides_local_replace) > 0) ||
        (g_hash_table_size (origin->cached_overrides_remove) > 0);
}

GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  return keyfile_dup (origin->kf);
}

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value)
{
  return g_key_file_get_string (origin->kf, section, value, NULL);
}

RpmOstreeOrigin*
rpmostree_origin_ref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  origin->refcount++;
  return origin;
}

void
rpmostree_origin_unref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  g_assert_cmpint (origin->refcount, >, 0);
  origin->refcount--;
  if (origin->refcount > 0)
    return;
  g_key_file_unref (origin->kf);
  g_free (origin->cached_refspec);
  g_free (origin->cached_unconfigured_state);
  g_strfreev (origin->cached_initramfs_args);
  g_clear_pointer (&origin->cached_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_modules_enable, g_hash_table_unref);
  g_clear_pointer (&origin->cached_modules_install, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_local_replace, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_remove, g_hash_table_unref);
  g_clear_pointer (&origin->cached_initramfs_etc_files, g_hash_table_unref);
  g_free (origin);
}

static void
update_string_list_from_hash_table (GKeyFile *kf,
                                    const char *group,
                                    const char *key,
                                    GHashTable *values)
{
  g_autofree char **strv = (char**)g_hash_table_get_keys_as_array (values, NULL);
  g_key_file_set_string_list (kf, group, key, (const char *const*)strv, g_strv_length (strv));
}

void
rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin,
                                            char **paths,
                                            gboolean *out_changed)
{
  gboolean changed = FALSE;
  for (char **path = paths; path && *path; path++)
    changed = (g_hash_table_add (origin->cached_initramfs_etc_files, g_strdup (*path)) || changed);

  if (changed)
    update_string_list_from_hash_table (origin->kf, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;
}

void
rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin,
                                              char **paths,
                                              gboolean *out_changed)
{
  gboolean changed = FALSE;
  for (char **path = paths; path && *path; path++)
    changed = (g_hash_table_remove (origin->cached_initramfs_etc_files, *path) || changed);

  if (changed)
    update_string_list_from_hash_table (origin->kf, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;
}

void
rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin,
                                                  gboolean *out_changed)
{
  const gboolean changed = (g_hash_table_size (origin->cached_initramfs_etc_files) > 0);
  g_hash_table_remove_all (origin->cached_initramfs_etc_files);
  if (changed)
    update_string_list_from_hash_table (origin->kf, "rpmostree", "initramfs-etc",
                                        origin->cached_initramfs_etc_files);
  if (out_changed)
    *out_changed = changed;
}

void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin,
                                           gboolean regenerate,
                                           char **args)
{
  const char *section = "rpmostree";
  const char *regeneratek = "regenerate-initramfs";
  const char *argsk = "initramfs-args";

  g_strfreev (origin->cached_initramfs_args);

  if (regenerate)
    {
      g_key_file_set_boolean (origin->kf, section, regeneratek, TRUE);
      if (args && *args)
        {
          g_key_file_set_string_list (origin->kf, section, argsk,
                                      (const char *const*)args,
                                      g_strv_length (args));
        }
      else
        g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }
  else
    {
      g_key_file_remove_key (origin->kf, section, regeneratek, NULL);
      g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }

  origin->cached_initramfs_args =
    g_key_file_get_string_list (origin->kf, "rpmostree", "initramfs-args", NULL, NULL);
}

void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin,
                                      const char      *checksum,
                                      const char      *version)
{
  if (checksum != NULL)
    {
      g_key_file_set_string (origin->kf, "origin", "override-commit", checksum);

      /* Add a comment with the version, to be nice. */
      if (version != NULL)
        {
          g_autofree char *comment =
            g_strdup_printf ("Version %s [%.10s]", version, checksum);
          g_key_file_set_comment (origin->kf, "origin", "override-commit", comment, NULL);
        }
    }
  else
    {
      g_key_file_remove_key (origin->kf, "origin", "override-commit", NULL);
    }

  g_free (origin->cached_override_commit);
  origin->cached_override_commit = g_strdup (checksum);
}

gboolean
rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "ex-cliwrap", NULL);
}

void
rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, gboolean cliwrap)
{
  const char k[] = "rpmostree";
  const char v[] = "ex-cliwrap";
  if (cliwrap)
    g_key_file_set_boolean (origin->kf, k, v, TRUE);
  else
    g_key_file_remove_key (origin->kf, k, v, NULL);
}

gboolean
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin,
                                    const char      *new_refspec,
                                    const char      *custom_origin_url,
                                    const char      *custom_origin_description,
                                    GError         **error)
{
  /* Require non-empty strings */
  if (custom_origin_url)
    {
      g_assert (*custom_origin_url);
      g_assert (custom_origin_description && *custom_origin_description);
    }

   /* We don't want to carry any commit overrides or version pinning during a
   * rebase by default.
   */
  rpmostree_origin_set_override_commit (origin, NULL, NULL);

  /* See related code in rpmostree_origin_parse_keyfile() */
  const char *refspecdata;
  if (!rpmostree_refspec_classify (new_refspec, &origin->refspec_type, &refspecdata, error))
    return FALSE;
  g_free (origin->cached_refspec);
  origin->cached_refspec = g_strdup (refspecdata);
  switch (origin->refspec_type)
    {
    case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
    case RPMOSTREE_REFSPEC_TYPE_OSTREE:
      {
        const char *refspec_key =
          g_key_file_has_key (origin->kf, "origin", "baserefspec", NULL) ?
          "baserefspec" : "refspec";
        g_key_file_set_string (origin->kf, "origin", refspec_key, origin->cached_refspec);
        if (!custom_origin_url)
          {
            g_key_file_remove_key (origin->kf, "origin", "custom-url", NULL);
            g_key_file_remove_key (origin->kf, "origin", "custom-description", NULL);
          }
        else
          {
            /* Custom origins have to be checksums */
            g_assert_cmpint (origin->refspec_type, ==, RPMOSTREE_REFSPEC_TYPE_CHECKSUM);
            g_key_file_set_string (origin->kf, "origin", "custom-url", custom_origin_url);
            if (custom_origin_description)
              g_key_file_set_string (origin->kf, "origin", "custom-description", custom_origin_description);
          }
      }
      break;
    }

  return TRUE;
}

gboolean
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin,
                             const char      *new_refspec,
                             GError         **error)
{
  return rpmostree_origin_set_rebase_custom (origin, new_refspec, NULL, NULL, error);
}

// Switch to `baserefspec` when changing the origin
// to something core ostree doesnt't understand, i.e.
// when `ostree admin upgrade` would no longer do the right thing.
static void
sync_baserefspec (RpmOstreeOrigin *origin)
{
  if (rpmostree_origin_may_require_local_assembly (origin))
    {
      g_key_file_set_value (origin->kf, "origin", "baserefspec",
                            origin->cached_refspec);
      g_key_file_remove_key (origin->kf, "origin", "refspec", NULL);
    }
  else
    {
      g_key_file_set_value (origin->kf, "origin", "refspec",
                            origin->cached_refspec);
      g_key_file_remove_key (origin->kf, "origin", "baserefspec", NULL);
    }
}

static void
update_keyfile_pkgs_from_cache (RpmOstreeOrigin *origin,
                                const char      *group,
                                const char      *key,
                                GHashTable      *pkgs,
                                gboolean         has_csum)
{
  /* we're abusing a bit the concept of cache here, though
   * it's just easier to go from cache to origin */

  if (g_hash_table_size (pkgs) == 0)
    {
      g_key_file_remove_key (origin->kf, group, key, NULL);
      sync_baserefspec (origin);
      return;
    }

  if (has_csum)
    {
      g_autoptr(GPtrArray) pkg_csum =
        g_ptr_array_new_with_free_func (g_free);
      GLNX_HASH_TABLE_FOREACH_KV (pkgs, const char*, k, const char*, v)
        g_ptr_array_add (pkg_csum, g_strconcat (v, ":", k, NULL));
      g_key_file_set_string_list (origin->kf, group, key,
                                  (const char *const*)pkg_csum->pdata,
                                  pkg_csum->len);
    }
  else
    {
      update_string_list_from_hash_table (origin->kf, group, key, pkgs);
    }

  sync_baserefspec (origin);
}

gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin   *origin,
                               char             **packages,
                               gboolean           local,
                               gboolean           allow_existing,
                               gboolean          *out_changed,
                               GError           **error)
{
  gboolean changed = FALSE;

  for (char **it = packages; it && *it; it++)
    {
      gboolean requested, requested_local;
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (local)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      requested = g_hash_table_contains (origin->cached_packages, pkg);
      requested_local = g_hash_table_contains (origin->cached_local_packages, pkg);

      /* The list of packages is really a list of provides, so string equality
       * is a bit weird here. Multiple provides can resolve to the same package
       * and we allow that. But still, let's make sure that silly users don't
       * request the exact string. */

      /* Also note that we check in *both* the requested and the requested-local
       * list: requested-local pkgs are treated like requested pkgs in the core.
       * The only "magical" thing about them is that requested-local pkgs are
       * specifically looked for in the pkgcache. Additionally, making sure the
       * strings are unique allow `rpm-ostree uninstall` to know exactly what
       * the user means. */

      if (requested || requested_local)
        {
          if (allow_existing)
            continue;

          if (requested)
            return glnx_throw (error, "Package/capability '%s' is already requested", pkg);
          else
            return glnx_throw (error, "Package '%s' is already layered", pkg);
        }

      if (local)
        g_hash_table_insert (origin->cached_local_packages,
                             g_strdup (pkg), util::move_nullify (sha256));
      else
        g_hash_table_add (origin->cached_packages, g_strdup (pkg));
      changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages",
                                    local ? "requested-local" : "requested",
                                    local ? origin->cached_local_packages
                                          : origin->cached_packages, local);

  *out_changed = changed;
  return TRUE;
}

static gboolean
build_name_to_nevra_map (GHashTable  *nevras,
                         GHashTable **out_name_to_nevra,
                         GError     **error)
{
  g_autoptr(GHashTable) name_to_nevra = /* nevra vals owned by @nevras */
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GLNX_HASH_TABLE_FOREACH (nevras, const char*, nevra)
    {
      g_autofree char *name = NULL;
      if (!rpmostree_decompose_nevra (nevra, &name, NULL, NULL, NULL, NULL, error))
        return FALSE;
      g_hash_table_insert (name_to_nevra, util::move_nullify (name), (gpointer)nevra);
    }

  *out_name_to_nevra = util::move_nullify (name_to_nevra);
  return TRUE;
}

gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin  *origin,
                                  char            **packages,
                                  gboolean          allow_noent,
                                  gboolean         *out_changed,
                                  GError          **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;

  /* lazily calculated */
  g_autoptr(GHashTable) name_to_nevra = NULL;

  for (char **it = packages; it && *it; it++)
    {
      /* really, either a NEVRA (local RPM) or freeform provides request (from repo) */
      const char *package = *it;
      if (g_hash_table_remove (origin->cached_local_packages, package))
        local_changed = TRUE;
      else if (g_hash_table_remove (origin->cached_packages, package))
        changed = TRUE;
      else
        {
          if (!name_to_nevra)
            {
              if (!build_name_to_nevra_map (origin->cached_local_packages,
                                            &name_to_nevra, error))
                return FALSE;
            }

          if (g_hash_table_contains (name_to_nevra, package))
            {
              g_assert (g_hash_table_remove (origin->cached_local_packages,
                                             g_hash_table_lookup (name_to_nevra, package)));
              local_changed = TRUE;
            }
          else if (!allow_noent)
            return glnx_throw (error, "Package/capability '%s' is not currently requested",
                               package);
        }
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested",
                                    origin->cached_packages, FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);

  sync_baserefspec (origin);
  *out_changed = changed || local_changed;
  return TRUE;
}

gboolean
rpmostree_origin_add_modules (RpmOstreeOrigin  *origin,
                              char           **modules,
                              gboolean         enable_only,
                              gboolean        *out_changed,
                              GError         **error)
{
  const char *key = enable_only ? "enable" : "install";
  GHashTable *target = enable_only ? origin->cached_modules_enable
                                   : origin->cached_modules_install;
  gboolean changed = FALSE;
  for (char **mod = modules; mod && *mod; mod++)
    changed = (g_hash_table_add (target, g_strdup (*mod)) || changed);

  if (changed)
    update_string_list_from_hash_table (origin->kf, "modules", key, target);
  if (out_changed)
    *out_changed = changed;

  return TRUE;
}

gboolean
rpmostree_origin_remove_modules (RpmOstreeOrigin  *origin,
                                 char           **modules,
                                 gboolean         enable_only,
                                 gboolean        *out_changed,
                                 GError         **error)
{
  const char *key = enable_only ? "enable" : "install";
  GHashTable *target = enable_only ? origin->cached_modules_enable
                                   : origin->cached_modules_install;
  gboolean changed = FALSE;
  for (char **mod = modules; mod && *mod; mod++)
    changed = (g_hash_table_remove (target, *mod) || changed);

  if (changed)
    update_string_list_from_hash_table (origin->kf, "modules", key, target);
  if (out_changed)
    *out_changed = changed;

  return TRUE;
}

gboolean
rpmostree_origin_remove_all_packages (RpmOstreeOrigin  *origin,
                                      gboolean         *out_changed,
                                      GError          **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;
  gboolean modules_enable_changed = FALSE;
  gboolean modules_install_changed = FALSE;

  if (g_hash_table_size (origin->cached_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_packages);
      changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_local_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_local_packages);
      local_changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_modules_enable) > 0)
    {
      g_hash_table_remove_all (origin->cached_modules_enable);
      modules_enable_changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_modules_install) > 0)
    {
      g_hash_table_remove_all (origin->cached_modules_install);
      modules_install_changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested",
                                    origin->cached_packages, FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);
  if (modules_enable_changed)
    update_keyfile_pkgs_from_cache (origin, "modules", "enable",
                                    origin->cached_modules_enable, FALSE);
  if (modules_install_changed)
    update_keyfile_pkgs_from_cache (origin, "modules", "install",
                                    origin->cached_modules_install, FALSE);
  if (out_changed)
    *out_changed = changed || local_changed || modules_enable_changed || modules_install_changed;
  return TRUE;
}

gboolean
rpmostree_origin_add_overrides (RpmOstreeOrigin  *origin,
                                char            **packages,
                                RpmOstreeOriginOverrideType type,
                                GError          **error)
{
  gboolean changed = FALSE;
  for (char **it = packages; it && *it; it++)
    {
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      /* Check that the same overrides don't already exist. Of course, in the local replace
       * case, this doesn't catch same pkg name but different EVRA; we'll just barf at that
       * later on in the core. This is just an early easy sanity check. */
      if (g_hash_table_contains (origin->cached_overrides_remove, pkg) ||
          g_hash_table_contains (origin->cached_overrides_local_replace, pkg))
        return glnx_throw (error, "Override already exists for package '%s'", pkg);

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        g_hash_table_insert (origin->cached_overrides_local_replace, g_strdup (pkg),
                             util::move_nullify (sha256));
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        g_hash_table_add (origin->cached_overrides_remove, g_strdup (pkg));
      else
        g_assert_not_reached ();

      changed = TRUE;
    }

  if (changed)
    {
      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                        origin->cached_overrides_local_replace, TRUE);
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                        origin->cached_overrides_remove, FALSE);
      else
        g_assert_not_reached ();
    }

  return TRUE;
}

/* Returns FALSE if the override does not exist. */
gboolean
rpmostree_origin_remove_override (RpmOstreeOrigin  *origin,
                                  const char       *package,
                                  RpmOstreeOriginOverrideType type)
{
  if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
    {
      if (!g_hash_table_remove (origin->cached_overrides_local_replace, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                      origin->cached_overrides_local_replace, TRUE);
    }
  else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
    {
      if (!g_hash_table_remove (origin->cached_overrides_remove, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                      origin->cached_overrides_remove, FALSE);
    }
  else
    g_assert_not_reached ();

  return TRUE;
}

gboolean
rpmostree_origin_remove_all_overrides (RpmOstreeOrigin  *origin,
                                       gboolean         *out_changed,
                                       GError          **error)
{
  gboolean remove_changed = (g_hash_table_size (origin->cached_overrides_remove) > 0);
  g_hash_table_remove_all (origin->cached_overrides_remove);

  gboolean local_replace_changed =
    (g_hash_table_size (origin->cached_overrides_local_replace) > 0);
  g_hash_table_remove_all (origin->cached_overrides_local_replace);

  if (remove_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                    origin->cached_overrides_remove, FALSE);
  if (local_replace_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                    origin->cached_overrides_local_replace, TRUE);
  if (out_changed)
    *out_changed = remove_changed || local_replace_changed;
  return TRUE;
}
