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
#include "ostree.h"

#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>
#include <libglnx.h>
#include <systemd/sd-journal.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-daemon.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include "rpmostree-output.h"
#include "rpmostree-core.h"
#include "rpmostree-importer.h"
#include "rpmostreed-utils.h"
#include "rpmostree-cxxrs.h"

static gboolean
vardict_lookup_bool (GVariantDict *dict,
                     const char   *key,
                     gboolean      dfault);

static void*
vardict_lookup_ptr (GVariantDict  *dict,
                    const char    *key,
                    const char    *fmt);

static gboolean
change_origin_refspec (GVariantDict    *options,
                       OstreeSysroot *sysroot,
                       RpmOstreeOrigin *origin,
                       const gchar *src_refspec,
                       GCancellable *cancellable,
                       gchar **out_old_refspec,
                       gchar **out_new_refspec,
                       GError **error)
{
  RpmOstreeRefspecType refspectype;
  const char *refspecdata;
  if (!rpmostree_refspec_classify (src_refspec, &refspectype, &refspecdata, error))
    return FALSE;

  RpmOstreeRefspecType current_refspectype;
  const char *current_refspecdata;
  rpmostree_origin_classify_refspec (origin, &current_refspectype, &current_refspecdata);

  /* Now here we "peel" it since the rest of the code assumes libostree */
  const char *refspec = refspecdata;

  g_autofree gchar *current_refspec =
    g_strdup (rpmostree_origin_get_refspec (origin));
  g_autofree gchar *new_refspec = NULL;

  if (!rpmostreed_refspec_parse_partial (refspec,
                                         current_refspec,
                                         &new_refspec,
                                         error))
    return FALSE;

  /* Re-classify after canonicalization to ensure we handle TYPE_CHECKSUM */
  if (!rpmostree_refspec_classify (new_refspec, &refspectype, &refspecdata, error))
    return FALSE;

  if (refspectype == RPMOSTREE_REFSPEC_TYPE_CHECKSUM)
    {
      const char *custom_origin_url = NULL;
      const char *custom_origin_description = NULL;
      g_variant_dict_lookup (options, "custom-origin", "(&s&s)",
                             &custom_origin_url,
                             &custom_origin_description);
      if (custom_origin_url && *custom_origin_url)
        {
          g_assert (custom_origin_description);
          if (!*custom_origin_description)
            return glnx_throw (error, "Invalid custom-origin");
        }
      if (!rpmostree_origin_set_rebase_custom (origin, new_refspec,
                                               custom_origin_url,
                                               custom_origin_description,
                                               error))
        return FALSE;
    }
  else
    {
      /* We only throw this error for non-checksum rebases; for
       * RHEL CoreOS + https://github.com/openshift/pivot/
       * we've had it happen that the same ostree commit can end up
       * in separate oscontainers.  We want to support changing
       * the custom origin that might point to the same commit.
       */
      if (strcmp (current_refspec, new_refspec) == 0)
        return glnx_throw (error, "Old and new refs are equal: %s", new_refspec);

      if (!rpmostree_origin_set_rebase (origin, new_refspec, error))
        return FALSE;
    }

  g_autofree gchar *current_remote = NULL;
  g_autofree gchar *current_branch = NULL;
  g_assert (ostree_parse_refspec (current_refspec, &current_remote, &current_branch, NULL));

  g_autofree gchar *new_remote = NULL;
  g_autofree gchar *new_branch = NULL;
  g_assert (ostree_parse_refspec (new_refspec, &new_remote, &new_branch, NULL));

  /* This version is a bit magical, so let's explain it.
     https://github.com/projectatomic/rpm-ostree/issues/569 */
  const gboolean switching_only_remote =
    g_strcmp0 (new_remote, current_remote) != 0 &&
    g_strcmp0 (new_branch, current_branch) == 0;
  if (switching_only_remote && new_remote != NULL)
    rpmostree_output_message ("Rebasing to %s:%s", new_remote, current_branch);

  if (out_new_refspec != NULL)
    *out_new_refspec = util::move_nullify (new_refspec);

  if (out_old_refspec != NULL)
    *out_old_refspec = util::move_nullify (current_refspec);

  return TRUE;
}

/* Handle `deploy` semantics of pinning to a version or checksum. See
 * rpmostreed_parse_revision() for available syntax for @revision */
static gboolean
apply_revision_override (RpmostreedTransaction    *transaction,
                         OstreeRepo               *repo,
                         OstreeAsyncProgress      *progress,
                         RpmOstreeOrigin          *origin,
                         gboolean                  skip_branch_check,
                         const char               *revision,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  RpmOstreeRefspecType refspectype;
  rpmostree_origin_classify_refspec (origin, &refspectype, NULL);

  if (refspectype == RPMOSTREE_REFSPEC_TYPE_CHECKSUM)
    return glnx_throw (error, "Cannot look up version while pinned to commit");

  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;
  if (!rpmostreed_parse_revision (revision, &checksum, &version, error))
    return FALSE;

  if (version != NULL)
    {
      switch (refspectype)
        {
        case RPMOSTREE_REFSPEC_TYPE_OSTREE:
          {
            /* Perhaps down the line we'll drive history traversal into libostree */
            rpmostree_output_message ("Resolving version '%s'", version);

            if (!rpmostreed_repo_lookup_version (repo, rpmostree_origin_get_refspec (origin),
                                                 version, progress,
                                                 cancellable, &checksum, error))
              return FALSE;

            rpmostree_origin_set_override_commit (origin, checksum, version);
          }
          break;
        case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
          g_assert_not_reached ();  /* Handled above */
        }
    }
  else
    {
      g_assert (checksum != NULL);

      switch (refspectype)
        {
        case RPMOSTREE_REFSPEC_TYPE_OSTREE:
          if (!skip_branch_check)
            {
              rpmostree_output_message ("Validating checksum '%s'", checksum);
              if (!rpmostreed_repo_lookup_checksum (repo, rpmostree_origin_get_refspec (origin),
                                                    checksum, progress, cancellable, error))
                return FALSE;
            }
          break;
        case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
          g_assert_not_reached ();  /* Handled above */
        }

      rpmostree_origin_set_override_commit (origin, checksum, version);
    }

  return TRUE;
}

/* Generates the update GVariant and caches it to disk. This is set as the CachedUpdate
 * property of RPMOSTreeOS by refresh_cached_update, but we calculate during transactions
 * only, since it's potentially costly to do. See:
 * https://github.com/projectatomic/rpm-ostree/pull/1268 */
