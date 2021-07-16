/* -*- mode: C++; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
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
#include <utility>
#include <optional>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-composeutil.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"

static gboolean
pull_local_into_target_repo (OstreeRepo   *src_repo,
                             OstreeRepo   *dest_repo,
                             const char   *checksum,
                             GCancellable *cancellable,
                             GError      **error);

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static char *opt_cachedir;
static gboolean opt_download_only;
static gboolean opt_download_only_rpms;
static gboolean opt_force_nocache;
static gboolean opt_cache_only;
static gboolean opt_unified_core;
static char *opt_proxy;
static char **opt_metadata_strings;
static char **opt_metadata_json;
static char *opt_repo;
static char *opt_touch_if_changed;
static char *opt_previous_commit;
static gboolean opt_dry_run;
static gboolean opt_print_only;
static char *opt_write_commitid_to;
static char *opt_write_composejson_to;
static gboolean opt_no_parent;
static char *opt_write_lockfile_to;
static char **opt_lockfiles;
static gboolean opt_lockfile_strict;
static char *opt_parent;

static char *opt_extensions_output_dir;
static char *opt_extensions_base_rev;

/* shared by both install & commit */
static GOptionEntry common_option_entries[] = {
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { NULL }
};

static GOptionEntry install_option_entries[] = {
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "cache-only", 0, 0, G_OPTION_ARG_NONE, &opt_cache_only, "Assume cache is present, do not attempt to update it", NULL },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Like --dry-run, but download and import RPMs as well; requires --cachedir", NULL },
  { "download-only-rpms", 0, 0, G_OPTION_ARG_NONE, &opt_download_only_rpms, "Like --dry-run, but download RPMs as well; requires --cachedir", NULL },
  { "ex-unified-core", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_unified_core, "Compat alias for --unified-core", NULL }, // Compat
  { "unified-core", 0, 0, G_OPTION_ARG_NONE, &opt_unified_core, "Use new \"unified core\" codepath", NULL },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Just print the transaction and exit", NULL },
  { "print-only", 0, 0, G_OPTION_ARG_NONE, &opt_print_only, "Just expand any includes and print treefile", NULL },
  { "touch-if-changed", 0, 0, G_OPTION_ARG_STRING, &opt_touch_if_changed, "Update the modification time on FILE if a new commit was created", "FILE" },
  { "previous-commit", 0, 0, G_OPTION_ARG_STRING, &opt_previous_commit, "Use this commit for change detection", "COMMIT" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "ex-write-lockfile-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_lockfile_to, "Write lockfile to FILE", "FILE" },
  { "ex-lockfile", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_lockfiles, "Read lockfile from FILE", "FILE" },
  { "ex-lockfile-strict", 0, 0, G_OPTION_ARG_NONE, &opt_lockfile_strict, "With --ex-lockfile, only allow installing locked packages", NULL },
  { NULL }
};

static GOptionEntry postprocess_option_entries[] = {
  { NULL }
};

static GOptionEntry commit_option_entries[] = {
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "add-metadata-from-json", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_json, "Parse the given JSON file as object, convert to GVariant, append to OSTree commit", "JSON" },
  { "write-commitid-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_commitid_to, "File to write the composed commitid to instead of updating the ref", "FILE" },
  { "write-composejson-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_composejson_to, "Write JSON to FILE containing information about the compose run", "FILE" },
  { "no-parent", 0, 0, G_OPTION_ARG_NONE, &opt_no_parent, "Always commit without a parent", NULL },
  { "parent", 0, 0, G_OPTION_ARG_STRING, &opt_parent, "Commit with specific parent", "REV" },
  { NULL }
};

static GOptionEntry extensions_option_entries[] = {
  { "output-dir", 0, 0, G_OPTION_ARG_STRING, &opt_extensions_output_dir, "Path to extensions output directory", "PATH" },
  { "base-rev", 0, 0, G_OPTION_ARG_STRING, &opt_extensions_base_rev, "Base OSTree revision", "REV" },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "touch-if-changed", 0, 0, G_OPTION_ARG_STRING, &opt_touch_if_changed, "Update the modification time on FILE if new extensions were downloaded", "FILE" },
  { NULL }
};

typedef struct {
  RpmOstreeContext *corectx;
  GFile *treefile_path;
  GHashTable *metadata;
  gboolean failed;
  GLnxTmpDir workdir_tmp;
  int workdir_dfd;
  int rootfs_dfd;
  int cachedir_dfd;
  gboolean unified_core_and_fuse;
  OstreeRepo *repo;          /* target repo provided by --repo */
  OstreeRepo *build_repo;    /* unified mode: repo we build into */
  OstreeRepo *pkgcache_repo; /* unified mode: pkgcache repo where we import pkgs */
  OstreeRepoDevInoCache *devino_cache;
  const char *ref;
  char *previous_checksum;

  std::optional<rust::Box<rpmostreecxx::Treefile>> treefile_rs;
  JsonParser *treefile_parser;
  JsonNode *treefile_rootval; /* Unowned */
  JsonObject *treefile; /* Unowned */
} RpmOstreeTreeComposeContext;

static void
rpm_ostree_tree_compose_context_free (RpmOstreeTreeComposeContext *ctx)
{
  g_clear_object (&ctx->corectx);
  g_clear_object (&ctx->treefile_path);
  g_clear_pointer (&ctx->metadata, g_hash_table_unref);
  ctx->treefile_rs.~optional();
  /* Only close workdir_dfd if it's not owned by the tmpdir */
  if (!ctx->workdir_tmp.initialized)
    glnx_close_fd (&ctx->workdir_dfd);
  const char *preserve = g_getenv ("RPMOSTREE_PRESERVE_TMPDIR");
  if (ctx->workdir_tmp.initialized &&
      (preserve && (!g_str_equal (preserve, "on-fail") || ctx->failed)))
    g_printerr ("Preserved workdir: %s\n", ctx->workdir_tmp.path);
  else
    (void)glnx_tmpdir_delete (&ctx->workdir_tmp, NULL, NULL);
  glnx_close_fd (&ctx->rootfs_dfd);
  glnx_close_fd (&ctx->cachedir_dfd);
  g_clear_object (&ctx->repo);
  g_clear_object (&ctx->build_repo);
  g_clear_object (&ctx->pkgcache_repo);
  g_clear_pointer (&ctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);
  g_free (ctx->previous_checksum);
  g_clear_object (&ctx->treefile_parser);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeTreeComposeContext, rpm_ostree_tree_compose_context_free)

static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  auto text = static_cast<const char*>(user_data);
  glnx_console_progress_text_percent (text, percentage);
}

static gboolean
inputhash_from_commit (OstreeRepo *repo,
                       const char *sha256,
                       char      **out_value, /* inout Option<String> */
                       GError    **error)
{
  g_autoptr(GVariant) commit_v = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 sha256, &commit_v, error))
    return FALSE;

  g_autoptr(GVariant) commit_metadata = g_variant_get_child_value (commit_v, 0);
  g_assert (out_value);
  *out_value = NULL;
  g_variant_lookup (commit_metadata, "rpmostree.inputhash", "s", out_value);
  return TRUE;
}

