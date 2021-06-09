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

#include <glib-unix.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmts.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-journal.h>
#include <libdnf/libdnf.h>
#include <librepo/librepo.h>
#include <utility>
#include <set>

#include "rpmostree-core-private.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-kernel.h"
#include "rpmostree-importer.h"
#include "rpmostree-output.h"
#include "rpmostree-cxxrs.h"

#define RPMOSTREE_MESSAGE_COMMIT_STATS SD_ID128_MAKE(e6,37,2e,38,41,21,42,a9,bc,13,b6,32,b3,f8,93,44)
#define RPMOSTREE_MESSAGE_SELINUX_RELABEL SD_ID128_MAKE(5a,e0,56,34,f2,d7,49,3b,b1,58,79,b7,0c,02,e6,5d)
#define RPMOSTREE_MESSAGE_PKG_REPOS SD_ID128_MAKE(0e,ea,67,9b,bf,a3,4d,43,80,2d,ec,99,b2,74,eb,e7)
#define RPMOSTREE_MESSAGE_PKG_IMPORT SD_ID128_MAKE(df,8b,b5,4f,04,fa,47,08,ac,16,11,1b,bf,4b,a3,52)

static OstreeRepo * get_pkgcache_repo (RpmOstreeContext *self);

/* Given a string, look for ostree:// prefix and
 * return its type and the remainder of the string.  Note
 * that ostree:// can be either a refspec (TYPE_OSTREE) or
 * a bare commit (TYPE_COMMIT).
 */
gboolean
rpmostree_refspec_classify (const char *refspec,
                            RpmOstreeRefspecType *out_type,
                            const char **out_remainder,
                            GError     **error)
{
  /* Add any other prefixes here */

  /* For compatibility, fall back to ostree:// when we have no prefix. */
  const char *remainder;
  if (g_str_has_prefix (refspec, RPMOSTREE_REFSPEC_OSTREE_PREFIX))
    remainder = refspec + strlen (RPMOSTREE_REFSPEC_OSTREE_PREFIX);
  else
    remainder = refspec;

  if (ostree_validate_checksum_string (remainder, NULL))
    *out_type = RPMOSTREE_REFSPEC_TYPE_CHECKSUM;
  else
    *out_type = RPMOSTREE_REFSPEC_TYPE_OSTREE;
  if (out_remainder)
    *out_remainder = remainder;
  return TRUE;
}

char*
rpmostree_refspec_to_string (RpmOstreeRefspecType  reftype,
                             const char           *data)
{
  const char *prefix = NULL;
  switch (reftype)
    {
    case RPMOSTREE_REFSPEC_TYPE_OSTREE:
    case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
      prefix = RPMOSTREE_REFSPEC_OSTREE_PREFIX;
      break;
    }
  g_assert (prefix);
  return g_strconcat (prefix, data, NULL);
}


/* Primarily this makes sure that ostree refspecs start with `ostree://`.
 */
char*
rpmostree_refspec_canonicalize (const char *orig_refspec,
                                GError    **error)
{
  RpmOstreeRefspecType refspectype;
  const char *refspec_data;
  if (!rpmostree_refspec_classify (orig_refspec, &refspectype, &refspec_data, error))
    return NULL;
  return rpmostree_refspec_to_string (refspectype, refspec_data);
}

static int
compare_pkgs (gconstpointer ap,
              gconstpointer bp)
{
  auto a = (DnfPackage**)ap;
  auto b = (DnfPackage**)bp;
  return dnf_package_cmp (*a, *b);
}

/***********************************************************
 *                    RpmOstreeTreespec                    *
 ***********************************************************
 */

struct _RpmOstreeTreespec {
  GObject parent;

  GVariant *spec;
  GVariantDict *dict;
};

G_DEFINE_TYPE (RpmOstreeTreespec, rpmostree_treespec, G_TYPE_OBJECT)

static void
rpmostree_treespec_finalize (GObject *object)
{
  RpmOstreeTreespec *self = RPMOSTREE_TREESPEC (object);

  g_clear_pointer (&self->spec, g_variant_unref);
  g_clear_pointer (&self->dict, g_variant_dict_unref);

  G_OBJECT_CLASS (rpmostree_treespec_parent_class)->finalize (object);
}

static void
rpmostree_treespec_class_init (RpmOstreeTreespecClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = rpmostree_treespec_finalize;
}

static void
rpmostree_treespec_init (RpmOstreeTreespec *self)
{
}

static int
qsort_cmpstr (const void*ap, const void *bp, gpointer data)
{
  const char *a = *((const char *const*) ap);
  const char *b = *((const char *const*) bp);
  return strcmp (a, b);
}

/* Handle converting "treespec" bits to GVariant.
 * Look for @key in @keyfile (under the section "tree"), and
 * if found, strip leading/trailing whitespace from each entry,
 * sort them, and add it to @builder under the same key name.
 * If there are no entries for it, and @notfound_key is provided,
 * set @notfound_key in the variant to TRUE.
 */
static void
add_canonicalized_string_array (GVariantBuilder *builder,
                                const char *key,
                                const char *notfound_key,
                                GKeyFile *keyfile)
{
  g_autoptr(GHashTable) set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_auto(GStrv) input = g_key_file_get_string_list (keyfile, "tree", key, NULL, NULL);
  if (!(input && *input))
    {
      if (notfound_key)
        {
          g_variant_builder_add (builder, "{sv}", notfound_key, g_variant_new_boolean (TRUE));
          return;
        }
    }

  /* Iterate over strings, strip leading/trailing whitespace */
  for (char ** iter = input; iter && *iter; iter++)
    {
      g_autofree char *stripped = g_strdup (*iter);
      g_strchug (g_strchomp (stripped));
      g_hash_table_add (set, g_steal_pointer (&stripped));
    }

  /* Ensure sorted */
  guint count;
  g_autofree char **sorted = (char**)g_hash_table_get_keys_as_array (set, &count);
  if (count > 1)
    g_qsort_with_data (sorted, count, sizeof (void*), qsort_cmpstr, NULL);

  g_variant_builder_add (builder, "{sv}", key,
                         g_variant_new_strv ((const char*const*)sorted, count));
}

RpmOstreeTreespec *
rpmostree_treespec_new_from_keyfile (GKeyFile   *keyfile,
                                     GError    **error)
{
  g_autoptr(RpmOstreeTreespec) ret = (RpmOstreeTreespec*)g_object_new (RPMOSTREE_TYPE_TREESPEC, NULL);
  g_auto(GVariantBuilder) builder;

  g_variant_builder_init (&builder, (GVariantType*)"a{sv}");

  add_canonicalized_string_array (&builder, "packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "exclude-packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "cached-packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "removed-base-packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "cached-replaced-base-packages", NULL, keyfile);

  /* We allow the "repos" key to be missing. This means that we rely on libdnf's
   * normal behaviour (i.e. look at repos in repodir with enabled=1). */
  { g_auto(GStrv) val = g_key_file_get_string_list (keyfile, "tree", "repos", NULL, NULL);
    if (val && *val)
      add_canonicalized_string_array (&builder, "repos", NULL, keyfile);
  }
  { g_auto(GStrv) val = g_key_file_get_string_list (keyfile, "tree", "lockfile-repos", NULL, NULL);
    if (val && *val)
      add_canonicalized_string_array (&builder, "lockfile-repos", NULL, keyfile);
  }
  add_canonicalized_string_array (&builder, "instlangs", "instlangs-all", keyfile);

  if (g_key_file_get_boolean (keyfile, "tree", "skip-sanity-check", NULL))
    g_variant_builder_add (&builder, "{sv}", "skip-sanity-check", g_variant_new_boolean (TRUE));


  ret->spec = g_variant_builder_end (&builder);
  ret->dict = g_variant_dict_new (ret->spec);

  return util::move_nullify (ret);
}

RpmOstreeTreespec *
rpmostree_treespec_new_from_path (const char *path, GError  **error)
{
  g_autoptr(GKeyFile) specdata = g_key_file_new();
  if (!g_key_file_load_from_file (specdata, path, static_cast<GKeyFileFlags>(0), error))
    return NULL;
  return rpmostree_treespec_new_from_keyfile (specdata, error);
}

RpmOstreeTreespec *
rpmostree_treespec_new (GVariant   *variant)
{
  g_autoptr(RpmOstreeTreespec) ret = (RpmOstreeTreespec*)g_object_new (RPMOSTREE_TYPE_TREESPEC, NULL);
  ret->spec = g_variant_ref (variant);
  ret->dict = g_variant_dict_new (ret->spec);
  return util::move_nullify (ret);
}

GVariant *
rpmostree_treespec_to_variant (RpmOstreeTreespec *spec)
{
  return g_variant_ref (spec->spec);
}

/***********************************************************
 *                    RpmOstreeContext                     *
 ***********************************************************
 */
G_DEFINE_TYPE (RpmOstreeContext, rpmostree_context, G_TYPE_OBJECT)

static void
rpmostree_context_finalize (GObject *object)
{
  RpmOstreeContext *rctx = RPMOSTREE_CONTEXT (object);

  g_clear_object (&rctx->spec);
  g_clear_object (&rctx->dnfctx);

  g_clear_pointer (&rctx->ref, g_free);

  g_clear_object (&rctx->pkgcache_repo);
  g_clear_object (&rctx->ostreerepo);
  g_clear_pointer (&rctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&rctx->sepolicy);

  g_clear_pointer (&rctx->passwd_dir, g_free);

  g_clear_pointer (&rctx->pkgs, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_download, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_import, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_relabel, g_ptr_array_unref);

  g_clear_pointer (&rctx->pkgs_to_remove, g_hash_table_unref);
  g_clear_pointer (&rctx->pkgs_to_replace, g_hash_table_unref);

  (void)glnx_tmpdir_delete (&rctx->tmpdir, NULL, NULL);
  (void)glnx_tmpdir_delete (&rctx->repo_tmpdir, NULL, NULL);

  g_clear_pointer (&rctx->rootfs_usrlinks, g_hash_table_unref);

  G_OBJECT_CLASS (rpmostree_context_parent_class)->finalize (object);
}

static void
rpmostree_context_class_init (RpmOstreeContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = rpmostree_context_finalize;
}

static void
rpmostree_context_init (RpmOstreeContext *self)
{
  self->tmprootfs_dfd = -1;
  self->dnf_cache_policy = RPMOSTREE_CONTEXT_DNF_CACHE_DEFAULT;
  self->enable_rofiles = TRUE;
}

static void
set_rpm_macro_define (const char *key, const char *value)
{
  g_autofree char *buf = g_strconcat ("%define ", key, " ", value, NULL);
  /* Calling expand with %define (ignoring the return
   * value) is apparently the way to change the global
   * macro context.
   */
  free (rpmExpand (buf, NULL));
}

/* Create a context intended for use "client side", e.g. for package layering
 * operations.
 */
RpmOstreeContext *
rpmostree_context_new_client (OstreeRepo   *repo)
{
  g_assert (repo != NULL);

  auto self = static_cast<RpmOstreeContext*>(g_object_new (RPMOSTREE_TYPE_CONTEXT, NULL));
  self->is_system = TRUE;
  self->ostreerepo = static_cast<OstreeRepo*>(g_object_ref (repo));

  /* We can always be control-c'd at any time; this is new API,
   * otherwise we keep calling _rpmostree_reset_rpm_sighandlers() in
   * various places.
   */
  rpmsqSetInterruptSafety (FALSE);

  self->dnfctx = dnf_context_new ();
  DECLARE_RPMSIGHANDLER_RESET;
  dnf_context_set_http_proxy (self->dnfctx, g_getenv ("http_proxy"));

  dnf_context_set_repo_dir (self->dnfctx, "/etc/yum.repos.d");
  dnf_context_set_cache_dir (self->dnfctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_REPOMD);
  dnf_context_set_solv_dir (self->dnfctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_SOLV);
  dnf_context_set_lock_dir (self->dnfctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);
  dnf_context_set_user_agent (self->dnfctx, PACKAGE_NAME "/" PACKAGE_VERSION);
  /* don't need SWDB: https://github.com/rpm-software-management/libdnf/issues/645 */
  dnf_context_set_write_history (self->dnfctx, FALSE);

  dnf_context_set_check_disk_space (self->dnfctx, FALSE);
  dnf_context_set_check_transaction (self->dnfctx, FALSE);

  /* we don't need any plugins */
  dnf_context_set_plugins_dir (self->dnfctx, NULL);

  /* Force disable internal libdnf Count Me logic */
  dnf_conf_add_setopt("*.countme", DNF_CONF_COMMANDLINE, "false", NULL);

  /* Hack until libdnf+librepo know how to better negotaiate zchunk.
   * see also the bits in configure.ac that define HAVE_ZCHUNK
   **/
#ifndef HAVE_ZCHUNK
  dnf_context_set_zchunk (self->dnfctx, FALSE);
#endif

  return self;
}

/* Create a context that assembles a new filesystem tree, possibly without root
 * privileges. Some behavioral distinctions are made by looking at the undelying
 * OstreeRepoMode.  In particular, an OSTREE_REPO_MODE_BARE_USER_ONLY repo
 * is assumed to be for unprivileged containers/buildroots.
 */
RpmOstreeContext *
rpmostree_context_new_compose (int               userroot_dfd,
                               OstreeRepo       *repo,
                               rpmostreecxx::Treefile &treefile_rs)
{
  /* Inherit the client baseline, but flip it back to be a "compose" context */
  auto ret = rpmostree_context_new_client (repo);
  ret->is_system = FALSE;
  ret->treefile_rs = &treefile_rs;

  { g_autofree char *reposdir = glnx_fdrel_abspath (userroot_dfd, "rpmmd.repos.d");
    dnf_context_set_repo_dir (ret->dnfctx, reposdir);
  }
  { const char *cache_rpmmd = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_REPOMD);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_rpmmd);
    dnf_context_set_cache_dir (ret->dnfctx, cachedir);
  }
  { const char *cache_solv = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_SOLV);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_solv);
    dnf_context_set_solv_dir (ret->dnfctx, cachedir);
  }
  { const char *lock = glnx_strjoina ("cache/", RPMOSTREE_DIR_LOCK);
    g_autofree char *lockdir = glnx_fdrel_abspath (userroot_dfd, lock);
    dnf_context_set_lock_dir (ret->dnfctx, lockdir);
  }

  // The ref needs special handling as it gets variable-substituted.
  auto ref = ret->treefile_rs->get_ref();
  if (ref.length() > 0)
    {
      auto varsubsts = rpmostree_dnfcontext_get_varsubsts(ret->dnfctx);
      auto subst_ref = rpmostreecxx::varsubstitute(ref, *varsubsts);
      ret->ref = g_strdup(subst_ref.c_str());
    }

  return util::move_nullify (ret);
}

static gboolean
rpmostree_context_ensure_tmpdir (RpmOstreeContext *self,
                                 const char       *subdir,
                                 GError          **error)
{
  if (!self->tmpdir.initialized)
    {
      if (!glnx_mkdtemp ("rpmostree-core-XXXXXX", 0700, &self->tmpdir, error))
        return FALSE;
    }
  g_assert (self->tmpdir.initialized);

  if (!glnx_shutil_mkdir_p_at (self->tmpdir.fd, subdir, 0755, NULL, error))
    return FALSE;

  return TRUE;
}

void
rpmostree_context_set_pkgcache_only (RpmOstreeContext *self,
                                     gboolean          pkgcache_only)
{
  /* if called, must always be before setup() */
  g_assert (!self->spec);
  self->pkgcache_only = pkgcache_only;
}

void
rpmostree_context_set_dnf_caching (RpmOstreeContext *self,
                                   RpmOstreeContextDnfCachePolicy policy)
{
  self->dnf_cache_policy = policy;
}

/* Pick up repos dir and passwd from @cfg_deployment. */
void
rpmostree_context_configure_from_deployment (RpmOstreeContext *self,
                                             OstreeSysroot    *sysroot,
                                             OstreeDeployment *cfg_deployment)
{
  g_autofree char *cfg_deployment_root =
    rpmostree_get_deployment_root (sysroot, cfg_deployment);
  g_autofree char *reposdir =
    g_build_filename (cfg_deployment_root, "etc/yum.repos.d", NULL);

  /* point libhif to the yum.repos.d and os-release of the merge deployment */
  dnf_context_set_repo_dir (self->dnfctx, reposdir);

  /* point the core to the passwd & group of the merge deployment */
  g_assert (!self->passwd_dir);
  self->passwd_dir = g_build_filename (cfg_deployment_root, "etc", NULL);
}

/* Use this if no packages will be installed, and we just want a "dummy" run.
 */
void
rpmostree_context_set_is_empty (RpmOstreeContext *self)
{
  self->empty = TRUE;
}

void 
rpmostree_context_disable_selinux (RpmOstreeContext *self)
{
  self->disable_selinux = TRUE;
}

const char *
rpmostree_context_get_ref (RpmOstreeContext *self)
{
  return self->ref;
}

/* XXX: or put this in new_system() instead? */
void
rpmostree_context_set_repos (RpmOstreeContext *self,
                             OstreeRepo       *base_repo,
                             OstreeRepo       *pkgcache_repo)
{
  g_set_object (&self->ostreerepo, base_repo);
  g_set_object (&self->pkgcache_repo, pkgcache_repo);
}

static OstreeRepo *
get_pkgcache_repo (RpmOstreeContext *self)
{
  return self->pkgcache_repo ?: self->ostreerepo;
}

/* I debated making this part of the treespec. Overall, I think it makes more
 * sense to define it outside since the policy to use depends on the context in
 * which the RpmOstreeContext is used, not something we can always guess on our
 * own correctly. */
void
rpmostree_context_set_sepolicy (RpmOstreeContext *self,
                                OstreeSePolicy   *sepolicy)
{
  g_set_object (&self->sepolicy, sepolicy);
}

void
rpmostree_context_set_devino_cache (RpmOstreeContext *self,
                                    OstreeRepoDevInoCache *devino_cache)
{
  g_assert (self->enable_rofiles);
  if (self->devino_cache)
    ostree_repo_devino_cache_unref (self->devino_cache);
  self->devino_cache = devino_cache ? ostree_repo_devino_cache_ref (devino_cache) : NULL;
}

void
rpmostree_context_disable_rofiles (RpmOstreeContext *self)
{
  g_assert (!self->devino_cache);
  self->enable_rofiles = FALSE;
}

DnfContext *
rpmostree_context_get_dnf (RpmOstreeContext *self)
{
  return self->dnfctx;
}

/* Add rpmmd repo information, since it's very useful for determining
 * state.  See also:
 *
 * - https://github.com/projectatomic/rpm-ostree/issues/774
 * - The repo_metadata_for_package() function in rpmostree-unpacker.c

 * This returns a value of key of type aa{sv} - the key should be
 * `rpmostree.rpmmd-repos`.
 */
GVariant *
rpmostree_context_get_rpmmd_repo_commit_metadata (RpmOstreeContext  *self)
{
  g_auto(GVariantBuilder) repo_list_builder;
  g_variant_builder_init (&repo_list_builder, (GVariantType*)"aa{sv}");
  g_autoptr(GPtrArray) repos =
    rpmostree_get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
  for (guint i = 0; i < repos->len; i++)
    {
      g_auto(GVariantBuilder) repo_builder;
      g_variant_builder_init (&repo_builder, (GVariantType*)"a{sv}");
      auto repo = static_cast<DnfRepo *>(repos->pdata[i]);
      const char *id = dnf_repo_get_id (repo);
      g_variant_builder_add (&repo_builder, "{sv}", "id", g_variant_new_string (id));
      guint64 ts = dnf_repo_get_timestamp_generated (repo);
      g_variant_builder_add (&repo_builder, "{sv}", "timestamp", g_variant_new_uint64 (ts));
      g_variant_builder_add (&repo_list_builder, "@a{sv}", g_variant_builder_end (&repo_builder));
    }
  return g_variant_ref_sink (g_variant_builder_end (&repo_list_builder));
}

std::unique_ptr<rust::Vec<rpmostreecxx::StringMapping>> 
rpmostree_dnfcontext_get_varsubsts (DnfContext *context)
{
  auto r = std::make_unique<rust::Vec<rpmostreecxx::StringMapping>>();
  r->push_back(rpmostreecxx::StringMapping{"basearch", dnf_context_get_base_arch (context) });
  return r;
}

/* Look for a repo named @reponame, and ensure it is enabled for package
 * downloads.
 */
static gboolean
enable_one_repo (GPtrArray           *sources,
                 const char          *reponame,
                 GError             **error)
{
  for (guint i = 0; i < sources->len; i++)
    {
      auto src = static_cast<DnfRepo *>(sources->pdata[i]);
      const char *id = dnf_repo_get_id (src);

      if (strcmp (reponame, id) != 0)
        continue;

      dnf_repo_set_enabled (src, DNF_REPO_ENABLED_PACKAGES);
      return TRUE;
    }

  return glnx_throw (error, "Unknown rpm-md repository: %s", reponame);
}

