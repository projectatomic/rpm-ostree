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
#include <stdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-journal.h>

#include "rpmostree-util.h"
#include "rpmostree-origin.h"
#include "rpmostree-output.h"
#include "libsd-locale-util.h"
#include "libglnx.h"

#define RPMOSTREE_OLD_PKGCACHE_DIR "extensions/rpmostree/pkgcache"

int
rpmostree_ptrarray_sort_compare_strings (gconstpointer ap,
                                         gconstpointer bp)
{
  char **asp = (char**)(gpointer)ap;
  char **bsp = (char**)(gpointer)bp;
  return strcmp (*asp, *bsp);
}

GVariant *
_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                          const char *key,
                                          const GVariantType *fmt,
                                          GError     **error)
{
  GVariant *r = g_variant_dict_lookup_value (dict, key, fmt);
  if (!r)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find metadata key %s (signature %s)", key, (char*)fmt);
      return NULL;
    }
  return r;
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;

  if (!glnx_openat_rdonly (dfd, path, TRUE, &fd, error))
    return FALSE;

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return FALSE;

  g_checksum_update (checksum, (guint8*)g_mapped_file_get_contents (mfile),
                     g_mapped_file_get_length (mfile));

  return TRUE;
}

/* Returns TRUE if a package is originally on a locally-accessible filesystem */
gboolean
rpmostree_pkg_is_local (DnfPackage *pkg)
{
  const char *reponame = dnf_package_get_reponame (pkg);
  return (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0 ||
          dnf_repo_is_local (dnf_package_get_repo (pkg)));
}

/* Returns the local filesystem path for a package; for non-local packages,
 * it must have already been downloaded.
 */
char *
rpmostree_pkg_get_local_path (DnfPackage *pkg)
{
  DnfRepo *pkg_repo = dnf_package_get_repo (pkg);
  const gboolean is_local = rpmostree_pkg_is_local (pkg);
  if (is_local)
    return g_strdup (dnf_package_get_filename (pkg));
  else
    {
      const char *pkg_location = dnf_package_get_location (pkg);
      return g_build_filename (dnf_repo_get_location (pkg_repo),
                               "packages", glnx_basename (pkg_location), NULL);
    }
}

gboolean
rpmostree_check_size_within_limit (guint64     actual,
                                   guint64     limit,
                                   const char *subject,
                                   GError    **error)
{
  if (actual <= limit)
    return TRUE;
  g_autofree char *max_formatted = g_format_size (limit);
  g_autofree char *found_formatted = g_format_size (actual);
  return glnx_throw (error, "Exceeded maximum size %s; %s is of size: %s",
                     max_formatted, subject, found_formatted);
}

/* Convert a "traditional" path (normally from e.g. an RPM) into its final location in
 * ostree */
char*
rpmostree_translate_path_for_ostree (const char *path)
{
  if (g_str_has_prefix (path, "etc/"))
    return g_strconcat ("usr/", path, NULL);
  else if (g_str_has_prefix (path, "boot/"))
    return g_strconcat ("usr/lib/ostree-boot/", path + strlen ("boot/"), NULL);
  /* Special hack for https://bugzilla.redhat.com/show_bug.cgi?id=1290659
   * See also commit 4a86bdd19665700fa308461510c9decd63e31a03
   * and rpmostree_postprocess_selinux_policy_store_location().
   */
  else if (g_str_has_prefix (path, VAR_SELINUX_TARGETED_PATH))
    return g_strconcat ("usr/etc/selinux/targeted/", path + strlen (VAR_SELINUX_TARGETED_PATH), NULL);
  else if (g_str_has_prefix (path, "opt/"))
    return g_strconcat ("usr/lib/", path, NULL);

  return NULL;
}

/* Replace [match_start, match_end) in version with rendered date_fmt.
 *
 * Returns TRUE on success, FALSE otherwise.
 */
static gboolean
handle_date_tag (GString    *prefix,
                 const char *date_fmt,
                 gint        tag_start,
                 gint        tag_end,
                 GError    **error)
{
  g_autoptr(GDateTime) date_time_utc = g_date_time_new_now_utc ();
  g_autofree char *date_time_str = g_date_time_format (date_time_utc,
                                                       date_fmt);
  if (!date_time_str)
    return glnx_throw (error, "error in formatting date string (%s)", date_fmt);
  g_string_erase (prefix, tag_start, tag_end - tag_start);
  g_string_insert (prefix, tag_start, date_time_str);
  return TRUE;
}