static gboolean
generate_update_variant (OstreeRepo       *repo,
                         OstreeDeployment *booted_deployment,
                         OstreeDeployment *staged_deployment,
                         DnfSack          *sack, /* allow-none */
                         GCancellable     *cancellable,
                         GError          **error)
{
  if (!glnx_shutil_mkdir_p_at (AT_FDCWD,
                               dirname (strdupa (RPMOSTREE_AUTOUPDATES_CACHE_FILE)),
                               0775, cancellable, error))
    return FALSE;

  /* always delete first since we might not be replacing it at all */
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, RPMOSTREE_AUTOUPDATES_CACHE_FILE,
                             cancellable, error))
    return FALSE;

  g_autoptr(GVariant) update = NULL;
  if (!rpmostreed_update_generate_variant (booted_deployment, staged_deployment, repo, sack,
                                           &update, cancellable, error))
    return FALSE;

  if (update != NULL)
    {
      if (!glnx_file_replace_contents_at (AT_FDCWD, RPMOSTREE_AUTOUPDATES_CACHE_FILE,
                                          static_cast<const guint8*>(g_variant_get_data (update)),
                                          g_variant_get_size (update),
                                          static_cast<GLnxFileReplaceFlags>(0), cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* ============================= Package Diff  ============================= */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char *refspec;
  char *revision;
} PackageDiffTransaction;

typedef RpmostreedTransactionClass PackageDiffTransactionClass;

GType package_diff_transaction_get_type (void);

G_DEFINE_TYPE (PackageDiffTransaction,
               package_diff_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
package_diff_transaction_finalize (GObject *object)
{
  PackageDiffTransaction *self;

  self = (PackageDiffTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);
  g_free (self->revision);

  G_OBJECT_CLASS (package_diff_transaction_parent_class)->finalize (object);
}

static gboolean
package_diff_transaction_execute (RpmostreedTransaction *transaction,
                                  GCancellable *cancellable,
                                  GError **error)
{
  /* XXX: we should just unify this with deploy_transaction_execute to take advantage of the
   * new pkglist metadata when possible */

  PackageDiffTransaction *self = (PackageDiffTransaction *) transaction;
  int upgrader_flags = 0;

  if (self->revision != NULL || self->refspec != NULL)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;

  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, (RpmOstreeSysrootUpgraderFlags)upgrader_flags,
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin =
    rpmostree_sysroot_upgrader_dup_origin (upgrader);

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  /* Determine if we're upgrading before we set the refspec. */
  gboolean upgrading = (self->refspec == NULL && self->revision == NULL);

  if (self->refspec != NULL)
    {
      if (!change_origin_refspec (NULL, sysroot, origin, self->refspec,
                                  cancellable, NULL, NULL, error))
        return FALSE;
    }

  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision != NULL)
    {
      g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
      rpmostreed_transaction_connect_download_progress (transaction, progress);
      if (!apply_revision_override (transaction, repo, progress, origin, FALSE,
                                    self->revision, cancellable, error))
        return FALSE;
      rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));
    }
  else if (upgrading)
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  if (self->refspec != NULL)
    {
      rpmostree_output_message ("Updating from: %s", self->refspec);
    }

  gboolean changed = FALSE;
  { g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
    rpmostreed_transaction_connect_download_progress (transaction, progress);
    if (!rpmostree_sysroot_upgrader_pull_base (upgrader, "/usr/share/rpm",
                                               (OstreeRepoPullFlags)0, progress, &changed,
                                               cancellable, error))
      return FALSE;
    rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));
  }

  if (!changed)
    {
      if (upgrading)
        rpmostree_output_message ("No upgrade available.");
      else
        rpmostree_output_message ("No change.");
    }

  if (upgrading)
    {
      /* For backwards compatibility, we still need to make sure the CachedUpdate property
       * is updated here. To do this, we cache to disk in libostree mode (no DnfSack), since
       * that's all we updated here. This conflicts with auto-updates for now, though we
       * need better test coverage before uniting those two paths. */
      OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
      if (!generate_update_variant (repo, booted_deployment, NULL, NULL,
                                    cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static void
package_diff_transaction_class_init (PackageDiffTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = package_diff_transaction_finalize;

  clazz->execute = package_diff_transaction_execute;
}

static void
package_diff_transaction_init (PackageDiffTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_package_diff (GDBusMethodInvocation *invocation,
                                         OstreeSysroot *sysroot,
                                         const char *osname,
                                         const char *refspec,
                                         const char *revision,
                                         GCancellable *cancellable,
                                         GError **error)
{

  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (osname != NULL);

  auto self = (PackageDiffTransaction *)
     g_initable_new (package_diff_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
    }

  return (RpmostreedTransaction *) self;
}

/* =============================== Rollback =============================== */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean reboot;
} RollbackTransaction;

typedef RpmostreedTransactionClass RollbackTransactionClass;

GType rollback_transaction_get_type (void);

G_DEFINE_TYPE (RollbackTransaction,
               rollback_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
rollback_transaction_finalize (GObject *object)
{
  RollbackTransaction *self;

  self = (RollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (rollback_transaction_parent_class)->finalize (object);
}

static gboolean
rollback_transaction_execute (RpmostreedTransaction *transaction,
                              GCancellable *cancellable,
                              GError **error)
{
  RollbackTransaction *self = (RollbackTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, "rollback");

  g_autoptr(OstreeDeployment) pending_deployment = NULL;
  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  ostree_sysroot_query_deployments_for (sysroot, self->osname,
                                        &pending_deployment, &rollback_deployment);

  if (!rollback_deployment && !pending_deployment) /* i.e. do we just have 1 deployment? */
    return glnx_throw (error, "No rollback deployment found");
  /* rollback with a staged deployment doesn't make any sense -- error out with hint */
  else if (pending_deployment && ostree_deployment_is_staged (pending_deployment))
    return glnx_throw (error, "Staged deployment (remove with cleanup -p)");
  else if (!rollback_deployment)
    {

      /* If there isn't a rollback deployment, but there *is* a pending deployment, then we
       * want "rpm-ostree rollback" to put the currently booted deployment back on top. This
       * also allows users to effectively undo a rollback operation. */
      rollback_deployment = (OstreeDeployment*)g_object_ref (booted_deployment);
    }

  g_autoptr(GPtrArray) old_deployments =
    ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GPtrArray) new_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  /* build out the reordered array; rollback is first now */
  g_ptr_array_add (new_deployments, g_object_ref (rollback_deployment));

  rpmostree_output_message ("Moving '%s.%d' to be first deployment",
                            ostree_deployment_get_csum (rollback_deployment),
                            ostree_deployment_get_deployserial (rollback_deployment));

  for (guint i = 0; i < old_deployments->len; i++)
    {
      auto deployment = static_cast<OstreeDeployment *>(old_deployments->pdata[i]);
      if (!ostree_deployment_equal (deployment, rollback_deployment))
        g_ptr_array_add (new_deployments, g_object_ref (deployment));
    }

  /* if default changed write it */
  if (old_deployments->pdata[0] != new_deployments->pdata[0])
    {
      if (!ostree_sysroot_write_deployments (sysroot,
                                             new_deployments,
                                             cancellable,
                                             error))
        return FALSE;
    }

  if (self->reboot)
    {
      if (!check_sd_inhibitor_locks (cancellable, error))
        return FALSE;
      rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
    }

  return TRUE;
}

static void
rollback_transaction_class_init (RollbackTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = rollback_transaction_finalize;

  clazz->execute = rollback_transaction_execute;
}

static void
rollback_transaction_init (RollbackTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_rollback (GDBusMethodInvocation *invocation,
                                     OstreeSysroot *sysroot,
                                     const char *osname,
                                     gboolean reboot,
                                     GCancellable *cancellable,
                                     GError **error)
{

  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (osname != NULL);

  auto self = (RollbackTransaction *)
  g_initable_new (rollback_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ============================ UpdateDeployment ============================ */

typedef struct {
  RpmostreedTransaction parent;
  RpmOstreeTransactionDeployFlags flags;
  char   *osname;
  GVariantDict *options;
  GVariantDict *modifiers;
  char   *refspec; /* NULL for non-rebases */
  const char   *revision; /* NULL for upgrade; owned by @options */
  char  **install_pkgs; /* strv but strings owned by modifiers */
  GUnixFDList *install_local_pkgs;
  char  **uninstall_pkgs; /* strv but strings owned by modifiers */
  char  **enable_modules; /* strv but strings owned by modifiers */
  char  **disable_modules; /* strv but strings owned by modifiers */
  char  **install_modules; /* strv but strings owned by modifiers */
  char  **uninstall_modules; /* strv but strings owned by modifiers */
  char  **override_replace_pkgs; /* strv but strings owned by modifiers */
  GUnixFDList *override_replace_local_pkgs;
  char  **override_remove_pkgs; /* strv but strings owned by modifiers */
  char  **override_reset_pkgs; /* strv but strings owned by modifiers */
  int local_repo_remote_dfd;
} DeployTransaction;

typedef RpmostreedTransactionClass DeployTransactionClass;

GType deploy_transaction_get_type (void);

G_DEFINE_TYPE (DeployTransaction,
               deploy_transaction,
               RPMOSTREED_TYPE_TRANSACTION)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DeployTransaction, g_object_unref)

static void
deploy_transaction_finalize (GObject *object)
{
  DeployTransaction *self;

  self = (DeployTransaction *) object;
  g_free (self->osname);
  g_clear_pointer (&self->options, g_variant_dict_unref);
  g_clear_pointer (&self->modifiers, g_variant_dict_unref);
  g_free (self->refspec);
  g_free (self->install_pkgs);
  g_clear_pointer (&self->install_local_pkgs, g_object_unref);
  g_free (self->uninstall_pkgs);
  g_free (self->enable_modules);
  g_free (self->disable_modules);
  g_free (self->install_modules);
  g_free (self->uninstall_modules);
  g_free (self->override_replace_pkgs);
  g_clear_pointer (&self->override_replace_local_pkgs, g_object_unref);
  g_free (self->override_remove_pkgs);
  g_free (self->override_reset_pkgs);
  glnx_close_fd (&self->local_repo_remote_dfd);

  G_OBJECT_CLASS (deploy_transaction_parent_class)->finalize (object);
}

static gboolean
import_local_rpm (OstreeRepo    *repo,
                  int           *fd,
                  char         **sha256_nevra,
                  GCancellable  *cancellable,
                  GError       **error)
{
  /* let's just use the current sepolicy -- we'll just relabel it if the new
   * base turns out to have a different one */
  glnx_autofd int rootfs_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &rootfs_dfd, error))
    return FALSE;
  g_autoptr(OstreeSePolicy) policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
  if (policy == NULL)
    return FALSE;

  g_autoptr(RpmOstreeImporter) unpacker = rpmostree_importer_new_take_fd (fd, repo, NULL, static_cast<RpmOstreeImporterFlags>(0), policy, error);
  if (unpacker == NULL)
    return FALSE;

  if (!rpmostree_importer_run (unpacker, NULL, cancellable, error))
    return FALSE;

  g_autofree char *nevra = rpmostree_importer_get_nevra (unpacker);
  *sha256_nevra = g_strconcat (rpmostree_importer_get_header_sha256 (unpacker),
                               ":", nevra, NULL);

  return TRUE;
}

static void
ptr_close_fd (gpointer fdp)
{
  int fd = GPOINTER_TO_INT (fdp);
  glnx_close_fd (&fd);
}

/* GUnixFDList doesn't allow stealing individual members */
static GPtrArray *
unixfdlist_to_ptrarray (GUnixFDList *fdl)
{
  gint len;
  gint *fds = g_unix_fd_list_steal_fds (fdl, &len);
  GPtrArray *ret = g_ptr_array_new_with_free_func ((GDestroyNotify)ptr_close_fd);
  for (int i = 0; i < len; i++)
    g_ptr_array_add (ret, GINT_TO_POINTER (fds[i]));
  return ret;
}

static gboolean
import_many_local_rpms (OstreeRepo    *repo,
                        GUnixFDList   *fdl,
                        GPtrArray    **out_pkgs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  /* Note that we record the SHA-256 of the RPM header in the origin to make sure that e.g.
   * if we somehow re-import the same NEVRA with different content, we error out. We don't
   * record the checksum of the branch itself, because it may need relabeling and that's OK.
   * */

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  /* Note use of commit-on-failure */
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) pkgs = g_ptr_array_new_with_free_func (g_free);

  g_autoptr(GPtrArray) fds = unixfdlist_to_ptrarray (fdl);
  for (guint i = 0; i < fds->len; i++)
    {
      /* Steal fd from the ptrarray */
      glnx_autofd int fd = GPOINTER_TO_INT (fds->pdata[i]);
      fds->pdata[i] = GINT_TO_POINTER (-1);
      g_autofree char *sha256_nevra = NULL;
      /* Transfer fd to import */
      if (!import_local_rpm (repo, &fd, &sha256_nevra, cancellable, error))
        return FALSE;

      g_ptr_array_add (pkgs, util::move_nullify (sha256_nevra));
    }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  *out_pkgs = util::move_nullify (pkgs);
  return TRUE;
}

static void
gv_nevra_add_nevra_name_mappings (GVariant *gv_nevra,
                                  GHashTable *name_to_nevra,
                                  GHashTable *nevra_to_name)
{
  const char *name = NULL;
  const char *nevra = NULL;
  g_variant_get_child (gv_nevra, 0, "&s", &nevra);
  g_variant_get_child (gv_nevra, 1, "&s", &name);
  g_hash_table_insert (name_to_nevra, (gpointer)name, (gpointer)nevra);
  g_hash_table_insert (nevra_to_name, (gpointer)nevra, (gpointer)name);
}

static gboolean
get_sack_for_booted (OstreeSysroot    *sysroot,
                     OstreeRepo       *repo,
                     OstreeDeployment *booted_deployment,
                     DnfSack         **out_sack,
                     GCancellable     *cancellable,
                     GError          **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading sack", error);

  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_client (repo);

  g_autofree char *source_root =
    rpmostree_get_deployment_root (sysroot, booted_deployment);
  if (!rpmostree_context_setup (ctx, NULL, source_root, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);

  /* we always want to force a refetch of the metadata */
  rpmostree_context_set_dnf_caching (ctx, RPMOSTREE_CONTEXT_DNF_CACHE_NEVER);

  /* point libdnf to our repos dir */
  rpmostree_context_configure_from_deployment (ctx, sysroot, booted_deployment);

  /* streamline: we don't need rpmdb or filelists, but we *do* need updateinfo */
  if (!rpmostree_context_download_metadata (ctx,
        (DnfContextSetupSackFlags)(DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_RPMDB |
        DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_FILELISTS |
        DNF_CONTEXT_SETUP_SACK_FLAG_LOAD_UPDATEINFO), cancellable, error))
    return FALSE;

  *out_sack = (DnfSack*)g_object_ref (dnf_context_get_sack (dnfctx));
  return TRUE;
}

static gboolean
deploy_has_bool_option (DeployTransaction *self,
                        const char        *option)
{
  return vardict_lookup_bool (self->options, option, FALSE);
}

static const char *
deploy_has_string_option (DeployTransaction *self,
                          const char        *option)
{
  return static_cast<const char*>(vardict_lookup_ptr (self->options, option, "s"));
}

/* Write a state file which records information about the agent that is "driving" updates */
static gboolean
record_driver_info (RpmostreedTransaction *transaction,
                    const gchar           *update_driver,
                    GCancellable          *cancellable,
                    GError               **error)
{
  g_auto(GVariantBuilder) caller_info_builder;
  g_variant_builder_init (&caller_info_builder, G_VARIANT_TYPE ("a{sv}"));

  const char *sd_unit = rpmostreed_transaction_get_sd_unit (transaction);
  if (!update_driver)
    return glnx_throw (error, "update driver name not provided");
  g_variant_builder_add (&caller_info_builder, "{sv}", RPMOSTREE_DRIVER_NAME,
                         g_variant_new_string (update_driver));
  if (!sd_unit)
    return glnx_throw (error, "could not find caller systemd unit");
  g_variant_builder_add (&caller_info_builder, "{sv}", RPMOSTREE_DRIVER_SD_UNIT,
                         g_variant_new_string (sd_unit));

  g_autoptr(GVariant) driver_info =
    g_variant_ref_sink (g_variant_builder_end (&caller_info_builder));

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, RPMOSTREE_RUN_DIR, 0755, cancellable, error))
    return FALSE;
  if (!glnx_file_replace_contents_at (AT_FDCWD, RPMOSTREE_DRIVER_STATE,
                                      static_cast<const guint8*>(g_variant_get_data (driver_info)),
                                      g_variant_get_size (driver_info),
                                      static_cast<GLnxFileReplaceFlags>(0), cancellable, error))
    return FALSE;

  return TRUE;
}

/* Sets `driver_info` if driver state file is found, leave as NULL otherwise. */
gboolean
get_driver_g_variant (GVariant **driver_info,
                      GError   **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GError) local_error = NULL;
  if (!glnx_openat_rdonly (AT_FDCWD, RPMOSTREE_DRIVER_STATE, TRUE, &fd,
                           &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return g_propagate_error (error, util::move_nullify (local_error)), FALSE;
      // Don't propagate error if state file not found; just return early.
      return TRUE;
    }

  g_autoptr(GBytes) data = glnx_fd_readall_bytes (fd, NULL, error);
  if (!data)
    return FALSE;

  *driver_info =
    g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT, data, FALSE));

  return TRUE;
}

