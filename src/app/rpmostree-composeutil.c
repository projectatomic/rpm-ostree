/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2018 Colin Walters <walters@verbum.org>
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
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <libdnf/dnf-repo.h>
#include <sys/mount.h>
#include <stdio.h>
#include <linux/magic.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-composeutil.h"
#include "rpmostree-util.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

gboolean
rpmostree_composeutil_checksum (HyGoal             goal,
                                OstreeRepo        *repo,
                                RORTreefile       *tf,
                                JsonObject        *treefile,
                                char             **out_checksum,
                                GError           **error)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  /* Hash in the treefile inputs (this includes all externals like postprocess, add-files,
   * etc... and the final flattened treefile -- see treefile.rs for more details). */
  g_checksum_update (checksum, (guint8*)ror_treefile_get_checksum (tf), -1);

  /* Hash in each package */
  if (!rpmostree_dnf_add_checksum_goal (checksum, goal, NULL, error))
    return FALSE;

  if (tf)
    {
      GLNX_AUTO_PREFIX_ERROR ("Resolving ostree-layers", error);
      g_auto(GStrv) layers = ror_treefile_get_all_ostree_layers (tf);
      for (char **iter = layers; iter && *iter; iter++)
        {
          const char *layer = *iter;
          g_autofree char *rev = NULL;
          if (!ostree_repo_resolve_rev (repo, layer, FALSE, &rev, error))
            return FALSE;
          g_autoptr(GVariant) commit = NULL;
          if (!ostree_repo_load_commit (repo, rev, &commit, NULL, error))
            return FALSE;
          const char *content_checksum = ostree_commit_get_content_checksum (commit);
          g_checksum_update (checksum, (const guint8*) content_checksum, strlen (content_checksum));
        }
    }

  *out_checksum = g_strdup (g_checksum_get_string (checksum));
  return TRUE;
}

/* Prepare /dev in the target root with the API devices.  TODO:
 * Delete this when we implement https://github.com/projectatomic/rpm-ostree/issues/729
 */