/* Increment the version number in last_version, and append it to
 * prefix. increment_start specifies the version number to start at
 * if a version number is not present in last_version.
 * 
 * If a version number cannot be parsed from last_version, or last_version
 * does not begin with prefix, then append increment_start to prefix.
 * 
 * Example:
 * last_version == NULL, prefix == 10, increment_start == 0 --> return 10.0
 * 
 * If both prefix and last_version are equal, then append increment_start.
 * 
 * Example:
 * last_version == 10, prefix == 10, increment_start == 5 --> return 10.5
 * 
 * If increment_start is set to NULL, then append ".1" to prefix if and only
 * if last_version is equal to prefix (a version increment *must* occur to
 * update the version string at this point).
 * 
 * Example:
 * last_version == NULL, prefix == 10, increment_start == NULL --> return 10
 * last_version == 10, prefix == 10, increment_start == NULL --> return 10.1
 * 
 * Returns the next version string.
 */
static char*
increment_version (const char *version_suffix_str,
                   const char *last_version,
                   const char *prefix,
                   const char *increment_start)
{
  const gboolean increment_given = increment_start != NULL;

  char version_suffix = '.';
  if (version_suffix_str)
    {
      g_assert_cmpint (strlen (version_suffix_str), ==, 1);
      version_suffix = version_suffix_str[0];
    }

  g_assert (prefix != NULL);

  g_autofree char *incremented = g_strdup_printf ("%s%c%s", prefix, version_suffix, increment_start);
  if (!last_version || !g_str_has_prefix (last_version, prefix))
    return increment_given ? util::move_nullify (incremented) : g_strdup (prefix);

  if (g_str_equal (last_version, prefix))
    return increment_given ? util::move_nullify (incremented)
                           : g_strdup_printf ("%s%c1", prefix, version_suffix);

  g_assert_cmpuint (strlen (last_version), >, strlen (prefix));
  const char *end = last_version + strlen (prefix);

  if (*end != version_suffix)
    return increment_given ? util::move_nullify (incremented) : g_strdup (prefix);
  ++end;

  unsigned long long num = g_ascii_strtoull (end, NULL, 10);
  return g_strdup_printf ("%s%c%llu", prefix, version_suffix, num + 1);
}

#define VERSION_TAG_REGEX "<([a-zA-Z]+):(.*)?>"

namespace rpmostreecxx {
/* Get the next version, given a version prefix and a last version.
 * Checks for supported version fields in the auto_version_prefix
 * and renders them.
 * 
 * Returns the next version string if successful.
 */
rust::String
util_next_version (rust::Str auto_version_prefix,
                   rust::Str version_suffix_rs, /* Option<&str> */
                   rust::Str last_version_rs)
{
  static gsize tag_regex_initialized;
  static GRegex *tag_regex;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  if (g_once_init_enter (&tag_regex_initialized))
    {
      tag_regex = g_regex_new (VERSION_TAG_REGEX, (GRegexCompileFlags)0, (GRegexMatchFlags)0, NULL);
      g_assert (tag_regex);
      g_once_init_leave (&tag_regex_initialized, 1);
    }

  g_autoptr(GString) next_version = g_string_new ("");
  g_string_append_len (next_version, auto_version_prefix.data(), auto_version_prefix.length());
  g_assert (next_version->str);
  bool date_tag_given = FALSE;

  g_autoptr(GMatchInfo) tag_match_info = NULL;
  g_regex_match (tag_regex, next_version->str,
                 (GRegexMatchFlags)0, &tag_match_info);

  while (g_match_info_matches (tag_match_info))
    {
      gint match_start, match_end;
      g_assert (g_match_info_fetch_pos (tag_match_info, 0, &match_start, &match_end));

      g_autofree char* tag = g_match_info_fetch (tag_match_info, 1);
      if (g_str_equal (tag, "date"))
        {
          g_autofree char* date_fmt = g_match_info_fetch (tag_match_info, 2);
          if (!handle_date_tag (next_version, date_fmt, match_start, match_end, error))
            util::throw_gerror(local_error);
          date_tag_given = TRUE;
        }
      else
        g_printerr ("ignoring unknown tag %s\n", tag);

      g_match_info_next (tag_match_info, NULL);
    }

  /* Just copy as NUL terminated C strings to avoid rewriting this; also
   * the inner function expects NULL instead of an empty string
   */
  g_autofree char *version_suffix = version_suffix_rs.length() > 0 ? g_strndup (version_suffix_rs.data(), version_suffix_rs.length()) : NULL;
  g_autofree char *last_version = last_version_rs.length() > 0 ? g_strndup (last_version_rs.data(), last_version_rs.length()) : NULL;
  g_autofree char *v = increment_version (version_suffix, last_version, next_version->str, date_tag_given ? "0" : NULL);
  return rust::String(v);
}

// A test function to validate that we can pass glib-rs types
// from Rust back through cxx-rs to C++.
int
testutil_validate_cxxrs_passthrough(OstreeRepo &repo) noexcept
{
  return ostree_repo_get_dfd(&repo);
}


} /* namespace */
#undef VERSION_TAG_REGEX