static void
disable_all_repos (RpmOstreeContext  *context)
{
  GPtrArray *sources = dnf_context_get_repos (context->dnfctx);
  for (guint i = 0; i < sources->len; i++)
    {
      auto src = static_cast<DnfRepo *>(sources->pdata[i]);
      dnf_repo_set_enabled (src, DNF_REPO_ENABLED_NONE);
    }
}

/* Enable all repos in @repos */
static gboolean
enable_repos (RpmOstreeContext  *context,
              const char        *const *repos,
              GError           **error)
{
  GPtrArray *sources = dnf_context_get_repos (context->dnfctx);
  for (const char *const *iter = repos; iter && *iter; iter++)
    {
      if (!enable_one_repo (sources, *iter, error))
        return FALSE;
    }

  return TRUE;
}

void 
rpmostree_context_set_treespec (RpmOstreeContext *self, RpmOstreeTreespec *treespec)
{
  g_assert (!self->spec);
  /* allow NULL for treespec, but canonicalize to an empty keyfile for cleaner queries */
  if (!treespec)
    {
      g_autoptr(GKeyFile) kf = g_key_file_new ();
      self->spec = rpmostree_treespec_new_from_keyfile (kf, NULL);
    }
  else
    self->spec = (RpmOstreeTreespec*)g_object_ref (treespec);
}

/* Wraps `dnf_context_setup()`, and initializes state based on the treespec
 * @spec. Another way to say it is we pair `DnfContext` with an
 * `RpmOstreeTreespec`. For example, we handle "instlangs", set the rpmdb root
 * to `/usr/share/rpm`, and handle the required rpm-md repos. The "packages"
 * entry from @spec is processed later.
 *
 * If either @install_root or @source_root are `NULL`, `/usr/share/empty` is
 * used. Typically @source_root being `NULL` is for "from scratch" root
 * filesystems.
 */
gboolean
rpmostree_context_setup (RpmOstreeContext    *self,
                         const char    *install_root,
                         const char    *source_root,
                         GCancellable  *cancellable,
                         GError       **error)
{
  std::string releasever;
  /* This exists (as a canonically empty dir) at least on RHEL7+ */
  static const char emptydir_path[] = "/usr/share/empty";

  /* Auto-synthesize a spec for now; this will be removed */
  if (!self->spec)
    rpmostree_context_set_treespec (self, NULL);

  if (self->treefile_rs)
    releasever = std::string(self->treefile_rs->get_releasever());

  if (!install_root)
    install_root = emptydir_path;
  if (!source_root)
    {
      source_root = emptydir_path;
      /* Hackaround libdnf's auto-introspection semantics. For e.g.
       * treecompose/container, we currently require using .repo files which
       * don't reference $releasever.
       */
      if (releasever.length() == 0)
        releasever = "rpmostree-unset-releasever";
      dnf_context_set_release_ver (self->dnfctx, releasever.c_str());
    }
  else if (releasever.length() > 0)
    dnf_context_set_release_ver (self->dnfctx, releasever.c_str());

  dnf_context_set_install_root (self->dnfctx, install_root);
  dnf_context_set_source_root (self->dnfctx, source_root);

  /* Set the RPM _install_langs macro, which gets processed by librpm; this is
   * currently only referenced in the traditional or non-"unified core" code.
   */
  if (!self->is_system && self->treefile_rs)
    {
      auto instlangs = self->treefile_rs->format_install_langs_macro();
      if (instlangs.length() > 0)
        dnf_context_set_rpm_macro (self->dnfctx, "_install_langs", instlangs.c_str());
    }

  /* This is what we use as default. Note we should be able to drop this in the
   * future now that we inject a macro to set that value in our OSTrees. */
  dnf_context_set_rpm_macro (self->dnfctx, "_dbpath", "/" RPMOSTREE_RPMDB_LOCATION);

  /* Set the database backend only in the compose path.  It then becomes the default
   * for any client side layering.
   */
  if (!self->is_system && self->treefile_rs)
    {
      auto rpmdb_backend = self->treefile_rs->get_rpmdb();
      dnf_context_set_rpm_macro (self->dnfctx, "_db_backend", rpmdb_backend.c_str());
    }

  if (!dnf_context_setup (self->dnfctx, cancellable, error))
    return FALSE;

  /* disable all repos in pkgcache-only mode, otherwise obey "repos" key */
  if (self->pkgcache_only)
    {
      disable_all_repos (self);
    }
  else
    {
      /* Makes sure we only disable all repos once. This is more for future proofing against
       * refactors for now since we don't support `lockfile-repos` on the client-side and on
       * the server-side we always require `repos` anyway. */
      bool disabled_all_repos = false;

      /* NB: missing "repos" --> let libdnf figure it out for itself (we're likely doing a
       * client-side compose where we want to use /etc/yum.repos.d/) */
      g_autofree char **enabled_repos = NULL;
      if (g_variant_dict_lookup (self->spec->dict, "repos", "^a&s", &enabled_repos))
        {
          if (!disabled_all_repos)
            {
              disable_all_repos (self);
              disabled_all_repos = true;
            }
          if (!enable_repos (self, (const char *const*)enabled_repos, error))
            return FALSE;
        }

      /* only enable lockfile-repos if we actually have a lockfile so we don't even waste
       * time fetching metadata */
      if (self->lockfile)
        {
          g_autofree char **enabled_lockfile_repos = NULL;
          if (g_variant_dict_lookup (self->spec->dict, "lockfile-repos", "^a&s", &enabled_lockfile_repos))
            {
              if (!disabled_all_repos)
                disable_all_repos (self);
              if (!enable_repos (self, (const char *const*)enabled_lockfile_repos, error))
                return FALSE;
            }
        }
    }

  g_autoptr(GPtrArray) repos =
    rpmostree_get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
  if (repos->len == 0 && !self->pkgcache_only)
    {
      /* To be nice, let's only make this fatal if "packages" is empty (e.g. if
       * we're only installing local RPMs. Missing deps will cause the regular
       * 'not found' error from libdnf. */
      g_autofree char **pkgs = NULL;
      if (g_variant_dict_lookup (self->spec->dict, "packages", "^a&s", &pkgs) &&
          g_strv_length (pkgs) > 0)
        return glnx_throw (error, "No enabled repositories");
    }

  /* Ensure that each repo that's enabled is marked as required; this should be
   * the default, but we make sure.  This is a bit of a messy topic, but for
   * rpm-ostree we're being more strict about requiring repos.
   */
  for (guint i = 0; i < repos->len; i++)
    dnf_repo_set_required (static_cast<DnfRepo*>(repos->pdata[i]), TRUE);

  if (self->treefile_rs && !self->treefile_rs->get_documentation())
    { 
      dnf_transaction_set_flags (dnf_context_get_transaction (self->dnfctx),
                                 DNF_TRANSACTION_FLAG_NODOCS);
    }

  /* We could likely delete this, but I'm keeping a log message just in case */
  if (g_variant_dict_contains (self->spec->dict, "ignore-scripts"))
    sd_journal_print (LOG_INFO, "ignore-scripts is no longer supported");

  bool selinux = !self->disable_selinux && (!self->treefile_rs || self->treefile_rs->get_selinux());
  /* Load policy from / if SELinux is enabled, and we haven't already loaded
   * a policy.  This is mostly for the "compose tree" case.
   */
  if (selinux && !self->sepolicy)
    {
      glnx_autofd int host_rootfs_dfd = -1;
      /* Ensure that the imported packages are labeled with *a* policy if
       * possible, even if it's not the final one. This helps avoid duplicating
       * all of the content.
       */
      if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &host_rootfs_dfd, error))
        return FALSE;
      g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (host_rootfs_dfd, cancellable, error);
      if (!sepolicy)
        return FALSE;
      /* For system contexts, whether SELinux required is dynamic based on the
       * filesystem state. If the base compose doesn't have a policy package,
       * then that's OK. It'd be cleaner if we loaded the no-SELinux state from
       * the origin, but that'd be a backwards-incompatible change.
       */
      if (!self->is_system && ostree_sepolicy_get_name (sepolicy) == NULL)
        return glnx_throw (error, "Unable to load SELinux policy from /");
      rpmostree_context_set_sepolicy (self, sepolicy);
    }

  return TRUE;
}

static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  auto progress = (rpmostreecxx::Progress*)user_data;
  progress->percent_update(percentage);
}

static gboolean
get_commit_metadata_string (GVariant    *commit,
                            const char  *key,
                            char       **out_str,
                            GError     **error)
{
  g_autoptr(GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) meta_dict = g_variant_dict_new (meta);
  g_autoptr(GVariant) str_variant = NULL;

  str_variant =
    _rpmostree_vardict_lookup_value_required (meta_dict, key,
                                              G_VARIANT_TYPE_STRING, error);
  if (!str_variant)
    return FALSE;

  g_assert (g_variant_is_of_type (str_variant, G_VARIANT_TYPE_STRING));
  *out_str = g_strdup (g_variant_get_string (str_variant, NULL));
  return TRUE;
}

static gboolean
get_commit_sepolicy_csum (GVariant *commit,
                          char    **out_csum,
                          GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.sepolicy",
                                     out_csum, error);
}

static gboolean
get_commit_header_sha256 (GVariant *commit,
                          char    **out_csum,
                          GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.metadata_sha256",
                                     out_csum, error);
}

static gboolean
get_commit_repodata_chksum_repr (GVariant *commit,
                            char    **out_csum,
                            GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.repodata_checksum",
                                     out_csum, error);
}

static char *
get_nevra_relpath (const char *nevra)
{
  return g_strdup_printf ("metarpm/%s.rpm", nevra);
}

static char *
get_package_relpath (DnfPackage *pkg)
{
  return get_nevra_relpath (dnf_package_get_nevra (pkg));
}

/* We maintain a temporary copy on disk of the RPM header value; librpm will
 * load this dynamically as the txn progresses.
 */
static gboolean
checkout_pkg_metadata (RpmOstreeContext *self,
                       const char       *nevra,
                       GVariant         *header,
                       GCancellable     *cancellable,
                       GError          **error)
{
  if (!rpmostree_context_ensure_tmpdir (self, "metarpm", error))
    return FALSE;

  /* give it a .rpm extension so we can fool the libdnf stack */
  g_autofree char *path = get_nevra_relpath (nevra);

  if (!glnx_fstatat_allow_noent (self->tmpdir.fd, path, NULL, 0, error))
    return FALSE;
  /* we may have already written the header out for this one */
  if (errno == 0)
    return TRUE;

  return glnx_file_replace_contents_at (self->tmpdir.fd, path,
                                        static_cast<const guint8*>(g_variant_get_data (header)),
                                        g_variant_get_size (header),
                                        GLNX_FILE_REPLACE_NODATASYNC,
                                        cancellable, error);
}

static gboolean
get_header_variant (OstreeRepo       *repo,
                    const char       *cachebranch,
                    GVariant        **out_header,
                    GCancellable     *cancellable,
                    GError          **error)
{
  g_autofree char *cached_rev = NULL;

  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &cached_rev, error))
    return FALSE;

  g_autoptr(GVariant) pkg_commit = NULL;
  if (!ostree_repo_load_commit (repo, cached_rev, &pkg_commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) pkg_meta = g_variant_get_child_value (pkg_commit, 0);
  g_autoptr(GVariantDict) pkg_meta_dict = g_variant_dict_new (pkg_meta);

  g_autoptr(GVariant) header =
    _rpmostree_vardict_lookup_value_required (pkg_meta_dict, "rpmostree.metadata",
                                              (GVariantType*)"ay", error);
  if (!header)
    {
      auto nevra = std::string (rpmostreecxx::cache_branch_to_nevra (cachebranch));
      g_prefix_error (error, "In commit %s of %s: ", cached_rev, nevra.c_str());
      return FALSE;
    }

  *out_header = util::move_nullify (header);
  return TRUE;
}

static gboolean
checkout_pkg_metadata_by_dnfpkg (RpmOstreeContext *self,
                                 DnfPackage       *pkg,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  const char *nevra = dnf_package_get_nevra (pkg);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_autoptr(GVariant) header = NULL;

  if (!get_header_variant (pkgcache_repo, cachebranch, &header,
                           cancellable, error))
    return FALSE;

  return checkout_pkg_metadata (self, nevra, header, cancellable, error);
}

gboolean
rpmostree_pkgcache_find_pkg_header (OstreeRepo    *pkgcache,
                                    const char    *nevra,
                                    const char    *expected_sha256,
                                    GVariant     **out_header,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  auto cachebranch = rpmostreecxx::nevra_to_cache_branch (nevra);

  if (expected_sha256 != NULL)
    {
      g_autofree char *commit_csum = NULL;
      g_autoptr(GVariant) commit = NULL;
      g_autofree char *actual_sha256 = NULL;

      if (!ostree_repo_resolve_rev (pkgcache, cachebranch->c_str(), FALSE, &commit_csum, error))
        return FALSE;

      if (!ostree_repo_load_commit (pkgcache, commit_csum, &commit, NULL, error))
        return FALSE;

      if (!get_commit_header_sha256 (commit, &actual_sha256, error))
        return FALSE;

      if (!g_str_equal (expected_sha256, actual_sha256))
        return glnx_throw (error, "Checksum mismatch for package %s", nevra);
    }

  return get_header_variant (pkgcache, cachebranch->c_str(), out_header, cancellable, error);
}

static gboolean
checkout_pkg_metadata_by_nevra (RpmOstreeContext *self,
                                const char       *nevra,
                                const char       *sha256,
                                GCancellable     *cancellable,
                                GError          **error)
{
  g_autoptr(GVariant) header = NULL;

  if (!rpmostree_pkgcache_find_pkg_header (get_pkgcache_repo (self), nevra, sha256,
                                           &header, cancellable, error))
    return FALSE;

  return checkout_pkg_metadata (self, nevra, header, cancellable, error);
}

/* Initiate download of rpm-md */
gboolean
rpmostree_context_download_metadata (RpmOstreeContext *self,
                                     DnfContextSetupSackFlags flags,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  g_assert (!self->empty);

  /* https://github.com/rpm-software-management/libdnf/pull/416
   * https://github.com/projectatomic/rpm-ostree/issues/1127
   */
  if (flags & DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_FILELISTS)
    dnf_context_set_enable_filelists (self->dnfctx, FALSE);

  g_autoptr(GPtrArray) rpmmd_repos =
    rpmostree_get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);

  if (self->pkgcache_only)
    {
      /* we already disabled all the repos in setup */
      g_assert_cmpint (rpmmd_repos->len, ==, 0);

      /* this is essentially a no-op */
      g_autoptr(DnfState) hifstate = dnf_state_new ();
      if (!dnf_context_setup_sack_with_flags (self->dnfctx, hifstate, flags, error))
        return FALSE;

      /* Note early return; no repos to fetch. */
      return TRUE;
    }

  g_autoptr(GString) enabled_repos = g_string_new ("");
  if (rpmmd_repos->len > 0)
    {
      g_string_append (enabled_repos, "Enabled rpm-md repositories:");
      for (guint i = 0; i < rpmmd_repos->len; i++)
        {
          auto repo = static_cast<DnfRepo *>(rpmmd_repos->pdata[i]);
          g_string_append_printf (enabled_repos, " %s", dnf_repo_get_id (repo));
        }
    }
  else
    {
      g_string_append (enabled_repos, "No enabled rpm-md repositories.");
    }
  rpmostree_output_message ("%s", enabled_repos->str);

  /* Update each repo individually, and print its timestamp, so users can keep
   * track of repo up-to-dateness more easily.
   */
  for (guint i = 0; i < rpmmd_repos->len; i++)
    {
      auto repo = static_cast<DnfRepo *>(rpmmd_repos->pdata[i]);
      g_autoptr(DnfState) hifstate = dnf_state_new ();

      /* Until libdnf speaks GCancellable: https://github.com/projectatomic/rpm-ostree/issues/897 */
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      /* For the cache_age bits, see https://github.com/projectatomic/rpm-ostree/pull/1562
       * AKA a7bbf5bc142d9dac5b1bfb86d0466944d38baa24
       * We have our own cache age as we want to default to G_MAXUINT so we
       * respect the repo's metadata_expire if set.  But the compose tree path
       * also sets this to 0 to force expiry.
       */
      guint cache_age = G_MAXUINT-1;
      switch (self->dnf_cache_policy)
        {
        case RPMOSTREE_CONTEXT_DNF_CACHE_FOREVER:
          cache_age = G_MAXUINT;
          break;
        case RPMOSTREE_CONTEXT_DNF_CACHE_DEFAULT:
          /* Handled above */
          break;
        case RPMOSTREE_CONTEXT_DNF_CACHE_NEVER:
          cache_age = 0;
          break;
        }
      gboolean did_update = FALSE;
      if (!dnf_repo_check(repo,
                          cache_age,
                          hifstate,
                          NULL))
        {
          dnf_state_reset (hifstate);
          auto msg = g_strdup_printf ("Updating metadata for '%s'", dnf_repo_get_id (repo));
          auto progress = rpmostreecxx::progress_percent_begin(msg);
          guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                                   G_CALLBACK (on_hifstate_percentage_changed),
                                                   (void*)progress.get());
          if (!dnf_repo_update (repo, DNF_REPO_UPDATE_FLAG_FORCE, hifstate, error))
            return glnx_prefix_error (error, "Updating rpm-md repo '%s'", dnf_repo_get_id (repo));

          did_update = TRUE;

          g_signal_handler_disconnect (hifstate, progress_sigid);
          progress->end("");
        }

      guint64 ts = dnf_repo_get_timestamp_generated (repo);
      g_autofree char *repo_ts_str = rpmostree_timestamp_str_from_unix_utc (ts);
      rpmostree_output_message ("rpm-md repo '%s'%s; generated: %s",
                                dnf_repo_get_id (repo), !did_update ? " (cached)" : "",
                                repo_ts_str);
    }

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* The _setup_sack function among other things imports the metadata into libsolv */
  { g_autoptr(DnfState) hifstate = dnf_state_new ();
    auto progress = rpmostreecxx::progress_percent_begin("Importing rpm-md");
    guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                             G_CALLBACK (on_hifstate_percentage_changed),
                                             (void*)progress.get());

    /* We already explictly checked the repos above; don't try to check them
     * again.
     */
    dnf_context_set_cache_age (self->dnfctx, G_MAXUINT);

    /* This will check the metadata again, but it *should* hit the cache; down
     * the line we should really improve the libdnf API around all of this.
     */
    DECLARE_RPMSIGHANDLER_RESET;
    if (!dnf_context_setup_sack_with_flags (self->dnfctx, hifstate, flags, error))
      return FALSE;
    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  /* For now, we don't natively support modules. But we still want to be able to install
   * modular packages if the repos are enabled, but libdnf automatically filters them out.
   * So for now, let's tell libdnf that we do want to be able to see them. See:
   * https://github.com/projectatomic/rpm-ostree/issues/1435 */
  dnf_sack_set_module_excludes (dnf_context_get_sack (self->dnfctx), NULL);
  /* And also mark all repos as hotfix repos so that we can indiscriminately cherry-pick
   * from modular repos and non-modular repos alike. */
  g_autoptr(GPtrArray) repos =
    rpmostree_get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
  for (guint i = 0; i < repos->len; i++)
    dnf_repo_set_module_hotfixes (static_cast<DnfRepo*>(repos->pdata[i]), TRUE);

  return TRUE;
}

static void
journal_rpmmd_info (RpmOstreeContext *self)
{
  /* A lot of code to simply log a message to the systemd journal with the state
   * of the rpm-md repos. This is intended to aid system admins with determining
   * system ""up-to-dateness"".
   */
  { g_autoptr(GPtrArray) repos =
      rpmostree_get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
    g_autoptr(GString) enabled_repos = g_string_new ("");
    g_autoptr(GString) enabled_repos_solvables = g_string_new ("");
    g_autoptr(GString) enabled_repos_timestamps = g_string_new ("");
    guint total_solvables = 0;
    gboolean first = TRUE;

    for (guint i = 0; i < repos->len; i++)
      {
        auto repo = static_cast<DnfRepo *>(repos->pdata[i]);

        if (first)
          first = FALSE;
        else
          {
            g_string_append (enabled_repos, ", ");
            g_string_append (enabled_repos_solvables, ", ");
            g_string_append (enabled_repos_timestamps, ", ");
          }
        g_autofree char *quoted = g_shell_quote (dnf_repo_get_id (repo));
        g_string_append (enabled_repos, quoted);
        total_solvables += dnf_repo_get_n_solvables (repo);
        g_string_append_printf (enabled_repos_solvables, "%u",
                                dnf_repo_get_n_solvables (repo));
        g_string_append_printf (enabled_repos_timestamps, "%" G_GUINT64_FORMAT,
                                dnf_repo_get_timestamp_generated (repo));
      }

    sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                        SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PKG_REPOS),
                     "MESSAGE=Preparing pkg txn; enabled repos: [%s] solvables: %u",
                        enabled_repos->str, total_solvables,
                     "SACK_N_SOLVABLES=%i",
                        dnf_sack_count (dnf_context_get_sack (self->dnfctx)),
                     "ENABLED_REPOS=[%s]", enabled_repos->str,
                     "ENABLED_REPOS_SOLVABLES=[%s]", enabled_repos_solvables->str,
                     "ENABLED_REPOS_TIMESTAMPS=[%s]", enabled_repos_timestamps->str,
                     NULL);
  }
}