static gboolean
try_load_previous_sepolicy (RpmOstreeTreeComposeContext *self,
                            GCancellable                 *cancellable,
                            GError                      **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading previous sepolicy", error);
  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (self->treefile, "selinux",
                                                               &selinux, error))
    return FALSE;

  if (!selinux || !self->previous_checksum)
    return TRUE; /* nothing to do! */

  OstreeRepoCommitState commitstate;
  if (!ostree_repo_load_commit (self->repo, self->previous_checksum, NULL,
                                &commitstate, error))
    return FALSE;

  if (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL)
    return TRUE;

#define TMP_SELINUX_ROOTFS "selinux.tmp/etc/selinux"

  /* By default, the core starts with the SELinux policy of the root, but if we have a
   * previous commit, it's much likelier that its policy will be closer to the final
   * policy than the host system's policy. And in the case they match, we skip a full
   * relabeling phase. Let's use that instead. */
  if (!glnx_shutil_mkdir_p_at (self->workdir_dfd,
                               dirname (strdupa (TMP_SELINUX_ROOTFS)), 0755,
                               cancellable, error))
    return FALSE;

  OstreeRepoCheckoutAtOptions opts = { .mode = OSTREE_REPO_CHECKOUT_MODE_USER,
                                       .subpath = "/usr/etc/selinux" };
  if (!ostree_repo_checkout_at (self->repo, &opts, self->workdir_dfd,
                                TMP_SELINUX_ROOTFS, self->previous_checksum,
                                cancellable, error))
    return FALSE;

#undef TMP_SELINUX_ROOTFS

  g_autofree char *abspath = glnx_fdrel_abspath (self->workdir_dfd, "selinux.tmp");
  g_autoptr(GFile) path = g_file_new_for_path (abspath);
  g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new (path, cancellable, error);
  if (sepolicy == NULL)
    return FALSE;

  rpmostree_context_set_sepolicy (self->corectx, sepolicy);

  return TRUE;
}