/* Replace every occurrence of @old in @buf with @new. */
char *
rpmostree_str_replace (const char  *buf,
                       const char  *old,
                       const char  *newv,
                       GError     **error)
{
  g_autofree char *literal_old = g_regex_escape_string (old, -1);
  g_autoptr(GRegex) regex = g_regex_new (literal_old, (GRegexCompileFlags)0, (GRegexMatchFlags)0, error);

  if (regex == NULL)
    return NULL;

  return g_regex_replace_literal (regex, buf, -1, 0, newv, static_cast<GRegexMatchFlags>(0), error);
}

/* FIXME: This is a copy of ot_admin_checksum_version */
char *
rpmostree_checksum_version (GVariant *checksum)
{
  g_autoptr(GVariant) metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

static gboolean
pull_content_only_recurse (OstreeRepo  *dest,
                           OstreeRepo  *src,
                           OstreeRepoCommitTraverseIter *iter,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean done = FALSE;

  while (!done)
    {
      OstreeRepoCommitIterResult iterres =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, error);

      switch (iterres)
        {
        case OSTREE_REPO_COMMIT_ITER_RESULT_ERROR:
          return FALSE;
        case OSTREE_REPO_COMMIT_ITER_RESULT_END:
          done = TRUE;
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_FILE:
          {
            char *name;
            char *checksum;

            ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

            if (!ostree_repo_import_object_from (dest, src, OSTREE_OBJECT_TYPE_FILE,
                                                 checksum, cancellable, error))
              return FALSE;
          }
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_DIR:
          {
            char *name;
            char *content_checksum;
            char *meta_checksum;
            g_autoptr(GVariant) dirtree = NULL;
            ostree_cleanup_repo_commit_traverse_iter
              OstreeRepoCommitTraverseIter subiter = { 0, };

            ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum, &meta_checksum);

            if (!ostree_repo_load_variant (src, OSTREE_OBJECT_TYPE_DIR_TREE,
                                           content_checksum, &dirtree,
                                           error))
              return FALSE;

            if (!ostree_repo_commit_traverse_iter_init_dirtree (&subiter, src, dirtree,
                                                                OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                                error))
              return FALSE;

            if (!pull_content_only_recurse (dest, src, &subiter, cancellable, error))
              return FALSE;
          }
          break;
        }
    }

  return TRUE;
}

/* Migrate only the content (.file) objects from src+src_commit into dest.
 * Used for package layering.
 */
gboolean
rpmostree_pull_content_only (OstreeRepo  *dest,
                             OstreeRepo  *src,
                             const char  *src_commit,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GVariant) commitdata = NULL;
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };

  if (!ostree_repo_load_commit (src, src_commit, &commitdata, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_traverse_iter_init_commit (&iter, src, commitdata,
                                                     OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                     error))
    return FALSE;

  if (!pull_content_only_recurse (dest, src, &iter, cancellable, error))
    return FALSE;

  return TRUE;
}

G_LOCK_DEFINE_STATIC (pathname_cache);

/**
 * rpmostree_file_get_path_cached:
 *
 * Like g_file_get_path(), but returns a constant copy so callers
 * don't need to free the result.
 */
const char *
rpmostree_file_get_path_cached (GFile *file)
{
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark) == 0)
    _file_path_quark = g_quark_from_static_string ("gsystem-file-path");

  G_LOCK (pathname_cache);

  auto path = (const char*)g_object_get_qdata ((GObject*)file, _file_path_quark);
  if (!path)
    {
      path = g_file_get_path (file);
      if (path == NULL)
        {
          G_UNLOCK (pathname_cache);
          return NULL;
        }
      g_object_set_qdata_full ((GObject*)file, _file_path_quark, (char*)path, (GDestroyNotify)g_free);
    }

  G_UNLOCK (pathname_cache);

  return path;
}

gboolean
rpmostree_str_has_prefix_in_strv (const char *str,
                                  char      **prefixes,
                                  int         n)
{
  if (n < 0)
    n = g_strv_length (prefixes);
  for (int i = 0; i < n; i++)
    {
      if (g_str_has_prefix (str, prefixes[i]))
        return TRUE;
    }
  return FALSE;
}

gboolean
rpmostree_str_has_prefix_in_ptrarray (const char *str,
                                      GPtrArray  *prefixes)
{
  return rpmostree_str_has_prefix_in_strv (str, (char**)prefixes->pdata, prefixes->len);
}