static gboolean
commit_has_matching_sepolicy (GVariant       *commit,
                              OstreeSePolicy *sepolicy,
                              gboolean       *out_matches,
                              GError        **error)
{
  const char *sepolicy_csum_wanted = ostree_sepolicy_get_csum (sepolicy);
  if (!sepolicy_csum_wanted)
    return glnx_throw (error, "SELinux enabled, but no policy found");

  g_autofree char *sepolicy_csum = NULL;
  if (!get_commit_sepolicy_csum (commit, &sepolicy_csum, error))
    return FALSE;

  *out_matches = g_str_equal (sepolicy_csum, sepolicy_csum_wanted);

  return TRUE;
}

static gboolean
get_pkgcache_repodata_chksum_repr (GVariant    *commit,
                                   char       **out_chksum_repr,
                                   gboolean     allow_noent,
                                   GError     **error)
{
  g_autofree char *chksum_repr = NULL;
  g_autoptr(GError) tmp_error = NULL;
  if (!get_commit_repodata_chksum_repr (commit, &chksum_repr, &tmp_error))
    {
      if (g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && allow_noent)
        {
          *out_chksum_repr = NULL;
          return TRUE;
        }
      g_propagate_error (error, util::move_nullify (tmp_error));
      return FALSE;
    }

  *out_chksum_repr = util::move_nullify (chksum_repr);
  return TRUE;
}

static gboolean
commit_has_matching_repodata_chksum_repr (GVariant    *commit,
                                          const char  *expected,
                                          gboolean    *out_matches,
                                          GError     **error)
{
  g_autofree char *chksum_repr = NULL;
  if (!get_pkgcache_repodata_chksum_repr (commit, &chksum_repr, TRUE, error))
    return FALSE;

  /* never match pkgs unpacked with older versions that didn't embed chksum_repr */
  if (chksum_repr == NULL)
    {
      *out_matches = FALSE;
      return TRUE;
    }

  *out_matches = g_str_equal (expected, chksum_repr);
  return TRUE;
}

static gboolean
pkg_is_cached (DnfPackage *pkg)
{
  if (rpmostree_pkg_is_local (pkg))
    return TRUE;

  /* Right now we're not re-checksumming cached RPMs, we
   * assume they are valid.  This is a change from the current
   * libhif behavior, but I think it's right.  We should
   * record validity once, then ensure it's immutable after
   * that - which is what happens with the ostree commits
   * above.
   */
  return g_file_test (dnf_package_get_filename (pkg), G_FILE_TEST_EXISTS);
}

/* Given @pkg, return its state in the pkgcache repo. It could be not present,
 * or present but have been imported with a different SELinux policy version
 * (and hence in need of relabeling).
 */
static gboolean
find_pkg_in_ostree (RpmOstreeContext *self,
                    DnfPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    gboolean       *out_in_ostree,
                    gboolean       *out_selinux_match,
                    GError        **error)
{
  OstreeRepo *repo = get_pkgcache_repo (self);
  /* Init output here, since we have several early returns */
  *out_in_ostree = FALSE;
  /* If there's no sepolicy, then we always match */
  *out_selinux_match = (sepolicy == NULL);

  /* NB: we're not using a pkgcache yet in the compose path */
  if (repo == NULL)
    return TRUE; /* Note early return */

  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  g_autofree char *cached_rev = NULL;
  if (!ostree_repo_resolve_rev (repo, cachebranch, TRUE,
                                &cached_rev, error))
    return FALSE;

  if (!cached_rev)
    return TRUE; /* Note early return */

  /* Below here prefix with the branch */
  const char *errprefix = glnx_strjoina ("Loading pkgcache branch ", cachebranch);
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);

  g_autoptr(GVariant) commit = NULL;
  OstreeRepoCommitState commitstate;
  if (!ostree_repo_load_commit (repo, cached_rev, &commit, &commitstate, error))
    return FALSE;
  g_assert (commit);

  /* If the commit is partial, then we need to redownload. This can happen if e.g. corrupted
   * commit objects were deleted with `ostree fsck --delete`. */
  if (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL)
    return TRUE; /* Note early return */

  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) metadata_dict = g_variant_dict_new (metadata);

  /* NB: we do an exception for LocalPackages here; we've already checked that
   * its cache is valid and matches what's in the origin. We never want to fetch
   * newer versions of LocalPackages from the repos. But we do want to check
   * whether a relabel will be necessary. */

  const char *reponame = dnf_package_get_reponame (pkg);
  if (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) != 0)
    {
      auto expected_chksum_repr = rpmostreecxx::get_repodata_chksum_repr(*pkg);

      gboolean same_pkg_chksum = FALSE;
      if (!commit_has_matching_repodata_chksum_repr (commit,
                                                     expected_chksum_repr.c_str(),
                                                     &same_pkg_chksum, error))
        return FALSE;

      if (!same_pkg_chksum)
        return TRUE; /* Note early return */

      /* We need to handle things like the nodocs flag changing; in that case we
       * have to redownload.
       */
      const bool global_nodocs = (self->treefile_rs && !self->treefile_rs->get_documentation());

      gboolean pkgcache_commit_is_nodocs;
      if (!g_variant_dict_lookup (metadata_dict, "rpmostree.nodocs", "b", &pkgcache_commit_is_nodocs))
        pkgcache_commit_is_nodocs = FALSE;

      /* We treat a mismatch of documentation state as simply not being
       * imported at all.
       */
      if (global_nodocs != (bool)pkgcache_commit_is_nodocs)
        return TRUE;
    }

  /* We found an import, let's load the sepolicy state */
  *out_in_ostree = TRUE;
  if (sepolicy)
    {
      if (!commit_has_matching_sepolicy (commit, sepolicy,
                                         out_selinux_match, error))
        return FALSE;
    }

  return TRUE;
}

void
rpmostree_set_repos_on_packages (DnfContext *dnfctx,
                                 GPtrArray  *packages)
{
  GPtrArray *sources = dnf_context_get_repos (dnfctx);
  /* ownership of key and val stays in sources */
  g_autoptr(GHashTable) name_to_repo = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < sources->len; i++)
    {
      DnfRepo *source = (DnfRepo*)sources->pdata[i];
      g_hash_table_insert (name_to_repo, (gpointer)dnf_repo_get_id (source), source);
    }

  for (guint i = 0; i < packages->len; i++)
    {
      DnfPackage *pkg = (DnfPackage*)packages->pdata[i];
      const char *reponame = dnf_package_get_reponame (pkg);
      gboolean is_locally_cached =
        (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0);

      if (is_locally_cached)
        continue;

      DnfRepo *repo = (DnfRepo*)g_hash_table_lookup (name_to_repo, reponame);
      g_assert (repo);

      dnf_package_set_repo (pkg, repo);
    }
}

/* determine of all the marked packages, which ones we'll need to download,
 * which ones we'll need to import, and which ones we'll need to relabel */
static gboolean
sort_packages (RpmOstreeContext *self,
               GPtrArray        *packages,
               GCancellable     *cancellable,
               GError          **error)
{
  DnfContext *dnfctx = self->dnfctx;

  g_assert (!self->pkgs_to_download);
  self->pkgs_to_download = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  g_assert (!self->pkgs_to_import);
  self->pkgs_to_import = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  self->n_async_pkgs_imported = 0;
  g_assert (!self->pkgs_to_relabel);
  self->pkgs_to_relabel = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  self->n_async_pkgs_relabeled = 0;

  /* make sure all the non-cached pkgs have their repos set */
  rpmostree_set_repos_on_packages (dnfctx, packages);

  for (guint i = 0; i < packages->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(packages->pdata[i]);

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      const char *reponame = dnf_package_get_reponame (pkg);
      gboolean is_locally_cached =
        (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0);

      /* NB: We're assuming here that the presence of an ostree repo means that
       * the user intends to import the pkg vs e.g. installing it like during a
       * treecompose. Even though in the treecompose case, an ostree repo *is*
       * given, since it shouldn't have imported pkgs in there, the logic below
       * will work. (So e.g. pkgs might still get added to the import array in
       * the treecompose path, but since it will never call import(), that
       * doesn't matter). In the future, we might want to allow the user of
       * RpmOstreeContext to express *why* they are calling prepare().
       * */

      {
        gboolean in_ostree = FALSE;
        gboolean selinux_match = FALSE;
        gboolean cached = pkg_is_cached (pkg);

        if (!find_pkg_in_ostree (self, pkg, self->sepolicy,
                                 &in_ostree, &selinux_match, error))
          return FALSE;

        if (is_locally_cached)
          g_assert (in_ostree);

        if (!in_ostree && !cached)
          g_ptr_array_add (self->pkgs_to_download, g_object_ref (pkg));
        if (!in_ostree)
          g_ptr_array_add (self->pkgs_to_import, g_object_ref (pkg));
        /* This logic is equivalent to that in rpmostree_context_force_relabel() */
        if (in_ostree && !selinux_match)
          g_ptr_array_add (self->pkgs_to_relabel, g_object_ref (pkg));
      }
    }

  return TRUE;
}

/* Set @error with a string containing an error relating to @pkgs */
static gboolean
throw_package_list (GError **error, const char *prefix, GPtrArray *pkgs)
{
  if (!error)
    return FALSE; /* Note early simultaneously happy and sad return */

  g_autoptr(GString) msg = g_string_new (prefix);
  g_string_append (msg, ": ");

  gboolean first = TRUE;
  for (guint i = 0; i < pkgs->len; i++)
    {
      if (!first)
        g_string_append (msg, ", ");
      g_string_append (msg, static_cast<char*>(pkgs->pdata[i]));
      first = FALSE;
    }

  /* need a glnx_set_error_steal */
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg->str);
  return FALSE;
}

static GVariant*
gv_nevra_from_pkg (DnfPackage *pkg)
{
  return g_variant_ref_sink (g_variant_new ("(sstsss)",
                             dnf_package_get_nevra (pkg),
                             dnf_package_get_name (pkg),
                             dnf_package_get_epoch (pkg),
                             dnf_package_get_version (pkg),
                             dnf_package_get_release (pkg),
                             dnf_package_get_arch (pkg)));
}

/* g_variant_hash can't handle container types */
static guint
gv_nevra_hash (gconstpointer v)
{
  g_autoptr(GVariant) nevra = g_variant_get_child_value ((GVariant*)v, 0);
  return g_str_hash (g_variant_get_string (nevra, NULL));
}

/* Before hy_goal_lock(), this function was the only way for us to make sure that libsolv
 * wasn't trying to modify a base package it wasn't supposed to. Its secondary purpose was
 * to collect the packages being replaced and removed into `self->pkgs_to_replace` and
 * `self->pkgs_to_remove` respectively.
 *
 * Nowadays, that secondary purpose is now its primary purpose because we're already
 * guaranteed the first part by hy_goal_lock(). Though we still leave all those original
 * checks in place as a fail-safe in case libsolv messes up.
 */
static gboolean
check_goal_solution (RpmOstreeContext *self,
                     GPtrArray        *removed_pkgnames,
                     GPtrArray        *replaced_nevras,
                     GError          **error)
{
  HyGoal goal = dnf_context_get_goal (self->dnfctx);

  /* check that we're not removing anything we didn't expect */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new_with_free_func (g_free);

    /* also collect info about those we're removing */
    g_assert (!self->pkgs_to_remove);
    self->pkgs_to_remove = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify)g_variant_unref);
    g_autoptr(GPtrArray) packages = dnf_goal_get_packages (goal,
                                                           DNF_PACKAGE_INFO_REMOVE,
                                                           DNF_PACKAGE_INFO_OBSOLETE, -1);
    for (guint i = 0; i < packages->len; i++)
      {
        auto pkg = static_cast<DnfPackage *>(packages->pdata[i]);
        const char *name = dnf_package_get_name (pkg);
        const char *nevra = dnf_package_get_nevra (pkg);

        /* did we expect this package to be removed? */
        if (rpmostree_str_ptrarray_contains (removed_pkgnames, name))
          g_hash_table_insert (self->pkgs_to_remove, g_strdup (name),
                                                     gv_nevra_from_pkg (pkg));
        else
          g_ptr_array_add (forbidden, g_strdup (nevra));
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "Base packages would be removed", forbidden);
  }

  /* check that all the pkgs we expect to remove are marked for removal */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new ();

    for (guint i = 0; i < removed_pkgnames->len; i++)
      {
        auto pkgname = static_cast<const char *>(removed_pkgnames->pdata[i]);
        if (!g_hash_table_contains (self->pkgs_to_remove, pkgname))
          g_ptr_array_add (forbidden, (gpointer)pkgname);
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "Base packages not marked to be removed", forbidden);
  }

  /* REINSTALLs should never happen since it doesn't make sense in the rpm-ostree flow, and
   * we check very early whether a package is already in the rootfs or not, but let's check
   * for it anyway so that we get a bug report in case it somehow happens. */
  { g_autoptr(GPtrArray) packages =
      dnf_goal_get_packages (goal, DNF_PACKAGE_INFO_REINSTALL, -1);
    g_assert_cmpint (packages->len, ==, 0);
  }

  /* Look at UPDATE and DOWNGRADE, and see whether they're doing what we expect */
  { g_autoptr(GHashTable) forbidden_replacements =
      g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref,
                                         (GDestroyNotify)g_object_unref);

    /* also collect info about those we're replacing */
    g_assert (!self->pkgs_to_replace);
    self->pkgs_to_replace = g_hash_table_new_full (gv_nevra_hash, g_variant_equal,
                                                   (GDestroyNotify)g_variant_unref,
                                                   (GDestroyNotify)g_variant_unref);
    g_autoptr(GPtrArray) packages = dnf_goal_get_packages (goal,
                                                           DNF_PACKAGE_INFO_UPDATE,
                                                           DNF_PACKAGE_INFO_DOWNGRADE, -1);
    for (guint i = 0; i < packages->len; i++)
      {
        auto pkg = static_cast<DnfPackage *>(packages->pdata[i]);
        const char *nevra = dnf_package_get_nevra (pkg);

        /* just pick the first pkg */
        g_autoptr(GPtrArray) old = hy_goal_list_obsoleted_by_package (goal, pkg);
        g_assert_cmpint (old->len, >, 0);
        auto old_pkg = static_cast<DnfPackage *>(old->pdata[0]);

        /* did we expect this nevra to replace a base pkg? */
        if (rpmostree_str_ptrarray_contains (replaced_nevras, nevra))
          g_hash_table_insert (self->pkgs_to_replace, gv_nevra_from_pkg (pkg),
                                                      gv_nevra_from_pkg (old_pkg));
        else
          g_hash_table_insert (forbidden_replacements, g_object_ref (old_pkg),
                                                       g_object_ref (pkg));
      }

    if (g_hash_table_size (forbidden_replacements) > 0)
      {
        rpmostree_output_message ("Forbidden base package replacements:");
        GLNX_HASH_TABLE_FOREACH_KV (forbidden_replacements, DnfPackage*, old_pkg,
                                                            DnfPackage*, new_pkg)
          {
            const char *old_name = dnf_package_get_name (old_pkg);
            const char *new_name = dnf_package_get_name (new_pkg);
            const char *new_repo = dnf_package_get_reponame (new_pkg);
            if (g_str_equal (old_name, new_name))
              rpmostree_output_message ("  %s %s -> %s (%s)", old_name,
                                        dnf_package_get_evr (old_pkg),
                                        dnf_package_get_evr (new_pkg), new_repo);
            else
              rpmostree_output_message ("  %s -> %s (%s)",
                                        dnf_package_get_nevra (old_pkg),
                                        dnf_package_get_nevra (new_pkg), new_repo);
          }
        rpmostree_output_message ("This likely means that some of your layered packages "
                                  "have requirements on newer or older versions of some "
                                  "base packages. Doing `rpm-ostree cleanup -m` and "
                                  "`rpm-ostree upgrade` first may help. For "
                                  "more details, see: "
                                  "https://github.com/projectatomic/rpm-ostree/issues/415");

        return glnx_throw (error, "Some base packages would be replaced");
      }
  }

  /* check that all the pkgs we expect to replace are marked for replacement */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new_with_free_func (g_free);

    GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_replace, GVariant*, newv, GVariant*, old)
      {
        g_autoptr(GVariant) nevra_v = g_variant_get_child_value (newv, 0);
        const char *nevra = g_variant_get_string (nevra_v, NULL);
        if (!rpmostree_str_ptrarray_contains (replaced_nevras, nevra))
          g_ptr_array_add (forbidden, g_strdup (nevra));
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "Base packages not marked to be installed", forbidden);
  }

  return TRUE;
}

static gboolean
add_pkg_from_cache (RpmOstreeContext *self,
                    const char       *nevra,
                    const char       *sha256,
                    DnfPackage       **out_pkg,
                    GCancellable     *cancellable,
                    GError          **error)
{
  if (!checkout_pkg_metadata_by_nevra (self, nevra, sha256, cancellable, error))
    return FALSE;

  /* This is the great lie: we make libdnf et al. think that they're dealing with a full
   * RPM, all while crossing our fingers that they don't try to look past the header.
   * Ideally, it would be best if libdnf could learn to treat the pkgcache repo as another
   * DnfRepo. We could do this all in-memory though it doesn't seem like libsolv has an
   * appropriate API for this. */
  g_autofree char *rpm = g_strdup_printf ("%s/metarpm/%s.rpm", self->tmpdir.path, nevra);
  g_autoptr(DnfPackage) pkg =
    dnf_sack_add_cmdline_package (dnf_context_get_sack (self->dnfctx), rpm);
  if (!pkg)
    return glnx_throw (error, "Failed to add local pkg %s to sack", nevra);

  *out_pkg = util::move_nullify (pkg);
  return TRUE;
}

/* This is a hacky way to bridge the gap between libdnf and our pkgcache. We extract the
 * metarpm for every RPM in our cache and present that as the cmdline repo to libdnf. But we
 * do still want all the niceties of the libdnf stack, e.g. HyGoal, libsolv depsolv, etc...
 */
static gboolean
add_remaining_pkgcache_pkgs (RpmOstreeContext *self,
                             GHashTable       *already_added,
                             GCancellable     *cancellable,
                             GError          **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_assert (pkgcache_repo);
  g_assert (self->pkgcache_only);

  DnfSack *sack = dnf_context_get_sack (self->dnfctx);
  g_assert (sack);

  g_autoptr(GHashTable) refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache_repo, "rpmostree/pkg", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH (refs, const char*, ref)
    {
      auto nevra = std::string (rpmostreecxx::cache_branch_to_nevra (ref));
      if (g_hash_table_contains (already_added, nevra.c_str()))
        continue;

      g_autoptr(GVariant) header = NULL;
      if (!get_header_variant (pkgcache_repo, ref, &header, cancellable, error))
        return FALSE;

      if (!checkout_pkg_metadata (self, nevra.c_str(), header, cancellable, error))
        return FALSE;

      g_autofree char *rpm = g_strdup_printf ("%s/metarpm/%s.rpm", self->tmpdir.path, nevra.c_str());
      g_autoptr(DnfPackage) pkg = dnf_sack_add_cmdline_package (sack, rpm);
      if (!pkg)
        return glnx_throw (error, "Failed to add local pkg %s to sack", nevra.c_str());
    }

  return TRUE;
}

/* Return all the packages that match lockfile constraints. Multiple packages may be
 * returned per NEVRA so that libsolv can respect e.g. repo costs. */