/* Read from state file that records information about the agent that is "driving" updates */
gboolean
get_driver_info (char   **name,
                 char   **sd_unit,
                 GError **error)
{
  g_autoptr(GVariant) driver_info = NULL;
  if (!get_driver_g_variant (&driver_info, error))
    return FALSE;
  if (!driver_info)
    return TRUE; // driver state file not found, return early.

  g_auto(GVariantDict) driver_info_dict;
  g_variant_dict_init (&driver_info_dict, driver_info);
  auto v = static_cast<char*>(vardict_lookup_ptr (&driver_info_dict,
                                                  RPMOSTREE_DRIVER_NAME, "s"));
  if (!v)
    return glnx_throw (error, "could not find update driver name in %s",
                       RPMOSTREE_DRIVER_STATE);
  *name = v;
  v = static_cast<char*>(vardict_lookup_ptr (&driver_info_dict,
                                             RPMOSTREE_DRIVER_SD_UNIT, "s"));
  if (!v)
    return glnx_throw (error, "could not find update driver systemd unit in %s",
                       RPMOSTREE_DRIVER_STATE);
  *sd_unit = v;

  return TRUE;
}

static gboolean
deploy_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  DeployTransaction *self = (DeployTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  const gboolean dry_run =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN) > 0);
  const gboolean no_overrides = deploy_has_bool_option (self, "no-overrides");
  const gboolean no_layering = deploy_has_bool_option (self, "no-layering");
  const gboolean no_initramfs = deploy_has_bool_option (self, "no-initramfs");
  const gboolean cache_only = deploy_has_bool_option (self, "cache-only");
  const gboolean idempotent_layering = deploy_has_bool_option (self, "idempotent-layering");
  const gboolean download_only =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY) > 0);
  /* Mainly for the `install`, `module install`, and `override` commands */
  const gboolean no_pull_base =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE) > 0);
  /* Used to background check for updates; this essentially means downloading the minimum
   * amount of metadata only to check if there's an upgrade */
  const gboolean download_metadata_only =
    ((self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_METADATA_ONLY) > 0);
  const gboolean allow_inactive = deploy_has_bool_option (self, "allow-inactive");
  g_autofree const char *update_driver = deploy_has_string_option (self, "register-driver");

  gboolean is_install = FALSE;
  gboolean is_uninstall = FALSE;
  gboolean is_override = FALSE;

  /* In practice today */
  if (no_pull_base)
    {
      /* this is a heuristic; by the end, once the proper switches are added, the two
       * commands can look indistinguishable at the D-Bus level */
      is_override = (self->override_reset_pkgs ||
                     self->override_remove_pkgs ||
                     self->override_replace_pkgs ||
                     self->override_replace_local_pkgs ||
                     no_overrides);
      if (!is_override)
        {
          if (self->install_pkgs || self->install_local_pkgs || self->install_modules)
            is_install = TRUE;
          else
            is_uninstall = TRUE;
        }
    }

  auto command_line = (const char *)
    vardict_lookup_ptr (self->options, "initiating-command-line", "&s");

  /* If we're not actively holding back pulling a new update and we're staying on the same
   * ref, then by definition we're upgrading. */
  const gboolean is_upgrade = (!no_pull_base && !self->refspec && !self->revision);

  /* Now set the transaction title before doing any work.
   * https://github.com/projectatomic/rpm-ostree/issues/454 */
  if (command_line)
    {
      /* special-case the automatic one, otherwise just use verbatim as title */
      const char *title = command_line;
      if (strstr (command_line, "--trigger-automatic-update-policy"))
        title = download_metadata_only ? "automatic (check)" : "automatic (stage)";
      rpmostree_transaction_set_title (RPMOSTREE_TRANSACTION (transaction), title);
    }
  else
    {
      g_autoptr(GString) txn_title = g_string_new ("");
      if (is_install)
        g_string_append (txn_title, "install");
      else if (is_uninstall)
        g_string_append (txn_title, "uninstall");
      else if (is_override)
        g_string_append (txn_title, "override");
      else if (self->refspec)
        g_string_append (txn_title, "rebase");
      else if (self->revision)
        g_string_append (txn_title, "deploy");
      else
        g_string_append (txn_title, "upgrade");

      /* so users know we were probably fired by the automated timer when looking at status */
      if (cache_only)
        g_string_append (txn_title, " (cache only)");
      else if (download_metadata_only)
        g_string_append (txn_title, " (check only)");
      else if (download_only)
        g_string_append (txn_title, " (download only)");

      if (self->uninstall_pkgs)
        g_string_append_printf (txn_title, "; uninstall: %u",
                                g_strv_length (self->uninstall_pkgs));
      if (self->disable_modules)
        g_string_append_printf (txn_title, "; module disable: %u",
                                g_strv_length (self->disable_modules));
      if (self->uninstall_modules)
        g_string_append_printf (txn_title, "; module uninstall: %u",
                                g_strv_length (self->uninstall_modules));
      if (self->install_pkgs)
        g_string_append_printf (txn_title, "; install: %u",
                                g_strv_length (self->install_pkgs));
      if (self->install_local_pkgs)
        g_string_append_printf (txn_title, "; localinstall: %u",
                                g_unix_fd_list_get_length (self->install_local_pkgs));
      if (self->enable_modules)
        g_string_append_printf (txn_title, "; module enable: %u",
                                g_strv_length (self->enable_modules));
      if (self->install_modules)
        g_string_append_printf (txn_title, "; module install: %u",
                                g_strv_length (self->install_modules));

      rpmostree_transaction_set_title (RPMOSTREE_TRANSACTION (transaction), txn_title->str);
    }

  if (update_driver)
    {
      if (!record_driver_info (transaction, update_driver, cancellable, error))
        return FALSE;
      /* If revision is an empty string, we interpret this to mean that the invocation
       * was called solely for the purpose of registering the update driver. Exit early without
       * doing any further work. */
      if (self->revision && self->revision[0] == '\0')
        {
          rpmostree_output_message ("Empty string revision found; registering update driver only");
          return TRUE;
        }
    }

  int upgrader_flags = 0;
  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;
  if (dry_run)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN;
  if (deploy_has_bool_option (self, "lock-finalization"))
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION;

  /* DOWNLOAD_METADATA_ONLY isn't directly exposed at the D-Bus API level, so we shouldn't
   * ever run into these conflicting options */
  if (download_metadata_only)
    g_assert (!(no_pull_base || cache_only || download_only));

  if (cache_only)
    {
      /* practically, we could unite those two into a single flag, though it's nice to be
       * able to keep them separate as well */

      /* don't pull, just resolve ref locally and timestamp check */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL;
      /* turn on rpmmd cache only in the upgrader */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY;
    }

  if (no_overrides)
    {
      g_assert (self->override_replace_pkgs == NULL);
      g_assert (self->override_replace_local_pkgs == NULL);
      g_assert (self->override_remove_pkgs == NULL);
      g_assert (self->override_reset_pkgs == NULL);
    }

  if (self->refspec)
    {
      /* When rebasing, we should be able to switch to a different tree even if
       * the current origin is unconfigured */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED;
    }

  /* before doing any real work, let's make sure the pkgcache is migrated */
  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  if (!rpmostree_migrate_pkgcache_repo (repo, cancellable, error))
    return FALSE;

  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, (RpmOstreeSysrootUpgraderFlags)upgrader_flags,
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin =
    rpmostree_sysroot_upgrader_dup_origin (upgrader);

  /* Handle local repo remotes immediately; the idea is that the remote is "transient"
   * (otherwise, one should set up a proper file:/// remote), so we only support rebasing to
   * a checksum. We don't want to import a ref. */
  if (self->local_repo_remote_dfd != -1)
    {
      /* self->refspec is the rev in the other local repo we'll rebase to */
      g_assert (self->refspec);

      RpmOstreeRefspecType refspectype;
      const char *ref;
      if (!rpmostree_refspec_classify (self->refspec, &refspectype, &ref, error))
        return FALSE;

      g_autoptr(OstreeRepo) local_repo_remote =
        ostree_repo_open_at (self->local_repo_remote_dfd, ".", cancellable, error);
      if (!local_repo_remote)
        return glnx_prefix_error (error, "Failed to open local repo");
      g_autofree char *rev = NULL;
      if (!ostree_repo_resolve_rev (local_repo_remote, ref, FALSE, &rev, error))
        return FALSE;

      g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
      rpmostreed_transaction_connect_download_progress (transaction, progress);

      /* pull-local into the system repo */
      const char *refs_to_fetch[] = { rev, NULL };
      g_autofree char *local_repo_uri =
        g_strdup_printf ("file:///proc/self/fd/%d", self->local_repo_remote_dfd);
      if (!ostree_repo_pull (repo, local_repo_uri, (char**)refs_to_fetch,
                             OSTREE_REPO_PULL_FLAGS_NONE, progress, cancellable, error))
        return glnx_prefix_error (error, "Pulling commit %s from local repo", rev);
      ostree_async_progress_finish (progress);
      rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

      /* as far as the rest of the code is concerned, we're rebasing to :SHA256 now */
      g_clear_pointer (&self->refspec, g_free);
      self->refspec = g_strdup_printf (":%s", rev);
      glnx_close_fd (&self->local_repo_remote_dfd);
    }

  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;
  if (self->refspec)
    {
      if (!change_origin_refspec (self->options, sysroot, origin, self->refspec, cancellable,
                                  &old_refspec, &new_refspec, error))
        return FALSE;
    }

  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision)
    {
      g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
      rpmostreed_transaction_connect_download_progress (transaction, progress);
      if (!apply_revision_override (transaction, repo, progress, origin,
                                    deploy_has_bool_option (self, "skip-branch-check"),
                                    self->revision, cancellable, error))
        return FALSE;
      rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));
    }
  else
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  gboolean changed = FALSE;
  GHashTable *initrd_etc_files = rpmostree_origin_get_initramfs_etc_files (origin);
  if (no_initramfs && (rpmostree_origin_get_regenerate_initramfs (origin) ||
                       g_hash_table_size (initrd_etc_files) > 0))
    {
      rpmostree_origin_set_regenerate_initramfs (origin, FALSE, NULL);
      rpmostree_origin_initramfs_etc_files_untrack_all (origin, NULL);
      changed = TRUE;
    }

  // Handle the --ex-cliwrap option
  {
    gboolean cliwrap = FALSE;
    if (g_variant_dict_lookup (self->options, "ex-cliwrap", "b", &cliwrap))
      {
        rpmostree_origin_set_cliwrap (origin, cliwrap);
        changed = TRUE;
      }
  }

  gboolean remove_changed = FALSE;
  if (no_layering)
    {
      if (!rpmostree_origin_remove_all_packages (origin, &remove_changed, error))
        return FALSE;
    }
  else
    {
      gboolean local_changed = FALSE;
      if (self->uninstall_pkgs)
        {
          if (!rpmostree_origin_remove_packages (origin, self->uninstall_pkgs,
                                                 idempotent_layering, &local_changed, error))
            return FALSE;
        }
      remove_changed = remove_changed || local_changed;
      if (self->disable_modules)
        {
          if (!rpmostree_origin_remove_modules (origin, self->disable_modules,
                                                TRUE, &local_changed, error))
            return FALSE;
        }
      remove_changed = remove_changed || local_changed;
      if (self->uninstall_modules)
        {
          if (!rpmostree_origin_remove_modules (origin, self->uninstall_modules,
                                                FALSE, &local_changed, error))
            return FALSE;
        }
      remove_changed = remove_changed || local_changed;
    }

  /* In reality, there may not be any new layer required even if `remove_changed` is TRUE
   * (if e.g. we're removing a duplicate provides). But the origin has changed so we need to
   * create a new deployment; see https://github.com/projectatomic/rpm-ostree/issues/753 */
  changed = changed || remove_changed;

  /* lazily loaded cache that's used in a few conditional blocks */
  g_autoptr(RpmOstreeRefSack) base_rsack = NULL;

  if (self->install_pkgs)
    {
      /* we run a special check here; let's just not allow trying to install a pkg that will
       * right away become inactive because it's already installed */

      if (!base_rsack)
        {
          const char *base = rpmostree_sysroot_upgrader_get_base (upgrader);
          base_rsack = rpmostree_get_refsack_for_commit (repo, base, cancellable, error);
          if (base_rsack == NULL)
            return FALSE;
        }

      for (char **it = self->install_pkgs; it && *it; it++)
        {
          const char *pkg = *it;
          g_autoptr(GPtrArray) pkgs =
            rpmostree_get_matching_packages (base_rsack->sack, pkg);
          if (pkgs->len > 0 && !allow_inactive)
            {
              g_autoptr(GString) pkgnames = g_string_new ("");
              for (guint i = 0; i < pkgs->len; i++)
                {
                  auto p = static_cast<DnfPackage *>(pkgs->pdata[i]);
                  g_string_append_printf (pkgnames, " %s", dnf_package_get_nevra (p));
                }
              return glnx_throw (error, "\"%s\" is already provided by:%s. Use "
                                        "--allow-inactive to explicitly "
                                        "require it.", pkg, pkgnames->str);
            }
        }

      gboolean add_changed = FALSE;
      if (!rpmostree_origin_add_packages (origin, self->install_pkgs, FALSE,
                                          idempotent_layering, &add_changed, error))
        return FALSE;

      changed = changed || add_changed;
    }

  if (self->enable_modules)
    {
      gboolean add_changed = FALSE;
      if (!rpmostree_origin_add_modules (origin, self->enable_modules, TRUE, &add_changed, error))
        return FALSE;

      changed = changed || add_changed;
    }

  if (self->install_modules)
    {
      gboolean add_changed = FALSE;
      if (!rpmostree_origin_add_modules (origin, self->install_modules, FALSE, &add_changed, error))
        return FALSE;

      changed = changed || add_changed;
    }

  if (self->install_local_pkgs != NULL)
    {
      g_autoptr(GPtrArray) pkgs = NULL;
      if (!import_many_local_rpms (repo, self->install_local_pkgs, &pkgs,
                                   cancellable, error))
        return FALSE;

      if (pkgs->len > 0)
        {
          g_ptr_array_add (pkgs, NULL);

          gboolean add_changed = FALSE;
          if (!rpmostree_origin_add_packages (origin, (char**)pkgs->pdata, TRUE,
                                              idempotent_layering, &add_changed, error))
            return FALSE;

          changed = changed || add_changed;
        }
    }

  if (no_overrides)
    {
      gboolean overrides_changed = FALSE;
      if (!rpmostree_origin_remove_all_overrides (origin, &overrides_changed, error))
        return FALSE;

      changed = changed || overrides_changed;
    }
  else if (self->override_reset_pkgs || self->override_replace_local_pkgs)
    {
      /* The origin stores removal overrides as pkgnames and replacement overrides as nevra.
       * To be nice, we support both name & nevra and do the translation here by just
       * looking at the commit metadata. */
      OstreeDeployment *merge_deployment =
        rpmostree_sysroot_upgrader_get_merge_deployment (upgrader);

      g_autoptr(GVariant) removed = NULL;
      g_autoptr(GVariant) replaced = NULL;
      if (!rpmostree_deployment_get_layered_info (repo, merge_deployment, NULL, NULL, NULL,
                                                  NULL, NULL, &removed, &replaced, error))
        return FALSE;

      g_autoptr(GHashTable) nevra_to_name = g_hash_table_new (g_str_hash, g_str_equal);
      g_autoptr(GHashTable) name_to_nevra = g_hash_table_new (g_str_hash, g_str_equal);

      /* keep a reference on the child nevras so that the hash tables above can directly
       * reference strings within them */
      g_autoptr(GPtrArray) gv_nevras =
        g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

      const guint nremoved = g_variant_n_children (removed);
      for (guint i = 0; i < nremoved; i++)
        {
          g_autoptr(GVariant) gv_nevra;
          g_variant_get_child (removed, i, "v", &gv_nevra);
          gv_nevra_add_nevra_name_mappings (gv_nevra, name_to_nevra, nevra_to_name);
          g_ptr_array_add (gv_nevras, util::move_nullify (gv_nevra));
        }

      const guint nreplaced = g_variant_n_children (replaced);
      for (guint i = 0; i < nreplaced; i++)
        {
          g_autoptr(GVariant) gv_nevra;
          g_variant_get_child (replaced, i, "(vv)", &gv_nevra, NULL);
          gv_nevra_add_nevra_name_mappings (gv_nevra, name_to_nevra, nevra_to_name);
          g_ptr_array_add (gv_nevras, util::move_nullify (gv_nevra));
        }

      /* and also add mappings from the origin replacements to handle inactive overrides */
      GHashTable *cur_local_overrides = rpmostree_origin_get_overrides_local_replace (origin);
      g_autoptr(GPtrArray) names_to_free = g_ptr_array_new_with_free_func (g_free); /* yuck */
      GLNX_HASH_TABLE_FOREACH (cur_local_overrides, const char*, nevra)
        {
          g_autofree char *name = NULL;
          if (!rpmostree_decompose_nevra (nevra, &name, NULL, NULL, NULL, NULL, error))
            return FALSE;

          g_hash_table_insert (name_to_nevra, (gpointer)name, (gpointer)nevra);
          g_hash_table_insert (nevra_to_name, (gpointer)nevra, (gpointer)name);
          g_ptr_array_add (names_to_free, g_steal_pointer (&name));
        }

      for (char **it = self->override_reset_pkgs; it && *it; it++)
        {
          const char *name_or_nevra = *it;
          auto name = static_cast<const char *>(g_hash_table_lookup (nevra_to_name, name_or_nevra));
          auto nevra = static_cast<const char *>(g_hash_table_lookup (name_to_nevra, name_or_nevra));

          if (name == NULL && nevra == NULL)
            {
              /* this is going to fail below because we should've covered all
               * cases above, but just try both ways anyway */
              name = name_or_nevra;
              nevra = name_or_nevra;
            }
          else if (name == NULL)
            name = name_or_nevra;
          else if (nevra == NULL)
            nevra = name_or_nevra;
          else
            {
              /* completely brush over the ridiculous corner-case of a
                 pkgname that's also a nevra for another package */
              g_assert_not_reached ();
            }

          if (rpmostree_origin_remove_override (origin, name,
                                                RPMOSTREE_ORIGIN_OVERRIDE_REMOVE))
            continue; /* override found; move on to the next one */

          if (rpmostree_origin_remove_override (origin, nevra,
                                                RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL))
            continue; /* override found; move on to the next one */

          return glnx_throw (error, "No overrides for package '%s'", name_or_nevra);
        }

      if (self->override_replace_local_pkgs)
        {
          g_autoptr(GPtrArray) pkgs = NULL;
          if (!import_many_local_rpms (repo, self->override_replace_local_pkgs, &pkgs,
                                       cancellable, error))
            return FALSE;

          for (guint i = 0; i < pkgs->len; i++)
            {

              auto *pkg = static_cast<const char *> (g_ptr_array_index(pkgs,i));
              g_autofree char *name = NULL;
              g_autofree char *sha256 = NULL;

              if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
                return FALSE;

              if (!rpmostree_decompose_nevra (pkg, &name, NULL, NULL, NULL, NULL, error))
                return FALSE;

              auto nevra = static_cast<const char *> (g_hash_table_lookup (name_to_nevra, name));

              if (nevra)
                rpmostree_origin_remove_override (origin, nevra,
                                                  RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL);
            }
          if (pkgs->len > 0)
            {
              g_ptr_array_add (pkgs, NULL);
              if (!rpmostree_origin_add_overrides (origin, (char**)pkgs->pdata,
                                                   RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL,
                                                   error))
                return FALSE;
            }
        }
      changed = TRUE;
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  if (!no_pull_base)
    {
      gboolean base_changed;

      int flags = OSTREE_REPO_PULL_FLAGS_NONE;
      if (download_metadata_only)
        flags |= OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY;

      g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
      rpmostreed_transaction_connect_download_progress (transaction, progress);
      if (!rpmostree_sysroot_upgrader_pull_base (upgrader, NULL, (OstreeRepoPullFlags)flags, progress,
                                                 &base_changed, cancellable, error))
        return FALSE;
      rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

      if (base_changed)
        changed = TRUE;
      else
        {
          /* If we're on a live deployment, then allow redeploying a clean version of the
           * same base commit. This is useful if e.g. the pushed rollback was cleaned up. */

          OstreeDeployment *deployment =
            rpmostree_sysroot_upgrader_get_merge_deployment (upgrader);

          auto is_live = rpmostreecxx::has_live_apply_state(*sysroot, *deployment);
          if (is_live)
            changed = TRUE;
        }
    }

  /* let's figure out if those new overrides are valid and if so, canonicalize
   * them -- we could have just pulled the rpmdb dir before to do this, and then
   * do the full pull afterwards, though that would complicate the pull code and
   * anyway in the common case even if there's an error with the overrides,
   * users will fix it and try again, so the second pull will be a no-op */

  if (self->override_remove_pkgs)
    {
      if (!base_rsack)
        {
          const char *base = rpmostree_sysroot_upgrader_get_base (upgrader);
          base_rsack = rpmostree_get_refsack_for_commit (repo, base, cancellable, error);
          if (base_rsack == NULL)
            return FALSE;
        }

      /* NB: the strings are owned by the sack pool */
      g_autoptr(GPtrArray) pkgnames = g_ptr_array_new ();
      for (char **it = self->override_remove_pkgs; it && *it; it++)
        {
          const char *pkg = *it;
          g_autoptr(GPtrArray) pkgs =
            rpmostree_get_matching_packages (base_rsack->sack, pkg);

          if (pkgs->len == 0)
            return glnx_throw (error, "Package \"%s\" not found", pkg);

          /* either the subject was somehow too broad, or it's one of the rare
           * packages that supports installonly (e.g. kernel, though that one
           * specifically should never have multiple instances in a compose),
           * which you'd never want to remove */
          if (pkgs->len > 1)
            return glnx_throw (error, "Multiple packages match \"%s\"", pkg);

          /* canonicalize to just the pkg name */
          const char *pkgname = dnf_package_get_name (static_cast<DnfPackage*>(pkgs->pdata[0]));
          g_ptr_array_add (pkgnames, (void*)pkgname);
        }

      g_ptr_array_add (pkgnames, NULL);
      if (!rpmostree_origin_add_overrides (origin, (char**)pkgnames->pdata,
                                           RPMOSTREE_ORIGIN_OVERRIDE_REMOVE, error))
        return FALSE;

      rpmostree_sysroot_upgrader_set_origin (upgrader, origin);
      changed = TRUE;
    }

  /* Past this point we've computed the origin */
  RpmOstreeRefspecType refspec_type;
  rpmostree_origin_classify_refspec (origin, &refspec_type, NULL);

  if (download_metadata_only)
    {
      /* We have to short-circuit the usual path here; we already downloaded the ostree
       * metadata, so now we just need to update the rpmmd data (but only if we actually
       * have pkgs layered). This is still just a heuristic, since e.g. an InactiveRequest
       * may in fact become active in the new base, but we don't have the full tree. */

      /* Note here that we use the booted deployment for releasever: the download metadata
       * only path is currently used only by the auto-update checker, and there we always
       * want to show updates/vulnerabilities relative to the *booted* releasever. */
      OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

      /* this is checked in AutomaticUpdateTrigger, but check here too to be safe */
      if (!booted_deployment ||
          !g_str_equal (self->osname, ostree_deployment_get_osname (booted_deployment)))
        return glnx_throw (error, "Refusing to download rpm-md for offline OS '%s'",
                           self->osname);

      g_autoptr(DnfSack) sack = NULL;
      if (g_hash_table_size (rpmostree_origin_get_packages (origin)) > 0)
        {
          if (!get_sack_for_booted (sysroot, repo, booted_deployment, &sack,
                                    cancellable, error))
            return FALSE;
        }

      if (!generate_update_variant (repo, booted_deployment, NULL, sack,
                                    cancellable, error))
        return FALSE;

      /* Note early return */
      return TRUE;
    }

  RpmOstreeSysrootUpgraderLayeringType layering_type;
  gboolean layering_changed = FALSE;
  if (!rpmostree_sysroot_upgrader_prep_layering (upgrader, &layering_type, &layering_changed,
                                                 cancellable, error))
    return FALSE;
  changed = changed || layering_changed;

  if (dry_run)
    /* Note early return here; we printed the transaction already */
    return TRUE;

  if (layering_changed)
    {
      if (!rpmostree_sysroot_upgrader_import_pkgs (upgrader, cancellable, error))
        return FALSE;
    }

  rpmostree_sysroot_upgrader_set_caller_info (upgrader, command_line, 
                                              rpmostreed_transaction_get_agent_id (RPMOSTREED_TRANSACTION(self)),
                                              rpmostreed_transaction_get_sd_unit (RPMOSTREED_TRANSACTION(self)));

  /* TODO - better logic for "changed" based on deployments */
  if (changed || self->refspec)
    {
      /* Note early return; we stop short of actually writing the deployment */
      if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY)
        {
          /* XXX: improve msg here; e.g. cache will be blown on next operation? */
          if (changed)
            rpmostree_output_message ("Update downloaded.");
          else
            rpmostree_output_message ("No changes.");
          return TRUE;
        }

      g_autoptr(OstreeDeployment) new_deployment = NULL;
      if (!rpmostree_sysroot_upgrader_deploy (upgrader, &new_deployment,
                                              cancellable, error))
        return FALSE;

      /* Are we rebasing?  May want to delete the previous ref */
      if (self->refspec && !(deploy_has_bool_option (self, "skip-purge")))
        {
          g_autofree char *remote = NULL;
          g_autofree char *ref = NULL;

          /* The actual rebase has already succeeded, so ignore errors. */
          if (ostree_parse_refspec (old_refspec, &remote, &ref, NULL))
            {
              /* Note: In some cases the source origin ref may not actually
               * exist; say the admin did a cleanup, or the OS expects post-
               * install configuration like subscription-manager. */
              (void) ostree_repo_set_ref_immediate (repo, remote, ref, NULL,
                                                    cancellable, NULL);
            }
        }

      /* Always write out an update variant on vanilla upgrades since it's clearly the most
       * up to date. If autoupdates "check" mode is enabled, the *next* run might yet
       * overwrite it again because we always diff against the booted deployment. */
      if (is_upgrade)
        {
          OstreeDeployment *booted_deployment =
            ostree_sysroot_get_booted_deployment (sysroot);

          DnfSack *sack = rpmostree_sysroot_upgrader_get_sack (upgrader, error);
          if (!generate_update_variant (repo, booted_deployment, new_deployment, sack,
                                        cancellable, error))
            return FALSE;
        }

      if (deploy_has_bool_option (self, "reboot"))
        {
          if (!check_sd_inhibitor_locks (cancellable, error))
            return FALSE;
          rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
        }
    }
  else
    {
      if (refspec_type == RPMOSTREE_REFSPEC_TYPE_CHECKSUM
          && layering_type < RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS)
        {
          g_autofree char *custom_origin_url = NULL;
          g_autofree char *custom_origin_description = NULL;
          rpmostree_origin_get_custom_description (origin, &custom_origin_url,
                                                   &custom_origin_description);
          if (custom_origin_description)
            rpmostree_output_message ("Pinned to commit by custom origin: %s", custom_origin_description);
          else
            rpmostree_output_message ("Pinned to commit; no upgrade available");
        }
      else if (is_upgrade)
        rpmostree_output_message ("No upgrade available.");
      else
        rpmostree_output_message ("No change.");
    }

  return TRUE;
}