/**
 * rpmostree_str_ptrarray_contains:
 *
 * Like g_strv_contains() but for ptrarray.
 * Handles strs==NULL as an empty array.
 */
gboolean
rpmostree_str_ptrarray_contains (GPtrArray  *strs,
                                 const char *str)
{
  g_assert (str);
  if (!strs)
    return FALSE;

  guint n = strs->len;
  for (guint i = 0; i < n; i++)
    {
      if (g_str_equal (str, strs->pdata[i]))
        return TRUE;
    }
  return FALSE;
}

gboolean
rpmostree_deployment_get_layered_info (OstreeRepo        *repo,
                                       OstreeDeployment  *deployment,
                                       gboolean          *out_is_layered,
                                       guint             *out_layer_version,
                                       char             **out_base_layer,
                                       char            ***out_layered_pkgs,
                                       char            ***out_layered_modules,
                                       GVariant         **out_removed_base_pkgs,
                                       GVariant         **out_replaced_base_pkgs,
                                       GError           **error)
{
  const char *csum = ostree_deployment_get_csum (deployment);
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, csum, &commit, NULL, error))
    return FALSE;

  auto layeredmeta = rpmostreecxx::deployment_layeredmeta_from_commit(*deployment, *commit);

  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) dict = g_variant_dict_new (metadata);

  /* only fetch pkgs if we have to */
  g_auto(GStrv) layered_pkgs = NULL;
  g_auto(GStrv) layered_modules = NULL;
  g_autoptr(GVariant) removed_base_pkgs = NULL;
  g_autoptr(GVariant) replaced_base_pkgs = NULL;
  if (layeredmeta.is_layered && (out_layered_pkgs != NULL || out_removed_base_pkgs != NULL))
    {
      /* starting from v1, we no longer embed a treespec in client layers */
      if (layeredmeta.clientlayer_version >= 1)
        {
          g_assert (g_variant_dict_lookup (dict, "rpmostree.packages", "^as",
                                           &layered_pkgs));
        }
      else
        {
          g_autoptr(GVariant) treespec_v = NULL;
          g_autoptr(GVariantDict) treespec = NULL;

          g_assert (g_variant_dict_contains (dict, "rpmostree.spec"));

           /* there should always be a treespec */
          treespec_v = g_variant_dict_lookup_value (dict, "rpmostree.spec",
                                                    G_VARIANT_TYPE ("a{sv}"));
          g_assert (treespec_v);

          /* there should always be a packages entry, even if empty */
          treespec = g_variant_dict_new (treespec_v);
          g_assert (g_variant_dict_lookup (treespec, "packages", "^as",
                                           &layered_pkgs));
        }

      if (layeredmeta.clientlayer_version >= 2)
        {
          removed_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.removed-base-packages",
                                         G_VARIANT_TYPE ("av"));
          g_assert (removed_base_pkgs);

          replaced_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.replaced-base-packages",
                                         G_VARIANT_TYPE ("a(vv)"));
          g_assert (replaced_base_pkgs);
        }

      if (layeredmeta.clientlayer_version >= 5)
        {
          g_assert (g_variant_dict_lookup (dict, "rpmostree.modules", "^as",
                                           &layered_modules));
        }
    }

  /* canonicalize outputs to empty array */

  if (out_is_layered != NULL)
    *out_is_layered = layeredmeta.is_layered;
  if (out_layer_version != NULL)
    *out_layer_version = layeredmeta.clientlayer_version;
  if (out_base_layer != NULL && layeredmeta.is_layered)
    *out_base_layer = util::ruststr_dup_c_optempty(layeredmeta.base_commit);
  if (out_layered_pkgs != NULL)
    {
      if (!layered_pkgs)
        layered_pkgs = g_new0 (char*, 1);
      *out_layered_pkgs = util::move_nullify (layered_pkgs);
    }
  if (out_layered_modules != NULL)
    {
      if (!layered_modules)
        layered_modules = g_new0 (char*, 1);
      *out_layered_modules = util::move_nullify (layered_modules);
    }
  if (out_removed_base_pkgs != NULL)
    {
      if (!removed_base_pkgs)
        removed_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0));
      *out_removed_base_pkgs = util::move_nullify (removed_base_pkgs);
    }
  if (out_replaced_base_pkgs != NULL)
    {
      if (!replaced_base_pkgs)
        replaced_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(vv)"), NULL, 0));
      *out_replaced_base_pkgs = util::move_nullify (replaced_base_pkgs);
    }

  return TRUE;
}