static gboolean
install_packages (RpmOstreeTreeComposeContext  *self,
                  gboolean                     *out_unmodified,
                  char                        **out_new_inputhash,
                  GCancellable                 *cancellable,
                  GError                      **error)
{
  int rootfs_dfd = self->rootfs_dfd;
  DnfContext *dnfctx = rpmostree_context_get_dnf (self->corectx);
  if (opt_proxy)
    dnf_context_set_http_proxy (dnfctx, opt_proxy);

  /* Hack this here... see https://github.com/rpm-software-management/libhif/issues/53
   * but in the future we won't be using librpm at all for unpack/scripts, so it won't
   * matter.
   */
  { const char *debuglevel = getenv ("RPMOSTREE_RPM_VERBOSITY");
    if (!debuglevel)
      debuglevel = "info";
    dnf_context_set_rpm_verbosity (dnfctx, debuglevel);
    rpmlogSetFile(NULL);
  }

  { int tf_dfd = (*self->treefile_rs)->get_workdir();
    g_autofree char *abs_tf_path = glnx_fdrel_abspath (tf_dfd, ".");
    dnf_context_set_repo_dir (dnfctx, abs_tf_path);
  }

  /* By default, retain packages in addition to metadata with --cachedir, unless
   * we're doing unified core, in which case the pkgcache repo is the cache.
   */
  if (opt_cachedir && !opt_unified_core)
    dnf_context_set_keep_cache (dnfctx, TRUE);
  /* For compose, always try to refresh metadata; we're used in build servers
   * where fetching should be cheap.  We also have --cache-only which is
   * used by coreos-assembler.  Today we don't expose the default, but we
   * could add --cache-default or something if someone wanted it.
   */
  rpmostree_context_set_dnf_caching (self->corectx,
                                     opt_cache_only ? RPMOSTREE_CONTEXT_DNF_CACHE_FOREVER :
                                     RPMOSTREE_CONTEXT_DNF_CACHE_NEVER);

  { g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (rootfs_dfd, ".");
    if (!rpmostree_context_setup (self->corectx, tmprootfs_abspath, NULL,
                                  cancellable, error))
      return FALSE;
  }

  if (!try_load_previous_sepolicy (self, cancellable, error))
    return FALSE;

  /* For unified core, we have a pkgcache repo. This is auto-created under the cachedir. */
  if (opt_unified_core)
    {
      g_assert (self->pkgcache_repo);

      if (!opt_cachedir)
        {
          /* This is part of enabling rpm-ostree inside Docker/Kubernetes/OpenShift;
           * in this case we probably don't have access to FUSE as today it uses a
           * suid binary which doesn't have the capabilities it needs.
           *
           * So this magical bit tells the core to disable FUSE, which we only do
           * if --cachedir isn't specified.  Another way to say this is that
           * running inside an unprivileged container today requires turning off
           * some of the rpm-ostree intelligence around caching.
           *
           * We don't make this actually conditional somehow on running in a
           * container since if you're not using a persistent cache there's no
           * real advantage to taking the overhead of FUSE. If the hardlinks are
           * corrupted, it doesn't matter as they're going to be deleted
           * anyways.
           */
          rpmostree_context_disable_rofiles (self->corectx);
        }
      else
        {
          self->unified_core_and_fuse = TRUE;
          /* We also only enable the devino cache if we know we have the FUSE protection
           * against mutation of the underlying files.
           */
          self->devino_cache = ostree_repo_devino_cache_new ();
          rpmostree_context_set_devino_cache (self->corectx, self->devino_cache);
        }

      rpmostree_context_set_repos (self->corectx, self->build_repo, self->pkgcache_repo);
    }
  else
    {
      /* Secret environment variable for those desperate */
      if (!g_getenv ("RPM_OSTREE_I_KNOW_NON_UNIFIED_CORE_IS_DEPRECATED"))
        {
          g_printerr ("\nNOTICE: Running rpm-ostree compose tree without --unified-core is deprecated.\n"
                      " Please add --unified-core to the command line and ensure your content\n"
                      " works with it.  For more information, see https://github.com/coreos/rpm-ostree/issues/729\n\n");
          g_usleep (G_USEC_PER_SEC * 10);
        }
    }

  if (!rpmostree_context_prepare (self->corectx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (dnfctx);

  if (opt_write_lockfile_to)
    {
      g_autoptr(GPtrArray) pkgs = rpmostree_context_get_packages (self->corectx);
      g_assert (pkgs);

      g_autoptr(GPtrArray) rpmmd_repos =
        rpmostree_get_enabled_rpmmd_repos (rpmostree_context_get_dnf (self->corectx),
                                           DNF_REPO_ENABLED_PACKAGES);
      auto pkgs_v = rpmostreecxx::CxxGObjectArray(pkgs);
      auto repos_v = rpmostreecxx::CxxGObjectArray(rpmmd_repos);
      rpmostreecxx::lockfile_write(opt_write_lockfile_to, pkgs_v, repos_v);
    }

  /* FIXME - just do a depsolve here before we compute download requirements */
  g_autofree char *ret_new_inputhash = NULL;
  if (!rpmostree_composeutil_checksum (dnf_context_get_goal (dnfctx), self->repo,
                                       **self->treefile_rs, self->treefile,
                                       &ret_new_inputhash, error))
    return FALSE;

  g_print ("Input state hash: %s\n", ret_new_inputhash);

  /* Only look for previous checksum if caller has passed *out_unmodified */
  if (self->previous_checksum && out_unmodified != NULL)
    {
      g_autofree char *previous_inputhash = NULL;
      if (!inputhash_from_commit (self->repo, self->previous_checksum,
                                  &previous_inputhash, error))
        return FALSE;

      if (previous_inputhash)
        {
          if (strcmp (previous_inputhash, ret_new_inputhash) == 0)
            {
              *out_unmodified = TRUE;
              return TRUE; /* NB: early return */
            }
        }
      else
        g_print ("Previous commit found, but without rpmostree.inputhash metadata key\n");
    }

  if (opt_dry_run)
    return TRUE; /* NB: early return */

  (*self->treefile_rs)->sanitycheck_externals();

  /* --- Downloading packages --- */
  if (!rpmostree_context_download (self->corectx, cancellable, error))
    return FALSE;

  if (opt_download_only || opt_download_only_rpms)
    {
      if (opt_unified_core && !opt_download_only_rpms)
        {
          if (!rpmostree_context_import (self->corectx, cancellable, error))
            return FALSE;
        }
      return TRUE; /* 🔚 Early return */
    }

  /* Before we install packages, inject /etc/{passwd,group} if configured. */
  g_assert (self->repo);
  auto previous_ref = self->previous_checksum?: "";
  rpmostreecxx::passwd_compose_prep_repo(rootfs_dfd, **self->treefile_rs, *self->repo,
                                         std::string(previous_ref), opt_unified_core);

  if (opt_unified_core)
    {
      if (!rpmostree_context_import (self->corectx, cancellable, error))
        return FALSE;
      rpmostree_context_set_tmprootfs_dfd (self->corectx, rootfs_dfd);
      if (!rpmostree_context_assemble (self->corectx, cancellable, error))
        return FALSE;

      /* Now reload the policy from the tmproot, and relabel the pkgcache - this
       * is the same thing done in rpmostree_context_commit().
       */
      g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
      if (sepolicy == NULL)
        return FALSE;

      rpmostree_context_set_sepolicy (self->corectx, sepolicy);

      if (!rpmostree_context_force_relabel (self->corectx, cancellable, error))
        return FALSE;
    }
  else
    {
      /* The non-unified core path */

      /* Before we install packages, drop a file to suppress the kernel.rpm dracut run.
       * <https://github.com/systemd/systemd/pull/4174> */
      const char *kernel_installd_path = "usr/lib/kernel/install.d";
      if (!glnx_shutil_mkdir_p_at (rootfs_dfd, kernel_installd_path, 0755, cancellable, error))
        return FALSE;
      const char skip_kernel_install_data[] = "#!/usr/bin/sh\nexit 77\n";
      const char *kernel_skip_path = glnx_strjoina (kernel_installd_path, "/00-rpmostree-skip.install");
      if (!glnx_file_replace_contents_with_perms_at (rootfs_dfd, kernel_skip_path,
                                                     (guint8*)skip_kernel_install_data,
                                                     strlen (skip_kernel_install_data),
                                                     0755, 0, 0,
                                                     GLNX_FILE_REPLACE_NODATASYNC,
                                                     cancellable, error))
        return FALSE;

      /* Now actually run through librpm to install the packages.  Note this bit
       * will be replaced in the future with a unified core:
       * https://github.com/projectatomic/rpm-ostree/issues/729
       */
      g_auto(GLnxConsoleRef) console = { 0, };
      g_autoptr(DnfState) hifstate = dnf_state_new ();

      guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                               reinterpret_cast<GCallback>(on_hifstate_percentage_changed),
                                               (void*)"Installing packages:");

      glnx_console_lock (&console);

      if (!rpmostree_composeutil_legacy_prep_dev (rootfs_dfd, error))
        return FALSE;

      if (!dnf_transaction_commit (dnf_context_get_transaction (dnfctx),
                                   dnf_context_get_goal (dnfctx),
                                   hifstate, error))
        return FALSE;

      g_signal_handler_disconnect (hifstate, progress_sigid);
    }

  if (out_unmodified)
    *out_unmodified = FALSE;
  *out_new_inputhash = util::move_nullify (ret_new_inputhash);
  return TRUE;
}

static gboolean
parse_metadata_keyvalue_strings (char             **strings,
                                 GHashTable        *metadata_hash,
                                 GError           **error)
{
  for (char **iter = strings; *iter; iter++)
    {
      const char *s = *iter;
      const char *eq = strchr (s, '=');
      if (!eq)
        return glnx_throw (error, "Missing '=' in KEY=VALUE metadata '%s'", s);

      g_hash_table_insert (metadata_hash, g_strndup (s, eq - s),
                           g_variant_ref_sink (g_variant_new_string (eq + 1)));
    }

  return TRUE;
}

static gboolean
process_touch_if_changed (GError **error)
{

  if (!opt_touch_if_changed)
    return TRUE;

  glnx_autofd int fd = open (opt_touch_if_changed, O_CREAT|O_WRONLY|O_NOCTTY, 0644);
  if (fd == -1)
    return glnx_throw_errno_prefix (error, "Updating '%s'", opt_touch_if_changed);
  if (futimens (fd, NULL) == -1)
    return glnx_throw_errno_prefix (error, "futimens");

  return TRUE;
}

/* https://pagure.io/atomic-wg/issue/387 */
static gboolean
repo_is_on_netfs (OstreeRepo  *repo)
{
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

  int dfd = ostree_repo_get_dfd (repo);
  struct statfs stbuf;
  if (fstatfs (dfd, &stbuf) != 0)
    return FALSE;
  switch (stbuf.f_type)
    {
    case NFS_SUPER_MAGIC:
    case FUSE_SUPER_MAGIC:
      return TRUE;
    default:
      return FALSE;
    }
}

/* Prepare a context - this does some generic pre-compose initialization from
 * the arguments such as loading the treefile and any specified metadata.
 */
static gboolean
rpm_ostree_compose_context_new (const char    *treefile_pathstr,
                                RpmOstreeTreeComposeContext **out_context,
                                GCancellable  *cancellable,
                                GError       **error)
{
  g_autoptr(RpmOstreeTreeComposeContext) self = g_new0 (RpmOstreeTreeComposeContext, 1);

  rpmostreecxx::core_libdnf_process_global_init();

  /* Init fds to -1 */
  self->workdir_dfd = self->rootfs_dfd = self->cachedir_dfd = -1;
  /* Test whether or not bwrap is going to work - we will fail inside e.g. a Docker
   * container without --privileged or userns exposed.
   */
  if (!(opt_download_only || opt_download_only_rpms))
    rpmostreecxx::bubblewrap_selftest();

  self->repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!self->repo)
    return FALSE;

  /* sanity check upfront we can even write to this repo; e.g. might be a mount */
  if (!ostree_repo_is_writable (self->repo, error))
    return glnx_prefix_error (error, "Cannot write to repository");

  if (opt_workdir_tmpfs)
    g_printerr ("note: --workdir-tmpfs is deprecated and will be ignored\n");

  if (opt_unified_core)
    {
      /* Unified mode works very differently. We ignore --workdir because we want to be sure
       * that we're going to get hardlinks. The only way to be sure of this is to place the
       * workdir underneath the cachedir; the same fs where the pkgcache repo is. */

      if (opt_workdir)
        g_printerr ("note: --workdir is ignored for --unified-core\n");

      if (opt_cachedir)
        {
          if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
            return glnx_prefix_error (error, "Opening cachedir");

          /* Put workdir beneath cachedir, which is where the pkgcache repo also is */
          if (!glnx_mkdtempat (self->cachedir_dfd, "rpm-ostree-compose.XXXXXX", 0700,
                               &self->workdir_tmp, error))
            return FALSE;
        }
      else
        {
          /* Put cachedir under the target repo if it's not on NFS or fuse. It makes things
           * more efficient if it's bare-user, and otherwise just restricts IO to within the
           * same fs. If for whatever reason users don't want to run the compose there (e.g.
           * weird filesystems that aren't fully POSIX compliant), they can just use
           * --cachedir.
           */
          if (!repo_is_on_netfs (self->repo))
            {
              if (!glnx_mkdtempat (ostree_repo_get_dfd (self->repo),
                                   "tmp/rpm-ostree-compose.XXXXXX", 0700,
                                   &self->workdir_tmp, error))
                return FALSE;
            }
          else
            {
              if (!glnx_mkdtempat (AT_FDCWD, "/var/tmp/rpm-ostree-compose.XXXXXX", 0700,
                                   &self->workdir_tmp, error))
                return FALSE;
            }

          self->cachedir_dfd = fcntl (self->workdir_tmp.fd, F_DUPFD_CLOEXEC, 3);
          if (self->cachedir_dfd < 0)
            return glnx_throw_errno_prefix (error, "fcntl");
        }

      self->pkgcache_repo = ostree_repo_create_at (self->cachedir_dfd, "pkgcache-repo",
                                                   OSTREE_REPO_MODE_BARE_USER, NULL,
                                                   cancellable, error);
      if (!self->pkgcache_repo)
        return FALSE;

      /* We use a temporary repo for building and committing on the same FS as the
       * pkgcache to guarantee links and devino caching. We then pull-local into the "real"
       * target repo. */
      self->build_repo = ostree_repo_create_at (self->cachedir_dfd, "repo-build",
                                          OSTREE_REPO_MODE_BARE_USER, NULL,
                                          cancellable, error);
      if (!self->build_repo)
        return glnx_prefix_error (error, "Creating repo-build");

      /* Note special handling of this aliasing in rpm_ostree_tree_compose_context_free() */
      self->workdir_dfd = self->workdir_tmp.fd;
    }
  else
    {
      if (!opt_workdir)
        {
          if (!glnx_mkdtempat (AT_FDCWD, "/var/tmp/rpm-ostree.XXXXXX", 0700, &self->workdir_tmp, error))
            return FALSE;
          /* Note special handling of this aliasing in rpm_ostree_tree_compose_context_free() */
          self->workdir_dfd = self->workdir_tmp.fd;
        }
      else
        {
          if (!glnx_opendirat (AT_FDCWD, opt_workdir, FALSE, &self->workdir_dfd, error))
            return FALSE;
        }

      if (opt_cachedir)
        {
          if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
            return glnx_prefix_error (error, "Opening cachedir");
        }
      else
        {
          self->cachedir_dfd = fcntl (self->workdir_dfd, F_DUPFD_CLOEXEC, 3);
          if (self->cachedir_dfd < 0)
            return glnx_throw_errno_prefix (error, "fcntl");
        }

      self->build_repo = static_cast<OstreeRepo*>(g_object_ref (self->repo));
    }

  /* FIXME refactor things to break the dependency of DnfContext -> arch -> treefile */
  std::string arch;
  {
      g_autoptr(DnfContext) ctx = dnf_context_new ();
      arch = dnf_context_get_base_arch (ctx);
  }
  self->treefile_path = g_file_new_for_path (treefile_pathstr);
  self->treefile_rs = rpmostreecxx::treefile_new_compose(gs_file_get_path_cached (self->treefile_path), arch, self->workdir_dfd);
  self->corectx = rpmostree_context_new_compose (self->cachedir_dfd, self->build_repo,
                                                 **self->treefile_rs);
  /* In the legacy compose path, we don't want to use any of the core's selinux stuff,
   * e.g. importing, relabeling, etc... so just disable it. We do still set the policy
   * to the final one right before commit as usual. */
  if (!opt_unified_core)
    rpmostree_context_disable_selinux (self->corectx);

  self->ref = g_strdup (rpmostree_context_get_ref (self->corectx));

  if (opt_lockfiles)
    {
      rpmostree_context_set_lockfile (self->corectx, opt_lockfiles, opt_lockfile_strict);
      g_print ("Loaded lockfiles:\n  %s\n", g_strjoinv ("\n  ", opt_lockfiles));
    }


  auto serialized = (*self->treefile_rs)->get_json_string();
  self->treefile_parser = json_parser_new ();
  if (!json_parser_load_from_data (self->treefile_parser, serialized.c_str(), -1, error))
    return FALSE;

  (*self->treefile_rs)->print_deprecation_warnings();

  self->treefile_rootval = json_parser_get_root (self->treefile_parser);
  if (!JSON_NODE_HOLDS_OBJECT (self->treefile_rootval))
    return glnx_throw (error, "Treefile root is not an object");

  self->treefile = json_node_get_object (self->treefile_rootval);

  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)g_variant_unref);

  /* metadata from the treefile itself has the lowest priority */
  JsonNode *add_commit_metadata_node =
    json_object_get_member (self->treefile, "add-commit-metadata");
  if (add_commit_metadata_node)
    {
      if (!rpmostree_composeutil_read_json_metadata (add_commit_metadata_node,
                                                     self->metadata, error))
        return FALSE;
    }

  /* then --add-metadata-from-json */
  if (opt_metadata_json)
    {
      for (char **iter = opt_metadata_json; iter && *iter; iter++)
        {
          const char *metadata_json_file = *iter;
          if (!rpmostree_composeutil_read_json_metadata_from_file (metadata_json_file,
                                                                   self->metadata, error))
            return FALSE;
        }
    }

  /* and finally --add-metadata-string */
  if (opt_metadata_strings)
    {
      if (!parse_metadata_keyvalue_strings (opt_metadata_strings, self->metadata, error))
        return FALSE;
    }

  auto layers = (*self->treefile_rs)->get_all_ostree_layers();
  if (layers.size() > 0 && !opt_unified_core)
    return glnx_throw (error, "ostree-layers requires unified-core mode");

  if (self->build_repo != self->repo)
    {
      for (auto layer : layers)
        {
          if (!pull_local_into_target_repo (self->repo, self->build_repo, layer.c_str(),
                                            cancellable, error))
            return FALSE;
        }
    }

  *out_context = util::move_nullify (self);
  return TRUE;
}