gboolean
rpmostree_composeutil_legacy_prep_dev (int         rootfs_dfd,
                                       GError    **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Preparing dev (legacy)", error);

  glnx_autofd int src_fd = openat (AT_FDCWD, "/dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (src_fd == -1)
    return glnx_throw_errno (error);

  if (mkdirat (rootfs_dfd, "dev", 0755) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno (error);
    }

  glnx_autofd int dest_fd = openat (rootfs_dfd, "dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dest_fd == -1)
    return glnx_throw_errno (error);

  static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
  for (guint i = 0; i < G_N_ELEMENTS (devnodes); i++)
    {
      const char *nodename = devnodes[i];
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (src_fd, nodename, &stbuf, 0, error))
        return FALSE;
      if (errno == ENOENT)
        continue;

      if (mknodat (dest_fd, nodename, stbuf.st_mode, stbuf.st_rdev) != 0)
        return glnx_throw_errno_prefix (error, "mknodat");
      if (fchmodat (dest_fd, nodename, stbuf.st_mode, 0) != 0)
        return glnx_throw_errno_prefix (error, "fchmodat");
    }

  { GLNX_AUTO_PREFIX_ERROR ("Testing /dev/null in target root (is nodev set?)", error);
    glnx_autofd int devnull_fd = -1;
    if (!glnx_openat_rdonly (dest_fd, "null", TRUE, &devnull_fd, error))
      return FALSE;
    char buf[1];
    ssize_t s = read (devnull_fd, buf, sizeof (buf));
    if (s < 0)
      return glnx_throw_errno_prefix (error, "read");
  }

  return TRUE;
}


gboolean
rpmostree_composeutil_sanity_checks (RORTreefile  *tf,
                                     JsonObject   *treefile,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  int fd = ror_treefile_get_postprocess_script_fd (tf);
  if (fd != -1)
    {
      /* Check that postprocess-script is executable; https://github.com/projectatomic/rpm-ostree/issues/817 */
      struct stat stbuf;
      if (!glnx_fstat (fd, &stbuf, error))
        return glnx_prefix_error (error, "postprocess-script");

      if ((stbuf.st_mode & S_IXUSR) == 0)
        return glnx_throw (error, "postprocess-script must be executable");
    }

  /* Insert other sanity checks here */

  return TRUE;
}

static gboolean
set_keyfile_string_array_from_json (GKeyFile    *keyfile,
                                    const char  *keyfile_group,
                                    const char  *keyfile_key,
                                    JsonArray   *a,
                                    GError     **error)
{
  g_autoptr(GPtrArray) instlangs_v = g_ptr_array_new ();

  guint len = json_array_get_length (a);
  for (guint i = 0; i < len; i++)
    {
      const char *elt = _rpmostree_jsonutil_array_require_string_element (a, i, error);

      if (!elt)
        return FALSE;

      g_ptr_array_add (instlangs_v, (char*)elt);
    }

  g_key_file_set_string_list (keyfile, keyfile_group, keyfile_key,
                              (const char*const*)instlangs_v->pdata, instlangs_v->len);

  return TRUE;
}

static gboolean
treespec_bind_array (JsonObject *treedata,
                     GKeyFile   *ts,
                     const char *src_name,
                     const char *dest_name,
                     gboolean    required,
                     GError    **error)
{
  if (!json_object_has_member (treedata, src_name))
    {
      if (required)
        return glnx_throw (error, "Treefile is missing required \"%s\" member", src_name);
      return TRUE;
    }
  JsonArray *a = json_object_get_array_member (treedata, src_name);
  g_assert (a);
  return set_keyfile_string_array_from_json (ts, "tree", dest_name ?: src_name, a, error);
}

/* Given a boolean value in JSON, add it to treespec
 * if it's not the default.
 */
static gboolean
treespec_bind_bool (JsonObject *treedata,
                    GKeyFile   *ts,
                    const char *name,
                    gboolean    default_value,
                    GError    **error)
{
  gboolean v = default_value;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata, name, &v, error))
    return FALSE;

  if (v != default_value)
    g_key_file_set_boolean (ts, "tree", name, v);

  return TRUE;
}

/* Convert a treefile into a "treespec" understood by the core.
 */
RpmOstreeTreespec *
rpmostree_composeutil_get_treespec (RpmOstreeContext  *ctx,
                                    RORTreefile *treefile_rs,
                                    JsonObject  *treedata,
                                    gboolean     bind_selinux,
                                    GError     **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Parsing treefile", error);
  g_autoptr(GHashTable) varsubsts = rpmostree_dnfcontext_get_varsubsts (rpmostree_context_get_dnf (ctx));
  g_autoptr(GKeyFile) treespec = g_key_file_new ();

  // TODO: Rework things so we always use this data going forward
  rpmostree_context_set_treefile (ctx, treefile_rs);

  if (!treespec_bind_array (treedata, treespec, "packages", NULL, TRUE, error))
    return FALSE;
  if (!treespec_bind_array (treedata, treespec, "repos", NULL, TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "documentation", TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "recommends", TRUE, error))
    return FALSE;
  if (!treespec_bind_array (treedata, treespec, "install-langs", "instlangs", FALSE, error))
    return FALSE;
  { const char *releasever;
    if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "releasever",
                                                                &releasever, error))
      return FALSE;
    if (releasever)
      g_key_file_set_string (treespec, "tree", "releasever", releasever);
  }

  if (bind_selinux)
    {
      if (!treespec_bind_bool (treedata, treespec, "selinux", TRUE, error))
        return FALSE;
    }
  else
    {
      /* In the legacy compose path, we don't want to use any of the core's selinux stuff,
       * e.g. importing, relabeling, etc... so just disable it. We do still set the policy
       * to the final one right before commit as usual. */
      g_key_file_set_boolean (treespec, "tree", "selinux", FALSE);
    }
  if (ror_treefile_get_sysusers (treefile_rs))
    g_key_file_set_boolean (treespec, "tree", "sysusers", TRUE);

  const char *input_ref = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "ref", &input_ref, error))
    return FALSE;
  if (input_ref)
    {
      g_autofree char *ref = _rpmostree_varsubst_string (input_ref, varsubsts, error);
      if (!ref)
        return FALSE;
      g_key_file_set_string (treespec, "tree", "ref", ref);
    }

  return rpmostree_treespec_new_from_keyfile (treespec, error);
}