/* Returns the base layer checksum if layered, NULL otherwise. */
gboolean
rpmostree_deployment_get_base_layer (OstreeRepo        *repo,
                                     OstreeDeployment  *deployment,
                                     char             **out_base_layer,
                                     GError           **error)
{
  return rpmostree_deployment_get_layered_info (repo, deployment, NULL, NULL,
                                                out_base_layer, NULL, NULL, NULL, NULL, error);
}

static gboolean
do_pkgcache_migration (OstreeRepo    *repo,
                       OstreeRepo    *pkgcache,
                       guint         *out_n_migrated,
                       GCancellable  *cancellable,
                       GError       **error)
{
  g_autoptr(GHashTable) pkgcache_refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache, "rpmostree/pkg", &pkgcache_refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  if (g_hash_table_size (pkgcache_refs) == 0)
    {
      *out_n_migrated = 0;
      return TRUE; /* Note early return */
    }

  g_autofree char **refs_strv =
    (char **)g_hash_table_get_keys_as_array (pkgcache_refs, NULL);

  /* fd-relative pull API anyone? build the path manually at least to avoid one malloc */
  g_autofree char *pkgcache_uri =
    g_strdup_printf ("file:///proc/self/fd/%d", ostree_repo_get_dfd (pkgcache));
  if (!ostree_repo_pull (repo, pkgcache_uri, refs_strv, OSTREE_REPO_PULL_FLAGS_NONE, NULL,
                         cancellable, error))
    return FALSE;

  *out_n_migrated = g_strv_length (refs_strv);
  return TRUE;
}

gboolean
rpmostree_migrate_pkgcache_repo (OstreeRepo   *repo,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);

  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR, &stbuf,
                                 AT_SYMLINK_NOFOLLOW, error))
    return FALSE;

  if (errno == 0 && S_ISLNK (stbuf.st_mode))
    return TRUE;

  /* if pkgcache exists, we expect it to be valid; we don't want to nuke it just because of
   * a transient error */
  if (errno == 0)
    {
      if (S_ISDIR (stbuf.st_mode))
        {
          auto task = rpmostreecxx::progress_begin_task("Migrating pkgcache");

          g_autoptr(OstreeRepo) pkgcache = ostree_repo_open_at (repo_dfd,
                                                                RPMOSTREE_OLD_PKGCACHE_DIR,
                                                                cancellable, error);
          if (!pkgcache)
            return FALSE;

          guint n_migrated;
          if (!do_pkgcache_migration (repo, pkgcache, &n_migrated, cancellable, error))
            return FALSE;

          auto msg = g_strdup_printf("%u done", n_migrated);
          task->end(msg);
          if (n_migrated > 0)
            sd_journal_print (LOG_INFO, "migrated %u cached package%s to system repo",
                              n_migrated, _NS(n_migrated));
        }

      if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!glnx_shutil_mkdir_p_at (repo_dfd, dirname (strdupa (RPMOSTREE_OLD_PKGCACHE_DIR)),
                                   0755, cancellable, error))
        return FALSE;
    }

  /* leave a symlink for compatibility with older rpm-ostree versions */
  if (symlinkat ("../..", repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR) < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
}

char*
rpmostree_get_deployment_root (OstreeSysroot     *sysroot,
                               OstreeDeployment *deployment)
{
  const char *sysroot_path = gs_file_get_path_cached (ostree_sysroot_get_path (sysroot));
  g_autofree char *deployment_dirpath =
    ostree_sysroot_get_deployment_dirpath (sysroot, deployment);
  return g_build_filename (sysroot_path, deployment_dirpath, NULL);
}

gboolean
rpmostree_decompose_sha256_nevra (const char **nevra, /* gets incremented */
                                  char       **out_sha256,
                                  GError     **error)
{
  const char *sha256_nevra = *nevra;
  g_autofree char *sha256 = NULL;

  if (strlen (sha256_nevra) < 66 || /* 64 + ":" + at least 1 char for nevra */
      sha256_nevra[64] != ':')
    return glnx_throw (error, "Invalid sha256-nevra: %s", sha256_nevra);

  sha256 = g_strndup (sha256_nevra, 64);
  if (!ostree_validate_checksum_string (sha256, error))
    return FALSE;

  *nevra += 65;
  if (out_sha256)
    *out_sha256 = util::move_nullify (sha256);

  return TRUE;
}

/* Comptute SHA-256(dirtree checksum, dirmeta checksum) of a commit;
 * this identifies the checksum of its *contents*, independent of metadata
 * like the commit timestamp.  https://github.com/ostreedev/ostree/issues/1315
 */