static GPtrArray*
find_locked_packages (RpmOstreeContext *self,
                      GError          **error)
{
  g_assert (self->lockfile);
  DnfContext *dnfctx = self->dnfctx;
  DnfSack *sack = dnf_context_get_sack (dnfctx);

  g_autoptr(GPtrArray) pkgs =
    g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  auto locked_src_pkgs = (*self->lockfile)->get_locked_src_packages();
  std::set<rust::String> locked_src_pkgnames;
  for (auto & pkg : locked_src_pkgs)
    {
      g_autofree char *srpm = g_strdup_printf ("%s-%s.src.rpm", pkg.name.c_str(), pkg.evr.c_str());
      hy_autoquery HyQuery query = hy_query_create (sack);
      hy_query_filter (query, HY_PKG_SOURCERPM, HY_EQ, srpm);
      g_autoptr(GPtrArray) matches = hy_query_run (query);
      if (matches->len == 0)
        {
          return (GPtrArray*)glnx_null_throw (error, "Couldn't find locked source package '%s-%s'",
                                              pkg.name.c_str(), pkg.evr.c_str());
        }
      for (guint i = 0; i < matches->len; i++)
        {
          auto match = static_cast<DnfPackage *>(matches->pdata[i]);
          /* we could optimize this path outside the loop in the future using the
           * g_ptr_array_extend_and_steal API, though that's still too new for e.g. el8 */
          g_ptr_array_add (pkgs, g_object_ref (match));
        }
      locked_src_pkgnames.insert(pkg.name);
    }

  auto locked_pkgs = (*self->lockfile)->get_locked_packages();
  for (auto & pkg : locked_pkgs)
    {
      /* This essentially makes `source-packages` have higher priority than `packages`. This
       * isn't technically correct: what we should do in lockfile.rs is to convert
       * `source-packages` to `packages` *before* merging all the lockfiles, so that e.g. a
       * `packages` in a higher-level lockfile can override a `source-packages` in a lower
       * level. For now this is fine because the primary use case is humans typing source
       * packages where we want it to have the highest precedence. */
      if (locked_src_pkgnames.count(pkg.name))
        continue;

      hy_autoquery HyQuery query = hy_query_create (sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkg.name.c_str());
      hy_query_filter (query, HY_PKG_EVR, HY_EQ, pkg.evr.c_str());
      if (pkg.arch.length() > 0)
        hy_query_filter (query, HY_PKG_ARCH, HY_EQ, pkg.arch.c_str());
      g_autoptr(GPtrArray) matches = hy_query_run (query);

      gboolean at_least_one = FALSE;
      guint n_checksum_mismatches = 0;
      for (guint i = 0; i < matches->len; i++)
        {
          auto match = static_cast<DnfPackage *>(matches->pdata[i]);
          if (pkg.digest.length() == 0)
            {
              /* we could optimize this path outside the loop in the future using the
               * g_ptr_array_extend_and_steal API, though that's still too new for e.g. el8 */
              g_ptr_array_add (pkgs, g_object_ref (match));
              at_least_one = TRUE;
            }
          else
            {
              auto repodata_chksum = rpmostreecxx::get_repodata_chksum_repr(*match);
              if (pkg.digest != repodata_chksum) /* we're comparing two rust::String here */
                n_checksum_mismatches++;
              else
                {
                  g_ptr_array_add (pkgs, g_object_ref (match));
                  at_least_one = TRUE;
                }
            }
        }
      if (!at_least_one)
        {
          g_autofree char *spec =
            g_strdup_printf ("%s-%s%s%s", pkg.name.c_str(), pkg.evr.c_str(),
                             pkg.arch.length() > 0 ? "." : "",
                             pkg.arch.length() > 0 ? pkg.arch.c_str() : "");
          return (GPtrArray*)glnx_null_throw (error, "Couldn't find locked package '%s'%s%s "
                                                     "(pkgs matching NEVRA: %d; mismatched checksums: %d)",
                                              spec, pkg.digest.length() > 0 ? " with checksum " : "",
                                              pkg.digest.length() > 0 ? pkg.digest.c_str() : "",
                                              matches->len, n_checksum_mismatches);
        }
    }

  return util::move_nullify (pkgs);
}

/* Check for/download new rpm-md, then depsolve */
gboolean
rpmostree_context_prepare (RpmOstreeContext *self,
                           GCancellable     *cancellable,
                           GError          **error)
{
  g_assert (!self->empty);

  DnfContext *dnfctx = self->dnfctx;
  g_autofree char **pkgnames = NULL;
  g_autofree char **exclude_packages = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "packages",
                                   "^a&s", &pkgnames));
  g_variant_dict_lookup (self->spec->dict, "exclude-packages",
                         "^a&s", &exclude_packages);

  g_autofree char **cached_pkgnames = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "cached-packages",
                                   "^a&s", &cached_pkgnames));

  g_autofree char **cached_replace_pkgs = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "cached-replaced-base-packages",
                                   "^a&s", &cached_replace_pkgs));

  g_autofree char **removed_base_pkgnames = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "removed-base-packages",
                                   "^a&s", &removed_base_pkgnames));

  /* setup sack if not yet set up */
  if (dnf_context_get_sack (dnfctx) == NULL)
    {
      /* default to loading updateinfo in this path; this allows the sack to be used later
       * on for advisories -- it's always downloaded anyway */
      if (!rpmostree_context_download_metadata (self,
                                                DNF_CONTEXT_SETUP_SACK_FLAG_LOAD_UPDATEINFO,
                                                cancellable, error))
        return FALSE;
      journal_rpmmd_info (self);
    }
  DnfSack *sack = dnf_context_get_sack (dnfctx);
  HyGoal goal = dnf_context_get_goal (dnfctx);

  /* Don't try to keep multiple kernels per root; that's a traditional thing,
   * ostree binds kernel + userspace.
   */
  dnf_sack_set_installonly (sack, NULL);
  dnf_sack_set_installonly_limit (sack, 0);


  if (self->lockfile)
    {
      /* we only support pure installs for now (compose case) */
      g_assert_cmpint (g_strv_length (cached_pkgnames), ==, 0);
      g_assert_cmpint (g_strv_length (cached_replace_pkgs), ==, 0);
      g_assert_cmpint (g_strv_length (removed_base_pkgnames), ==, 0);
    }

  /* track cached pkgs already added to the sack so far */
  g_autoptr(GHashTable) local_pkgs_to_install =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

  /* Handle packages to replace; only add them to the sack for now */
  g_autoptr(GPtrArray) replaced_nevras = g_ptr_array_new ();
  g_autoptr(GPtrArray) replaced_pkgnames = g_ptr_array_new_with_free_func (g_free);
  for (char **it = cached_replace_pkgs; it && *it; it++)
    {
      const char *nevra = *it;
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_autoptr(DnfPackage) pkg = NULL;
      if (!add_pkg_from_cache (self, nevra, sha256, &pkg, cancellable, error))
        return FALSE;

      const char *name = dnf_package_get_name (pkg);
      g_assert (name);

      g_ptr_array_add (replaced_nevras, (gpointer)nevra);
      // this is a bit wasteful, but for locking purposes, we need the pkgname to match
      // against the base, not the nevra which naturally will be different
      g_ptr_array_add (replaced_pkgnames, g_strdup (name));
      g_hash_table_insert (local_pkgs_to_install, (gpointer)nevra, g_steal_pointer (&pkg));
    }

  /* Now handle local package; only add them to the sack for now */
  for (char **it = cached_pkgnames; it && *it; it++)
    {
      const char *nevra = *it;
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_autoptr(DnfPackage) pkg = NULL;
      if (!add_pkg_from_cache (self, nevra, sha256, &pkg, cancellable, error))
        return FALSE;

      g_hash_table_insert (local_pkgs_to_install, (gpointer)nevra, g_steal_pointer (&pkg));
    }

  /* If we're in cache-only mode, add all the remaining pkgs now. We do this *after* the
   * replace & local pkgs since they already handle a subset of the cached pkgs and have
   * SHA256 checks. But we do it *before* dnf_context_install() since those subjects are not
   * directly linked to a cached pkg, so we need to teach libdnf about them beforehand. */
  if (self->pkgcache_only)
    {
      if (!add_remaining_pkgcache_pkgs (self, local_pkgs_to_install, cancellable, error))
        return FALSE;
    }

  /* Now that we're done adding stuff to the sack, we can actually mark pkgs for install and
   * uninstall. We don't want to mix those two steps, otherwise we might confuse libdnf,
   * see: https://github.com/rpm-software-management/libdnf/issues/700 */

  if (self->lockfile)
    {
      /* first, find our locked pkgs in the rpmmd */
      g_autoptr(GPtrArray) locked_pkgs = find_locked_packages (self, error);
      if (!locked_pkgs)
        return FALSE;

      /* build a packageset from it */
      DnfPackageSet *locked_pset = dnf_packageset_new (sack);
      for (guint i = 0; i < locked_pkgs->len; i++)
        dnf_packageset_add (locked_pset, static_cast<DnfPackage*>(locked_pkgs->pdata[i]));

      if (self->lockfile_strict)
        {
          /* In strict mode, we basically *only* want locked packages to be considered, so
           * exclude everything else. Note we still don't directly do `hy_goal_install`
           * here; we want the treefile to still be canonical, but we just make sure that
           * the end result matches what we expect. */
          DnfPackageSet *pset = dnf_packageset_new (sack);
          Map *map = dnf_packageset_get_map (pset);
          map_setall (map);
          map_subtract (map, dnf_packageset_get_map (locked_pset));
          dnf_sack_add_excludes (sack, pset);
          dnf_packageset_free (pset);
        }
      else
        {
          /* Exclude all the packages in lockfile repos except locked packages. */
          g_autofree char **lockfile_repos = NULL;
          g_variant_dict_lookup (self->spec->dict, "lockfile-repos", "^a&s", &lockfile_repos);
          for (char **it = lockfile_repos; it && *it; it++)
            {
              const char *repo = *it;
              hy_autoquery HyQuery query = hy_query_create (sack);
              hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, repo);
              DnfPackageSet *pset = hy_query_run_set (query);
              Map *map = dnf_packageset_get_map (pset);
              map_subtract (map, dnf_packageset_get_map (locked_pset));
              dnf_sack_add_excludes (sack, pset);
              dnf_packageset_free (pset);
            }

          /* In relaxed mode, we allow packages to be added or removed without having to
           * edit lockfiles. However, we still want to make sure that if a package does get
           * installed which is in the lockfile, it can only pick that NEVRA. To do this, we
           * filter out from the sack all pkgs which don't match. */

          /* map of all packages with names found in the lockfile (note here we derive this
           * from `locked_pkgs` instead of from the lockfile again to account for
           * source-packages too (which have been resolved to packages in `locked_pkgs`) */
          DnfPackageSet *named_pkgs = dnf_packageset_new (sack);
          Map *named_pkgs_map = dnf_packageset_get_map (named_pkgs);
          for (guint i = 0; i < locked_pkgs->len; i++)
            {
              DnfPackage *pkg = (DnfPackage*)locked_pkgs->pdata[i];
              hy_autoquery HyQuery query = hy_query_create (sack);
              hy_query_filter (query, HY_PKG_NAME, HY_EQ, dnf_package_get_name (pkg));
              DnfPackageSet *pset = hy_query_run_set (query);
              map_or (named_pkgs_map, dnf_packageset_get_map (pset));
              dnf_packageset_free (pset);
            }

          /* remove our locked packages from the exclusion set */
          map_subtract (named_pkgs_map, dnf_packageset_get_map (locked_pset));
          dnf_sack_add_excludes (sack, named_pkgs);
          dnf_packageset_free (named_pkgs);
        }

      dnf_packageset_free (locked_pset);
    }

  /* Process excludes */
  for (char **iter = exclude_packages; iter && *iter; iter++)
    {
      const char *pkgname = *iter;
      hy_autoquery HyQuery query = hy_query_create (sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkgname);
      DnfPackageSet *pset = hy_query_run_set (query);
      dnf_sack_add_excludes (sack, pset);
      dnf_packageset_free (pset);
    }

  /* First, handle packages to remove */
  g_autoptr(GPtrArray) removed_pkgnames = g_ptr_array_new ();
  for (char **it = removed_base_pkgnames; it && *it; it++)
    {
      const char *pkgname = *it;
      if (!dnf_context_remove (dnfctx, pkgname, error))
        return FALSE;

      g_ptr_array_add (removed_pkgnames, (gpointer)pkgname);
    }

  /* Then, handle local packages to install */
  GLNX_HASH_TABLE_FOREACH_V (local_pkgs_to_install, DnfPackage*, pkg)
    hy_goal_install (goal,  pkg);

  /* Now repo-packages; only supported during server composes for now. */
  if (!self->is_system)
    {
      auto repo_pkgs = self->treefile_rs->get_repo_packages();
      for (auto & repo_pkg : repo_pkgs)
        {
          auto repo = std::string(repo_pkg.get_repo());
          auto pkgs = repo_pkg.get_packages();
          for (auto & pkg_str : pkgs)
            {
              auto pkg = std::string(pkg_str);
              g_auto(HySubject) subject = hy_subject_create(pkg.c_str());
              /* this is basically like a slightly customized dnf_context_install() */
              HyNevra nevra = NULL;
              hy_autoquery HyQuery query =
                hy_subject_get_best_solution(subject, sack, NULL, &nevra, FALSE, TRUE, TRUE, TRUE, FALSE);
              hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, repo.c_str());
              g_autoptr(DnfPackageSet) pset = hy_query_run_set (query);
              if (dnf_packageset_count (pset) == 0)
                return glnx_throw (error, "No matches for '%s' in repo '%s'", pkg.c_str(), repo.c_str());

              g_auto(HySelector) selector = hy_selector_create (sack);
              hy_selector_pkg_set (selector, pset);
              if (!hy_goal_install_selector (goal, selector, error))
                return FALSE;
            }
        }
    }

  /* And finally, handle repo packages to install */
  g_autoptr(GPtrArray) missing_pkgs = NULL;
  for (char **it = pkgnames; it && *it; it++)
    {
      const char *pkgname = *it;
      g_autoptr(GError) local_error = NULL;
      if (!dnf_context_install (dnfctx, pkgname, &local_error))
        {
          /* Only keep going if it's ENOENT, so we coalesce into one msg at the end */
          if (!g_error_matches (local_error, DNF_ERROR, DNF_ERROR_PACKAGE_NOT_FOUND))
            {
              g_propagate_error (error, util::move_nullify (local_error));
              return FALSE;
            }
          /* lazy init since it's unlikely in the common case (e.g. upgrades) */
          if (!missing_pkgs)
            missing_pkgs = g_ptr_array_new ();
          g_ptr_array_add (missing_pkgs, (gpointer)pkgname);
        }
    }

  if (missing_pkgs && missing_pkgs->len > 0)
    return throw_package_list (error, "Packages not found", missing_pkgs);

  /* And lock all the base packages we don't expect to be replaced. */
  { hy_autoquery HyQuery query = hy_query_create (sack);
    hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
    g_autoptr(GPtrArray) pkgs = hy_query_run (query);
    for (guint i = 0; i < pkgs->len; i++)
      {
        auto pkg = static_cast<DnfPackage *>(pkgs->pdata[i]);
        const char *pkgname = dnf_package_get_name (pkg);
        g_assert (pkgname);

        if (rpmostree_str_ptrarray_contains (removed_pkgnames, pkgname) ||
            rpmostree_str_ptrarray_contains (replaced_pkgnames, pkgname))
          continue;
        if (hy_goal_lock (goal, pkg, error) != 0)
          return glnx_prefix_error (error, "while locking pkg '%s'", pkgname);
      }
  }

  auto actions = static_cast<DnfGoalActions>(DNF_INSTALL | DNF_ALLOW_UNINSTALL);
  if (self->treefile_rs && !self->treefile_rs->get_recommends())
    actions = static_cast<DnfGoalActions>(static_cast<int>(actions) | DNF_IGNORE_WEAK_DEPS);
  auto task = rpmostreecxx::progress_begin_task("Resolving dependencies");
  /* XXX: consider a --allow-uninstall switch? */
  if (!dnf_goal_depsolve (goal, actions, error) ||
      !check_goal_solution (self, removed_pkgnames, replaced_nevras, error))
    return FALSE;
  g_clear_pointer (&self->pkgs, (GDestroyNotify)g_ptr_array_unref);
  self->pkgs = dnf_goal_get_packages (goal,
                                      DNF_PACKAGE_INFO_INSTALL,
                                      DNF_PACKAGE_INFO_UPDATE,
                                      DNF_PACKAGE_INFO_DOWNGRADE, -1);
  if (!sort_packages (self, self->pkgs, cancellable, error))
    return glnx_prefix_error (error, "Sorting packages");

  return TRUE;
}

/* Must have invoked rpmostree_context_prepare().
 * Returns: (transfer container): All packages in the depsolved list.
 */
GPtrArray *
rpmostree_context_get_packages (RpmOstreeContext *self)
{
  return g_ptr_array_ref (self->pkgs);
}

/* Returns a reference to the set of packages that will be imported */
GPtrArray *
rpmostree_context_get_packages_to_import (RpmOstreeContext *self)
{
  g_assert (self->pkgs_to_import);
  return g_ptr_array_ref (self->pkgs_to_import);
}

/* Note this must be called *before* rpmostree_context_setup(). */
void
rpmostree_context_set_lockfile (RpmOstreeContext *self,
                                char            **lockfiles,
                                gboolean          strict)
{
  g_assert (!self->lockfile);
  rust::Vec<rust::String> rs_lockfiles;
  for (char **it = lockfiles; it && *it; it++)
    rs_lockfiles.push_back(std::string(*it));
  self->lockfile = rpmostreecxx::lockfile_read(rs_lockfiles);
  self->lockfile_strict = strict;
}

/* XXX: push this into libdnf */
static const char*
convert_dnf_action_to_string (DnfStateAction action)
{
  switch (action)
    {
    case DNF_STATE_ACTION_INSTALL:
      return "install";
    case DNF_STATE_ACTION_UPDATE:
      return "update";
    case DNF_STATE_ACTION_DOWNGRADE:
      return "downgrade";
    case DNF_STATE_ACTION_REMOVE:
      return "remove";
    case DNF_STATE_ACTION_OBSOLETE:
      return "obsolete";
    default:
      g_assert_not_reached ();
    }
  return NULL; /* satisfy static analysis tools */
}

/* Generate a checksum from a goal in a repeatable fashion - we checksum an ordered array of
 * the checksums of individual packages as well as the associated action. We *used* to just
 * checksum the NEVRAs but that breaks with RPM gpg signatures.
 *
 * This can be used to efficiently see if the goal has changed from a previous one.
 */
gboolean
rpmostree_dnf_add_checksum_goal (GChecksum   *checksum,
                                 HyGoal       goal,
                                 OstreeRepo  *pkgcache,
                                 GError     **error)
{
  g_autoptr(GPtrArray) pkglist = dnf_goal_get_packages (goal, DNF_PACKAGE_INFO_INSTALL,
                                                              DNF_PACKAGE_INFO_UPDATE,
                                                              DNF_PACKAGE_INFO_DOWNGRADE,
                                                              DNF_PACKAGE_INFO_REMOVE,
                                                              DNF_PACKAGE_INFO_OBSOLETE,
                                                              -1);
  g_assert (pkglist);
  g_ptr_array_sort (pkglist, compare_pkgs);
  for (guint i = 0; i < pkglist->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(pkglist->pdata[i]);
      DnfStateAction action = dnf_package_get_action (pkg);
      const char *action_str = convert_dnf_action_to_string (action);
      g_checksum_update (checksum, (guint8*)action_str, strlen (action_str));

      /* For pkgs that were added from the pkgcache repo (e.g. local RPMs and replacement
       * overrides), make sure to pick up the SHA256 from the pkg metadata, rather than what
       * libsolv figured out (which is based on our chopped off fake RPM, which clearly will
       * not match what's in the repo). This ensures that two goals are equivalent whether
       * the same RPM comes from a yum repo or from the pkgcache. */
      const char *reponame = NULL;
      if (pkgcache)
        reponame = dnf_package_get_reponame (pkg);
      if (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0)
        {
          g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
          g_autofree char *cached_rev = NULL;
          if (!ostree_repo_resolve_rev (pkgcache, cachebranch, FALSE, &cached_rev, error))
            return FALSE;

          g_autoptr(GVariant) commit = NULL;
          if (!ostree_repo_load_commit (pkgcache, cached_rev, &commit, NULL, error))
            return FALSE;

          g_autofree char *chksum_repr = NULL;
          if (!get_pkgcache_repodata_chksum_repr (commit, &chksum_repr, TRUE, error))
            return FALSE;

          if (chksum_repr)
            {
              g_checksum_update (checksum, (guint8*)chksum_repr, strlen (chksum_repr));
              continue;
            }
        }

      auto chksum_repr = rpmostreecxx::get_repodata_chksum_repr(*pkg);
      g_checksum_update (checksum, (guint8*)chksum_repr.data(), chksum_repr.size());
    }

  return TRUE;
}

gboolean
rpmostree_context_get_state_sha512 (RpmOstreeContext *self,
                                    char            **out_checksum,
                                    GError          **error)
{
  g_autoptr(GChecksum) state_checksum = g_checksum_new (G_CHECKSUM_SHA512);
  g_checksum_update (state_checksum, static_cast<const guchar*>(g_variant_get_data (self->spec->spec)),
                     g_variant_get_size (self->spec->spec));

  if (!self->empty)
    {
      if (!rpmostree_dnf_add_checksum_goal (state_checksum,
                                            dnf_context_get_goal (self->dnfctx),
                                            get_pkgcache_repo (self), error))
        return FALSE;
    }

  *out_checksum = g_strdup (g_checksum_get_string (state_checksum));
  return TRUE;
}