static gboolean
inject_advisories (RpmOstreeTreeComposeContext *self,
                   GCancellable    *cancellable,
                   GError         **error)
{
  g_autoptr(GPtrArray) pkgs = rpmostree_context_get_packages (self->corectx);
  DnfContext *dnfctx = rpmostree_context_get_dnf (self->corectx);
  DnfSack *yum_sack = dnf_context_get_sack (dnfctx);
  g_autoptr(GVariant) advisories = rpmostree_advisories_variant (yum_sack, pkgs);

  if (advisories && g_variant_n_children (advisories) > 0)
    g_hash_table_insert (self->metadata, g_strdup ("rpmostree.advisories"), g_steal_pointer (&advisories));

  return TRUE;
}

static gboolean
impl_install_tree (RpmOstreeTreeComposeContext *self,
                   gboolean        *out_changed,
                   GCancellable    *cancellable,
                   GError         **error)
{
  /* Set this early here, so we only have to set it one more time in the
   * complete exit path too.
   */
  *out_changed = FALSE;

  /* Print version number */
  g_printerr ("rpm-ostree version: %s\n", PACKAGE_VERSION);

  /* Without specifying --cachedir we'd just toss the data we download, so let's
   * catch that.
   */
  if ((opt_download_only || opt_download_only_rpms) && !opt_unified_core && !opt_cachedir)
    return glnx_throw (error, "--download-only can only be used with --cachedir");

  if (getuid () != 0)
    {
      if (!opt_unified_core)
        return glnx_throw (error, "This command requires root privileges");
      g_printerr ("NOTICE: Running this command as non-root is currently known not to work completely.\n");
      g_printerr ("NOTICE: Proceeding anyways.\n");
    }

  /* This fchdir() call is...old, dates back to when rpm-ostree wrapped
   * running yum as a subprocess.  It shouldn't be necessary any more,
   * but let's be conservative and not do it in unified core mode.
   */
  if (!opt_unified_core)
    {
      if (fchdir (self->workdir_dfd) != 0)
        return glnx_throw_errno_prefix (error, "fchdir");
    }

  /* Read the previous commit. Note we don't actually *need* the full commit; really, only
   * if one uses `check-passwd: { "type": "previous" }`. There are a few other optimizations
   * too, e.g. using the previous SELinux policy in unified core. Also, we might need the
   * commit *object* for next version incrementing. */
  if (opt_previous_commit)
    {
      if (!ostree_repo_resolve_rev (self->repo, opt_previous_commit, FALSE,
                                    &self->previous_checksum, error))
        return FALSE;
    }
  else if (self->ref)
    {
      if (!ostree_repo_resolve_rev (self->repo, self->ref, TRUE,
                                    &self->previous_checksum, error))
        return FALSE;

      if (!self->previous_checksum)
        g_print ("No previous commit for %s\n", self->ref);
      else
        g_print ("Previous commit: %s\n", self->previous_checksum);
    }

  const char rootfs_name[] = "rootfs.tmp";
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
    return FALSE;
  if (mkdirat (self->workdir_dfd, rootfs_name, 0755) < 0)
    return glnx_throw_errno_prefix (error, "mkdirat(%s)", rootfs_name);

  if (!glnx_opendirat (self->workdir_dfd, rootfs_name, TRUE,
                       &self->rootfs_dfd, error))
    return FALSE;

  rust::String next_version;
  if (json_object_has_member (self->treefile, "automatic-version-prefix") &&
      /* let --add-metadata-string=version=... take precedence */
      !g_hash_table_contains (self->metadata, OSTREE_COMMIT_META_KEY_VERSION))
    {
      const char *ver_prefix =
        _rpmostree_jsonutil_object_require_string_member (self->treefile, "automatic-version-prefix", error);
      if (!ver_prefix)
        return FALSE;
      const char *ver_suffix = NULL;
      if (!_rpmostree_jsonutil_object_get_optional_string_member (self->treefile, "automatic-version-suffix", &ver_suffix, error))
        return FALSE;

      g_autofree char *last_version = NULL;
      if (self->previous_checksum)
        {
          g_autoptr(GVariant) previous_commit = NULL;
          if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         self->previous_checksum, &previous_commit, error))
            return FALSE;

          g_autoptr(GVariant) previous_metadata = g_variant_get_child_value (previous_commit, 0);
          (void)g_variant_lookup (previous_metadata, OSTREE_COMMIT_META_KEY_VERSION, "s", &last_version);
        }

      next_version = rpmostreecxx::util_next_version (ver_prefix, ver_suffix ?: "", last_version ?: "");
      g_hash_table_insert (self->metadata, g_strdup (OSTREE_COMMIT_META_KEY_VERSION),
                           g_variant_ref_sink (g_variant_new_string (next_version.c_str())));
    }
  else
    {
      gpointer vp = g_hash_table_lookup (self->metadata, OSTREE_COMMIT_META_KEY_VERSION);
      auto v = static_cast<GVariant*>(vp);
      if (v)
        {
          g_assert (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING));
          next_version = rust::String(static_cast<char*>(g_variant_dup_string (v, NULL)));
        }
    }

  /* Download rpm-md repos, packages, do install */
  g_autofree char *new_inputhash = NULL;
  { gboolean unmodified = FALSE;

    if (!install_packages (self, opt_force_nocache ? NULL : &unmodified,
                           &new_inputhash, cancellable, error))
      return FALSE;

    gboolean is_dry_run = opt_dry_run || (opt_download_only || opt_download_only_rpms);
    if (unmodified)
      {
        const char *force_nocache_msg = "; use --force-nocache to override";
        g_print ("No apparent changes since previous commit%s\n",
                 is_dry_run ? "." : force_nocache_msg);
        /* Note early return */
        return TRUE;
      }
    else if (is_dry_run)
      {
        g_print ("--dry-run complete");
        if (opt_touch_if_changed)
          g_print (", updating --touch-if-changed=%s", opt_touch_if_changed);
        g_print ("; exiting\n");
        if (!process_touch_if_changed (error))
          return FALSE;
        /* Note early return */
        return TRUE;
      }
  }

  /* Bind metadata from the libdnf context */
  if (!g_hash_table_contains (self->metadata, "rpmostree.rpmmd-repos"))
    {
      g_hash_table_insert (self->metadata, g_strdup ("rpmostree.rpmmd-repos"),
                           rpmostree_context_get_rpmmd_repo_commit_metadata (self->corectx));
    }

  if (!inject_advisories (self, cancellable, error))
    return FALSE;

  /* Destroy this now so the libdnf stack won't have any references
   * into the filesystem before we manipulate it.
   */
  g_clear_object (&self->corectx);

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    return FALSE;

  /* Start postprocessing */
  rpmostreecxx::compose_postprocess(self->rootfs_dfd, **self->treefile_rs, next_version, self->unified_core_and_fuse);

  /* Until here, we targeted "rootfs.tmp" in the working directory. Most
   * user-configured postprocessing has run. Now, we need to perform required
   * conversions like handling /boot. We generate a new directory "rootfs" that
   * has just what we want using "rootfs.tmp", as a source. This implicitly
   * discards anything else that happens to be in rootfs.tmp, like the `/dev`
   * nodes we create for example.
   */
  const char final_rootfs_name[] = "rootfs";
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, final_rootfs_name, cancellable, error))
    return FALSE;
  if (!glnx_ensure_dir (self->workdir_dfd, final_rootfs_name, 0755, error))
    return FALSE;
  { glnx_autofd int target_rootfs_dfd = -1;
    if (!glnx_opendirat (self->workdir_dfd, final_rootfs_name, TRUE,
                         &target_rootfs_dfd, error))
      return FALSE;

    rpmostreecxx::compose_prepare_rootfs(self->rootfs_dfd, target_rootfs_dfd, **self->treefile_rs);

    glnx_close_fd (&self->rootfs_dfd);

    /* Remove the old root, then retarget rootfs_dfd to the final one */
    if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
      return FALSE;

    self->rootfs_dfd = glnx_steal_fd (&target_rootfs_dfd);
  }

  /* Insert our input hash */
  g_hash_table_replace (self->metadata, g_strdup ("rpmostree.inputhash"),
                        g_variant_ref_sink (g_variant_new_string (new_inputhash)));

  *out_changed = TRUE;
  return TRUE;
}