char *
rpmostree_commit_content_checksum (GVariant *commit)
{
  g_autoptr(GChecksum) hasher = g_checksum_new (G_CHECKSUM_SHA256);
  char checksum[OSTREE_SHA256_STRING_LEN+1];
  const guint8 *csum;

  /* Hash content checksum */
  g_autoptr(GVariant) csum_bytes = NULL;
  g_variant_get_child (commit, 6, "@ay", &csum_bytes);
  csum = ostree_checksum_bytes_peek (csum_bytes);
  ostree_checksum_inplace_from_bytes (csum, checksum);
  g_checksum_update (hasher, (guint8*)checksum, OSTREE_SHA256_STRING_LEN);
  g_clear_pointer (&csum_bytes, (GDestroyNotify)g_variant_unref);

  /* Hash meta checksum */
  g_variant_get_child (commit, 7, "@ay", &csum_bytes);
  csum = ostree_checksum_bytes_peek (csum_bytes);
  ostree_checksum_inplace_from_bytes (csum, checksum);
  g_checksum_update (hasher, (guint8*)checksum, OSTREE_SHA256_STRING_LEN);

  return g_strdup (g_checksum_get_string (hasher));
}

char*
rpmostree_generate_diff_summary (guint upgraded,
                                 guint downgraded,
                                 guint removed,
                                 guint added)
{
  guint args[] = { upgraded, downgraded, removed, added };
  const char *arg_desc[] = { "upgraded", "downgraded", "removed", "added" };

  g_autoptr(GString) summary = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (args); i++)
    {
      if (args[i] == 0)
        continue;
      if (summary->len > 0)
        g_string_append (summary, ", ");
      g_string_append_printf (summary, "%u %s", args[i], arg_desc[i]);
    }
  return g_string_free (util::move_nullify (summary), FALSE);
}

/* Given the result of rpm_ostree_db_diff(), print it in a nice formatted way for humans. */
void
rpmostree_diff_print_formatted (RpmOstreeDiffPrintFormat format,
                                const char *prefix,
                                guint      max_key_len,
                                GPtrArray *removed,
                                GPtrArray *added,
                                GPtrArray *modified_old,
                                GPtrArray *modified_new)
{
  const char *sprefix = prefix ?: "";
  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  guint upgraded = 0;
  for (guint i = 0; i < modified_old->len; i++)
    {
      auto oldpkg = static_cast<RpmOstreePackage *>(modified_old->pdata[i]);
      auto newpkg = static_cast<RpmOstreePackage *>(modified_new->pdata[i]);
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) > 0)
        continue;

      switch (format)
        {
        /* we deal with summary after; just need a count */
        case RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY:
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED:
          g_print ("  %*s%s %s %s -> %s\n", max_key_len, i == 0 ? "Upgraded" : "",
                   i == 0 ? ":" : " ", name, rpm_ostree_package_get_evr (oldpkg),
                                             rpm_ostree_package_get_evr (newpkg));
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE:
          if (upgraded == 0)
            g_print ("%sUpgraded:\n", sprefix);
          g_print ("  %s %s -> %s\n", name,
                   rpm_ostree_package_get_evr (oldpkg),
                   rpm_ostree_package_get_evr (newpkg));
          break;
        default:
          g_assert_not_reached ();
        }
      upgraded++;
    }

  guint downgraded = 0;
  for (guint i = 0; i < modified_old->len; i++)
    {
      auto oldpkg = static_cast<RpmOstreePackage *>(modified_old->pdata[i]);
      auto newpkg = static_cast<RpmOstreePackage *>(modified_new->pdata[i]);
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) < 0)
        continue;

      switch (format)
        {
        /* we deal with summary after; just need a count */
        case RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY:
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED:
          {
            g_autofree char *header_owned = prefix ? g_strconcat (prefix, "Downgraded", NULL) : NULL;
            const char *header = header_owned ?: "Downgraded";
            g_print ("  %*s%s %s %s -> %s\n", max_key_len, i == 0 ? header : "",
                     i == 0 ? ":" : " ", name, rpm_ostree_package_get_evr (oldpkg),
                                               rpm_ostree_package_get_evr (newpkg));
          }
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE:
          if (downgraded == 0)
            g_print ("%sDowngraded:\n", sprefix);
          g_print ("  %s %s -> %s\n", name,
                   rpm_ostree_package_get_evr (oldpkg),
                   rpm_ostree_package_get_evr (newpkg));
          break;
        default:
          g_assert_not_reached ();
        }
      downgraded++;
    }

  /* now that we have all the counts, we can handle the SUMMARY path */
  if (format == RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY)
    {
      g_autofree char *diff_summary =
        rpmostree_generate_diff_summary (upgraded, downgraded, removed->len, added->len);
      g_autofree char *header_owned = prefix ? g_strconcat (prefix, "Diff", NULL) : NULL;
      const char *header = header_owned ?: "Diff";
      if (strlen (diff_summary) > 0) /* only print if we have something to print */
        g_print ("  %*s: %s\n", max_key_len, header, diff_summary);
      return;
    }

  for (guint i = 0; i < removed->len; i++)
    {
      auto pkg = static_cast<RpmOstreePackage *>(removed->pdata[i]);
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      switch (format)
        {
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED:
          {
            g_autofree char *header_owned = prefix ? g_strconcat (prefix, "Removed", NULL) : NULL;
            const char *header = header_owned ?: "Removed";
            g_print ("  %*s%s %s\n", max_key_len, i == 0 ? header : "",
                     i == 0 ? ":" : " ", nevra);
          }
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE:
          if (i == 0)
            g_print ("%sRemoved:\n", sprefix);
          g_print ("  %s\n", nevra);
          break;
        default:
          g_assert_not_reached ();
        }
    }

  for (guint i = 0; i < added->len; i++)
    {
      auto pkg = static_cast<RpmOstreePackage *>(added->pdata[i]);
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      switch (format)
        {
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED:
          {
            g_autofree char *header_owned = prefix ? g_strconcat (prefix, "Added", NULL) : NULL;
            const char *header = header_owned ?: "Added";
            g_print ("  %*s%s %s\n", max_key_len, i == 0 ? header : "",
                     i == 0 ? ":" : " ", nevra);
          }
          break;
        case RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE:
          if (i == 0)
            g_print ("%sAdded:\n", sprefix);
          g_print ("  %s\n", nevra);
          break;
        default:
          g_assert_not_reached ();
        }
    }
}