static void
deploy_transaction_class_init (DeployTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = deploy_transaction_finalize;

  clazz->execute = deploy_transaction_execute;
}

static void
deploy_transaction_init (DeployTransaction *self)
{
  self->local_repo_remote_dfd = -1;
}

static char **
strdupv_canonicalize (const char *const *strv)
{
  if (strv && *strv)
    return g_strdupv ((char**)strv);
  return NULL;
}

static gboolean
vardict_lookup_bool (GVariantDict *dict,
                     const char   *key,
                     gboolean      dfault)
{
  gboolean val;
  if (g_variant_dict_lookup (dict, key, "b", &val))
    return val;
  return dfault;
}

static inline void*
vardict_lookup_ptr (GVariantDict  *dict,
                    const char    *key,
                    const char    *fmt)
{
  void *val;
  if (g_variant_dict_lookup (dict, key, fmt, &val))
    return val;
  return NULL;
}

/* Look up a strv, but canonicalize the zero-length
 * array to NULL.
 */
static inline char **
vardict_lookup_strv_canonical (GVariantDict  *dict,
                               const char    *key)
{
  auto v = (char**)vardict_lookup_ptr (dict, key, "^a&s");
  if (!v)
    return NULL;
  if (v && !*v)
    {
      g_free (v);
      return NULL;
    }
  return v;
}