static GHashTable *
gather_source_to_packages (GPtrArray *packages)
{
  g_autoptr(GHashTable) source_to_packages =
    g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  for (guint i = 0; i < packages->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(packages->pdata[i]);

      /* ignore local packages */
      if (rpmostree_pkg_is_local (pkg))
        continue;

      DnfRepo *src = dnf_package_get_repo (pkg);
      GPtrArray *source_packages;

      g_assert (src);

      source_packages = static_cast<GPtrArray*>(g_hash_table_lookup (source_to_packages, src));
      if (!source_packages)
        {
          source_packages = g_ptr_array_new ();
          g_hash_table_insert (source_to_packages, src, source_packages);
        }
      g_ptr_array_add (source_packages, pkg);
    }

  return util::move_nullify (source_to_packages);
}

gboolean
rpmostree_download_packages (GPtrArray      *packages,
                             GCancellable   *cancellable,
                             GError        **error)
{
  guint progress_sigid;
  g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (packages);
  GLNX_HASH_TABLE_FOREACH_KV (source_to_packages, DnfRepo*, src, GPtrArray*, src_packages)
    {
      glnx_unref_object DnfState *hifstate = dnf_state_new ();
      auto msg = g_strdup_printf("Downloading from '%s'", dnf_repo_get_id(src));
      auto progress = rpmostreecxx::progress_percent_begin(msg);
      progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                          G_CALLBACK (on_hifstate_percentage_changed),
                                          (void*)progress.get());
      g_autofree char *target_dir = g_build_filename (dnf_repo_get_location (src), "/packages/", NULL);
      if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
        return FALSE;

      if (!dnf_repo_download_packages (src, src_packages, target_dir,
                                        hifstate, error))
        return FALSE;

      g_signal_handler_disconnect (hifstate, progress_sigid);
    }

  return TRUE;
}

gboolean
rpmostree_context_download (RpmOstreeContext *self,
                            GCancellable     *cancellable,
                            GError          **error)
{
  int n = self->pkgs_to_download->len;

  if (n > 0)
    {
      guint64 size =
        dnf_package_array_get_download_size (self->pkgs_to_download);
      g_autofree char *sizestr = g_format_size (size);
      rpmostree_output_message ("Will download: %u package%s (%s)", n, _NS(n), sizestr);
    }
  else
    return TRUE;

  return rpmostree_download_packages (self->pkgs_to_download, cancellable, error);
}

static gboolean
async_imports_mainctx_iter (gpointer user_data);

/* Called on completion of an async import; runs on main thread */
static void
on_async_import_done (GObject                    *obj,
                      GAsyncResult               *res,
                      gpointer                    user_data)
{
  auto importer = (RpmOstreeImporter*)(obj);
  auto self = static_cast<RpmOstreeContext *>(user_data);
  g_autofree char *rev =
    rpmostree_importer_run_async_finish (importer, res,
                                         self->async_error ? NULL : &self->async_error);
  if (!rev)
    {
      if (self->async_cancellable)
        g_cancellable_cancel (self->async_cancellable);
      g_assert (self->async_error != NULL);
    }

  g_assert_cmpint (self->n_async_pkgs_imported, <, self->pkgs_to_import->len);
  self->n_async_pkgs_imported++;
  g_assert_cmpint (self->n_async_running, >, 0);
  self->n_async_running--;
  self->async_progress->nitems_update(self->n_async_pkgs_imported);
  async_imports_mainctx_iter (self);
}

/* Queue an asynchronous import of a package */
static gboolean
start_async_import_one_package (RpmOstreeContext *self, DnfPackage *pkg,
                                GCancellable *cancellable,
                                GError **error)
{
  glnx_fd_close int fd = -1;
  if (!rpmostree_context_consume_package (self, pkg, &fd, error))
    return FALSE;

  /* Only set SKIP_EXTRANEOUS for packages we know need it, so that
   * people doing custom composes don't have files silently discarded.
   * (This will also likely need to be configurable).
   */
  const char *pkg_name = dnf_package_get_name (pkg);

  int flags = 0;
  if (g_str_equal (pkg_name, "filesystem") ||
      g_str_equal (pkg_name, "rootfiles"))
    flags |= RPMOSTREE_IMPORTER_FLAGS_SKIP_EXTRANEOUS;

  if (self->treefile_rs && !self->treefile_rs->get_documentation())
    flags |= RPMOSTREE_IMPORTER_FLAGS_NODOCS;

  if (self->treefile_rs && self->treefile_rs->get_readonly_executables())
    flags |= RPMOSTREE_IMPORTER_FLAGS_RO_EXECUTABLES;

  /* TODO - tweak the unpacker flags for containers */
  OstreeRepo *ostreerepo = get_pkgcache_repo (self);
  g_autoptr(RpmOstreeImporter) unpacker =
    rpmostree_importer_new_take_fd (&fd, ostreerepo, pkg, static_cast<RpmOstreeImporterFlags>(flags),
                                    self->sepolicy, error);
  if (!unpacker)
    return glnx_prefix_error (error, "creating importer");

  rpmostree_importer_run_async (unpacker, cancellable, on_async_import_done, self);

  return TRUE;
}

/* First function run on mainloop, and called after completion as well. Ensures
 * that we have a bounded number of tasks concurrently executing until
 * finishing.
 */
static gboolean
async_imports_mainctx_iter (gpointer user_data)
{
  auto self = static_cast<RpmOstreeContext *>(user_data);

  while (self->async_index < self->pkgs_to_import->len &&
         self->n_async_running < self->n_async_max &&
         self->async_error == NULL)
    {
      auto pkg = static_cast<DnfPackage *>(self->pkgs_to_import->pdata[self->async_index]);
      if (!start_async_import_one_package (self, pkg, self->async_cancellable, &self->async_error))
        {
          g_cancellable_cancel (self->async_cancellable);
          break;
        }
      self->async_index++;
      self->n_async_running++;
    }

  if (self->n_async_running == 0)
    {
      self->async_running = FALSE;
      g_main_context_wakeup (g_main_context_get_thread_default ());
    }

  return FALSE;
}

gboolean
rpmostree_context_import (RpmOstreeContext *self,
                          GCancellable     *cancellable,
                          GError          **error)
{
  DnfContext *dnfctx = self->dnfctx;
  const int n = self->pkgs_to_import->len;
  if (n == 0)
    return TRUE;

  OstreeRepo *repo = get_pkgcache_repo (self);
  g_assert (repo != NULL);

  if (!dnf_transaction_import_keys (dnf_context_get_transaction (dnfctx), error))
    return FALSE;

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  /* Note use of commit-on-failure */
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, cancellable, error))
    return FALSE;

  self->async_running = TRUE;
  self->async_index = 0;
  self->n_async_running = 0;
  /* We're CPU bound, so just use processors */
  self->n_async_max = g_get_num_processors ();
  self->async_cancellable = cancellable;

  self->async_progress = rpmostreecxx::progress_nitems_begin(self->pkgs_to_import->len, "Importing packages");

  /* Process imports */
  GMainContext *mainctx = g_main_context_get_thread_default ();
  { g_autoptr(GSource) src = g_timeout_source_new (0);
    g_source_set_priority (src, G_PRIORITY_HIGH);
    g_source_set_callback (src, async_imports_mainctx_iter, self, NULL);
    g_source_attach (src, mainctx); /* Note takes a ref */
  }

  self->async_error = NULL;
  while (self->async_running)
    g_main_context_iteration (mainctx, TRUE);
  if (self->async_error)
    {
      g_propagate_error (error, util::move_nullify (self->async_error));
      return glnx_prefix_error (error, "importing RPMs");
    }

  self->async_progress->end("");
  self->async_progress.release();

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PKG_IMPORT),
                   "MESSAGE=Imported %u pkg%s", n, _NS(n),
                   "IMPORTED_N_PKGS=%u", n, NULL);

  return TRUE;
}

// Perform GPG verification for a package, if the repo has gpgcheck=1.
static gboolean
validate_package (RpmOstreeContext  *self, DnfPackage *pkg, GError **error)
{
  if (!dnf_transaction_ensure_repo (dnf_context_get_transaction (self->dnfctx), pkg, error))
    return FALSE;
  DnfRepo *repo = dnf_package_get_repo(pkg);
  // If there's no repo, or gpgcheck is disabled, we're done.
  if (repo == NULL || !dnf_repo_get_gpgcheck(repo))
    return TRUE;

  // Use the QubesOS rpm-oxide to parse and GPG check the RPM
  try {
    rpmostreecxx::rpm_gpgcheck_and_payload_validate(dnf_package_get_filename (pkg));
  } catch (std::exception &e) {
    g_autofree char *prefix = g_strdup_printf ("Failed to GPG check %s", dnf_package_get_nevra (pkg));
    util::rethrow_prefixed(e, prefix);
  }

  // Also use the librpm version for now to ensure we're not reducing security.
  // This may change in the future.
  if (!dnf_transaction_gpgcheck_package (dnf_context_get_transaction (self->dnfctx), pkg, error))
    return FALSE;

  return TRUE;
}

/* Given a single package, verify its GPG signature (if enabled), open a file
 * descriptor for it, and delete the on-disk downloaded copy.
 */
gboolean
rpmostree_context_consume_package (RpmOstreeContext  *self,
                                   DnfPackage        *pkg,
                                   int               *out_fd,
                                   GError           **error)
{
  if (!validate_package (self, pkg, error))
    return FALSE;

  g_autofree char *pkg_path = rpmostree_pkg_get_local_path (pkg);
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (AT_FDCWD, pkg_path, TRUE, &fd, error))
    return FALSE;

  /* And delete it now; this does mean if we fail it'll have been
   * deleted and hence more annoying to debug, but in practice people
   * should be able to redownload, and if the error was something like
   * ENOSPC, deleting it was the right move I'd say.
   */
  if (!rpmostree_pkg_is_local (pkg))
    {
      if (!glnx_unlinkat (AT_FDCWD, pkg_path, 0, error))
        return FALSE;
    }

  *out_fd = glnx_steal_fd (&fd);
  return TRUE;
}

/* Builds a mapping from filename to rpm color */
static void
add_te_files_to_ht (rpmte        te,
                    GHashTable  *ht)
{
  g_auto(rpmfiles) files = rpmteFiles (te);
  g_auto(rpmfi) fi = rpmfilesIter (files, RPMFI_ITER_FWD);

  while (rpmfiNext (fi) >= 0)
    {
      const char *fn = rpmfiFN (fi);
      rpm_color_t color = rpmfiFColor (fi);
      g_hash_table_insert (ht, g_strdup (fn), GUINT_TO_POINTER (color));
    }
}

static char*
canonicalize_rpmfi_path (const char *path)
{
  /* this is a bit awkward; we relativize for the translation, but then make it absolute
   * again to match libostree */
  path += strspn (path, "/");
  g_autofree char *translated =
    rpmostree_translate_path_for_ostree (path) ?: g_strdup (path);
  return g_build_filename ("/", translated, NULL);
}

/* Convert e.g. lib/foo/bar → usr/lib/foo/bar */
static char *
canonicalize_non_usrmove_path (RpmOstreeContext  *self,
                               const char *path)
{
  const char *slash = strchr (path, '/');
  if (!slash)
    return NULL;
  const char *prefix = strndupa (path, slash - path);
  auto link = static_cast<const char *>(g_hash_table_lookup (self->rootfs_usrlinks, prefix));
  if (!link)
    return NULL;
  return g_build_filename (link, slash + 1, NULL);
}

/* This is a lighter version of calculations that librpm calls "file disposition".
 * Essentially, we determine which file removals/installations should be skipped. The librpm
 * functions and APIs for these are unfortunately private since they're just run as part of
 * rpmtsRun(). XXX: see if we can make the rpmfs APIs public, but we'd still need to support
 * RHEL/CentOS anyway. */
static gboolean
handle_file_dispositions (RpmOstreeContext *self,
                          int           tmprootfs_dfd,
                          rpmts         ts,
                          GHashTable  **out_files_skip_add,
                          GHashTable  **out_files_skip_delete,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(GHashTable) pkgs_deleted = g_hash_table_new (g_direct_hash, g_direct_equal);

  /* note these entries are *not* canonicalized for ostree conventions */
  g_autoptr(GHashTable) files_deleted =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) files_added =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  const guint n_rpmts_elements = (guint)rpmtsNElements (ts);
  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ts, i);
      rpmElementType type = rpmteType (te);
      if (type == TR_REMOVED)
        g_hash_table_add (pkgs_deleted, GUINT_TO_POINTER (rpmteDBInstance (te)));
      add_te_files_to_ht (te, type == TR_REMOVED ? files_deleted : files_added);
    }

  /* this we *do* canonicalize since we'll be comparing against ostree paths */
  g_autoptr(GHashTable) files_skip_add =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  /* this one we *don't* canonicalize since we'll be comparing against rpmfi paths */
  g_autoptr(GHashTable) files_skip_delete =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* we deal with color similarly to librpm (compare with skipInstallFiles()) */
  rpm_color_t ts_color = rpmtsColor (ts);
  rpm_color_t ts_prefcolor = rpmtsPrefColor (ts);

  /* ignore colored files not in our rainbow */
  GLNX_HASH_TABLE_FOREACH_IT (files_added, it, const char*, fn, gpointer, colorp)
    {
      rpm_color_t color = GPOINTER_TO_UINT (colorp);
      if (color && ts_color && !(ts_color & color))
        g_hash_table_add (files_skip_add, canonicalize_rpmfi_path (fn));
    }

  g_auto(rpmdbMatchIterator) it = rpmtsInitIterator (ts, RPMDBI_PACKAGES, NULL, 0);
  Header h;
  while ((h = rpmdbNextIterator (it)) != NULL)
    {
      /* is this pkg to be deleted? */
      guint off = rpmdbGetIteratorOffset (it);
      if (g_hash_table_contains (pkgs_deleted, GUINT_TO_POINTER (off)))
        continue;

      /* try to only load what we need: filenames and colors */
      rpmfiFlags flags = RPMFI_FLAGS_ONLY_FILENAMES;
      flags &= ~RPMFI_NOFILECOLORS;

      g_auto(rpmfi) fi = rpmfiNew (ts, h, RPMTAG_BASENAMES, flags);

      fi = rpmfiInit (fi, 0);
      while (rpmfiNext (fi) >= 0)
        {
          const char *fn = rpmfiFN (fi);
          g_assert (fn != NULL);

          /* check if one of the pkgs to delete wants to delete our file */
          if (g_hash_table_contains (files_deleted, fn))
            g_hash_table_add (files_skip_delete, g_strdup (fn));

          rpm_color_t color = (rpmfiFColor (fi) & ts_color);

          /* let's make the safe assumption that the color mess is only an issue for /usr */
          const char *fn_rel = fn + strspn (fn, "/");

          /* be sure we've canonicalized usr/ */
          g_autofree char *fn_rel_owned = canonicalize_non_usrmove_path (self, fn_rel);
          if (fn_rel_owned)
            fn_rel = fn_rel_owned;

          if (!g_str_has_prefix (fn_rel, "usr/"))
            continue;

          /* check if one of the pkgs to install wants to overwrite our file */
          rpm_color_t other_color =
            GPOINTER_TO_UINT (g_hash_table_lookup (files_added, fn));
          other_color &= ts_color;

          /* see handleColorConflict() */
          if (color && other_color && (color != other_color))
            {
              /* do we already have the preferred color installed? */
              if (color & ts_prefcolor)
                g_hash_table_add (files_skip_add, canonicalize_rpmfi_path (fn));
              else if (other_color & ts_prefcolor)
                {
                  /* the new pkg is bringing our favourite color, give way now so we let
                   * checkout silently write into it */
                  if (!glnx_shutil_rm_rf_at (tmprootfs_dfd, fn_rel, cancellable, error))
                    return FALSE;
                }
            }
        }
    }

  *out_files_skip_add = util::move_nullify (files_skip_add);
  *out_files_skip_delete = util::move_nullify (files_skip_delete);
  return TRUE;
}

typedef struct {
  GHashTable *files_skip;
  GPtrArray  *files_remove_regex;
} FilterData;

static OstreeRepoCheckoutFilterResult
checkout_filter (OstreeRepo         *self,
                 const char         *path,
                 struct stat        *st_buf,
                 gpointer            user_data)
{
  GHashTable *files_skip = ((FilterData*)user_data)->files_skip;
  GPtrArray *files_remove_regex = ((FilterData*)user_data)->files_remove_regex;

  if (files_skip && g_hash_table_size (files_skip) > 0)
    {
      if (g_hash_table_contains (files_skip, path))
        return OSTREE_REPO_CHECKOUT_FILTER_SKIP;
    }

  if (files_remove_regex)
    {
      for (guint i = 0; i < files_remove_regex->len; i++)
        {
          auto regex = static_cast<GRegex *>(g_ptr_array_index (files_remove_regex, i));
          if (g_regex_match (regex, path, static_cast<GRegexMatchFlags>(0), NULL))
            {
              g_print ("Skipping file %s from checkout\n", path);
              return OSTREE_REPO_CHECKOUT_FILTER_SKIP;
            }
        }
    }
  
  /* Hack for nsswitch.conf: the glibc.i686 copy is identical to the one in glibc.x86_64,
   * but because we modify it at treecompose time, UNION_IDENTICAL wouldn't save us here. A
   * better heuristic here might be to skip all /etc files which have a different digest
   * than the one registered in the rpmdb */
  if (g_str_equal (path, "/usr/etc/nsswitch.conf"))
    return OSTREE_REPO_CHECKOUT_FILTER_SKIP;

  return OSTREE_REPO_CHECKOUT_FILTER_ALLOW;
}

static gboolean
checkout_package (OstreeRepo   *repo,
                  int           dfd,
                  const char   *path,
                  OstreeRepoDevInoCache *devino_cache,
                  const char   *pkg_commit,
                  GHashTable   *files_skip,
                  GPtrArray    *files_remove_regex,
                  OstreeRepoCheckoutOverwriteMode ovwmode,
                  gboolean      force_copy_zerosized,
                  GCancellable *cancellable,
                  GError      **error)
{
  OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       ovwmode, };

  /* We want the checkout to match the repo type so that we get hardlinks. */
  if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
    opts.mode = OSTREE_REPO_CHECKOUT_MODE_NONE;

  opts.devino_to_csum_cache = devino_cache;

  /* Always want hardlinks */
  opts.no_copy_fallback = TRUE;
  /* Used in the no-rofiles-fuse path */
  opts.force_copy_zerosized = force_copy_zerosized;

  /* If called by `checkout_package_into_root()`, there may be files that need to be filtered. */
  FilterData filter_data = { files_skip, files_remove_regex, };
  if ((files_skip && g_hash_table_size (files_skip) > 0) ||
      (files_remove_regex && files_remove_regex->len > 0))
      {
        opts.filter = checkout_filter;
        opts.filter_user_data = &filter_data;
      }

  return ostree_repo_checkout_at (repo, &opts, dfd, path,
                                  pkg_commit, cancellable, error);
}

static gboolean
checkout_package_into_root (RpmOstreeContext *self,
                            DnfPackage   *pkg,
                            int           dfd,
                            const char   *path,
                            OstreeRepoDevInoCache *devino_cache,
                            const char   *pkg_commit,
                            GHashTable   *files_skip,
                            OstreeRepoCheckoutOverwriteMode ovwmode,
                            GCancellable *cancellable,
                            GError      **error)
{
  /* If called on compose-side, there may be files to remove from packages specified in the treefile. */
  g_autoptr(GPtrArray) files_remove_regex = NULL;
  if (self->treefile_rs)
    {
      auto files_remove_regex_patterns = self->treefile_rs->get_files_remove_regex(dnf_package_get_name (pkg));
      files_remove_regex = g_ptr_array_new_full (files_remove_regex_patterns.size(), (GDestroyNotify)g_regex_unref);
      for (auto pattern : files_remove_regex_patterns) 
        {
          GRegex *regex = g_regex_new (pattern.c_str(), G_REGEX_JAVASCRIPT_COMPAT, static_cast<GRegexMatchFlags>(0), NULL);
          if (!regex)
            return FALSE;
          g_ptr_array_add (files_remove_regex, regex);
        }
    }
  
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);

  /* The below is currently TRUE only in the --unified-core path. We probably want to
   * migrate that over to always use a separate cache repo eventually, which would allow us
   * to completely drop the pkgcache_repo/ostreerepo dichotomy in the core. See:
   * https://github.com/projectatomic/rpm-ostree/pull/1055 */
  if (pkgcache_repo != self->ostreerepo)
    {
      if (!rpmostree_pull_content_only (self->ostreerepo, pkgcache_repo, pkg_commit,
                                        cancellable, error))
        {
          g_prefix_error (error, "Linking cached content for %s: ", dnf_package_get_nevra (pkg));
          return FALSE;
        }
    }

  if (!checkout_package (pkgcache_repo, dfd, path,
                         devino_cache, pkg_commit, files_skip, files_remove_regex, ovwmode,
                         !self->enable_rofiles,
                         cancellable, error))
    return glnx_prefix_error (error, "Checkout %s", dnf_package_get_nevra (pkg));

  return TRUE;
}