gboolean
rpmostree_composeutil_read_json_metadata (JsonNode    *root,
                                          GHashTable  *metadata,
                                          GError     **error)
{
  g_autoptr(GVariant) jsonmetav = json_gvariant_deserialize (root, "a{sv}", error);
  if (!jsonmetav)
    return FALSE;

  GVariantIter viter;
  g_variant_iter_init (&viter, jsonmetav);
  { char *key;
    GVariant *value;
    while (g_variant_iter_loop (&viter, "{sv}", &key, &value))
      g_hash_table_replace (metadata, g_strdup (key), g_variant_ref (value));
  }

  return TRUE;
}

/* compose tree accepts JSON metadata via file; convert it
 * to a hash table of a{sv}; suitable for further extension.
 */
gboolean
rpmostree_composeutil_read_json_metadata_from_file (const char *path,
                                                    GHashTable *metadata,
                                                    GError    **error)
{
  const char *errprefix = glnx_strjoina ("While parsing JSON file ", path);
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);

  glnx_unref_object JsonParser *jparser = json_parser_new ();
  if (!json_parser_load_from_file (jparser, path, error))
    return FALSE;

  JsonNode *metarootval = json_parser_get_root (jparser);
  return rpmostree_composeutil_read_json_metadata (metarootval, metadata, error);
}

/* Convert hash table of metadata into finalized GVariant */
GVariant *
rpmostree_composeutil_finalize_metadata (GHashTable *metadata,
                                         int         rootfs_dfd,
                                         GError    **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  GLNX_HASH_TABLE_FOREACH_KV (metadata, const char*, strkey, GVariant*, v)
    g_variant_builder_add (metadata_builder, "{sv}", strkey, v);

  /* include list of packages in rpmdb; this is used client-side for easily previewing
   * pending updates. once we only support unified core composes, this can easily be much
   * more readily injected during assembly */
  g_autoptr(GVariant) rpmdb_v = NULL;
  if (!rpmostree_create_rpmdb_pkglist_variant (rootfs_dfd, ".", &rpmdb_v,
                                               NULL, error))
    return FALSE;
  g_variant_builder_add (metadata_builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb_v);

  g_autoptr(GVariant) ret = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
  /* Canonicalize to big endian, like OSTree does. Without this, any numbers
   * we place in the metadata will be unreadable since clients won't know
   * their endianness.
   */
  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      GVariant *swapped = g_variant_byteswap (ret);
      GVariant *orig = ret;
      ret = swapped;
      g_variant_unref (orig);
    }

  return g_steal_pointer (&ret);
}

/* Implements --write-composejson-to, and also prints values.
 * If `path` is NULL, we'll just print some data.
 */