/* See canonical version of this in ot-builtin-pull.c */
static void
noninteractive_console_progress_changed (OstreeAsyncProgress *progress,
                                         gpointer             user_data)
{
  /* We do nothing here - we just want the final status */
}

static gboolean
pull_local_into_target_repo (OstreeRepo   *src_repo,
                             OstreeRepo   *dest_repo,
                             const char   *checksum,
                             GCancellable *cancellable,
                             GError      **error)
{
  const char *refs[] = { checksum, NULL };

  /* really should enhance the pull API so we can just pass the src OstreeRepo directly */
  g_autofree char *src_repo_uri =
    g_strdup_printf ("file:///proc/self/fd/%d", ostree_repo_get_dfd (src_repo));

  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_console_lock (&console);
  g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new_and_connect (
      console.is_tty ? ostree_repo_pull_default_console_progress_changed
                     : noninteractive_console_progress_changed, &console);

  /* no fancy flags here, so just use the old school simpler API */
  if (!ostree_repo_pull (dest_repo, src_repo_uri, (char**)refs, static_cast<OstreeRepoPullFlags>(0), progress,
                         cancellable, error))
    return FALSE;

  if (!console.is_tty)
    {
      const char *status = ostree_async_progress_get_status (progress);
      if (status)
        g_print ("%s\n", status);
    }
  ostree_async_progress_finish (progress);

  return TRUE;
}