static Header
get_rpmdb_pkg_header (rpmts rpmdb_ts,
                      DnfPackage *pkg,
                      GCancellable *cancellable,
                      GError **error)
{
  g_auto(rpmts) rpmdb_ts_owned = NULL;
  if (!rpmdb_ts) /* allow callers to pass NULL */
    rpmdb_ts = rpmdb_ts_owned = rpmtsCreate ();
  (void)rpmdb_ts_owned; /* Pacify static analysis */

  unsigned int dbid = dnf_package_get_rpmdbid (pkg);
  g_assert (dbid > 0);

  g_auto(rpmdbMatchIterator) it =
    rpmtsInitIterator (rpmdb_ts, RPMDBI_PACKAGES, &dbid, sizeof(dbid));

  Header hdr = it ? rpmdbNextIterator (it) : NULL;
  if (hdr == NULL)
    return (Header)glnx_null_throw (error, "Failed to find package '%s' in rpmdb",
                            dnf_package_get_nevra (pkg));

  return headerLink (hdr);
}

/* Scan rootfs and build up a mapping like lib → usr/lib, etc. */
static gboolean
build_rootfs_usrlinks (RpmOstreeContext *self,
                       GError          **error)
{
  g_assert (!self->rootfs_usrlinks);
  self->rootfs_usrlinks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  if (!glnx_dirfd_iterator_init_at (self->tmprootfs_dfd, ".", TRUE, &dfd_iter, error))
    return FALSE;
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_LNK)
        continue;

      g_autofree char *link = glnx_readlinkat_malloc (dfd_iter.fd, dent->d_name, NULL, error);
      if (!link)
        return FALSE;
      const char *link_rel = link + strspn (link, "/");
      if (g_str_has_prefix (link_rel, "usr/"))
        g_hash_table_insert (self->rootfs_usrlinks, g_strdup (dent->d_name),
                             g_strdup (link_rel));
    }

  return TRUE;
}

/* Order by decreasing string length, used by package removals/replacements */
static int
compare_strlen (const void *a,
                const void *b,
                void *      data)
{
  size_t a_len = strlen (static_cast<const char *>(a));
  size_t b_len = strlen (static_cast<const char *>(b));
  if (a_len == b_len)
    return 0;
  else if (a_len > b_len)
    return -1;
  return 1;
}

/* Given a single package, remove its files (non-directories), unless they're
 * included in @files_skip.  Add its directories to @dirs_to_remove, which
 * are handled in a second pass.
 */
static gboolean
delete_package_from_root (RpmOstreeContext *self,
                          rpmte         pkg,
                          int           rootfs_dfd,
                          GHashTable   *files_skip,
                          GSequence    *dirs_to_remove,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_auto(rpmfiles) files = rpmteFiles (pkg);
  /* NB: new librpm uses RPMFI_ITER_BACK here to empty out dirs before deleting them using
   * unlink/rmdir. Older rpm doesn't support this API, so rather than doing some fancy
   * compat, we just use shutil_rm_rf anyway since we now skip over shared dirs/files. */
  g_auto(rpmfi) fi = rpmfilesIter (files, RPMFI_ITER_FWD);

  while (rpmfiNext (fi) >= 0)
    {
      /* see also apply_rpmfi_overrides() for a commented version of the loop */
      const char *fn = rpmfiFN (fi);
      rpm_mode_t mode = rpmfiFMode (fi);

      if (!(S_ISREG (mode) ||
            S_ISLNK (mode) ||
            S_ISDIR (mode)))
        continue;

      if (g_hash_table_contains (files_skip, fn))
        continue;

      g_assert (fn != NULL);
      fn += strspn (fn, "/");
      g_assert (fn[0]);

      g_autofree char *fn_owned = NULL;
      /* Handle ostree's /usr/etc */
      if (g_str_has_prefix (fn, "etc/"))
        fn = fn_owned = g_strconcat ("usr/", fn, NULL);
      else
        {
          /* Otherwise be sure we've canonicalized usr/ */
          fn_owned = canonicalize_non_usrmove_path (self, fn);
          if (fn_owned)
            fn = fn_owned;
        }
      (void)fn_owned; /* Pacify static analysis */

      /* for now, we only remove files from /usr */
      if (!g_str_has_prefix (fn, "usr/"))
        continue;

      /* Delete files first, we'll handle directories next */
      if (S_ISDIR (mode))
        g_sequence_insert_sorted (dirs_to_remove, g_strdup (fn), compare_strlen, NULL);
      else
        {
          if (unlinkat (rootfs_dfd, fn, 0) < 0)
            {
              if (errno != ENOENT)
                return glnx_throw_errno_prefix (error, "unlinkat(%s)", fn);
            }
        }
    }

  return TRUE;
}

/* Process the directories which we were queued up by
 * delete_package_from_root().  We ignore non-empty directories
 * since it's valid for a package being replaced to own a directory
 * to which dependent packages install files (e.g. systemd).
 */
static gboolean
handle_package_deletion_directories (int           rootfs_dfd,
                                     GSequence    *dirs_to_remove,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  /* We want to perform a bottom-up removal of directories. This is implemented
   * by having a sequence of filenames sorted by decreasing length.  A filename
   * of greater length must be a subdirectory, or unrelated.
   *
   * If a directory isn't empty, that's fine - a package may own
   * the directory, but it's valid for other packages to put content under it.
   * Particularly while we're doing a replacement.
   */
  GSequenceIter *iter = g_sequence_get_begin_iter (dirs_to_remove);
  while (!g_sequence_iter_is_end (iter))
    {
      auto fn = static_cast<const char *>(g_sequence_get (iter));
      if (unlinkat (rootfs_dfd, fn, AT_REMOVEDIR) < 0)
        {
          if (!G_IN_SET (errno, ENOENT, EEXIST, ENOTEMPTY))
            return glnx_throw_errno_prefix (error, "rmdir(%s)", fn);
        }
      iter = g_sequence_iter_next (iter);
    }

  return TRUE;
}

/* Given a directory referred to by @dfd and @dirpath, ensure that physical (or
 * reflink'd) copies of all files are done. */
static gboolean
break_hardlinks_at (int             dfd,
                    const char     *dirpath,
                    GCancellable   *cancellable,
                    GError        **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  if (!glnx_dirfd_iterator_init_at (dfd, dirpath, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (!ostree_break_hardlink (dfd_iter.fd, dent->d_name, FALSE,
                                  cancellable, error))
        return FALSE;
    }

  return TRUE;
}

typedef struct {
  int tmpdir_dfd;
  const char *name;
  const char *evr;
  const char *arch;
} RelabelTaskData;

static gboolean
relabel_in_thread_impl (RpmOstreeContext *self,
                        const char       *name,
                        const char       *evr,
                        const char       *arch,
                        int               tmpdir_dfd,
                        gboolean         *out_changed,
                        GCancellable     *cancellable,
                        GError          **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  const char *nevra = glnx_strjoina (name, "-", evr, ".", arch);
  const char *errmsg = glnx_strjoina ("Relabeling ", nevra);
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);
  const char *pkg_dirname = nevra;

  OstreeRepo *repo = get_pkgcache_repo (self);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_for_n_evr_a (name, evr, arch);
  g_autofree char *commit_csum = NULL;
  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &commit_csum, error))
    return FALSE;

  /* Compute the original content checksum */
  g_autoptr(GVariant) orig_commit = NULL;
  if (!ostree_repo_load_commit (repo, commit_csum, &orig_commit, NULL, error))
    return FALSE;
  g_autofree char *orig_content_checksum = rpmostree_commit_content_checksum (orig_commit);

  /* checkout the pkg and relabel, breaking hardlinks */
  g_autoptr(OstreeRepoDevInoCache) cache = ostree_repo_devino_cache_new ();

  if (!checkout_package (repo, tmpdir_dfd, pkg_dirname, cache,
                         commit_csum, NULL, NULL, OSTREE_REPO_CHECKOUT_OVERWRITE_NONE, FALSE,
                         cancellable, error))
    return FALSE;

  /* write to the tree */
  g_autoptr(OstreeRepoCommitModifier) modifier =
    ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME,
                                       NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_devino_cache (modifier, cache);
  ostree_repo_commit_modifier_set_sepolicy (modifier, self->sepolicy);

  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  if (!ostree_repo_write_dfd_to_mtree (repo, tmpdir_dfd, pkg_dirname, mtree,
                                       modifier, cancellable, error))
    return glnx_prefix_error (error, "Writing dfd");

  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    return FALSE;

  /* build metadata and commit */
  g_autoptr(GVariant) commit_var = NULL;
  g_autoptr(GVariantDict) meta_dict = NULL;

  if (!ostree_repo_load_commit (repo, commit_csum, &commit_var, NULL, error))
    return FALSE;

  /* let's just copy the metadata from the previous commit and only change the
   * rpmostree.sepolicy value */
  g_autoptr(GVariant) meta = g_variant_get_child_value (commit_var, 0);
  meta_dict = g_variant_dict_new (meta);

  g_variant_dict_insert (meta_dict, "rpmostree.sepolicy", "s",
                         ostree_sepolicy_get_csum (self->sepolicy));

  g_autofree char *new_commit_csum = NULL;
  if (!ostree_repo_write_commit (repo, NULL, "", "",
                                 g_variant_dict_end (meta_dict),
                                 OSTREE_REPO_FILE (root), &new_commit_csum,
                                 cancellable, error))
    return FALSE;

  /* Compute new content checksum */
  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (repo, new_commit_csum, &new_commit, NULL, error))
    return FALSE;
  g_autofree char *new_content_checksum = rpmostree_commit_content_checksum (new_commit);

  /* Queue an update to the ref */
  ostree_repo_transaction_set_ref (repo, NULL, cachebranch, new_commit_csum);

  /* Return whether or not we actually changed content */
  *out_changed = !g_str_equal (orig_content_checksum, new_content_checksum);

  return TRUE;
}

static void
relabel_in_thread (GTask            *task,
                   gpointer          source,
                   gpointer          task_data,
                   GCancellable     *cancellable)
{
  g_autoptr(GError) local_error = NULL;
  auto self = static_cast<RpmOstreeContext *>(source);
  auto tdata = static_cast<RelabelTaskData *>(task_data);

  gboolean changed = FALSE;
  if (!relabel_in_thread_impl (self, tdata->name, tdata->evr, tdata->arch,
                               tdata->tmpdir_dfd, &changed,
                               cancellable, &local_error))
    g_task_return_error (task, util::move_nullify (local_error));
  else
    g_task_return_int (task, changed ? 1 : 0);
}

static void
relabel_package_async (RpmOstreeContext   *self,
                       DnfPackage         *pkg,
                       int                 tmpdir_dfd,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
  g_autoptr(GTask) task = g_task_new (self, cancellable, callback, user_data);
  RelabelTaskData *tdata = g_new (RelabelTaskData, 1);
  /* We can assume lifetime is greater than the task */
  tdata->tmpdir_dfd = tmpdir_dfd;
  tdata->name = dnf_package_get_name (pkg);
  tdata->evr = dnf_package_get_evr (pkg);
  tdata->arch = dnf_package_get_arch (pkg);
  g_task_set_task_data (task, tdata, g_free);
  g_task_run_in_thread (task, relabel_in_thread);
}

static gssize
relabel_package_async_finish (RpmOstreeContext   *self,
                              GAsyncResult       *result,
                              GError            **error)
{
  g_assert (g_task_is_valid (result, self));
  return g_task_propagate_int ((GTask*)result, error);
}

typedef struct {
  RpmOstreeContext *self;
  guint n_changed_files;
  guint n_changed_pkgs;
} RpmOstreeAsyncRelabelData;

static void
on_async_relabel_done (GObject                    *obj,
                       GAsyncResult               *res,
                       gpointer                    user_data)
{
  auto data = static_cast<RpmOstreeAsyncRelabelData *>(user_data);
  RpmOstreeContext *self = data->self;
  gssize n_relabeled =
    relabel_package_async_finish (self, res, self->async_error ? NULL : &self->async_error);
  if (n_relabeled < 0)
    {
      g_assert (self->async_error != NULL);
      if (self->async_cancellable)
        g_cancellable_cancel (self->async_cancellable);
    }

  g_assert_cmpint (self->n_async_pkgs_relabeled, <, self->pkgs_to_relabel->len);
  self->n_async_pkgs_relabeled++;
  if (n_relabeled > 0)
    {
      data->n_changed_files += n_relabeled;
      data->n_changed_pkgs++;
    }
  self->async_progress->nitems_update(self->n_async_pkgs_relabeled);
  if (self->n_async_pkgs_relabeled == self->pkgs_to_relabel->len)
    self->async_running = FALSE;
}

static gboolean
relabel_if_necessary (RpmOstreeContext *self,
                      GCancellable     *cancellable,
                      GError          **error)
{
  if (!self->pkgs_to_relabel)
    return TRUE;

  const int n = self->pkgs_to_relabel->len;
  OstreeRepo *ostreerepo = get_pkgcache_repo (self);

  if (n == 0)
    return TRUE;

  g_assert (self->sepolicy);

  g_assert (ostreerepo != NULL);

  /* Prep a txn and tmpdir for all of the relabels */
  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, ostreerepo, FALSE, cancellable, error))
    return FALSE;

  g_auto(GLnxTmpDir) relabel_tmpdir = { 0, };
  if (!glnx_mkdtempat (ostree_repo_get_dfd (ostreerepo), "tmp/rpm-ostree-relabel.XXXXXX", 0700,
                       &relabel_tmpdir, error))
    return FALSE;

  self->async_running = TRUE;
  self->async_cancellable = cancellable;

  RpmOstreeAsyncRelabelData data = { self, 0, };
  const guint n_to_relabel = self->pkgs_to_relabel->len;
  self->async_progress = rpmostreecxx::progress_nitems_begin(n_to_relabel, "Relabeling");
  for (guint i = 0; i < n_to_relabel; i++)
    {
      auto pkg = static_cast<DnfPackage *>(self->pkgs_to_relabel->pdata[i]);
      relabel_package_async (self, pkg, relabel_tmpdir.fd, cancellable,
                             on_async_relabel_done, &data);
    }

  /* Wait for all of the relabeling to complete */
  GMainContext *mainctx = g_main_context_get_thread_default ();
  self->async_error = NULL;
  while (self->async_running)
    g_main_context_iteration (mainctx, TRUE);
  if (self->async_error)
    {
      g_propagate_error (error, util::move_nullify (self->async_error));
      return glnx_prefix_error (error, "relabeling");
    }

  self->async_progress->end("");
  self->async_progress.release();

  /* Commit */
  if (!ostree_repo_commit_transaction (ostreerepo, NULL, cancellable, error))
    return FALSE;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_SELINUX_RELABEL),
                   "MESSAGE=Relabeled %u/%u pkgs", data.n_changed_pkgs, n_to_relabel,
                   "RELABELED_PKGS=%u/%u", data.n_changed_pkgs, n_to_relabel,
                   NULL);

  g_clear_pointer (&self->pkgs_to_relabel, (GDestroyNotify)g_ptr_array_unref);
  self->n_async_pkgs_relabeled = 0;

  return TRUE;
}

/* Forcibly relabel all packages */
gboolean
rpmostree_context_force_relabel (RpmOstreeContext *self,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  g_clear_pointer (&self->pkgs_to_relabel, (GDestroyNotify)g_ptr_array_unref);
  self->pkgs_to_relabel = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_autoptr(GPtrArray) packages = dnf_goal_get_packages (dnf_context_get_goal (self->dnfctx),
                                                         DNF_PACKAGE_INFO_INSTALL,
                                                         DNF_PACKAGE_INFO_UPDATE,
                                                         DNF_PACKAGE_INFO_DOWNGRADE, -1);

  for (guint i = 0; i < packages->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(packages->pdata[i]);

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      /* This logic is equivalent to that in sort_packages() */
      gboolean in_ostree, selinux_match;
      if (!find_pkg_in_ostree (self, pkg, self->sepolicy,
                               &in_ostree, &selinux_match, error))
        return FALSE;

      if (in_ostree && !selinux_match)
        g_ptr_array_add (self->pkgs_to_relabel, g_object_ref (pkg));
    }

  return relabel_if_necessary (self, cancellable, error);
}

typedef struct {
  FD_t current_trans_fd;
  RpmOstreeContext *ctx;
} TransactionData;

static void *
ts_callback (const void * h,
             const rpmCallbackType what,
             const rpm_loff_t amount,
             const rpm_loff_t total,
             fnpyKey key,
             rpmCallbackData data)
{
  auto tdata = static_cast<TransactionData *>(data);

  switch (what)
    {
    case RPMCALLBACK_INST_OPEN_FILE:
      {
        auto pkg = static_cast<DnfPackage *>((void*)key);
        /* just bypass fdrel_abspath here since we know the dfd is not ATCWD and
         * it saves us an allocation */
        g_autofree char *relpath = get_package_relpath (pkg);
        g_autofree char *path = g_build_filename (tdata->ctx->tmpdir.path, relpath, NULL);
        g_assert (tdata->current_trans_fd == NULL);
        tdata->current_trans_fd = Fopen (path, "r.ufdio");
        return tdata->current_trans_fd;
      }
      break;

    case RPMCALLBACK_INST_CLOSE_FILE:
      g_clear_pointer (&tdata->current_trans_fd, Fclose);
      break;

    default:
      break;
    }

  return NULL;
}

static gboolean
get_package_metainfo (RpmOstreeContext *self,
                      const char *path,
                      Header *out_header,
                      rpmfi *out_fi,
                      GError **error)
{
  glnx_autofd int metadata_fd = -1;
  if (!glnx_openat_rdonly (self->tmpdir.fd, path, TRUE, &metadata_fd, error))
    return FALSE;

  return rpmostree_importer_read_metainfo (metadata_fd, out_header, NULL,
                                           out_fi, error);
}

typedef enum {
  RPMOSTREE_TS_FLAG_UPGRADE = (1 << 0),
  RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS = (1 << 1),
} RpmOstreeTsAddInstallFlags;

static gboolean
rpmts_add_install (RpmOstreeContext *self,
                   rpmts  ts,
                   DnfPackage *pkg,
                   RpmOstreeTsAddInstallFlags flags,
                   GCancellable *cancellable,
                   GError **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, &hdr, NULL, error))
    return FALSE;

  if (!(flags & RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS))
    {
      if (!rpmostree_script_txn_validate (pkg, hdr, cancellable, error))
        return FALSE;
    }

  const gboolean is_upgrade = (flags & RPMOSTREE_TS_FLAG_UPGRADE) > 0;
  if (rpmtsAddInstallElement (ts, hdr, pkg, is_upgrade, NULL) != 0)
    return glnx_throw (error, "Failed to add install element for %s",
                       dnf_package_get_filename (pkg));

  return TRUE;
}

static gboolean
rpmts_add_erase (RpmOstreeContext *self,
                 rpmts ts,
                 DnfPackage *pkg,
                 GCancellable *cancellable,
                 GError **error)
{
  /* NB: we're not running any %*un scriptlets right now */
  g_auto(Header) hdr = get_rpmdb_pkg_header (ts, pkg, cancellable, error);
  if (hdr == NULL)
    return FALSE;

  if (rpmtsAddEraseElement (ts, hdr, -1))
    return glnx_throw (error, "Failed to add erase element for package '%s'",
                       dnf_package_get_nevra (pkg));

  return TRUE;
}

/* Look up the header for a package, and pass it
 * to the script core to execute.
 */