static inline RpmOstreeTransactionDeployFlags
set_deploy_flag (RpmOstreeTransactionDeployFlags flags,
                 RpmOstreeTransactionDeployFlags flag,
                 gboolean val)
{
  /* first, make sure it's cleared */
  flags = static_cast<RpmOstreeTransactionDeployFlags>(flags & ~flag);
  if (val)
    flags = static_cast<RpmOstreeTransactionDeployFlags>(flags | flag);
  return flags;
}

/* @defaults contains some default flags. They only take effect if the vardict option they
 * correspond to wasn't specified. */
static RpmOstreeTransactionDeployFlags
deploy_flags_from_options (GVariantDict *dict,
                           RpmOstreeTransactionDeployFlags defaults)
{
  RpmOstreeTransactionDeployFlags ret = defaults;
  gboolean val;
  if (g_variant_dict_lookup (dict, "allow-downgrade", "b", &val))
    ret = set_deploy_flag (ret, RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE, val);
  if (g_variant_dict_lookup (dict, "no-pull-base", "b", &val))
    ret = set_deploy_flag (ret, RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE, val);
  if (g_variant_dict_lookup (dict, "dry-run", "b", &val))
    ret = set_deploy_flag (ret, RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN, val);
  if (g_variant_dict_lookup (dict, "download-only", "b", &val))
    ret = set_deploy_flag (ret, RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY, val);
  return ret;
}

static gint*
get_fd_array_from_sparse (gint     *fds,
                          gint      nfds,
                          GVariant *idxs)
{
  const guint n = g_variant_n_children (idxs);
  gint *new_fds = g_new0 (gint, n+1);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(GVariant) hv = g_variant_get_child_value (idxs, i);
      g_assert (hv);
      gint32 h = g_variant_get_handle (hv);
      g_assert (0 <= h && h < nfds);
      new_fds[i] = fds[h];
    }

  new_fds[n] = -1;
  return new_fds;
}