static void
variant_diff_print_modified (guint       max_key_len,
                             GVariant   *modified,
                             const char *type)
{
  guint n = g_variant_n_children (modified);
  for (guint i = 0; i < n; i++)
    {
      const char *name, *evr_old, *evr_new;
      g_variant_get_child (modified, i, "(u&s(&ss)(&ss))",
                           NULL, &name, &evr_old, NULL, &evr_new, NULL);
      g_print ("  %*s%s %s %s -> %s\n", max_key_len, i == 0 ? type : "", i == 0 ? ":" : " ",
               name, evr_old, evr_new);
    }
}

static void
variant_diff_print_singles (guint       max_key_len,
                            GVariant   *singles,
                            const char *type)
{
  guint n = g_variant_n_children (singles);
  for (guint i = 0; i < n; i++)
    {
      const char *name, *evr, *arch;
      g_variant_get_child (singles, i, "(u&s&s&s)", NULL, &name, &evr, &arch);
      g_print ("  %*s%s %s-%s.%s\n", max_key_len, i == 0 ? type : "", i == 0 ? ":" : " ",
               name, evr, arch);
    }
}

void
rpmostree_variant_diff_print_formatted (guint     max_key_len,
                                        GVariant *upgraded,
                                        GVariant *downgraded,
                                        GVariant *removed,
                                        GVariant *added)
{
  variant_diff_print_modified (max_key_len, upgraded, "Upgraded");
  variant_diff_print_modified (max_key_len, downgraded, "Downgraded");
  variant_diff_print_singles (max_key_len, removed, "Removed");
  variant_diff_print_singles (max_key_len, added, "Added");
}

static int
pkg_cmp_end (RpmOstreePackage *a, RpmOstreePackage *b)
{
  if (!b)
    return -1;
  if (!a)
    return +1;

  return rpm_ostree_package_cmp (a, b);
}

/* Given the result of rpm_ostree_db_diff(), print it in diff format for scripts. */
void
rpmostree_diff_print (GPtrArray *removed,
                      GPtrArray *added,
                      GPtrArray *modified_old,
                      GPtrArray *modified_new)
{
  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  const guint an = added->len;
  const guint rn = removed->len;
  const guint mn = modified_old->len;

  guint cur_a = 0;
  guint cur_r = 0;
  guint cur_m = 0;
  while (cur_a < an || cur_r < rn || cur_m < mn)
    {
      auto pkg_a = (RpmOstreePackage *)(cur_a < an ? added->pdata[cur_a] : NULL);
      auto pkg_r = (RpmOstreePackage *)(cur_r < rn ? removed->pdata[cur_r] : NULL);
      auto pkg_m = (RpmOstreePackage *)(cur_m < mn ? modified_old->pdata[cur_m] : NULL);

      if (pkg_cmp_end (pkg_m, pkg_r) < 0)
        if (pkg_cmp_end (pkg_m, pkg_a) < 0)
          { /* mod is first */
            g_print ("!%s\n", rpm_ostree_package_get_nevra (pkg_m));
            g_print ("=%s\n", rpm_ostree_package_get_nevra (static_cast<RpmOstreePackage*>(modified_new->pdata[cur_m])));
            cur_m++;
          }
        else
          { /* add is first */
            g_print ("+%s\n", rpm_ostree_package_get_nevra (pkg_a));
            cur_a++;
          }
      else
        if (pkg_cmp_end (pkg_r, pkg_a) < 0)
          { /* del is first */
            g_print ("-%s\n", rpm_ostree_package_get_nevra (pkg_r));
            cur_r++;
          }
        else
          { /* add is first */
            g_print ("+%s\n", rpm_ostree_package_get_nevra (pkg_a));
            cur_a++;
          }
    }
}