static gboolean
run_script_sync (RpmOstreeContext *self,
                 int rootfs_dfd,
                 GLnxTmpDir *var_lib_rpm_statedir,
                 DnfPackage *pkg,
                 RpmOstreeScriptKind kind,
                 guint        *out_n_run,
                 GCancellable *cancellable,
                 GError    **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, &hdr, NULL, error))
    return FALSE;

  if (!rpmostree_script_run_sync (pkg, hdr, kind, rootfs_dfd, var_lib_rpm_statedir,
                                  self->enable_rofiles, out_n_run, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
apply_rpmfi_overrides (RpmOstreeContext *self,
                       int            tmprootfs_dfd,
                       DnfPackage    *pkg,
                       rpmostreecxx::PasswdEntries &passwd_entries,
                       GCancellable  *cancellable,
                       GError       **error)
{
  /* In an unprivileged case, we can't do this on the real filesystem. For `ex
   * container`, we want to completely ignore uid/gid.
   *
   * TODO: For non-root `--unified-core` we need to do it as a commit modifier.
   */
  if (getuid () != 0)
    return TRUE;  /* 🔚 Early return */

  g_auto(rpmfi) fi = NULL;
  gboolean emitted_nonusr_warning = FALSE;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, NULL, &fi, error))
    return FALSE;

  while (rpmfiNext (fi) >= 0)
    {
      const char *fn = rpmfiFN (fi);
      const char *user = rpmfiFUser (fi) ?: "root";
      const char *group = rpmfiFGroup (fi) ?: "root";
      const char *fcaps = rpmfiFCaps (fi) ?: "";
      const gboolean have_fcaps = fcaps[0] != '\0';
      rpm_mode_t mode = rpmfiFMode (fi);
      rpmfileAttrs fattrs = rpmfiFFlags (fi);
      const gboolean is_ghost = fattrs & RPMFILE_GHOST;
      /* If we hardlinked from a bare-user repo, we won't have these higher bits
       * set. The intention there is to avoid having transient suid binaries
       * exposed, but in practice today for rpm-ostree we use the "inaccessible
       * directory" pattern in repo/tmp.
       *
       * Another thing we could do down the line is to not chown things on disk
       * and instead pass this data down into the commit modifier. That's in
       * fact how gnome-continuous always worked.
       */
      const gboolean has_non_bare_user_mode =
        (mode & (S_ISUID | S_ISGID | S_ISVTX)) > 0;

      if (g_str_equal (user, "root") &&
          g_str_equal (group, "root") &&
          !has_non_bare_user_mode &&
          !have_fcaps)
        continue;

      /* In theory, RPMs could contain block devices or FIFOs; we would normally
       * have rejected that at the import time, but let's also be sure here.
       */
      if (!(S_ISREG (mode) ||
            S_ISLNK (mode) ||
            S_ISDIR (mode)))
        continue;

      g_assert (fn != NULL);
      fn += strspn (fn, "/");
      g_assert (fn[0]);

      /* Be sure we've canonicalized usr/ */
      g_autofree char *fn_canonical = canonicalize_non_usrmove_path (self, fn);
      if (fn_canonical)
        fn = fn_canonical;

      /* /run and /var paths have already been translated to tmpfiles during
       * unpacking */
      if (g_str_has_prefix (fn, "run/") ||
          g_str_has_prefix (fn, "var/"))
        continue;
      else if (g_str_has_prefix (fn, "etc/"))
        {
          /* Changing /etc is OK; note "normally" we maintain
           * usr/etc but this runs right after %pre, where
           * we're in the middle of running scripts.
           */
        }
      else if (!g_str_has_prefix (fn, "usr/"))
        {
          /* TODO: query whether Fedora has anything in this category we care about */
          if (!emitted_nonusr_warning)
            {
              sd_journal_print (LOG_WARNING, "Ignoring rpm mode for non-/usr content: %s", fn);
              emitted_nonusr_warning = TRUE;
            }
          continue;
        }

      struct stat stbuf;
      if (fstatat (tmprootfs_dfd, fn, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
        {
          /* Not early loop skip; in the ghost case, we expect it to not
           * exist.
           */
          if (errno == ENOENT && is_ghost)
            continue;
          return glnx_throw_errno_prefix (error, "fstatat(%s)", fn);
        }

      if ((S_IFMT & stbuf.st_mode) != (S_IFMT & mode))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Inconsistent file type between RPM and checkout "
                       "for file '%s' in package '%s'", fn,
                       dnf_package_get_name (pkg));
          return FALSE;
        }

      if (!S_ISDIR (stbuf.st_mode))
        {
          if (!ostree_break_hardlink (tmprootfs_dfd, fn, FALSE, cancellable, error))
            return FALSE;
        }

      uid_t uid = 0;
      if (!g_str_equal (user, "root"))
        {
          if (!passwd_entries.contains_user(user))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find user '%s' in passwd file", user);
              return FALSE;
            }
          uid = passwd_entries.lookup_user_id(user);
        }

      gid_t gid = 0;
      if (!g_str_equal (group, "root"))
        {
          if (!passwd_entries.contains_group(group))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find group '%s' in group file",
                           group);
              return FALSE;
            }

          gid = passwd_entries.lookup_group_id(group);
        }

      if (fchownat (tmprootfs_dfd, fn, uid, gid, AT_SYMLINK_NOFOLLOW) != 0)
        return glnx_throw_errno_prefix (error, "fchownat(%s)", fn);

      /* the chown clears away file caps, so reapply it here */
      if (have_fcaps)
        {
          g_autoptr(GVariant) xattrs = rpmostree_fcap_to_xattr_variant (fcaps);
          if (!glnx_dfd_name_set_all_xattrs (tmprootfs_dfd, fn, xattrs,
                                             cancellable, error))
            return FALSE;
        }

      /* also reapply chmod since e.g. at least the setuid gets taken off */
      if (S_ISREG (mode))
        {
          g_assert (S_ISREG (stbuf.st_mode));
          if (fchmodat (tmprootfs_dfd, fn, mode, 0) != 0)
            return glnx_throw_errno_prefix (error, "fchmodat(%s)", fn);
        }
    }

  return TRUE;
}

static gboolean
add_install (RpmOstreeContext *self,
             DnfPackage       *pkg,
             rpmts             ts,
             gboolean          is_upgrade,
             GHashTable       *pkg_to_ostree_commit,
             GCancellable     *cancellable,
             GError          **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  g_autofree char *cached_rev = NULL;

  if (!ostree_repo_resolve_rev (pkgcache_repo, cachebranch, FALSE,
                                &cached_rev, error))
    return FALSE;

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (pkgcache_repo, cached_rev, &commit, NULL, error))
    return FALSE;

  gboolean sepolicy_matches = FALSE;
  if (self->sepolicy)
    {
      if (!commit_has_matching_sepolicy (commit, self->sepolicy, &sepolicy_matches,
                                         error))
        return FALSE;

      /* We already did any relabeling/reimporting above */
      g_assert (sepolicy_matches);
    }

  if (!checkout_pkg_metadata_by_dnfpkg (self, pkg, cancellable, error))
    return FALSE;

  auto flags = static_cast<RpmOstreeTsAddInstallFlags>(0);
  if (is_upgrade)
    flags = static_cast<RpmOstreeTsAddInstallFlags>(static_cast<int>(flags) | RPMOSTREE_TS_FLAG_UPGRADE);
  if (!rpmts_add_install (self, ts, pkg, flags, cancellable, error))
    return FALSE;

  g_hash_table_insert (pkg_to_ostree_commit, g_object_ref (pkg),
                                             g_steal_pointer (&cached_rev));
  return TRUE;
}

/* Run %transfiletriggerin */
static gboolean
run_all_transfiletriggers (RpmOstreeContext *self,
                           rpmts         ts,
                           int           rootfs_dfd,
                           guint        *out_n_run,
                           GCancellable *cancellable,
                           GError      **error)
{
  /* Triggers from base packages, but only if we already have an rpmdb,
   * otherwise librpm will whine on our stderr.
   */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      g_auto(rpmdbMatchIterator) mi = rpmtsInitIterator (ts, RPMDBI_PACKAGES, NULL, 0);
      Header hdr;
      while ((hdr = rpmdbNextIterator (mi)) != NULL)
        {
          if (!rpmostree_transfiletriggers_run_sync (hdr, rootfs_dfd, self->enable_rofiles,
                                                     out_n_run,
                                                     cancellable, error))
            return FALSE;
        }
    }

  /* Triggers from newly added packages */
  const guint n = (guint)rpmtsNElements (ts);
  for (guint i = 0; i < n; i++)
    {
      rpmte te = rpmtsElement (ts, i);
      if (rpmteType (te) != TR_ADDED)
        continue;
      DnfPackage *pkg = (DnfPackage*)rpmteKey (te);
      g_autofree char *path = get_package_relpath (pkg);
      g_auto(Header) hdr = NULL;
      if (!get_package_metainfo (self, path, &hdr, NULL, error))
        return FALSE;

      if (!rpmostree_transfiletriggers_run_sync (hdr, rootfs_dfd, self->enable_rofiles,
                                                 out_n_run, cancellable, error))
        return FALSE;
    }
  return TRUE;
}

/* Set the root directory fd used for assemble(); used
 * by the sysroot upgrader for the base tree.  This is optional;
 * assemble() will use a tmpdir if not provided.
 */
void
rpmostree_context_set_tmprootfs_dfd (RpmOstreeContext *self,
                                     int               dfd)
{
  g_assert_cmpint (self->tmprootfs_dfd, ==, -1);
  self->tmprootfs_dfd = dfd;
}

int
rpmostree_context_get_tmprootfs_dfd  (RpmOstreeContext *self)
{
  return self->tmprootfs_dfd;
}

/* Determine if a txn element contains vmlinuz via provides.
 * There's also some hacks for this in libdnf.
 */
static gboolean
rpmte_is_kernel (rpmte te)
{
  const char *kernel_names[] = {"kernel", "kernel-core", "kernel-rt", "kernel-rt-core", NULL};
  rpmds provides = rpmdsInit (rpmteDS (te, RPMTAG_PROVIDENAME));
  while (rpmdsNext (provides) >= 0)
   {
     const char *provname = rpmdsN (provides);
     if (g_strv_contains (kernel_names, provname))
       return TRUE;
   }
  return FALSE;
}

/* TRUE if a package providing vmlinuz changed in this transaction;
 * normally this is for `override replace` operations.
 * The core will not handle things like initramfs regeneration,
 * this is done in the sysroot upgrader.
 */
gboolean
rpmostree_context_get_kernel_changed (RpmOstreeContext *self)
{
  return self->kernel_changed;
}

static gboolean
process_one_ostree_layer (RpmOstreeContext *self,
                          int               rootfs_dfd,
                          const char       *ref,
                          OstreeRepoCheckoutOverwriteMode ovw_mode,
                          GCancellable     *cancellable,
                          GError          **error)
{
  OstreeRepo *repo = self->ostreerepo;
  OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       ovw_mode };

  /* We want the checkout to match the repo type so that we get hardlinks. */
  if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
    opts.mode = OSTREE_REPO_CHECKOUT_MODE_NONE;

  /* Explicitly don't provide a devino cache here.  We can't do it reliably because
   * of SELinux.  The ostree layering infrastructure doesn't have the relabeling
   * that we do for the pkgcache repo because we can't assume that we can mutate
   * the input commits right now.
   * opts.devino_to_csum_cache = self->devino_cache;
   */
  /* Always want hardlinks */
  opts.no_copy_fallback = TRUE;

  g_autofree char *rev = NULL;
  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &rev, error))
    return FALSE;

  return ostree_repo_checkout_at (repo, &opts, rootfs_dfd, ".",
                                  rev, cancellable, error);
}

static gboolean
process_ostree_layers (RpmOstreeContext *self,
                       int               rootfs_dfd,
                       GCancellable     *cancellable,
                       GError          **error)
{
  if (!self->treefile_rs)
    return TRUE;
  
  auto layers = self->treefile_rs->get_ostree_layers();
  auto override_layers = self->treefile_rs->get_ostree_override_layers();
  const size_t n = layers.size() + override_layers.size();
  if (n == 0)
    return TRUE;

  auto progress = rpmostreecxx::progress_nitems_begin(n, "Checking out ostree layers");
  size_t i = 0;
  for (auto ref : layers)
    {
      if (!process_one_ostree_layer (self, rootfs_dfd, ref.c_str(),
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL,
                                     cancellable, error))
        return FALSE;
      i++;
      progress->nitems_update(i);
    }
  for (auto ref: override_layers)
    {
      if (!process_one_ostree_layer (self, rootfs_dfd, ref.c_str(),
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES,
                                     cancellable, error))
        return FALSE;
      i++;
      progress->nitems_update(i);
    }
  progress->end("");
  return TRUE;
}