RpmostreedTransaction *
rpmostreed_transaction_new_deploy (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   const char             *osname,
                                   RpmOstreeTransactionDeployFlags default_flags,
                                   GVariant               *options,
                                   RpmOstreeUpdateDeploymentModifiers *modifiers,
                                   GUnixFDList            *fd_list,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (osname != NULL);

  /* Parse this one early as it's used by an object property */
  g_autoptr(GVariantDict) options_dict = g_variant_dict_new (options);

  const gboolean output_to_self =
    vardict_lookup_bool (options_dict, "output-to-self", FALSE);

  g_autoptr(DeployTransaction) self = (DeployTransaction*)
    g_initable_new (deploy_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    "output-to-self", output_to_self,
                    NULL);
  if (!self)
    return NULL;

  /* Now we do further validation and parsing; the "GObject way" would be to
   * pass all of these as GObject properties, but that's tedious and painful for
   * no value, so we just do further error checking and discard the partially
   * constructed object on failure.
   */

  self->osname = g_strdup (osname);
  self->options = g_variant_dict_ref (options_dict);
  self->modifiers = g_variant_dict_new (modifiers);

  self->flags = deploy_flags_from_options (self->options, default_flags);

  auto refspec = (const char *)vardict_lookup_ptr (self->modifiers, "set-refspec", "&s");
  /* Canonicalize here; the later code actually ends up peeling it
   * again, but long term we want to manipulate canonicalized refspecs
   * internally, and only peel when writing origin files for ostree:// types.
   */
  if (refspec)
    {
      self->refspec = rpmostree_refspec_canonicalize (refspec, error);
      if (!self->refspec)
        return NULL;
    }
  const gboolean refspec_or_revision = (self->refspec != NULL || self->revision != NULL);

  self->revision = (char*)vardict_lookup_ptr (self->modifiers, "set-revision", "&s");
  self->install_pkgs = vardict_lookup_strv_canonical (self->modifiers, "install-packages");
  self->uninstall_pkgs = vardict_lookup_strv_canonical (self->modifiers, "uninstall-packages");
  self->override_replace_pkgs = vardict_lookup_strv_canonical (self->modifiers, "override-replace-packages");
  self->override_remove_pkgs = vardict_lookup_strv_canonical (self->modifiers, "override-remove-packages");
  self->override_reset_pkgs = vardict_lookup_strv_canonical (self->modifiers, "override-reset-packages");
  self->enable_modules = vardict_lookup_strv_canonical (self->modifiers, "enable-modules");
  self->disable_modules = vardict_lookup_strv_canonical (self->modifiers, "disable-modules");
  self->install_modules = vardict_lookup_strv_canonical (self->modifiers, "install-modules");
  self->uninstall_modules = vardict_lookup_strv_canonical (self->modifiers, "uninstall-modules");

  /* default to allowing downgrades for rebases & deploys (without --disallow-downgrade) */
  if (vardict_lookup_bool (self->options, "allow-downgrade", refspec_or_revision))
    self->flags = static_cast<RpmOstreeTransactionDeployFlags>(self-> flags | RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE);

  g_autoptr(GVariant) install_local_pkgs_idxs =
    g_variant_dict_lookup_value (self->modifiers, "install-local-packages",
                                 G_VARIANT_TYPE("ah"));
  g_autoptr(GVariant) override_replace_local_pkgs_idxs =
    g_variant_dict_lookup_value (self->modifiers, "override-replace-local-packages",
                                 G_VARIANT_TYPE("ah"));
  int local_repo_remote_idx = -1;
  /* See related blurb in get_modifiers_variant() */
  g_variant_dict_lookup (self->modifiers, "ex-local-repo-remote", "h", &local_repo_remote_idx);

  /* First in the fd list is local RPM fds, which are relevant in the
   * `install foo.rpm` case and the `override replace foo.rpm` case. Let's make sure that
   * the actual number of fds passed is what we expect.
   *
   * A more recent addition is the local-repo-remote fd.
   *
   * Here we validate the number of fds provided against the arguments.
   */
  guint expected_fdn = 0;
  if (install_local_pkgs_idxs)
    expected_fdn += g_variant_n_children (install_local_pkgs_idxs);
  if (override_replace_local_pkgs_idxs)
    expected_fdn += g_variant_n_children (override_replace_local_pkgs_idxs);
  if (local_repo_remote_idx != -1)
    expected_fdn += 1;

  guint actual_fdn = 0;
  if (fd_list)
    actual_fdn = g_unix_fd_list_get_length (fd_list);

  if (expected_fdn != actual_fdn)
    return (RpmostreedTransaction*)glnx_null_throw (error, "Expected %u fds but received %u",
                            expected_fdn, actual_fdn);

  /* split into two fd lists to make it easier for deploy_transaction_execute */
  if (fd_list)
    {
      gint nfds = 0; /* the strange constructions below allow us to avoid dup()s */
      g_autofree gint *fds = g_unix_fd_list_steal_fds (fd_list, &nfds);

      if (install_local_pkgs_idxs)
        {
          g_autofree gint *new_fds =
            get_fd_array_from_sparse (fds, nfds, install_local_pkgs_idxs);
          self->install_local_pkgs = g_unix_fd_list_new_from_array (new_fds, -1);
        }

      if (override_replace_local_pkgs_idxs)
        {
          g_autofree gint *new_fds =
            get_fd_array_from_sparse (fds, nfds, override_replace_local_pkgs_idxs);
          self->override_replace_local_pkgs = g_unix_fd_list_new_from_array (new_fds, -1);
        }

      if (local_repo_remote_idx != -1)
        {
          g_assert_cmpint (local_repo_remote_idx, >=, 0);
          g_assert_cmpint (local_repo_remote_idx, <, nfds);
          self->local_repo_remote_dfd = fds[local_repo_remote_idx];
        }
    }

  /* Also check for conflicting options -- this is after all a public API. */

  if (!self->refspec && vardict_lookup_bool (self->options, "skip-purge", FALSE))
    return (RpmostreedTransaction*)glnx_null_throw (error, "Can't specify skip-purge if not setting a "
                                   "new refspec");
  if (refspec_or_revision &&
      vardict_lookup_bool (self->options, "no-pull-base", FALSE))
    return (RpmostreedTransaction*)glnx_null_throw (error, "Can't specify no-pull-base if setting a "
                                   "new refspec or revision");
  if (vardict_lookup_bool (self->options, "cache-only", FALSE) &&
      vardict_lookup_bool (self->options, "download-only", FALSE))
    return (RpmostreedTransaction*)glnx_null_throw (error, "Can't specify cache-only and download-only");
  if (vardict_lookup_bool (self->options, "dry-run", FALSE) &&
      vardict_lookup_bool (self->options, "download-only", FALSE))
    return (RpmostreedTransaction*)glnx_null_throw (error, "Can't specify dry-run and download-only");
  if (self->override_replace_pkgs)
    return (RpmostreedTransaction*)glnx_null_throw (error, "Non-local replacement overrides not implemented yet");

  if (vardict_lookup_bool (self->options, "no-overrides", FALSE) &&
      (self->override_remove_pkgs || self->override_reset_pkgs ||
       self->override_replace_pkgs || override_replace_local_pkgs_idxs))
    return (RpmostreedTransaction*)glnx_null_throw (error, "Can't specify no-overrides if setting "
                                   "override modifiers");
  if (!self->refspec && self->local_repo_remote_dfd != -1)
    return (RpmostreedTransaction*)glnx_null_throw (error, "Missing ref for transient local rebases");
  if (self->revision && self->local_repo_remote_dfd != -1)
    return (RpmostreedTransaction*)glnx_null_throw (error, "Revision overrides for transient local rebases not implemented yet");

  return (RpmostreedTransaction *) util::move_nullify (self);
}

/* ================================ InitramfsEtc ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char **track;
  char **untrack;
  gboolean untrack_all;
  gboolean force_sync;
  GVariantDict *options;
} InitramfsEtcTransaction;

typedef RpmostreedTransactionClass InitramfsEtcTransactionClass;

GType initramfs_etc_transaction_get_type (void);

G_DEFINE_TYPE (InitramfsEtcTransaction,
               initramfs_etc_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
initramfs_etc_transaction_finalize (GObject *object)
{
  InitramfsEtcTransaction *self;

  self = (InitramfsEtcTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->track);
  g_strfreev (self->untrack);
  g_clear_pointer (&self->options, g_variant_dict_unref);

  G_OBJECT_CLASS (initramfs_etc_transaction_parent_class)->finalize (object);
}

static gboolean
initramfs_etc_transaction_execute (RpmostreedTransaction *transaction,
                                   GCancellable *cancellable,
                                   GError **error)
{
  InitramfsEtcTransaction *self = (InitramfsEtcTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  auto command_line = (const char *)
    vardict_lookup_ptr (self->options, "initiating-command-line", "&s");

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, command_line ?: "initramfs-etc");

  int upgrader_flags = 0;
  if (vardict_lookup_bool (self->options, "lock-finalization", FALSE))
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION;

  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, static_cast<RpmOstreeSysrootUpgraderFlags>(upgrader_flags), cancellable, error);
  if (upgrader == NULL)
    return FALSE;
  rpmostree_sysroot_upgrader_set_caller_info (upgrader, command_line, 
                                              rpmostreed_transaction_get_agent_id (RPMOSTREED_TRANSACTION(self)),
                                              rpmostreed_transaction_get_sd_unit (RPMOSTREED_TRANSACTION(self)));

  g_autoptr(RpmOstreeOrigin) origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);

  gboolean changed = FALSE;
  if (self->untrack_all)
    {
      gboolean subchanged = FALSE;
      rpmostree_origin_initramfs_etc_files_untrack_all (origin, &subchanged);
      changed = changed || subchanged;
    }
  else if (self->untrack)
    {
      gboolean subchanged = FALSE;
      rpmostree_origin_initramfs_etc_files_untrack (origin, self->untrack, &subchanged);
      changed = changed || subchanged;
    }

  if (self->track)
    {
      gboolean subchanged = FALSE;
      rpmostree_origin_initramfs_etc_files_track (origin, self->track, &subchanged);
      changed = changed || subchanged;
    }

  if (!changed && !self->force_sync)
    {
      rpmostree_output_message ("No changes.");
      return TRUE; /* Note early return */
    }

  GHashTable *files = rpmostree_origin_get_initramfs_etc_files (origin);
  GLNX_HASH_TABLE_FOREACH (files, const char*, file)
    {
      if (!g_str_has_prefix (file, "/etc/"))
        return glnx_throw (error, "Path outside /etc forbidden: %s", file);
      /* could add more checks here in the future */
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);
  if (!rpmostree_sysroot_upgrader_deploy (upgrader, NULL, cancellable, error))
    return FALSE;

  if (vardict_lookup_bool (self->options, "reboot", FALSE))
    {
      if (!check_sd_inhibitor_locks (cancellable, error))
        return FALSE;
      rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
    }

  return TRUE;
}

static void
initramfs_etc_transaction_class_init (InitramfsEtcTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = initramfs_etc_transaction_finalize;

  clazz->execute = initramfs_etc_transaction_execute;
}