gboolean
rpmostree_composeutil_write_composejson (OstreeRepo  *repo,
                                         const char *path,
                                         const OstreeRepoTransactionStats *stats,
                                         const char *new_revision,
                                         GVariant   *new_commit,
                                         GVariantBuilder *builder,
                                         GCancellable *cancellable,
                                         GError    **error)
{
  g_autoptr(GVariant) new_commit_inline_meta = g_variant_get_child_value (new_commit, 0);

  if (stats)
    {
      g_variant_builder_add (builder, "{sv}", "ostree-n-metadata-total",
                             g_variant_new_uint32 (stats->metadata_objects_total));
      g_print ("Metadata Total: %u\n", stats->metadata_objects_total);

      g_variant_builder_add (builder, "{sv}", "ostree-n-metadata-written",
                             g_variant_new_uint32 (stats->metadata_objects_written));
      g_print ("Metadata Written: %u\n", stats->metadata_objects_written);

      g_variant_builder_add (builder, "{sv}", "ostree-n-content-total",
                             g_variant_new_uint32 (stats->content_objects_total));
      g_print ("Content Total: %u\n", stats->content_objects_total);

      g_print ("Content Written: %u\n", stats->content_objects_written);
      g_variant_builder_add (builder, "{sv}", "ostree-n-content-written",
                             g_variant_new_uint32 (stats->content_objects_written));

      g_print ("Content Cache Hits: %u\n", stats->devino_cache_hits);
      g_variant_builder_add (builder, "{sv}", "ostree-n-cache-hits",
                             g_variant_new_uint32 (stats->devino_cache_hits));

      g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats->content_bytes_written);
      g_variant_builder_add (builder, "{sv}", "ostree-content-bytes-written",
                             g_variant_new_uint64 (stats->content_bytes_written));
    }
  g_variant_builder_add (builder, "{sv}", "ostree-commit", g_variant_new_string (new_revision));
  g_autofree char *content_checksum = ostree_commit_get_content_checksum (new_commit);
  g_variant_builder_add (builder, "{sv}", "ostree-content-checksum", g_variant_new_string (content_checksum));
  const char *commit_version = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, OSTREE_COMMIT_META_KEY_VERSION, "&s", &commit_version);
  if (commit_version)
    g_variant_builder_add (builder, "{sv}", "ostree-version", g_variant_new_string (commit_version));
  /* Since JavaScript doesn't have 64 bit integers and hence neither does JSON,
   * store this as a string:
   * https://stackoverflow.com/questions/10286204/the-right-json-date-format
   * */
  { guint64 commit_ts = ostree_commit_get_timestamp (new_commit);
    g_autofree char *commit_ts_iso_8601 = rpmostree_timestamp_str_from_unix_utc (commit_ts);
    g_variant_builder_add (builder, "{sv}", "ostree-timestamp", g_variant_new_string (commit_ts_iso_8601));
  }

  const char *inputhash = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, "rpmostree.inputhash", "&s", &inputhash);
  /* We may not have the inputhash in the split-up installroot case */
  if (inputhash)
    g_variant_builder_add (builder, "{sv}", "rpm-ostree-inputhash", g_variant_new_string (inputhash));

  g_autofree char *parent_revision = ostree_commit_get_parent (new_commit);
  if (path && parent_revision)
    {
      /* don't error if the parent doesn't exist */
      gboolean parent_exists;
      if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, parent_revision,
                                   &parent_exists, cancellable, error))
        return FALSE;

      if (parent_exists)
        {
          g_autoptr(GVariant) diffv = NULL;
          if (!rpm_ostree_db_diff_variant (repo, parent_revision, new_revision,
                                           TRUE, &diffv, cancellable, error))
            return FALSE;
          g_variant_builder_add (builder, "{sv}", "pkgdiff", diffv);
        }
    }

  if (path)
    {
      g_autoptr(GVariant) composemeta_v = g_variant_builder_end (builder);
      JsonNode *composemeta_node = json_gvariant_serialize (composemeta_v);
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_root (generator, composemeta_node);

      char *dnbuf = strdupa (path);
      const char *dn = dirname (dnbuf);
      g_auto(GLnxTmpfile) tmpf = { 0, };
      if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dn, O_WRONLY | O_CLOEXEC,
                                          &tmpf, error))
        return FALSE;
      g_autoptr(GOutputStream) out = g_unix_output_stream_new (tmpf.fd, FALSE);
      /* See also similar code in status.c */
      if (json_generator_to_stream (generator, out, cancellable, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;

      /* World readable to match --write-commitid-to which uses umask */
      if (!glnx_fchmod (tmpf.fd, 0644, error))
        return FALSE;

      if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_REPLACE,
                                 AT_FDCWD, path, error))
        return FALSE;
    }

  return TRUE;
}