/* Perform required postprocessing, and invoke rpmostree_compose_commit(). */
static gboolean
impl_commit_tree (RpmOstreeTreeComposeContext *self,
                  GCancellable    *cancellable,
                  GError         **error)
{
  const char *gpgkey = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (self->treefile, "gpg-key", &gpgkey, error))
    return FALSE;

  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (self->treefile, "selinux", &selinux, error))
    return FALSE;

  /* pick up any initramfs regeneration args to shove into the metadata */
  JsonNode *initramfs_args = json_object_get_member (self->treefile, "initramfs-args");
  if (initramfs_args)
    {
      GVariant *v = json_gvariant_deserialize (initramfs_args, "as", error);
      if (!v)
       return FALSE;
      g_hash_table_insert (self->metadata, g_strdup ("rpmostree.initramfs-args"),
                           g_variant_ref_sink (v));
    }

  /* Convert metadata hash to GVariant */
  g_autoptr(GVariant) metadata = rpmostree_composeutil_finalize_metadata (self->metadata, self->rootfs_dfd, error);
  if (!metadata)
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (self->rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (self->rootfs_dfd,
                                    self->treefile, self->unified_core_and_fuse,
                                    cancellable, error))
    return FALSE;

  if (self->treefile_rs)
    {
      auto previous_rev = self->previous_checksum?: "";
      rpmostreecxx::check_passwd_group_entries (*self->repo, self->rootfs_dfd,
                                                **self->treefile_rs, previous_rev);
    }

  /* See comment above */
  const gboolean txn_explicitly_disabled = (getenv ("RPMOSTREE_COMMIT_NO_TXN") != NULL);
  const gboolean using_netfs = repo_is_on_netfs (self->repo);
  if (txn_explicitly_disabled)
    g_print ("libostree transactions explicitly disabled\n");
  else if (using_netfs)
    g_print ("Network filesystem detected for repo; disabling transaction\n");
  const gboolean use_txn = !(txn_explicitly_disabled || using_netfs);

  if (use_txn)
    {
      if (!ostree_repo_prepare_transaction (self->build_repo, NULL, cancellable, error))
        return FALSE;
    }

  g_autofree char *parent_revision = NULL;
  if (opt_parent)
    {
      if (!ostree_repo_resolve_rev (self->repo, opt_parent, FALSE, &parent_revision, error))
        return FALSE;
    }
  else if (self->ref && !opt_no_parent)
    {
      if (!ostree_repo_resolve_rev (self->repo, self->ref, TRUE, &parent_revision, error))
        return FALSE;
    }

  /* The penultimate step, just basically `ostree commit` */
  g_autofree char *new_revision = NULL;
  if (!rpmostree_compose_commit (self->rootfs_dfd, self->build_repo, parent_revision,
                                 metadata, gpgkey, selinux, self->devino_cache,
                                 &new_revision, cancellable, error))
    return glnx_prefix_error (error, "Writing commit");
  g_assert(new_revision != NULL);

  OstreeRepoTransactionStats stats = { 0, };
  OstreeRepoTransactionStats *statsp = NULL;

  if (use_txn)
    {
      if (!ostree_repo_commit_transaction (self->build_repo, &stats, cancellable, error))
        return glnx_prefix_error (error, "Commit");
      statsp = &stats;
      rpmostreecxx::print_ostree_txn_stats (stats);
    }

  if (!opt_unified_core)
    g_assert (self->repo == self->build_repo);
  else
    {
      /* Now we actually pull it into the target repo specified by the user */
      g_assert (self->repo != self->build_repo);

      if (!pull_local_into_target_repo (self->build_repo, self->repo, new_revision,
                                        cancellable, error))
        return FALSE;
    }

  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (self->repo, new_revision, &new_commit, NULL, error))
    return FALSE;

  /* --write-commitid-to overrides writing the ref */
  const char *new_ref = NULL;
  if (self->ref && !opt_write_commitid_to)
    {
      new_ref = self->ref;
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, self->ref, new_revision,
                                          cancellable, error))
        return FALSE;
      g_print ("%s => %s\n", self->ref, new_revision);
    }
  else
    g_print ("Wrote commit: %s\n", new_revision);

  /* Optionally write a JSON summary of this compose-commit run */
  if (opt_write_commitid_to)
    if (!rpmostree_composeutil_write_composejson (self->repo,
                                                  opt_write_composejson_to, statsp,
                                                  new_revision, new_commit, new_ref,
                                                  cancellable, error))
     return FALSE;

  if (opt_write_commitid_to)
    rpmostreecxx::write_commit_id(opt_write_commitid_to, new_revision);

  return TRUE;
}