gboolean
rpmostree_context_assemble (RpmOstreeContext      *self,
                            GCancellable          *cancellable,
                            GError               **error)
{
  /* Synthesize a tmpdir if we weren't provided a base */
  if (self->tmprootfs_dfd == -1)
    {
      g_assert (!self->repo_tmpdir.initialized);
      int repo_dfd = ostree_repo_get_dfd (self->ostreerepo); /* Borrowed */
      if (!glnx_mkdtempat (repo_dfd, "tmp/rpmostree-assemble-XXXXXX", 0700,
                           &self->repo_tmpdir, error))
        return FALSE;
      self->tmprootfs_dfd = self->repo_tmpdir.fd;
    }

  int tmprootfs_dfd = self->tmprootfs_dfd; /* Alias to avoid bigger diff */

  /* In e.g. removing a package we walk librpm which doesn't have canonical
   * /usr, so we need to build up a mapping.
   */
  if (!build_rootfs_usrlinks (self, error))
    return FALSE;

  /* We need up to date labels; the set of things needing relabeling
   * will have been calculated in sort_packages()
   */
  if (!relabel_if_necessary (self, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = self->dnfctx;
  TransactionData tdata = { 0, NULL };
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, (GDestroyNotify)g_free);
  DnfPackage *filesystem_package = NULL;   /* It's special, see below */
  DnfPackage *setup_package = NULL;   /* Also special due to composes needing to inject /etc/passwd */

  g_auto(rpmts) ordering_ts = rpmtsCreate ();
  rpmtsSetRootDir (ordering_ts, dnf_context_get_install_root (dnfctx));

  /* First for the ordering TS, set the dbpath to relative, which will also gain
   * the root dir. Note we should be able to drop this in the future now that we
   * inject a macro to set that value in our OSTrees.
   */
  set_rpm_macro_define ("_dbpath", "/" RPMOSTREE_RPMDB_LOCATION);

  /* Determine now if we have an existing rpmdb; whether this is a layering
   * operation or a fresh root. Mostly we use this to change how we render
   * output.
   */
  if (!glnx_fstatat_allow_noent (self->tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  const gboolean layering_on_base = (errno == 0);

  /* Don't verify checksums here (we should have done this on ostree
   * import).  Also, avoid updating the database or anything by
   * flagging it as a test.  We'll do the database next.
   */
  rpmtsSetVSFlags (ordering_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS | RPMTRANS_FLAG_TEST);

  g_autoptr(GPtrArray) overlays =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_INSTALL,
                           -1);

  g_autoptr(GPtrArray) overrides_replace =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_UPDATE,
                           DNF_PACKAGE_INFO_DOWNGRADE,
                           -1);

  g_autoptr(GPtrArray) overrides_remove =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_REMOVE,
                           DNF_PACKAGE_INFO_OBSOLETE,
                           -1);

  if (overlays->len == 0 && overrides_remove->len == 0 && overrides_replace->len == 0)
    return glnx_throw (error, "No packages in transaction");

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */

  for (guint i = 0; i < overrides_remove->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(overrides_remove->pdata[i]);
      if (!rpmts_add_erase (self, ordering_ts, pkg, cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overrides_replace->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(overrides_replace->pdata[i]);
      if (!add_install (self, pkg, ordering_ts, TRUE, pkg_to_ostree_commit,
                        cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overlays->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(overlays->pdata[i]);
      if (!add_install (self, pkg, ordering_ts, FALSE,
                        pkg_to_ostree_commit, cancellable, error))
        return FALSE;

      if (strcmp (dnf_package_get_name (pkg), "filesystem") == 0)
        filesystem_package = (DnfPackage*)g_object_ref (pkg);
      else if (strcmp (dnf_package_get_name (pkg), "setup") == 0)
        setup_package = (DnfPackage*)g_object_ref (pkg);
    }

  { DECLARE_RPMSIGHANDLER_RESET;
    rpmtsOrder (ordering_ts);
  }

  guint overrides_total = overrides_remove->len + overrides_replace->len;
  const char *progress_msg = "Checking out packages";
  if (!layering_on_base)
    {
      g_assert_cmpint (overrides_total, ==, 0);
    }
  else if (overrides_total > 0)
    {
      g_assert (layering_on_base);
      progress_msg = "Processing packages";
      if (overlays->len > 0)
        rpmostree_output_message ("Applying %u override%s and %u overlay%s",
                                  overrides_total, _NS(overrides_total),
                                  overlays->len, _NS(overlays->len));
      else
        rpmostree_output_message ("Applying %u override%s", overrides_total,
                                  _NS(overrides_total));
    }
  else if (overlays->len > 0)
    ;
  else
    g_assert_not_reached ();

  const guint n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);
  g_assert (n_rpmts_elements > 0);
  guint n_rpmts_done = 0;

  auto progress = rpmostreecxx::progress_nitems_begin(n_rpmts_elements, progress_msg);

  /* Okay so what's going on in Fedora with incestuous relationship
   * between the `filesystem`, `setup`, `libgcc` RPMs is actively
   * ridiculous.  If we unpack libgcc first it writes to /lib64 which
   * is really /usr/lib64, then filesystem blows up since it wants to symlink
   * /lib64 -> /usr/lib64.
   *
   * Really `filesystem` should be first but it depends on `setup` for
   * stupid reasons which is hacked around in `%pretrans` which we
   * don't run.  Just forcibly unpack it first.
   */
  if (filesystem_package)
    {
      progress->set_sub_message("filesystem");
      auto c = static_cast<const char*>(g_hash_table_lookup (pkg_to_ostree_commit, filesystem_package));
      if (!checkout_package_into_root (self, filesystem_package,
                                       tmprootfs_dfd, ".", self->devino_cache,
                                       c, NULL,
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL,
                                       cancellable, error))
        return FALSE;
      n_rpmts_done++;
      progress->nitems_update(n_rpmts_done);
    }

  g_autoptr(GHashTable) files_skip_add = NULL;
  g_autoptr(GHashTable) files_skip_delete = NULL;
  if (!handle_file_dispositions (self, tmprootfs_dfd, ordering_ts, &files_skip_add,
                                 &files_skip_delete, cancellable, error))
    return FALSE;

  g_autoptr(GSequence) dirs_to_remove = g_sequence_new (g_free);
  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      rpmElementType type = rpmteType (te);

      if (type == TR_ADDED)
        continue;
      g_assert_cmpint (type, ==, TR_REMOVED);

      if (rpmte_is_kernel (te))
        {
          self->kernel_changed = TRUE;
          /* Remove all of our kernel data first, to ensure it's done
           * consistently. For example, in some of the rpmostree kernel handling code we
           * won't look for an initramfs if the vmlinuz binary isn't found.  This
           * is also taking over the role of running the kernel.spec's `%preun`.
           */
          if (!rpmostree_kernel_remove (tmprootfs_dfd, cancellable, error))
            return FALSE;
        }

      if (!delete_package_from_root (self, te, tmprootfs_dfd, files_skip_delete,
                                     dirs_to_remove, cancellable, error))
        return FALSE;
      n_rpmts_done++;
      progress->nitems_update(n_rpmts_done);
    }
  g_clear_pointer (&files_skip_delete, g_hash_table_unref);

  if (!handle_package_deletion_directories (tmprootfs_dfd, dirs_to_remove, cancellable, error))
    return FALSE;
  g_clear_pointer (&dirs_to_remove, g_sequence_free);

  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      rpmElementType type = rpmteType (te);

      if (type == TR_REMOVED)
        continue;
      g_assert (type == TR_ADDED);

      DnfPackage *pkg = (DnfPackage*)rpmteKey (te);
      if (pkg == filesystem_package)
        continue;

      if (rpmte_is_kernel (te))
        self->kernel_changed = TRUE;

      /* The "setup" package currently contains /etc/passwd; in the treecompose
       * case we need to inject that beforehand, so use "add files" just for
       * that.
       */
      OstreeRepoCheckoutOverwriteMode ovwmode =
        (pkg == setup_package) ? OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES :
        OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL;

      progress->set_sub_message(dnf_package_get_name (pkg));
      if (!checkout_package_into_root (self, pkg, tmprootfs_dfd, ".", self->devino_cache,
                                       static_cast<const char*>(g_hash_table_lookup (pkg_to_ostree_commit, pkg)),
                                       files_skip_add, ovwmode, cancellable, error))
        return FALSE;
      n_rpmts_done++;
      progress->nitems_update(n_rpmts_done);
    }

  progress->end("");

  /* Some packages expect to be able to make temporary files here
   * for obvious reasons, but we otherwise make `/var` read-only.
   */
  if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, "var/tmp", 0755, cancellable, error))
    return FALSE;
  rpmostreecxx::rootfs_prepare_links(tmprootfs_dfd);

  gboolean skip_sanity_check = FALSE;
  g_variant_dict_lookup (self->spec->dict, "skip-sanity-check", "b", &skip_sanity_check);

  auto etc_guard = rpmostreecxx::prepare_tempetc_guard (tmprootfs_dfd);

  /* NB: we're not running scripts right now for removals, so this is only for overlays and
   * replacements */
  if (overlays->len > 0 || overrides_replace->len > 0)
    {
      gboolean have_passwd;
      gboolean have_systemctl;

      auto passwd_entries = rpmostreecxx::new_passwd_entries();

      std::string passwd_dir(self->passwd_dir ?: "");
      have_passwd = rpmostreecxx::prepare_rpm_layering (tmprootfs_dfd, passwd_dir);

      /* Also neuter systemctl - at least glusterfs for example calls `systemctl
       * start` in its %post which both violates Fedora policy and also will not
       * work with the rpm-ostree model.
       * See also https://github.com/projectatomic/rpm-ostree/issues/550
       *
       * See also the SYSTEMD_OFFLINE bits in rpmostree-scripts.c; at some
       * point in the far future when we don't support CentOS7 we can drop
       * our wrapper script.  If we remember.
       */
      if (renameat (tmprootfs_dfd, "usr/bin/systemctl",
                    tmprootfs_dfd, "usr/bin/systemctl.rpmostreesave") < 0)
        {
          if (errno == ENOENT)
            have_systemctl = FALSE;
          else
            return glnx_throw_errno_prefix (error, "rename(usr/bin/systemctl)");
        }
      else
        {
          have_systemctl = TRUE;
          auto systemctl_wrapper = rpmostreecxx::get_systemctl_wrapper ();
          if (!glnx_file_replace_contents_with_perms_at (tmprootfs_dfd, "usr/bin/systemctl",
                                                         systemctl_wrapper.data(), systemctl_wrapper.length(), 0755, (uid_t) -1, (gid_t) -1,
                                                         GLNX_FILE_REPLACE_NODATASYNC,
                                                         cancellable, error))
            return FALSE;
        }

      /* Necessary for unified core to work with semanage calls in %post, like container-selinux */
      if (!rpmostree_rootfs_fixup_selinux_store_root (tmprootfs_dfd, cancellable, error))
        return FALSE;

      g_auto(GLnxTmpDir) var_lib_rpm_statedir = { 0, };
      if (!glnx_mkdtempat (AT_FDCWD, "/tmp/rpmostree-state.XXXXXX", 0700,
                           &var_lib_rpm_statedir, error))
        return FALSE;
      /* We need to pre-create this dir */
      if (!glnx_ensure_dir (tmprootfs_dfd, "var/lib/rpm-state", 0755, error))
        return FALSE;

      /* Workaround for https://github.com/projectatomic/rpm-ostree/issues/1804 */
      gboolean created_etc_selinux_config = FALSE;
      static const char usr_etc_selinux_config[] = "usr/etc/selinux/config";
      if (!glnx_fstatat_allow_noent (tmprootfs_dfd, "usr/etc/selinux", NULL, 0, error))
        return FALSE;
      if (errno == 0)
        {
          if (!glnx_fstatat_allow_noent (tmprootfs_dfd, usr_etc_selinux_config, NULL, 0, error))
            return FALSE;
          if (errno == ENOENT)
            {
              if (!glnx_file_replace_contents_at (tmprootfs_dfd, usr_etc_selinux_config, (guint8*)"", 0,
                                                  GLNX_FILE_REPLACE_NODATASYNC,
                                                  cancellable, error))
                return FALSE;
              created_etc_selinux_config = TRUE;
            }
        }

      /* We're technically deviating from RPM here by running all the %pre's
       * beforehand, rather than each package's %pre & %post in order. Though I
       * highly doubt this should cause any issues. The advantage of doing it
       * this way is that we only need to read the passwd/group files once
       * before applying the overrides, rather than after each %pre.
       */
      { auto task = rpmostreecxx::progress_begin_task("Running pre scripts");
        guint n_pre_scripts_run = 0;
        for (guint i = 0; i < n_rpmts_elements; i++)
          {
            rpmte te = rpmtsElement (ordering_ts, i);
            if (rpmteType (te) != TR_ADDED)
              continue;

            DnfPackage *pkg = (DnfPackage*)rpmteKey (te);
            g_assert (pkg);

            task->set_sub_message(dnf_package_get_name(pkg));
            if (!run_script_sync (self, tmprootfs_dfd, &var_lib_rpm_statedir,
                                  pkg, RPMOSTREE_SCRIPT_PREIN,
                                  &n_pre_scripts_run, cancellable, error))
              return FALSE;
          }
        auto msg = g_strdup_printf ("%u done", n_pre_scripts_run);
        task->end(msg);
      }

      /* Now undo our hack above */
      if (created_etc_selinux_config)
        {
          if (!glnx_unlinkat (tmprootfs_dfd, usr_etc_selinux_config, 0, error))
            return FALSE;
        }

      if (faccessat (tmprootfs_dfd, "etc/passwd", F_OK, 0) == 0)
        {
          passwd_entries->add_passwd_content(tmprootfs_dfd, "etc/passwd");
        }

      if (faccessat (tmprootfs_dfd, "etc/group", F_OK, 0) == 0)
        {
          passwd_entries->add_group_content(tmprootfs_dfd, "etc/group");
        }

      {
      auto task = rpmostreecxx::progress_begin_task("Running post scripts");
      guint n_post_scripts_run = 0;

      /* %post */
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          if (rpmteType (te) != TR_ADDED)
            continue;

          auto pkg = (DnfPackage *)(rpmteKey (te));
          g_assert (pkg);

          task->set_sub_message(dnf_package_get_name(pkg));
          if (!apply_rpmfi_overrides (self, tmprootfs_dfd, pkg, *passwd_entries,
                                      cancellable, error))
            return glnx_prefix_error (error, "While applying overrides for pkg %s",
                                      dnf_package_get_name (pkg));

          if (!run_script_sync (self, tmprootfs_dfd, &var_lib_rpm_statedir,
                                pkg, RPMOSTREE_SCRIPT_POSTIN,
                                &n_post_scripts_run, cancellable, error))
            return FALSE;
        }
      }

      /* Any ostree refs to overlay */
      if (!process_ostree_layers (self, tmprootfs_dfd, cancellable, error))
        return FALSE;

      {
      auto task = rpmostreecxx::progress_begin_task("Running posttrans scripts");
      guint n_posttrans_scripts_run = 0;

      /* %posttrans */
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          if (rpmteType (te) != TR_ADDED)
            continue;

          auto pkg = (DnfPackage *)(rpmteKey (te));
          g_assert (pkg);

          task->set_sub_message(dnf_package_get_name(pkg));
          if (!run_script_sync (self, tmprootfs_dfd, &var_lib_rpm_statedir,
                                pkg, RPMOSTREE_SCRIPT_POSTTRANS,
                                &n_posttrans_scripts_run, cancellable, error))
            return FALSE;
        }

      /* file triggers */
      if (!run_all_transfiletriggers (self, ordering_ts, tmprootfs_dfd,
                                      &n_posttrans_scripts_run, cancellable, error))
        return FALSE;

      auto msg = g_strdup_printf ("%u done", n_posttrans_scripts_run);
      task->end(msg);
      }

      /* We want this to be the first error message if something went wrong
       * with a script; see https://github.com/projectatomic/rpm-ostree/pull/888
       * (otherwise, on a script that did `rm -rf`, we'd fail first on the renameat below)
       */
      if (!skip_sanity_check &&
          !rpmostree_deployment_sanitycheck_true (tmprootfs_dfd, cancellable, error))
        return FALSE;

      if (have_systemctl)
        {
          if (!glnx_renameat (tmprootfs_dfd, "usr/bin/systemctl.rpmostreesave",
                              tmprootfs_dfd, "usr/bin/systemctl", error))
            return FALSE;
        }

      if (have_passwd)
        {
          rpmostreecxx::complete_rpm_layering (tmprootfs_dfd);
        }
    }
  else
    {
      /* Also do a sanity check even if we have no layered packages */
      if (!skip_sanity_check &&
          !rpmostree_deployment_sanitycheck_true (tmprootfs_dfd, cancellable, error))
        return FALSE;
    }

  if (self->treefile_rs && self->treefile_rs->get_cliwrap())
    rpmostreecxx::cliwrap_write_wrappers (tmprootfs_dfd);

  /* Undo the /etc move above */
  etc_guard->undo();

  /* And clean up var/tmp, we don't want it in commits */
  if (!glnx_shutil_rm_rf_at (tmprootfs_dfd, "var/tmp", cancellable, error))
    return FALSE;
  // And the state dir
  if (!glnx_shutil_rm_rf_at (tmprootfs_dfd, "var/lib/rpm-state", cancellable, error))
    return FALSE;

  g_clear_pointer (&ordering_ts, rpmtsFree);

  auto task = rpmostreecxx::progress_begin_task("Writing rpmdb");

  if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, 0755, cancellable, error))
    return FALSE;

  /* Now, we use the separate rpmdb ts which *doesn't* have a rootdir set,
   * because if it did rpmtsRun() would try to chroot which it won't be able to
   * if we're unprivileged, even though we're not trying to run %post scripts
   * now.
   *
   * Instead, this rpmts has the dbpath as absolute.
   */
  { g_autofree char *rpmdb_abspath = glnx_fdrel_abspath (tmprootfs_dfd,
                                                         RPMOSTREE_RPMDB_LOCATION);

    /* if we were passed an existing tmprootfs, and that tmprootfs already has
     * an rpmdb, we have to make sure to break its hardlinks as librpm mutates
     * the db in place */
    if (!break_hardlinks_at (tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, cancellable, error))
      return FALSE;

    set_rpm_macro_define ("_dbpath", rpmdb_abspath);
  }

  g_auto(rpmts) rpmdb_ts = rpmtsCreate ();
  /* Always call rpmtsSetRootDir() here so rpmtsRootDir() isn't NULL -- see rhbz#1613517 */
  rpmtsSetRootDir (rpmdb_ts, "/");
  rpmtsSetVSFlags (rpmdb_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
  /* https://bugzilla.redhat.com/show_bug.cgi?id=1607223
   * Newer librpm defaults to doing a full payload checksum, which we can't
   * do at this point because we imported the RPMs into ostree commits, saving
   * just the header in metadata - we don't have the exact original content to
   * provide again.
   */
  rpmtsSetVfyLevel (rpmdb_ts, 0);
  /* We're just writing the rpmdb, hence _JUSTDB. Also disable the librpm
   * SELinux plugin since rpm-ostree (and ostree) have fundamentally better
   * code.
   */
  rpmtsSetFlags (rpmdb_ts, RPMTRANS_FLAG_JUSTDB | RPMTRANS_FLAG_NOCONTEXTS);

  tdata.ctx = self;
  rpmtsSetNotifyCallback (rpmdb_ts, ts_callback, &tdata);

  /* Skip validating scripts since we already validated them above */
  RpmOstreeTsAddInstallFlags rpmdb_instflags = RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS;
  for (guint i = 0; i < overlays->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(overlays->pdata[i]);

      if (!rpmts_add_install (self, rpmdb_ts, pkg, rpmdb_instflags,
                              cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overrides_replace->len; i++)
    {
      auto pkg = static_cast<DnfPackage *>(overrides_replace->pdata[i]);

      if (!rpmts_add_install (self, rpmdb_ts, pkg,
                              static_cast<RpmOstreeTsAddInstallFlags>(rpmdb_instflags | RPMOSTREE_TS_FLAG_UPGRADE),
                              cancellable, error))
        return FALSE;
    }

  /* and mark removed packages as such so they drop out of rpmdb */
  for (guint i = 0; i < overrides_remove->len; i++)
    {
      auto pkg = static_cast<DnfPackage*>(overrides_remove->pdata[i]);
      if (!rpmts_add_erase (self, rpmdb_ts, pkg, cancellable, error))
        return FALSE;
    }

  rpmtsOrder (rpmdb_ts);

  /* NB: Because we're using the real root here (see above for reason why), rpm
   * will see the read-only /usr mount and think that there isn't any disk space
   * available for install. For now, we just tell rpm to ignore space
   * calculations, but then we lose that nice check. What we could do is set a
   * root dir at least if we have CAP_SYS_CHROOT, or maybe do the space req
   * check ourselves if rpm makes that information easily accessible (doesn't
   * look like it from a quick glance). */
  /* Also enable OLDPACKAGE to allow replacement overrides to older version. */
  int r = rpmtsRun (rpmdb_ts, NULL, RPMPROB_FILTER_DISKSPACE | RPMPROB_FILTER_OLDPACKAGE);
  if (r < 0)
    return glnx_throw (error, "Failed to update rpmdb (rpmtsRun code %d)", r);
  if (r > 0)
    {
      if (!dnf_rpmts_look_for_problems (rpmdb_ts, error))
        return FALSE;
    }

  task->end("");

  /* And finally revert the _dbpath setting because libsolv relies on it as well
   * to find the rpmdb and RPM macros are global state. */
  set_rpm_macro_define ("_dbpath", "/" RPMOSTREE_RPMDB_LOCATION);

  /* And now also sanity check the rpmdb */
  if (!skip_sanity_check)
    {
      if (!rpmostree_deployment_sanitycheck_rpmdb (tmprootfs_dfd, overlays,
                                                   overrides_replace, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_context_commit (RpmOstreeContext      *self,
                          const char            *parent,
                          RpmOstreeAssembleType  assemble_type,
                          char                 **out_commit,
                          GCancellable          *cancellable,
                          GError               **error)
{
  g_autoptr(OstreeRepoCommitModifier) commit_modifier = NULL;
  g_autofree char *ret_commit_checksum = NULL;

  auto task = rpmostreecxx::progress_begin_task("Writing OSTree commit");

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, self->ostreerepo, FALSE, cancellable, error))
    return FALSE;

  { glnx_unref_object OstreeMutableTree *mtree = NULL;
    g_autoptr(GFile) root = NULL;
    g_auto(GVariantBuilder) metadata_builder;
    g_autofree char *state_checksum = NULL;

    g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

    if (assemble_type == RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING)
      {
        g_autoptr(GVariant) commit = NULL;
        g_autofree char *parent_version = NULL;

        g_assert (parent != NULL);

        if (!ostree_repo_load_commit (self->ostreerepo, parent, &commit, NULL, error))
          return FALSE;

        parent_version = rpmostree_checksum_version (commit);

        /* copy the version tag */
        if (parent_version)
          g_variant_builder_add (&metadata_builder, "{sv}", "version",
                                 g_variant_new_string (parent_version));

        /* Flag the commit as a client layer */
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.clientlayer",
                               g_variant_new_boolean (TRUE));


        /* Record the rpm-md information on the client just like we do on the server side */
        g_autoptr(GVariant) rpmmd_meta = rpmostree_context_get_rpmmd_repo_commit_metadata (self);
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.rpmmd-repos", rpmmd_meta);

        /* embed packages (really, "patterns") layered */
        g_autoptr(GVariant) pkgs =
          g_variant_dict_lookup_value (self->spec->dict, "packages", G_VARIANT_TYPE ("as"));
        g_assert (pkgs);
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.packages", pkgs);

        /* embed packages removed */
        /* we have to embed both the pkgname and the full nevra to make it easier to match
         * them up with origin directives. the full nevra is used for status -v */
        g_auto(GVariantBuilder) removed_base_pkgs;
        g_variant_builder_init (&removed_base_pkgs, (GVariantType*)"av");
        if (self->pkgs_to_remove)
          {
            GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_remove,
                                        const char*, name, GVariant*, nevra)
              g_variant_builder_add (&removed_base_pkgs, "v", nevra);
          }
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.removed-base-packages",
                               g_variant_builder_end (&removed_base_pkgs));

        /* embed packages replaced */
        g_auto(GVariantBuilder) replaced_base_pkgs;
        g_variant_builder_init (&replaced_base_pkgs, (GVariantType*)"a(vv)");
        if (self->pkgs_to_replace)
          {
            GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_replace,
                                        GVariant*, new_nevra, GVariant*, old_nevra)
              g_variant_builder_add (&replaced_base_pkgs, "(vv)", new_nevra, old_nevra);
          }
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.replaced-base-packages",
                               g_variant_builder_end (&replaced_base_pkgs));

        /* this is used by the db commands, and auto updates to diff against the base */
        g_autoptr(GVariant) rpmdb = NULL;
        if (!rpmostree_create_rpmdb_pkglist_variant (self->tmprootfs_dfd, ".", &rpmdb,
                                                     cancellable, error))
          return FALSE;
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb);

        /* be nice to our future selves */
        g_variant_builder_add (&metadata_builder, "{sv}",
                               "rpmostree.clientlayer_version",
                               g_variant_new_uint32 (4));
      }
    else if (assemble_type == RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE)
      {
        /* Note this branch isn't actually used today; the compose side commit
         * code is in impl_commit_tree(). */
        g_autoptr(GVariant) spec_v =
          g_variant_ref_sink (rpmostree_treespec_to_variant (self->spec));

        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.spec",
                              spec_v);

        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.serverbase",
                               g_variant_new_uint32 (1));
      }
    else
      {
        g_assert_not_reached ();
      }

    if (!rpmostree_context_get_state_sha512 (self, &state_checksum, error))
      return FALSE;

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.state-sha512",
                           g_variant_new_string (state_checksum));

    /* And finally, make sure we clean up rpmdb left over files */
    if (!rpmostree_cleanup_leftover_rpmdb_files (self->tmprootfs_dfd, cancellable, error))
      return FALSE;

    auto modflags = static_cast<OstreeRepoCommitModifierFlags>(OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE);
    /* For ex-container (bare-user-only), we always need canonical permissions */
    if (ostree_repo_get_mode (self->ostreerepo) == OSTREE_REPO_MODE_BARE_USER_ONLY)
      modflags = static_cast<OstreeRepoCommitModifierFlags>(static_cast<int>(modflags) | OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS);

    /* if we're SELinux aware, then reload the final policy from the tmprootfs in case it
     * was changed by a scriptlet; this covers the foobar/foobar-selinux path */
    g_autoptr(OstreeSePolicy) final_sepolicy = NULL;
    if (self->sepolicy)
      {
        if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, &final_sepolicy,
                                                    cancellable, error))
          return FALSE;

        /* If policy didn't change (or SELinux is disabled), then we can treat
         * the on-disk xattrs as canonical; loading the xattrs is a noticeable
         * slowdown for commit.
         */
        if (ostree_sepolicy_get_name (final_sepolicy) == NULL ||
            g_strcmp0 (ostree_sepolicy_get_csum (self->sepolicy),
                       ostree_sepolicy_get_csum (final_sepolicy)) == 0)
          modflags = static_cast<OstreeRepoCommitModifierFlags>(static_cast<int>(modflags) | OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL);
      }

    commit_modifier = ostree_repo_commit_modifier_new (modflags, NULL, NULL, NULL);
    if (final_sepolicy)
      ostree_repo_commit_modifier_set_sepolicy (commit_modifier, final_sepolicy);

    if (self->devino_cache)
      ostree_repo_commit_modifier_set_devino_cache (commit_modifier, self->devino_cache);

    mtree = ostree_mutable_tree_new ();

    const guint64 start_time_ms = g_get_monotonic_time () / 1000;
    if (!ostree_repo_write_dfd_to_mtree (self->ostreerepo, self->tmprootfs_dfd, ".",
                                         mtree, commit_modifier,
                                         cancellable, error))
      return FALSE;

    if (!ostree_repo_write_mtree (self->ostreerepo, mtree, &root, cancellable, error))
      return FALSE;

    g_autoptr(GVariant) metadata_so_far = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
    // Unfortunately this API takes GVariantDict, not GVariantBuilder, so convert
    g_autoptr(GVariantDict) metadata_dict = g_variant_dict_new (metadata_so_far);
    if (!ostree_commit_metadata_for_bootable (root, metadata_dict, cancellable, error))
      return FALSE;
    g_autoptr(GVariant) metadata = g_variant_dict_end (metadata_dict);

    { 
      if (!ostree_repo_write_commit (self->ostreerepo, parent, "", "",
                                     metadata,
                                     OSTREE_REPO_FILE (root),
                                     &ret_commit_checksum, cancellable, error))
      return FALSE;
    }

    if (self->ref != NULL)
      ostree_repo_transaction_set_ref (self->ostreerepo, NULL, self->ref,
                                       ret_commit_checksum);

    { OstreeRepoTransactionStats stats;
      g_autofree char *bytes_written_formatted = NULL;

      if (!ostree_repo_commit_transaction (self->ostreerepo, &stats, cancellable, error))
        return FALSE;

      bytes_written_formatted = g_format_size (stats.content_bytes_written);
      const guint64 end_time_ms = g_get_monotonic_time () / 1000;
      const guint64 elapsed_ms = end_time_ms - start_time_ms;

      /* TODO: abstract a variant of this into libglnx which does
       *
       * if (journal)
       *   journal_send ()
       * else
       *   print ()
       *
       * Or possibly:
       *
       * if (journal)
       *   journal_send ()
       * if (!journal || getppid () != 1)
       *   print ()
       *
       * https://github.com/projectatomic/rpm-ostree/pull/661
       */
      sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_COMMIT_STATS),
                       "MESSAGE=Wrote commit: %s; New objects: meta:%u content:%u totaling %s)",
                       ret_commit_checksum, stats.metadata_objects_written,
                       stats.content_objects_written, bytes_written_formatted,
                       "OSTREE_METADATA_OBJECTS_WRITTEN=%u", stats.metadata_objects_written,
                       "OSTREE_CONTENT_OBJECTS_WRITTEN=%u", stats.content_objects_written,
                       "OSTREE_CONTENT_BYTES_WRITTEN=%" G_GUINT64_FORMAT, stats.content_bytes_written,
                       "OSTREE_TXN_ELAPSED_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                       NULL);
    }
  }

  self->tmprootfs_dfd = -1;
  if (!glnx_tmpdir_delete (&self->repo_tmpdir, cancellable, error))
    return FALSE;

  if (out_commit)
    *out_commit = util::move_nullify (ret_commit_checksum);
  return TRUE;
}