/* Copy of ot_variant_bsearch_str() from libostree
 * @array: A GVariant array whose first element must be a string
 * @str: Search for this string
 * @out_pos: Output position
 *
 * Binary search in a GVariant array, which must be of the form 'a(s...)', where
 * '...' may be anything.  The array elements must be sorted. If duplicates are
 * present, the earliest match will be returned.
 *
 * Returns: %TRUE iff found
 */
gboolean
rpmostree_variant_bsearch_str (GVariant   *array,
                               const char *str,
                               int        *out_pos)
{
  const gsize n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  gsize imax = n - 1;
  gsize imin = 0;
  gsize imid = -1;
  while (imax >= imin)
    {
      const char *cur;

      imid = (imin + imax) / 2;

      g_autoptr(GVariant) child = g_variant_get_child_value (array, imid);
      g_variant_get_child (child, 0, "&s", &cur, NULL);

      int cmp = strcmp (cur, str);
      if (cmp < 0)
        imin = imid + 1;
      else if (cmp > 0)
        {
          if (imid == 0)
            break;
          imax = imid - 1;
        }
      else
        {
          if (imin < imid)
            imax = imid;
          else
            {
              *out_pos = imid;
              return TRUE;
            }
        }
    }

  *out_pos = imid;
  return FALSE;
}

const char*
rpmostree_auto_update_policy_to_str (RpmostreedAutomaticUpdatePolicy policy,
                                     GError **error)
{
  switch (policy)
    {
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE:
      return "none";
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK:
      return "check";
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE:
      return "stage";
    default:
      return (char*)glnx_null_throw (error, "Invalid policy value %u", policy);
    }
}

gboolean
rpmostree_str_to_auto_update_policy (const char *str,
                                     RpmostreedAutomaticUpdatePolicy *out_policy,
                                     GError    **error)
{
  g_assert (str);
  if (g_str_equal (str, "none") || g_str_equal (str, "off"))
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE;
  else if (g_str_equal (str, "check"))
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK;
  else if (g_str_equal (str, "stage") || g_str_equal (str, "ex-stage") /* backcompat */)
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE;
  else
    return glnx_throw (error, "Invalid value for AutomaticUpdatePolicy: '%s'", str);
  return TRUE;
}

/* Get an ISO8601-formatted string for UTC timestamp t (seconds) */
char*
rpmostree_timestamp_str_from_unix_utc (guint64 t)
{
  g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (t);
  if (timestamp != NULL)
    return g_date_time_format (timestamp, "%FT%H:%M:%SZ");
  return g_strdup_printf ("(invalid timestamp)");
}

char*
rpmostree_maybe_shell_quote (const char *s)
{
  static gsize regex_initialized;
  static GRegex *safe_chars_regex;
  if (g_once_init_enter (&regex_initialized))
    {
      safe_chars_regex = g_regex_new ("^[[:alnum:]-._/=:]+$", (GRegexCompileFlags)0, (GRegexMatchFlags)0, NULL);
      g_assert (safe_chars_regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (g_regex_match (safe_chars_regex, s, (GRegexMatchFlags)0, 0))
    return NULL;
  return g_shell_quote (s);
}

/* Given an error, log it to the systemd journal; use this
 * for code paths where we can't easily propagate it back
 * up the stack - particularly "shouldn't happen" errors.
 */
void
rpmostree_journal_error (GError *error)
{
  sd_journal_print (LOG_WARNING, "%s", error->message);
}

void
rpmostree_variant_be_to_native (GVariant **v)
{
  g_assert (v);
  g_assert (*v);
  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      GVariant *swapped = g_variant_byteswap (*v);
      GVariant *orig = *v;
      *v = swapped;
      g_variant_unref (orig);
    }
}

void
rpmostree_variant_native_to_be (GVariant **v)
{
  rpmostree_variant_be_to_native (v);
}