static gboolean
parse_and_print_treefile (const char *treefile_path, GError **error)
{
  g_autofree char *arch = rpm_ostree_get_basearch ();
  auto treefile_rs = rpmostreecxx::treefile_new (treefile_path, arch, -1);
  auto buf = treefile_rs->get_json_string();
  g_print ("%s\n", buf.c_str());
  return TRUE;
}

gboolean
rpmostree_compose_builtin_install (int             argc,
                                   char          **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE DESTDIR");
  g_option_context_add_main_entries (context, common_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       install_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc != 3)
   {
      rpmostree_usage_error (context, "TREEFILE and DESTDIR required", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];

  if (opt_print_only)
    return parse_and_print_treefile (treefile_path, error);

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  if (opt_workdir)
    {
      rpmostree_usage_error (context, "--workdir is ignored with install-root", error);
      return FALSE;
    }

  /* Destination is turned into workdir */
  const char *destdir = argv[2];
  opt_workdir = g_strdup (destdir);

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  g_assert (self); /* Pacify static analysis */
  gboolean changed;
  /* Need to handle both GError and C++ exceptions here */
  try {
    if (!impl_install_tree (self, &changed, cancellable, error))
      {
        self->failed = TRUE;
        return FALSE;
      }
  } catch (std::exception &e) {
    self->failed = TRUE;
    throw;
  }
  if (opt_unified_core)
    {
      if (!glnx_renameat (self->workdir_tmp.src_dfd, self->workdir_tmp.path,
                          AT_FDCWD, destdir, error))
        return FALSE;
      glnx_tmpdir_unset (&self->workdir_tmp);
      self->workdir_dfd = -1;
    }
  g_print ("rootfs: %s/rootfs\n", destdir);

  return TRUE;
}

gboolean
rpmostree_compose_builtin_postprocess (int             argc,
                                       char          **argv,
                                       RpmOstreeCommandInvocation *invocation,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("ROOTFS [TREEFILE]");
  if (!rpmostree_option_context_parse (context,
                                       postprocess_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2 || argc > 3)
    {
      rpmostree_usage_error (context, "ROOTFS must be specified", error);
      return FALSE;
    }

  const char *rootfs_path = argv[1];
  /* Here we *optionally* process a treefile; some things like `tmp-is-dir` and
   * `boot-location` are configurable and relevant here, but a lot of users
   * will also probably be OK with the defaults, and part of the idea here is
   * to avoid at least some of the use cases requiring a treefile.
   */
  const char *treefile_path = argc > 2 ? argv[2] : NULL;
  glnx_unref_object JsonParser *treefile_parser = NULL;
  JsonObject *treefile = NULL; /* Owned by parser */
  g_auto(GLnxTmpDir) workdir_tmp = { 0, };

  if (treefile_path)
    {
      if (!glnx_mkdtempat (AT_FDCWD, "/var/tmp/rpm-ostree.XXXXXX", 0700, &workdir_tmp, error))
        return FALSE;
      auto treefile_rs = rpmostreecxx::treefile_new_compose(treefile_path, "", workdir_tmp.fd);
      auto serialized = treefile_rs->get_json_string();
      treefile_parser = json_parser_new ();
      if (!json_parser_load_from_data (treefile_parser, serialized.c_str(), -1, error))
        return FALSE;

      JsonNode *treefile_rootval = json_parser_get_root (treefile_parser);
      if (!JSON_NODE_HOLDS_OBJECT (treefile_rootval))
        return glnx_throw (error, "Treefile root is not an object");
      treefile = json_node_get_object (treefile_rootval);
    }

  glnx_fd_close int rootfs_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, rootfs_path, TRUE, &rootfs_dfd, error))
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (rootfs_dfd, treefile, opt_unified_core,
                                    cancellable, error))
    return FALSE;
  return TRUE;
}

gboolean
rpmostree_compose_builtin_commit (int             argc,
                                  char          **argv,
                                  RpmOstreeCommandInvocation *invocation,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE ROOTFS");
  g_option_context_add_main_entries (context, common_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "TREEFILE must be specified", error);
      return FALSE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];
  const char *rootfs_path = argv[2];

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  if (!glnx_opendirat (AT_FDCWD, rootfs_path, TRUE, &self->rootfs_dfd, error))
    return FALSE;
  if (!impl_commit_tree (self, cancellable, error))
     return FALSE;
  return TRUE;
}