static void
initramfs_etc_transaction_init (InitramfsEtcTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_etc (GDBusMethodInvocation *invocation,
                                          OstreeSysroot         *sysroot,
                                          const char            *osname,
                                          char                 **track,
                                          char                 **untrack,
                                          gboolean               untrack_all,
                                          gboolean               force_sync,
                                          GVariant              *options,
                                          GCancellable          *cancellable,
                                          GError               **error)
{

  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (InitramfsEtcTransaction *)
    g_initable_new (initramfs_etc_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->track = g_strdupv (track);
      self->untrack = g_strdupv (untrack);
      self->untrack_all = untrack_all;
      self->force_sync = force_sync;
      self->options = g_variant_dict_new (options);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ SetInitramfsState ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean regenerate;
  char **args;
  GVariantDict *options;
} InitramfsStateTransaction;

typedef RpmostreedTransactionClass InitramfsStateTransactionClass;

GType initramfs_state_transaction_get_type (void);

G_DEFINE_TYPE (InitramfsStateTransaction,
               initramfs_state_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
initramfs_state_transaction_finalize (GObject *object)
{
  InitramfsStateTransaction *self;

  self = (InitramfsStateTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->args);
  g_clear_pointer (&self->options, g_variant_dict_unref);

  G_OBJECT_CLASS (initramfs_state_transaction_parent_class)->finalize (object);
}

static gboolean
initramfs_state_transaction_execute (RpmostreedTransaction *transaction,
                                     GCancellable *cancellable,
                                     GError **error)
{

  InitramfsStateTransaction *self = (InitramfsStateTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  auto command_line = (const char*)
    vardict_lookup_ptr (self->options, "initiating-command-line", "&s");

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, command_line ?: "initramfs");

  int upgrader_flags = 0;
  if (vardict_lookup_bool (self->options, "lock-finalization", FALSE))
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION;

  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, static_cast<RpmOstreeSysrootUpgraderFlags>(upgrader_flags), cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  g_autoptr(RpmOstreeOrigin) origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);
  gboolean current_regenerate = rpmostree_origin_get_regenerate_initramfs (origin);
  const char *const* current_initramfs_args = rpmostree_origin_get_initramfs_args (origin);

  /* We don't deep-compare the args right now, we assume if you were using them
   * you want to rerun. This can be important if you edited a config file, which
   * we can't really track without actually regenerating anyways.
   */
  if (current_regenerate == self->regenerate
      && (current_initramfs_args == NULL || !*current_initramfs_args)
      && (self->args == NULL || !*self->args))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "initramfs regeneration state is already %s",
                   current_regenerate ? "enabled" : "disabled");
      return FALSE;
    }

  rpmostree_origin_set_regenerate_initramfs (origin, self->regenerate, self->args);
  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);
  rpmostree_sysroot_upgrader_set_caller_info (upgrader, command_line, 
                                              rpmostreed_transaction_get_agent_id (RPMOSTREED_TRANSACTION(self)),
                                              rpmostreed_transaction_get_sd_unit (RPMOSTREED_TRANSACTION(self)));

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, NULL, cancellable, error))
    return FALSE;

  if (vardict_lookup_bool (self->options, "reboot", FALSE))
    {
      if (!check_sd_inhibitor_locks (cancellable, error))
        return FALSE;
      rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
    }

  return TRUE;
}

static void
initramfs_state_transaction_class_init (InitramfsStateTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = initramfs_state_transaction_finalize;

  clazz->execute = initramfs_state_transaction_execute;
}

static void
initramfs_state_transaction_init (InitramfsStateTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_state (GDBusMethodInvocation *invocation,
                                            OstreeSysroot         *sysroot,
                                            const char            *osname,
                                            gboolean               regenerate,
                                            char                 **args,
                                            GVariant              *options,
                                            GCancellable          *cancellable,
                                            GError               **error)
{
  
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (InitramfsStateTransaction *)
    g_initable_new (initramfs_state_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->regenerate = regenerate;
      self->args = g_strdupv (args);
      self->options = g_variant_dict_new (options);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ Cleanup ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  RpmOstreeTransactionCleanupFlags flags;
} CleanupTransaction;

typedef RpmostreedTransactionClass CleanupTransactionClass;

GType cleanup_transaction_get_type (void);

G_DEFINE_TYPE (CleanupTransaction,
               cleanup_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
cleanup_transaction_finalize (GObject *object)
{
  CleanupTransaction *self;

  self = (CleanupTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (cleanup_transaction_parent_class)->finalize (object);
}

static gboolean
remove_directory_content_if_exists (int dfd,
                                    const char *path,
                                    GCancellable *cancellable,
                                    GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  glnx_autofd int fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendir(%s)", path);
    }
  else
    {
      if (!glnx_dirfd_iterator_init_take_fd (&fd, &dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *dent = NULL;

          if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (dent == NULL)
            break;

          if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
            return FALSE;
        }
    }
  return TRUE;
}

static gboolean
cleanup_transaction_execute (RpmostreedTransaction *transaction,
                             GCancellable *cancellable,
                             GError **error)
{
  CleanupTransaction *self = (CleanupTransaction *) transaction;
  const gboolean cleanup_pending = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY) > 0;
  const gboolean cleanup_rollback = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY) > 0;

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, "cleanup");

  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  if (cleanup_pending || cleanup_rollback)
    {
      g_autoptr(GPtrArray) new_deployments =
        rpmostree_syscore_filter_deployments (sysroot, self->osname,
                                              cleanup_pending,
                                              cleanup_rollback);

      if (new_deployments)
        {
          OstreeSysrootWriteDeploymentsOpts write_opts = { .do_postclean = FALSE };

          if (!ostree_sysroot_write_deployments_with_options (sysroot, new_deployments,
                                                              &write_opts, cancellable, error))
            return FALSE;

          /* And ensure we fall through to base cleanup */
          self->flags = static_cast<RpmOstreeTransactionCleanupFlags>(self->flags | RPMOSTREE_TRANSACTION_CLEANUP_BASE);
        }
      else
        {
          rpmostree_output_message ("Deployments unchanged.");
        }
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_BASE)
    {
      if (!rpmostree_syscore_cleanup (sysroot, repo, cancellable, error))
        return FALSE;
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_REPOMD)
    {
      if (!remove_directory_content_if_exists (AT_FDCWD, RPMOSTREE_CORE_CACHEDIR, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static void
cleanup_transaction_class_init (CleanupTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = cleanup_transaction_finalize;

  clazz->execute = cleanup_transaction_execute;
}

static void
cleanup_transaction_init (CleanupTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_cleanup (GDBusMethodInvocation *invocation,
                                    OstreeSysroot         *sysroot,
                                    const char            *osname,
                                    RpmOstreeTransactionCleanupFlags flags,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (CleanupTransaction *)
    g_initable_new (cleanup_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;

}

/* ================================ RefreshMd ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  RpmOstreeTransactionRefreshMdFlags flags;
} RefreshMdTransaction;

typedef RpmostreedTransactionClass RefreshMdTransactionClass;

GType refresh_md_transaction_get_type (void);

G_DEFINE_TYPE (RefreshMdTransaction,
               refresh_md_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
refresh_md_transaction_finalize (GObject *object)
{
  RefreshMdTransaction *self;

  self = (RefreshMdTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (refresh_md_transaction_parent_class)->finalize (object);
}

static gboolean
refresh_md_transaction_execute (RpmostreedTransaction *transaction,
                                GCancellable *cancellable,
                                GError **error)
{
  RefreshMdTransaction *self = (RefreshMdTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  const gboolean force = ((self->flags & RPMOSTREE_TRANSACTION_REFRESH_MD_FLAG_FORCE) > 0);

  g_autoptr(GString) title = g_string_new ("refresh-md");
  if (force)
    g_string_append (title, " (force)");
  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, title->str);

  g_autoptr(OstreeDeployment) cfg_merge_deployment =
    ostree_sysroot_get_merge_deployment (sysroot, self->osname);
  g_autoptr(OstreeDeployment) origin_merge_deployment =
    rpmostree_syscore_get_origin_merge_deployment (sysroot, self->osname);

  /* but set the source root to be the origin merge deployment's so we pick up releasever */
  g_autofree char *origin_deployment_root =
    rpmostree_get_deployment_root (sysroot, origin_merge_deployment);

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_client (repo);

  /* We could bypass rpmostree_context_setup() here and call dnf_context_setup() ourselves
   * since we're not actually going to perform any installation. Though it does provide us
   * with the right semantics for install/source_root. */
  if (!rpmostree_context_setup (ctx, NULL, origin_deployment_root, cancellable, error))
    return FALSE;

  if (force)
    rpmostree_context_set_dnf_caching (ctx, RPMOSTREE_CONTEXT_DNF_CACHE_NEVER);

  /* point libdnf to our repos dir */
  rpmostree_context_configure_from_deployment (ctx, sysroot, cfg_merge_deployment);

  /* don't even bother loading the rpmdb */
  if (!rpmostree_context_download_metadata (ctx, DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_RPMDB,
                                            cancellable, error))
    return FALSE;

  return TRUE;
}

static void
refresh_md_transaction_class_init (CleanupTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = refresh_md_transaction_finalize;

  clazz->execute = refresh_md_transaction_execute;
}

static void
refresh_md_transaction_init (RefreshMdTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_refresh_md (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       RpmOstreeTransactionRefreshMdFlags flags,
                                       const char            *osname,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (RefreshMdTransaction *)
    g_initable_new (refresh_md_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;

}

/* ================================ ModifyYumRepo ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char *repo_id;
  GVariant *settings;
} ModifyYumRepoTransaction;

typedef RpmostreedTransactionClass ModifyYumRepoTransactionClass;

GType modify_yum_repo_transaction_get_type (void);

G_DEFINE_TYPE (ModifyYumRepoTransaction,
               modify_yum_repo_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
modify_yum_repo_transaction_finalize (GObject *object)
{
  ModifyYumRepoTransaction *self;

  self = (ModifyYumRepoTransaction *) object;
  g_free (self->osname);
  g_free (self->repo_id);
  g_variant_unref (self->settings);

  G_OBJECT_CLASS (modify_yum_repo_transaction_parent_class)->finalize (object);
}

static DnfRepo *
get_dnf_repo_by_id (RpmOstreeContext *ctx, const char *repo_id)
{
  DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);

  GPtrArray *repos = dnf_context_get_repos (dnfctx);
  for (guint i = 0; i < repos->len; i++)
    {
      auto repo = static_cast<DnfRepo *>(repos->pdata[i]);
      if (g_strcmp0 (dnf_repo_get_id (repo), repo_id) == 0)
        return repo;
    }

  return NULL;
}

static gboolean
modify_yum_repo_transaction_execute (RpmostreedTransaction *transaction,
                                     GCancellable *cancellable,
                                     GError **error)
{
  ModifyYumRepoTransaction *self = (ModifyYumRepoTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, "modify-yum-repo");

  g_autoptr(OstreeDeployment) cfg_merge_deployment =
    ostree_sysroot_get_merge_deployment (sysroot, self->osname);

  OstreeRepo *ot_repo = ostree_sysroot_repo (sysroot);
  g_autoptr(RpmOstreeContext) ctx = rpmostree_context_new_client (ot_repo);

  /* We could bypass rpmostree_context_setup() here and call dnf_context_setup() ourselves
   * since we're not actually going to perform any installation. Though it does provide us
   * with the right semantics for install/source_root. */
  if (!rpmostree_context_setup (ctx, NULL, NULL, cancellable, error))
    return FALSE;

  /* point libdnf to our repos dir */
  rpmostree_context_configure_from_deployment (ctx, sysroot, cfg_merge_deployment);

  DnfRepo *repo = get_dnf_repo_by_id (ctx, self->repo_id);
  if (repo == NULL)
    return glnx_throw (error, "Yum repo '%s' not found", self->repo_id);

  GVariantIter iter;
  GVariant *child;
  g_variant_iter_init (&iter, self->settings);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      const char *parameter = NULL;
      const char *value = NULL;
      g_variant_get_child (child, 0, "&s", &parameter);
      g_variant_get_child (child, 1, "&s", &value);

      /* Only allow changing 'enabled' for now. See the discussion about changing arbitrary
       * .repo settings in https://github.com/projectatomic/rpm-ostree/pull/1780 */
      if (g_strcmp0 (parameter, "enabled") != 0)
        return glnx_throw (error, "Changing '%s' not allowed in yum .repo files", parameter);

      if (g_strcmp0 (value, "0") != 0 &&
          g_strcmp0 (value, "1") != 0)
        return glnx_throw (error, "Only '0' and '1' are allowed for the '%s' key", parameter);

      if (!dnf_repo_set_data (repo, parameter, value, error))
        return FALSE;
    }

  if (!dnf_repo_commit (repo, error))
    return FALSE;

  return TRUE;
}

static void
modify_yum_repo_transaction_class_init (CleanupTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = modify_yum_repo_transaction_finalize;

  clazz->execute = modify_yum_repo_transaction_execute;
}

static void
modify_yum_repo_transaction_init (ModifyYumRepoTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_modify_yum_repo (GDBusMethodInvocation *invocation,
                                            OstreeSysroot         *sysroot,
                                            const char            *osname,
                                            const char            *repo_id,
                                            GVariant              *settings,
                                            GCancellable          *cancellable,
                                            GError               **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (ModifyYumRepoTransaction*)
    g_initable_new (modify_yum_repo_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->repo_id = g_strdup (repo_id);
      self->settings = g_variant_ref (settings);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ FinalizeDeployment ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  GVariantDict *options;
} FinalizeDeploymentTransaction;

typedef RpmostreedTransactionClass FinalizeDeploymentTransactionClass;

GType finalize_deployment_transaction_get_type (void);

G_DEFINE_TYPE (FinalizeDeploymentTransaction,
               finalize_deployment_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
finalize_deployment_transaction_finalize (GObject *object)
{
  FinalizeDeploymentTransaction *self;

  self = (FinalizeDeploymentTransaction *) object;
  g_free (self->osname);
  g_clear_pointer (&self->options, g_variant_dict_unref);

  G_OBJECT_CLASS (finalize_deployment_transaction_parent_class)->finalize (object);
}

static gboolean
finalize_deployment_transaction_execute (RpmostreedTransaction *transaction,
                                         GCancellable *cancellable,
                                         GError **error)
{
  FinalizeDeploymentTransaction *self = (FinalizeDeploymentTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
    
  auto command_line = (const char*)
    vardict_lookup_ptr (self->options, "initiating-command-line", "&s");

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, command_line ?: "finalize-deployment");

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len == 0)
    return glnx_throw (error, "No deployments found");

  auto default_deployment = static_cast<OstreeDeployment *>(deployments->pdata[0]);
  if (!ostree_deployment_is_staged (default_deployment))
    return glnx_throw (error, "No pending staged deployment found");
  if (!g_str_equal (ostree_deployment_get_osname (default_deployment), self->osname))
    return glnx_throw (error, "Staged deployment is not for osname '%s'", self->osname);

  auto layeredmeta = rpmostreecxx::deployment_layeredmeta_load(*repo, *default_deployment);
  const char *checksum = layeredmeta.base_commit.c_str();

  auto expected_checksum =
    (char*)vardict_lookup_ptr (self->options, "checksum", "&s");
  const gboolean allow_missing_checksum =
    vardict_lookup_bool (self->options, "allow-missing-checksum", FALSE);
  if (!expected_checksum && !allow_missing_checksum)
    return glnx_throw (error, "Missing expected checksum");
  if (expected_checksum && !g_str_equal (checksum, expected_checksum))
    return glnx_throw (error, "Expected staged base checksum %s, but found %s",
                       expected_checksum, checksum);

  // Check for inhibitor locks before unlocking staged deployment.
  if (!check_sd_inhibitor_locks (cancellable, error))
    return FALSE;

  if (unlink (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED) < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "unlink(%s)",
                                        _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED);
      if (!vardict_lookup_bool (self->options, "allow-unlocked", FALSE))
        return glnx_throw (error, "Staged deployment already unlocked");
    }

  /* And bump sysroot mtime so we reload... a bit awkward, though this is similar to
   * libostree itself doing this for `ostree admin unlock` (and possibly an `ostree admin`
   * version of `rpm-ostree finalize-deployment`). */
  (void) rpmostree_syscore_bump_mtime (sysroot, NULL);

  sd_journal_print (LOG_INFO, "Finalized deployment; rebooting into %s", checksum);
  rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
  return TRUE;
}

static void
finalize_deployment_transaction_class_init (FinalizeDeploymentTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = finalize_deployment_transaction_finalize;

  clazz->execute = finalize_deployment_transaction_execute;
}

static void
finalize_deployment_transaction_init (FinalizeDeploymentTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_finalize_deployment (GDBusMethodInvocation *invocation,
                                                OstreeSysroot         *sysroot,
                                                const char            *osname,
                                                GVariant              *options,
                                                GCancellable          *cancellable,
                                                GError               **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  g_autoptr(GVariantDict) options_dict = g_variant_dict_new (options);

  auto self = (FinalizeDeploymentTransaction*)
    g_initable_new (finalize_deployment_transaction_get_type (),
                    cancellable, error,
                    "invocation", invocation,
                    "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                    NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->options = g_variant_dict_ref (options_dict);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================KernelArg================================ */

typedef struct {
  RpmostreedTransaction parent;
  char  *osname;
  char  *existing_kernel_args;
  char **kernel_args_added;
  char **kernel_args_deleted;
  char **kernel_args_replaced;
  GVariantDict *options;
} KernelArgTransaction;

typedef RpmostreedTransactionClass KernelArgTransactionClass;

GType kernel_arg_transaction_get_type (void);

G_DEFINE_TYPE (KernelArgTransaction,
               kernel_arg_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
kernel_arg_transaction_finalize (GObject *object)
{
  KernelArgTransaction *self;

  self = (KernelArgTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->kernel_args_added);
  g_strfreev (self->kernel_args_deleted);
  g_strfreev (self->kernel_args_replaced);
  g_free (self->existing_kernel_args);
  g_clear_pointer (&self->options, g_variant_dict_unref);
  G_OBJECT_CLASS (kernel_arg_transaction_parent_class)->finalize (object);
}

static gboolean
kernel_arg_transaction_execute (RpmostreedTransaction *transaction,
                                GCancellable *cancellable,
                                GError **error)
{
  KernelArgTransaction *self = (KernelArgTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);
  int upgrader_flags = 0;
  auto command_line = static_cast<const char *>(vardict_lookup_ptr (self->options, "initiating-command-line", "&s"));
  g_autofree char **append_if_missing = static_cast<char **>(vardict_lookup_strv_canonical (self->options, "append-if-missing"));
  g_autofree char **delete_if_present = static_cast<char **>(vardict_lookup_strv_canonical (self->options, "delete-if-present"));
  gboolean changed = FALSE;

  /* don't want to pull new content for this */
  upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL;
  upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY;
  if (vardict_lookup_bool (self->options, "lock-finalization", FALSE))
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION;

  rpmostree_transaction_set_title ((RPMOSTreeTransaction*)self, command_line ?: "kargs");

  /* Read in the existing kernel args and convert those to an #OstreeKernelArg instance for API usage */
  g_autoptr(OstreeKernelArgs) kargs = ostree_kernel_args_from_string (self->existing_kernel_args);
  g_autoptr(RpmOstreeSysrootUpgrader) upgrader =
    rpmostree_sysroot_upgrader_new (sysroot, self->osname, static_cast<RpmOstreeSysrootUpgraderFlags>(upgrader_flags),
                                    cancellable, error);
  if (upgrader == NULL)
    return FALSE;
  rpmostree_sysroot_upgrader_set_caller_info (upgrader, command_line, 
                                              rpmostreed_transaction_get_agent_id (RPMOSTREED_TRANSACTION(self)),
                                              rpmostreed_transaction_get_sd_unit (RPMOSTREED_TRANSACTION(self)));
  g_auto(GStrv) existing_kargs = g_strsplit (self->existing_kernel_args, " ", -1);

  /* Delete all the entries included in the kernel args */
  for (char **iter = self->kernel_args_deleted; iter && *iter; iter++)
    {
      const char*  arg =  *iter;
      if (!ostree_kernel_args_delete (kargs, arg, error))
        return FALSE;
      changed = TRUE;
    }

  for (char **iter = self->kernel_args_replaced; iter && *iter; iter++)
    {
      const char *arg = *iter;
      if (!ostree_kernel_args_new_replace (kargs, arg, error))
        return FALSE;
      changed = TRUE;
    }

  if (self->kernel_args_added)
    {
      ostree_kernel_args_append_argv (kargs, self->kernel_args_added);
      changed = TRUE;
    }

  for (char **iter = append_if_missing; iter && *iter; iter++)
    {
      const char *arg = *iter;
      if (!g_strv_contains (existing_kargs, arg))
        {
          ostree_kernel_args_append (kargs, arg);
          changed = TRUE;
        }
    }

  for (char **iter = delete_if_present; iter && *iter; iter++)
    {
      const char *arg = *iter;
      if (g_strv_contains (existing_kargs, arg))
        {
          if (!ostree_kernel_args_delete (kargs, arg, error))
            return FALSE;
          changed = TRUE;
        }
    }

  if (!changed)
    {
      rpmostree_output_message ("No changes.");
      return TRUE;
    }

  /* After all the arguments are processed earlier, we convert it to a string list*/
  g_auto(GStrv) kargs_strv = ostree_kernel_args_to_strv (kargs);
  rpmostree_sysroot_upgrader_set_kargs (upgrader, kargs_strv);

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, NULL, cancellable, error))
    return FALSE;

  if (vardict_lookup_bool (self->options, "reboot", FALSE))
    {
      if (!check_sd_inhibitor_locks (cancellable, error))
        return FALSE;
      rpmostreed_daemon_reboot (rpmostreed_daemon_get ());
    }

  return TRUE;
}

static void
kernel_arg_transaction_class_init (KernelArgTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = kernel_arg_transaction_finalize;

  clazz->execute = kernel_arg_transaction_execute;
}

static void
kernel_arg_transaction_init (KernelArgTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_kernel_arg (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       const char            *osname,
                                       const char            *existing_kernel_args,
                                       const char * const *kernel_args_added,
                                       const char * const *kernel_args_replaced,
                                       const char * const *kernel_args_deleted,
                                       GVariant              *options,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (OSTREE_IS_SYSROOT (sysroot));

  auto self = (KernelArgTransaction*)g_initable_new (kernel_arg_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->kernel_args_added = strdupv_canonicalize (kernel_args_added);
      self->kernel_args_replaced = strdupv_canonicalize (kernel_args_replaced);
      self->kernel_args_deleted = strdupv_canonicalize (kernel_args_deleted);
      self->existing_kernel_args = g_strdup (existing_kernel_args);
      self->options = g_variant_dict_new (options);
    }

  return (RpmostreedTransaction *) self;
}