gboolean
rpmostree_compose_builtin_tree (int             argc,
                                char          **argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable   *cancellable,
                                GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE");
  g_option_context_add_main_entries (context, common_option_entries, NULL);
  g_option_context_add_main_entries (context, install_option_entries, NULL);
  g_option_context_add_main_entries (context, postprocess_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "TREEFILE must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];

  if (opt_print_only)
    return parse_and_print_treefile (treefile_path, error);

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  g_assert (self); /* Pacify static analysis */
  gboolean changed;
  /* Need to handle both GError and C++ exceptions here */
  try {
    if (!impl_install_tree (self, &changed, cancellable, error))
      {
        self->failed = TRUE;
        return FALSE;
      }
  } catch (std::exception &e) {
    self->failed = TRUE;
    throw;
  }
  if (changed)
    {
      /* Do the ostree commit */
      if (!impl_commit_tree (self, cancellable, error))
        {
          self->failed = TRUE;
          return FALSE;
        }
      /* Finally process the --touch-if-changed option  */
      if (!process_touch_if_changed (error))
        return FALSE;
    }


  return TRUE;
}

gboolean
rpmostree_compose_builtin_extensions (int             argc,
                                      char          **argv,
                                      RpmOstreeCommandInvocation *invocation,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE EXTYAML");
  g_option_context_add_main_entries (context, common_option_entries, NULL);
  g_option_context_add_main_entries (context, extensions_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       NULL,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 3)
    {
      rpmostree_usage_error (context, "TREEFILE and EXTYAML must be specified", error);
      return FALSE;
    }
  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }
  if (!opt_extensions_output_dir)
    {
      rpmostree_usage_error (context, "--output-dir must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];
  const char *extensions_path = argv[2];

  g_autofree char *basearch = rpm_ostree_get_basearch ();
  auto src_treefile = rpmostreecxx::treefile_new_compose(treefile_path, basearch, -1);

  g_autoptr(OstreeRepo) repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!repo)
    return FALSE;

  if (!opt_extensions_base_rev)
    {
      auto treeref = src_treefile->get_ostree_ref();
      if (treeref.length() == 0)
        return glnx_throw (error, "--base-rev not specified and treefile doesn't have a ref");
      opt_extensions_base_rev = g_strdup(treeref.c_str());
    }

  /* this is a similar construction to what's in rpm_ostree_compose_context_new() */
  g_auto(GLnxTmpDir) cachedir_tmp = { 0, };
  glnx_autofd int cachedir_dfd = -1;
  if (opt_cachedir)
    {
      if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &cachedir_dfd, error))
        return glnx_prefix_error (error, "Opening cachedir");
    }
  else
    {
      if (!glnx_mkdtempat (ostree_repo_get_dfd (repo),
                           "tmp/rpm-ostree-compose.XXXXXX", 0700,
                           &cachedir_tmp, error))
        return FALSE;

      cachedir_dfd = fcntl (cachedir_tmp.fd, F_DUPFD_CLOEXEC, 3);
      if (cachedir_dfd < 0)
        return glnx_throw_errno_prefix (error, "fcntl");
    }

  g_autofree char *base_rev = NULL;
  if (!ostree_repo_resolve_rev (repo, opt_extensions_base_rev, FALSE, &base_rev, error))
    return FALSE;

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, base_rev, &commit, NULL, error))
    return FALSE;

  g_autoptr(GPtrArray) packages =
      rpm_ostree_db_query_all (repo, opt_extensions_base_rev, cancellable, error);
  if (!packages)
      return FALSE;

  auto packages_mapping = std::make_unique<rust::Vec<rpmostreecxx::StringMapping>>();
  for (guint i = 0; i < packages->len; i++)
    {
      RpmOstreePackage *pkg = (RpmOstreePackage*)packages->pdata[i];
      const char *name = rpm_ostree_package_get_name (pkg);
      const char *evr = rpm_ostree_package_get_evr (pkg);
      packages_mapping->push_back(rpmostreecxx::StringMapping{name, evr});
    }

  auto extensions = rpmostreecxx::extensions_load (extensions_path, basearch, *packages_mapping);

  // This treefile basically tells the core to download the extension packages
  // from the repos, and that's it.
  auto extension_tf = extensions->generate_treefile(*src_treefile);

  // notice we don't use a pkgcache repo here like in the treecompose path: we
  // want RPMs, so having them already imported isn't useful to us (and anyway,
  // for OS extensions by definition they're not expected to be cached since
  // they're not in the base tree)
  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_compose (cachedir_dfd, repo, *extension_tf);

  { int tf_dfd = src_treefile->get_workdir();
    g_autofree char *abs_tf_path = glnx_fdrel_abspath (tf_dfd, ".");
    dnf_context_set_repo_dir (rpmostree_context_get_dnf (ctx), abs_tf_path);
  }

#define TMP_EXTENSIONS_ROOTFS "rpmostree-extensions.tmp"

  if (!glnx_shutil_rm_rf_at (cachedir_dfd, TMP_EXTENSIONS_ROOTFS, cancellable, error))
    return FALSE;

  g_print ("Checking out %.7s... ", base_rev);
  OstreeRepoCheckoutAtOptions opts = { .mode = OSTREE_REPO_CHECKOUT_MODE_USER };
  if (!ostree_repo_checkout_at (repo, &opts, cachedir_dfd, TMP_EXTENSIONS_ROOTFS,
                                base_rev, cancellable, error))
    return FALSE;
  g_print ("done!\n");

  g_autofree char *checkout_path = glnx_fdrel_abspath (cachedir_dfd, TMP_EXTENSIONS_ROOTFS);
  if (!rpmostree_context_setup (ctx, checkout_path, checkout_path, cancellable, error))
    return FALSE;

#undef TMP_EXTENSIONS_ROOTFS

  if (!rpmostree_context_prepare (ctx, cancellable, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, opt_extensions_output_dir, 0755, cancellable, error))
    return FALSE;

  glnx_autofd int output_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, opt_extensions_output_dir, TRUE, &output_dfd, error))
    return glnx_prefix_error (error, "Opening output dir");

  g_autofree char *state_checksum;
  if (!rpmostree_context_get_state_sha512 (ctx, &state_checksum, error))
    return FALSE;

  if (!extensions->state_checksum_changed (state_checksum, opt_extensions_output_dir))
    {
      g_print ("No change.\n");
      return TRUE;
    }

  if (!rpmostree_context_download (ctx, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) extensions_pkgs = rpmostree_context_get_packages (ctx);
  for (guint i = 0; i < extensions_pkgs->len; i++)
    {
      DnfPackage *pkg = (DnfPackage*)extensions_pkgs->pdata[i];
      const char *src = dnf_package_get_filename (pkg);
      const char *basename = glnx_basename (src);
      GLnxFileCopyFlags flags = static_cast<GLnxFileCopyFlags>(GLNX_FILE_COPY_NOXATTRS | GLNX_FILE_COPY_NOCHOWN);
      if (!glnx_file_copy_at (AT_FDCWD, dnf_package_get_filename (pkg), NULL, output_dfd,
                              basename, flags, cancellable, error))
        return FALSE;
    }

  /* This is hacky: for "development" extensions, we don't want any depsolving
   * against the base OS. Rather than awkwardly teach the core about this, we
   * just reuse its sack and keep all the functionality here. */

  DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);
  DnfSack *sack = dnf_context_get_sack (dnfctx);

  /* disable the system repo; we always want to download, even if already in the base */
  dnf_sack_repo_enabled (sack, HY_SYSTEM_REPO_NAME, 0);

  auto pkgs = extensions->get_development_packages();
  g_autoptr(GPtrArray) devel_pkgs_to_download =
    g_ptr_array_new_with_free_func (g_object_unref);
  for (auto & pkg : pkgs)
    {
      g_autoptr(GPtrArray) matches = rpmostree_get_matching_packages (sack, pkg.c_str());
      if (matches->len == 0)
        return glnx_throw (error, "Package %s not found", pkg.c_str());
      DnfPackage *found_pkg = (DnfPackage*)matches->pdata[0];
      g_ptr_array_add (devel_pkgs_to_download, g_object_ref (found_pkg));
    }

  rpmostree_set_repos_on_packages (dnfctx, devel_pkgs_to_download);

  if (!rpmostree_download_packages (devel_pkgs_to_download, cancellable, error))
    return FALSE;

  for (guint i = 0; i < devel_pkgs_to_download->len; i++)
    {
      DnfPackage *pkg = (DnfPackage*)devel_pkgs_to_download->pdata[i];
      const char *src = dnf_package_get_filename (pkg);
      const char *basename = glnx_basename (src);
      GLnxFileCopyFlags flags = static_cast<GLnxFileCopyFlags>(GLNX_FILE_COPY_NOXATTRS | GLNX_FILE_COPY_NOCHOWN);
      if (!glnx_file_copy_at (AT_FDCWD, dnf_package_get_filename (pkg), NULL, output_dfd,
                              basename, flags, cancellable, error))
        return FALSE;
    }

  // XXX: account for development extensions
  extensions->update_state_checksum (state_checksum, opt_extensions_output_dir);
  extensions->serialize_to_dir (opt_extensions_output_dir);
  if (!process_touch_if_changed (error))
    return FALSE;

  return TRUE;
}
