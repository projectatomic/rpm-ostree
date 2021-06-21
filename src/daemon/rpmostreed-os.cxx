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

#include <libglnx.h>
#include <systemd/sd-journal.h>

#include "rpmostreed-sysroot.h"
#include "rpmostreed-daemon.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostree-package-variants.h"
#include "rpmostreed-errors.h"
#include "rpmostree-origin.h"
#include "rpmostree-core.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostreed-os.h"
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-transaction-types.h"

typedef struct _RpmostreedOSClass RpmostreedOSClass;

struct _RpmostreedOS
{
  RPMOSTreeOSSkeleton parent_instance;
  gboolean on_session_bus;
  guint signal_id;
};

struct _RpmostreedOSClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

static void rpmostreed_os_iface_init (RPMOSTreeOSIface *iface);

static gboolean rpmostreed_os_load_internals (RpmostreedOS *self, GError **error);

static inline void *vardict_lookup_ptr (GVariantDict *dict, const char *key, const char *fmt);
static inline char **vardict_lookup_strv (GVariantDict *dict, const char *key);

static gboolean vardict_lookup_bool (GVariantDict *dict, const char *key, gboolean dfault);

G_DEFINE_TYPE_WITH_CODE (RpmostreedOS,
                         rpmostreed_os,
                         RPMOSTREE_TYPE_OS_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OS,
                                                rpmostreed_os_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
sysroot_changed (RpmostreedSysroot *sysroot,
                 gpointer user_data)
{
  RpmostreedOS *self = RPMOSTREED_OS (user_data);
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  if (!rpmostreed_os_load_internals (self, error))
      goto out;

 out:
  if (local_error)
    g_warning ("%s", local_error->message);
}

static gboolean
os_authorize_method (GDBusInterfaceSkeleton *interface,
                     GDBusMethodInvocation  *invocation)
{
  RpmostreedSysroot *sysroot = rpmostreed_sysroot_get ();
  PolkitAuthority *authority = rpmostreed_sysroot_get_polkit_authority (sysroot);
  const gchar *method_name = g_dbus_method_invocation_get_method_name (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_autoptr(GPtrArray) actions = g_ptr_array_new ();
  gboolean authorized = FALSE;

  if (rpmostreed_sysroot_is_on_session_bus (sysroot))
    {
      /* The daemon is on the session bus, running self tests */
      authorized = TRUE;
    }
  else if (g_strcmp0 (method_name, "GetDeploymentsRpmDiff") == 0 ||
           g_strcmp0 (method_name, "GetCachedDeployRpmDiff") == 0 ||
           g_strcmp0 (method_name, "DownloadDeployRpmDiff") == 0 ||
           g_strcmp0 (method_name, "GetCachedUpdateRpmDiff") == 0 ||
           g_strcmp0 (method_name, "DownloadUpdateRpmDiff") == 0 ||
           g_strcmp0 (method_name, "GetCachedRebaseRpmDiff") == 0 ||
           g_strcmp0 (method_name, "DownloadRebaseRpmDiff") == 0 ||
           g_strcmp0 (method_name, "RefreshMd") == 0)

    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.repo-refresh");
    }
  else if (g_strcmp0 (method_name, "ModifyYumRepo") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.repo-modify");
    }
  else if (g_strcmp0 (method_name, "Deploy") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.deploy");
    }
  /* unite these for now; it could make sense at least to make "check" its own action */
  else if (g_strcmp0 (method_name, "Upgrade") == 0 ||
           g_strcmp0 (method_name, "AutomaticUpdateTrigger") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.upgrade");
    }
  else if (g_strcmp0 (method_name, "Rebase") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.rebase");
    }
  else if (g_strcmp0 (method_name, "GetDeploymentBootConfig") == 0)
    {
      /* Note: early return here because no need authentication
       * for this method
       */
      return TRUE;
    }
  else if (g_strcmp0 (method_name, "SetInitramfsState") == 0 ||
           g_strcmp0 (method_name, "KernelArgs") == 0 ||
           g_strcmp0 (method_name, "InitramfsEtc") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.bootconfig");
    }
  else if (g_strcmp0 (method_name, "Cleanup") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.cleanup");
    }
  else if (g_strcmp0 (method_name, "Rollback") == 0 ||
           g_strcmp0 (method_name, "ClearRollbackTarget") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.rollback");
    }
  else if (g_strcmp0 (method_name, "PkgChange") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.install-uninstall-packages");
    }
  else if (g_strcmp0 (method_name, "UpdateDeployment") == 0)
    {
      g_autoptr(GVariant) modifiers = g_variant_get_child_value (parameters, 0);
      g_autoptr(GVariant) options = g_variant_get_child_value (parameters, 1);
      g_auto(GVariantDict) modifiers_dict;
      g_auto(GVariantDict) options_dict;
      g_variant_dict_init (&modifiers_dict, modifiers);
      g_variant_dict_init (&options_dict, options);
      auto refspec =
        static_cast<const char*>(vardict_lookup_ptr (&modifiers_dict, "set-refspec", "&s"));
      auto revision =
        static_cast<const char *>(vardict_lookup_ptr (&modifiers_dict, "set-revision", "&s"));
      g_autofree char **install_pkgs =
        vardict_lookup_strv (&modifiers_dict, "install-packages");
      g_autofree char **uninstall_pkgs =
        vardict_lookup_strv (&modifiers_dict, "uninstall-packages");
      g_autofree char **enable_modules =
        vardict_lookup_strv (&modifiers_dict, "enable-modules");
      g_autofree char **disable_modules =
        vardict_lookup_strv (&modifiers_dict, "disable-modules");
      g_autofree char **install_modules =
        vardict_lookup_strv (&modifiers_dict, "install-modules");
      g_autofree char **uninstall_modules =
        vardict_lookup_strv (&modifiers_dict, "uninstall-modules");
      g_autofree const char *const *override_replace_pkgs =
        vardict_lookup_strv (&modifiers_dict, "override-replace-packages");
      g_autofree const char *const *override_remove_pkgs =
        vardict_lookup_strv (&modifiers_dict, "override-remove-packages");
      g_autofree const char *const *override_reset_pkgs =
        vardict_lookup_strv (&modifiers_dict, "override-reset-packages");
      g_autoptr(GVariant) install_local_pkgs =
        g_variant_dict_lookup_value (&modifiers_dict, "install-local-packages",
                                     G_VARIANT_TYPE("ah"));
      g_autoptr(GVariant) override_replace_local_pkgs =
        g_variant_dict_lookup_value (&modifiers_dict, "override-replace-local-packages",
                                     G_VARIANT_TYPE("ah"));
      gboolean no_pull_base =
        vardict_lookup_bool (&options_dict, "no-pull-base", FALSE);
      gboolean no_overrides =
        vardict_lookup_bool (&options_dict, "no-overrides", FALSE);
      gboolean no_layering =
        vardict_lookup_bool (&options_dict, "no-layering", FALSE);

      if (vardict_lookup_bool (&options_dict, "no-initramfs", FALSE))
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.bootconfig");

      if (refspec != NULL)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.rebase");
      else if (revision != NULL)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.deploy");
      else if (!no_pull_base)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.upgrade");

      if (install_pkgs != NULL || uninstall_pkgs != NULL ||
          enable_modules != NULL || disable_modules != NULL ||
          install_modules != NULL || uninstall_modules != NULL ||
          no_layering)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.install-uninstall-packages");

      if (install_local_pkgs != NULL && g_variant_n_children (install_local_pkgs) > 0)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.install-local-packages");

      if (override_replace_pkgs != NULL || override_remove_pkgs != NULL || override_reset_pkgs != NULL ||
          (override_replace_local_pkgs != NULL && g_variant_n_children (override_replace_local_pkgs) > 0) ||
          no_overrides)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.override");
      /* If we couldn't figure out what's going on, count it as an override.  This occurs
       * right now with `deploy --ex-cliwrap=true`.
       */
      if (actions->len == 0)
        g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.override");
    }
  else if (g_strcmp0 (method_name, "FinalizeDeployment") == 0)
    {
      g_ptr_array_add (actions, (void*)"org.projectatomic.rpmostree1.finalize-deployment");
    }
  else
    {
      authorized = FALSE;
    }

  for (guint i = 0; i < actions->len; i++)
    {
      auto action = static_cast<const gchar *>(g_ptr_array_index (actions, i));
      glnx_unref_object PolkitSubject *subject = polkit_system_bus_name_new (sender);
      glnx_unref_object PolkitAuthorizationResult *result = NULL;
      g_autoptr(GError) error = NULL;

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, NULL,
                                                          POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
      if (!authorized)
        break;
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "rpmostreed OS operation %s not allowed for user", method_name);
    }

  return authorized;
}

static void
os_dispose (GObject *object)
{
  RpmostreedOS *self = RPMOSTREED_OS (object);
  const gchar *object_path;

  object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON(self));
  if (object_path != NULL)
    {
      rpmostreed_daemon_unpublish (rpmostreed_daemon_get (),
                                   object_path, object);
    }

  if (self->signal_id > 0)
      g_signal_handler_disconnect (rpmostreed_sysroot_get (), self->signal_id);

  self->signal_id = 0;

  G_OBJECT_CLASS (rpmostreed_os_parent_class)->dispose (object);
}

static void
os_constructed (GObject *object)
{
  RpmostreedOS *self = RPMOSTREED_OS (object);

  self->signal_id = g_signal_connect (rpmostreed_sysroot_get (),
                                      "updated",
                                      G_CALLBACK (sysroot_changed), self);
  G_OBJECT_CLASS (rpmostreed_os_parent_class)->constructed (object);
}

static void
rpmostreed_os_class_init (RpmostreedOSClass *klass)
{
  GObjectClass *gobject_class;
  GDBusInterfaceSkeletonClass *gdbus_interface_skeleton_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = os_dispose;
  gobject_class->constructed  = os_constructed;

  gdbus_interface_skeleton_class = G_DBUS_INTERFACE_SKELETON_CLASS (klass);
  gdbus_interface_skeleton_class->g_authorize_method = os_authorize_method;
}

static void
rpmostreed_os_init (RpmostreedOS *self)
{
}

/* ---------------------------------------------------------------------------------------------------- */

static GVariant *
new_variant_diff_result (GVariant *diff,
                         GVariant *details)
{
  return g_variant_new ("(@a(sua{sv})@a{sv})", diff, details);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
os_handle_get_deployments_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_deployid0,
                                    const char *arg_deployid1)
{
  g_autoptr(GCancellable) cancellable = NULL;
  RpmostreedSysroot *global_sysroot;
  glnx_unref_object OstreeDeployment *deployment0 = NULL;
  glnx_unref_object OstreeDeployment *deployment1 = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  g_autoptr(GVariant) value = NULL;
  GError *local_error = NULL;
  const gchar *ref0;
  const gchar *ref1;

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  deployment0 = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid0);
  if (!deployment0)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Invalid deployment id %s",
                                 arg_deployid0);
      goto out;
    }
  ref0 = ostree_deployment_get_csum (deployment0);

  deployment1 = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid1);
  if (!deployment1)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Invalid deployment id %s",
                                 arg_deployid1);
      goto out;
    }
  ref1 = ostree_deployment_get_csum (deployment1);

  if (!rpm_ostree_db_diff_variant (ot_repo, ref0, ref1, FALSE, &value,
                                   cancellable, &local_error))
    goto out;

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(@a(sua{sv}))", value));
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_update_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_deployid)
{
  RpmostreedSysroot *global_sysroot;
  const gchar *name;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  GCancellable *cancellable = NULL;
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GVariant) details = NULL;
  GError *local_error = NULL;

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  name = rpmostree_os_get_name (interface);
  if (arg_deployid == NULL || arg_deployid[0] == '\0')
    {
      base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
      if (base_deployment == NULL)
        {
          local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No deployments found for os %s", name);
          goto out;
        }
    }
  else
    {
      base_deployment = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid);
      if (!base_deployment)
        {
          local_error = g_error_new (RPM_OSTREED_ERROR,
                                     RPM_OSTREED_ERROR_FAILED,
                                     "Invalid deployment id %s",
                                     arg_deployid);
          goto out;
        }
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, &local_error);
  if (!origin)
    goto out;

  if (!rpm_ostree_db_diff_variant (ot_repo, ostree_deployment_get_csum (base_deployment),
                                   rpmostree_origin_get_refspec (origin), FALSE, &value,
                                   cancellable, &local_error))
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment, ot_repo,
                                                               rpmostree_origin_get_refspec (origin),
                                                               NULL,
                                                               &local_error);
  if (!details)
    goto out;

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
                                             new_variant_diff_result (value, details));
    }

  return TRUE;
}

static gboolean
refresh_cached_update (RpmostreedOS*, GError **error);

static void
on_auto_update_done (RpmostreedTransaction *transaction, RpmostreedOS *self)
{
  g_autoptr(GError) local_error = NULL;
  if (!refresh_cached_update (self, &local_error))
    {
      sd_journal_print (LOG_WARNING, "Failed to refresh CachedUpdate property: %s",
                        local_error->message);
    }
}

static gboolean
os_handle_download_update_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         NULL,  /* refspec */
                                                         NULL,  /* revision */
                                                         cancellable,
                                                         &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

  /* Make sure we refresh CachedUpdate after the transaction. This normally happens
   * automatically if new data was downloaded (through the repo mtime bump --> UPDATED
   * signal), but we also want to do this even if the data was already present. */
  g_signal_connect (transaction, "closed", G_CALLBACK (on_auto_update_done), self);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_update_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
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

static inline char **
vardict_lookup_strv (GVariantDict  *dict,
                     const char    *key)
{
  return static_cast<char**>(vardict_lookup_ptr (dict, key, "^a&s"));
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

typedef void (*InvocationCompleter)(RPMOSTreeOS*,
                                    GDBusMethodInvocation*,
                                    GUnixFDList*,
                                    const gchar*);

static gboolean
os_merge_or_start_deployment_txn (RPMOSTreeOS            *interface,
                                  GDBusMethodInvocation  *invocation,
                                  RpmOstreeTransactionDeployFlags default_flags,
                                  GVariant               *options,
                                  RpmOstreeUpdateDeploymentModifiers *modifiers,
                                  GUnixFDList            *fd_list,
                                  InvocationCompleter     completer)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  g_autoptr(GError) local_error = NULL;
  const char *client_address = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto err;

  if (!transaction)
    {
      glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (), cancellable,
                                          &ot_sysroot, NULL, &local_error))
        goto err;

      transaction = rpmostreed_transaction_new_deploy (invocation, ot_sysroot,
                                                       rpmostree_os_get_name (interface),
                                                       default_flags, options, modifiers, fd_list,
                                                       cancellable, &local_error);
      if (!transaction)
        goto err;

      rpmostreed_sysroot_set_txn (rsysroot, transaction);

      /* For the AutomaticUpdateTrigger "check" case, we want to make sure we refresh
       * the CachedUpdate property; "stage" will do this through sysroot_changed */
      const char *method_name = g_dbus_method_invocation_get_method_name (invocation);
      if (g_str_equal (method_name, "AutomaticUpdateTrigger") &&
          (default_flags & (RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY |
                            RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_METADATA_ONLY)))
        g_signal_connect (transaction, "closed", G_CALLBACK (on_auto_update_done), self);
    }

  client_address = rpmostreed_transaction_get_client_address (transaction);
  completer (interface, invocation, NULL, client_address);
  return TRUE;
 err:
  if (!local_error) /* we should've gotten an error, but let's be safe */
    glnx_throw (&local_error, "Failed to start the transaction");
  g_dbus_method_invocation_take_error (invocation,
                                       util::move_nullify (local_error));
  /* We always return TRUE to signal that we handled the invocation. */
  return TRUE;
}

static gboolean
os_handle_deploy (RPMOSTreeOS *interface,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fd_list,
                  const char *arg_revision,
                  GVariant *arg_options)
{
  g_autoptr(GVariantBuilder) vbuilder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  if (arg_revision)
    g_variant_builder_add (vbuilder, "{sv}", "set-revision",
                           g_variant_new_string (arg_revision));
  g_autoptr(RpmOstreeUpdateDeploymentModifiers) modifiers =
    g_variant_ref_sink (g_variant_builder_end (vbuilder));
  return os_merge_or_start_deployment_txn (interface, invocation,
                                           RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE,
                                           arg_options, modifiers, fd_list,
                                           rpmostree_os_complete_deploy);
}

static gboolean
os_handle_upgrade (RPMOSTreeOS *interface,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList *fd_list,
                   GVariant *arg_options)
{
  g_autoptr(GVariantBuilder) vbuilder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_autoptr(RpmOstreeUpdateDeploymentModifiers) modifiers =
    g_variant_ref_sink (g_variant_builder_end (vbuilder));
  return os_merge_or_start_deployment_txn (interface, invocation, static_cast<RpmOstreeTransactionDeployFlags>(0),
                                           arg_options, modifiers, fd_list,
                                           rpmostree_os_complete_upgrade);
}

static gboolean
os_handle_rebase (RPMOSTreeOS *interface,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fd_list,
                  GVariant *arg_options,
                  const char *arg_refspec,
                  const char * const *arg_packages)
{
  /* back-compat -- the revision is specified in the options variant; take it
   * out of there and make it a proper argument */
  g_auto(GVariantDict) options_dict;
  g_variant_dict_init (&options_dict, arg_options);
  const char *opt_revision = NULL;
  g_variant_dict_lookup (&options_dict, "revision", "&s", &opt_revision);

  g_autoptr(GVariantBuilder) vbuilder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  if (arg_refspec)
    g_variant_builder_add (vbuilder, "{sv}", "set-refspec",
                           g_variant_new_string (arg_refspec));
  if (opt_revision)
    g_variant_builder_add (vbuilder, "{sv}", "set-revision",
                           g_variant_new_string (opt_revision));
  g_autoptr(RpmOstreeUpdateDeploymentModifiers) modifiers =
    g_variant_ref_sink (g_variant_builder_end (vbuilder));

  return os_merge_or_start_deployment_txn (interface, invocation,
                                           RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE,
                                           arg_options, modifiers, fd_list,
                                           rpmostree_os_complete_rebase);
}

static gboolean
os_handle_pkg_change (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      GUnixFDList *fd_list,
                      GVariant *arg_options,
                      const char * const *arg_packages_added,
                      const char * const *arg_packages_removed)
{
  g_autoptr(GVariantBuilder) vbuilder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (vbuilder, "{sv}", "install-packages",
                         g_variant_new_strv (arg_packages_added, -1));
  g_variant_builder_add (vbuilder, "{sv}", "uninstall-packages",
                         g_variant_new_strv (arg_packages_removed, -1));
  g_autoptr(RpmOstreeUpdateDeploymentModifiers) modifiers =
    g_variant_ref_sink (g_variant_builder_end (vbuilder));
  return os_merge_or_start_deployment_txn (interface, invocation,
                                           RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE,
                                           arg_options, modifiers, NULL,
                                           rpmostree_os_complete_pkg_change);
}

static gboolean
os_handle_update_deployment (RPMOSTreeOS *interface,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *fd_list,
                             GVariant *arg_modifiers,
                             GVariant *arg_options)
{
  RpmOstreeUpdateDeploymentModifiers *modifiers = arg_modifiers;
  return os_merge_or_start_deployment_txn (interface, invocation,
                                           static_cast<RpmOstreeTransactionDeployFlags>(0), arg_options, modifiers, fd_list,
                                           rpmostree_os_complete_update_deployment);
}

/* compat shim for call completer */
static void
automatic_update_trigger_completer (RPMOSTreeOS            *os,
                                    GDBusMethodInvocation  *invocation,
                                    GUnixFDList            *dummy,
                                    const gchar            *address)
{                                                              /* enabled */
  rpmostree_os_complete_automatic_update_trigger (os, invocation, TRUE, address);
}

/* we make this a separate method to keep the D-Bus API clean, but the actual
 * implementation is done by our dear friend deploy_transaction_execute(). ❤️
 */

static gboolean
os_handle_automatic_update_trigger (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    GVariant *arg_options)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  const char *osname = rpmostree_os_get_name (interface);
  OstreeSysroot *sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);
  if (!booted || !g_str_equal (osname, ostree_deployment_get_osname (booted)))
    {
      /* We're not even booted in our osname; let's just not handle this case for now until
       * someone shows up with a good use case for it. We only want the booted OS to use the
       * current /var/cache, since it's per-OS. If we ever allow this, we'll want to
       * invalidate the auto-update cache on the osname as well. */
      glnx_throw (error, "Cannot trigger auto-update for offline OS '%s'", osname);
      g_dbus_method_invocation_take_error (invocation, util::move_nullify (local_error));
      return TRUE;
    }

  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, arg_options);
  auto mode = static_cast<const char *>(vardict_lookup_ptr (&dict, "mode", "&s") ?: "auto");

  RpmostreedAutomaticUpdatePolicy autoupdate_policy;
  if (g_str_equal (mode, "auto"))
    autoupdate_policy = rpmostreed_get_automatic_update_policy (rpmostreed_daemon_get ());
  else
    {
      if (!rpmostree_str_to_auto_update_policy (mode, &autoupdate_policy, error))
        {
          g_dbus_method_invocation_take_error (invocation, util::move_nullify (local_error));
          return TRUE;
        }
    }

  /* Now we translate policy into flags the deploy transaction understands. But avoid
   * starting it at all if we're not even on. The benefit of this approach is that we keep
   * the Deploy transaction simpler. */

  auto dfault = static_cast<RpmOstreeTransactionDeployFlags>(0);
  switch (autoupdate_policy)
    {
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE:
      {
        /* NB: we return the empty string here rather than NULL, because gdbus converts this
         * to a gvariant, which doesn't support NULL strings */             /* enabled */
        rpmostree_os_complete_automatic_update_trigger (interface, invocation, FALSE, "");
        return TRUE;
      }
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK:
      dfault = RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_METADATA_ONLY;
      break;
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE:
      break;
    default:
      g_assert_not_reached ();
    }

  /* if output-to-self is not explicitly set, default to TRUE */
  g_autoptr(GVariant) arg_options_owned = NULL;
  if (!g_variant_dict_contains (&dict, "output-to-self"))
    {
      g_variant_dict_insert (&dict, "output-to-self", "b", TRUE);
      arg_options = arg_options_owned = g_variant_ref_sink (g_variant_dict_end (&dict));
    }
  (void) arg_options_owned; /* Pacify static analysis */

  return os_merge_or_start_deployment_txn (
      interface,
      invocation,
      dfault,
      arg_options,
      NULL,
      NULL,
      automatic_update_trigger_completer);
}


static gboolean
os_handle_rollback (RPMOSTreeOS *interface,
                    GDBusMethodInvocation *invocation,
                    GVariant *arg_options)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  gboolean opt_reboot = FALSE;
  GVariantDict options_dict;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  g_variant_dict_init (&options_dict, arg_options);

  g_variant_dict_lookup (&options_dict,
                         "reboot", "b",
                         &opt_reboot);

  g_variant_dict_clear (&options_dict);

  transaction = rpmostreed_transaction_new_rollback (invocation,
                                                     ot_sysroot,
                                                     osname,
                                                     opt_reboot,
                                                     cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_rollback (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_refresh_md (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_options)
{
  g_autoptr(OstreeSysroot) ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;
  int flags = 0;
  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, arg_options);

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);


  if (vardict_lookup_bool (&dict, "force", FALSE))
    flags |= RPMOSTREE_TRANSACTION_REFRESH_MD_FLAG_FORCE;

  transaction = rpmostreed_transaction_new_refresh_md (invocation,
                                                       ot_sysroot,
                                                       static_cast<RpmOstreeTransactionRefreshMdFlags>(flags),
                                                       osname,
                                                       cancellable,
                                                       &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_refresh_md (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_modify_yum_repo (RPMOSTreeOS *interface,
                           GDBusMethodInvocation *invocation,
                           const char *arg_repo_id,
                           GVariant *arg_settings)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_modify_yum_repo (invocation,
                                                            ot_sysroot,
                                                            osname,
                                                            arg_repo_id,
                                                            arg_settings,
                                                            cancellable,
                                                            &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_modify_yum_repo (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_finalize_deployment (RPMOSTreeOS *interface,
                               GDBusMethodInvocation *invocation,
                               GVariant *arg_options)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();

  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rsysroot, cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_finalize_deployment (invocation, sysroot, osname,
                                                                arg_options, cancellable,
                                                                &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_finalize_deployment (interface, invocation, client_address);
    }

  return TRUE;
}

/* This is an older variant of Cleanup, kept for backcompat */
static gboolean
os_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                 GDBusMethodInvocation *invocation,
                                 GVariant *arg_options)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  auto flags = static_cast<RpmOstreeTransactionCleanupFlags>(0);
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable, &ot_sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  /* Note - intentionally ignoring the reboot option since I don't
   * know why anyone would want that.
   */

  flags = RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY;
  transaction = rpmostreed_transaction_new_cleanup (invocation, ot_sysroot,
                                                    osname, flags,
                                                    cancellable, &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_clear_rollback_target (interface, invocation, client_address);
    }
  return TRUE;
}

static gboolean
os_handle_initramfs_etc (RPMOSTreeOS *interface,
                         GDBusMethodInvocation *invocation,
                         const char *const* track,
                         const char *const* untrack,
                         gboolean untrack_all,
                         gboolean force_sync,
                         GVariant *options)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_initramfs_etc (invocation,
                                                          ot_sysroot,
                                                          osname,
                                                          (char**)track,
                                                          (char**)untrack,
                                                          untrack_all,
                                                          force_sync,
                                                          options,
                                                          cancellable,
                                                          &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_initramfs_etc (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_set_initramfs_state (RPMOSTreeOS *interface,
                               GDBusMethodInvocation *invocation,
                               gboolean regenerate,
                               const char *const*args,
                               GVariant *arg_options)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_initramfs_state (invocation,
                                                            ot_sysroot,
                                                            osname,
                                                            regenerate,
                                                            (char**)args,
                                                            arg_options,
                                                            cancellable,
                                                            &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_set_initramfs_state (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_kernel_args (RPMOSTreeOS *interface,
                       GDBusMethodInvocation *invocation,
                       const char * existing_kernel_args,
                       const char * const *kernel_args_added,
                       const char * const *kernel_args_replaced,
                       const char * const *kernel_args_deleted,
                       GVariant *arg_options)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  GError *local_error = NULL;
  const char *osname = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;
  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_kernel_arg (invocation,
                                                       ot_sysroot,
                                                       osname,
                                                       existing_kernel_args,
                                                       kernel_args_added,
                                                       kernel_args_replaced,
                                                       kernel_args_deleted,
                                                       arg_options,
                                                       cancellable,
                                                       &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);
  else
    {
      const char *client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_kernel_args (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_get_deployment_boot_config (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_deploy_index,
                                      gboolean is_pending)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  GError *local_error = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  glnx_unref_object OstreeDeployment *target_deployment  = NULL;
  GVariantDict boot_config_dict;
  GVariant *boot_config_result = NULL;
  OstreeBootconfigParser *bootconfig = NULL;
  const char* osname = NULL;
  /* Note because boot config is a private structure.. currently I have no good way
   * other than specifying all the content directly */
  const char *bootconfig_keys[] = {
    "title",
    "linux",
    "initrd",
    "options",
    OSTREE_COMMIT_META_KEY_VERSION,
    NULL
  };
  gboolean staged = FALSE;

  /* Load the sysroot */
  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;
  osname = rpmostree_os_get_name (interface);
  if (!*arg_deploy_index || arg_deploy_index[0] == '\0')
    {
      if (is_pending)
        target_deployment = rpmostree_syscore_get_origin_merge_deployment (ot_sysroot, osname);
      else
        target_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, osname);
      if (target_deployment  == NULL)
        {
          local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No deployments found for os %s", osname);
          goto out;
        }
    }
  else
    {
      /* If the deploy_index is specified, we ignore the pending option */
      target_deployment = rpmostreed_deployment_get_for_index (ot_sysroot, arg_deploy_index,
                                                               &local_error);
      if (target_deployment == NULL)
        goto out;
    }
  bootconfig = ostree_deployment_get_bootconfig (target_deployment);


  /* We initalize a dictionary and put key/value pair in bootconfig into it */
  g_variant_dict_init (&boot_config_dict, NULL);

  staged = ostree_deployment_is_staged (target_deployment);
  g_variant_dict_insert (&boot_config_dict, "staged", "b", staged);

  /* We loop through the key  and add each key/value pair value into the variant dict */
  for (char **iter = (char **)bootconfig_keys; iter && *iter; iter++)
    {
      const char *key = *iter;
      /* We can only populate "options" in the staged path. */
      if (staged && !g_str_equal (key, "options"))
        continue;

      const char *value = ostree_bootconfig_parser_get (bootconfig, key);
      g_assert (value);

      g_variant_dict_insert (&boot_config_dict, key, "s", value);
    }
  boot_config_result = g_variant_dict_end (&boot_config_dict);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);
  else
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(@a{sv})", boot_config_result));

  return TRUE;
}

static gboolean
os_handle_cleanup (RPMOSTreeOS *interface,
                   GDBusMethodInvocation *invocation,
                   const char *const*args)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname = NULL;
  GError *local_error = NULL;
  const char *client_address = NULL;
  int flags = 0;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  for (char **iter = (char**) args; iter && *iter; iter++)
    {
      const char *v = *iter;
      if (strcmp (v, "base") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_BASE;
      else if (strcmp (v, "pending-deploy") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY;
      else if (strcmp (v, "rollback-deploy") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY;
      else if (strcmp (v, "repomd") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_REPOMD;
      else
        {
          g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid cleanup type: %s", v);
          goto out;
        }
    }


  transaction = rpmostreed_transaction_new_cleanup (invocation,
                                                    ot_sysroot,
                                                    osname,
                                                    static_cast<RpmOstreeTransactionCleanupFlags>(flags),
                                                    cancellable,
                                                    &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_cleanup (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_rebase_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_refspec,
                                      const char * const *arg_packages)
{
  RpmostreedSysroot *global_sysroot;
  g_autoptr(GCancellable) cancellable = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  const gchar *name;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  g_autofree gchar *comp_ref = NULL;
  GError *local_error = NULL;
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GVariant) details = NULL;

  /* TODO: Totally ignoring packages for now */

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  name = rpmostree_os_get_name (interface);
  base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (base_deployment == NULL)
    {
      local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "No deployments found for os %s", name);
      goto out;
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, &local_error);
  if (!origin)
    goto out;

  if (!rpmostreed_refspec_parse_partial (arg_refspec,
                                         rpmostree_origin_get_refspec (origin),
                                         &comp_ref,
                                         &local_error))
    goto out;

  if (!rpm_ostree_db_diff_variant (ot_repo, ostree_deployment_get_csum (base_deployment),
                                   comp_ref, FALSE, &value, cancellable, &local_error))
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment,
                                                               ot_repo,
                                                               comp_ref,
                                                               NULL,
                                                               &local_error);
  if (!details)
    goto out;

out:
  if (local_error == NULL)
    {
      g_dbus_method_invocation_return_value (invocation, new_variant_diff_result (value, details));
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
os_handle_download_rebase_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_refspec,
                                    const char * const *arg_packages)
{
  /* TODO: Totally ignoring arg_packages for now */
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         arg_refspec,
                                                         NULL,  /* revision */
                                                         cancellable,
                                                         &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_rebase_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_deploy_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_revision,
                                      const char * const *arg_packages)
{
  const char *base_checksum;
  const char *osname;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GVariant) details = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  /* XXX Ignoring arg_packages for now. */

  ot_sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  ot_repo = rpmostreed_sysroot_get_repo (rpmostreed_sysroot_get ());

  osname = rpmostree_os_get_name (interface);
  base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, osname);
  if (base_deployment == NULL)
    {
      local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "No deployments found for os %s", osname);
      goto out;
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, error);
  if (!origin)
    goto out;

  base_checksum = ostree_deployment_get_csum (base_deployment);

  if (!rpmostreed_parse_revision (arg_revision,
                                  &checksum,
                                  &version,
                                  &local_error))
    goto out;

  if (version != NULL)
    {
      if (!rpmostreed_repo_lookup_cached_version (ot_repo,
                                                  rpmostree_origin_get_refspec (origin),
                                                  version,
                                                  cancellable,
                                                  &checksum,
                                                  &local_error))
        goto out;
    }

  if (!rpm_ostree_db_diff_variant (ot_repo, base_checksum, checksum, FALSE, &value,
                                   cancellable, &local_error))
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment,
                                                               ot_repo,
                                                               rpmostree_origin_get_refspec (origin),
                                                               checksum,
                                                               &local_error);
  if (!details)
    goto out;

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
                                             new_variant_diff_result (value, details));
    }

  return TRUE;
}

static gboolean
os_handle_download_deploy_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_revision,
                                    const char * const *arg_packages)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* XXX Ignoring arg_packages for now. */

  /* try to merge with an existing transaction, otherwise start a new one */
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();
  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, &local_error))
    goto out;
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         NULL, /* refspec */
                                                         arg_revision,
                                                         cancellable,
                                                         &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_sysroot_set_txn (rsysroot, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_deploy_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
}

/* split out for easier handling of the NULL case */
static gboolean
refresh_cached_update_impl (RpmostreedOS *self,
                            GVariant    **out_cached_update,
                            GError      **error)
{
  g_autoptr(GVariant) cached_update = NULL;

  /* if we're not booted into our OS, don't look at cache; it's for another OS interface */
  const char *osname = rpmostree_os_get_name (RPMOSTREE_OS (self));
  OstreeSysroot *sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);
  if (!booted || !g_str_equal (osname, ostree_deployment_get_osname (booted)))
    return TRUE; /* Note early return */

  glnx_autofd int fd = -1;
  g_autoptr(GError) local_error = NULL;
  if (!glnx_openat_rdonly (AT_FDCWD, RPMOSTREE_AUTOUPDATES_CACHE_FILE, TRUE, &fd,
                           &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return g_propagate_error (error, util::move_nullify (local_error)), FALSE;
      return TRUE; /* Note early return */
    }

  /* sanity check there isn't something fishy going on before even reading it in */
  struct stat stbuf;
  if (!glnx_fstat (fd, &stbuf, error))
    return FALSE;

  if (!rpmostree_check_size_within_limit (stbuf.st_size, OSTREE_MAX_METADATA_SIZE,
                                          RPMOSTREE_AUTOUPDATES_CACHE_FILE, error))
    return FALSE;

  g_autoptr(GBytes) data = glnx_fd_readall_bytes (fd, NULL, error);
  if (!data)
    return FALSE;

  cached_update =
    g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT, data, FALSE));

  /* check if cache is still valid -- see rpmostreed_update_generate_variant() */
  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, cached_update);
  auto state = static_cast<const char*>(vardict_lookup_ptr (&dict, "update-sha256", "&s"));
  if (g_strcmp0 (state, ostree_deployment_get_csum (booted)) != 0)
    {
      sd_journal_print (LOG_INFO, "Deleting outdated cached update for OS '%s'", osname);
      g_clear_pointer (&cached_update, (GDestroyNotify)g_variant_unref);
      if (!glnx_unlinkat (AT_FDCWD, RPMOSTREE_AUTOUPDATES_CACHE_FILE, 0, error))
        return FALSE;
    }

  *out_cached_update = util::move_nullify (cached_update);
  return TRUE;
}

static gboolean
refresh_cached_update (RpmostreedOS *self, GError **error)
{
  g_autoptr(GVariant) cached_update = NULL;
  if (!refresh_cached_update_impl (self, &cached_update, error))
    return FALSE;

  rpmostree_os_set_cached_update (RPMOSTREE_OS (self), cached_update);
  rpmostree_os_set_has_cached_update_rpm_diff (RPMOSTREE_OS (self), cached_update != NULL);
  return TRUE;
}

static gboolean
rpmostreed_os_load_internals (RpmostreedOS *self, GError **error)
{
  const gchar *name = rpmostree_os_get_name (RPMOSTREE_OS (self));
  g_debug ("loading %s", name);

  OstreeSysroot *ot_sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  OstreeRepo *ot_repo = rpmostreed_sysroot_get_repo (rpmostreed_sysroot_get ());

  /* Booted */
  g_autofree gchar* booted_id = NULL;
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (ot_sysroot);
  g_autoptr(GVariant) booted_variant = NULL; /* Strong ref as we reuse it below */
  if (booted_deployment && g_strcmp0 (ostree_deployment_get_osname (booted_deployment), name) == 0)
    {
      booted_variant =
        g_variant_ref_sink (
            rpmostreed_deployment_generate_variant (ot_sysroot, booted_deployment,
                                                    booted_id, ot_repo, TRUE, error));
      if (!booted_variant)
        return FALSE;
      auto bootedid_v = rpmostreecxx::deployment_generate_id(*booted_deployment);
      booted_id = g_strdup(bootedid_v.c_str());
    }
  else
    booted_variant = g_variant_ref_sink (rpmostreed_deployment_generate_blank_variant ());
  rpmostree_os_set_booted_deployment (RPMOSTREE_OS (self), booted_variant);

  /* Default (pending or booted) and rollback */
  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  g_autoptr(OstreeDeployment) pending_deployment = NULL;
  ostree_sysroot_query_deployments_for (ot_sysroot, name,
                                        &pending_deployment, &rollback_deployment);

  g_autoptr(GVariant) default_variant = NULL;
  if (pending_deployment)
    {
      default_variant =
        g_variant_ref_sink (rpmostreed_deployment_generate_variant (ot_sysroot,
                                                                    pending_deployment,
                                                                    booted_id,
                                                                    ot_repo, TRUE, error));
      if (!default_variant)
        return FALSE;
    }
  else
    default_variant = g_variant_ref (booted_variant); /* Default to booted */
  rpmostree_os_set_default_deployment (RPMOSTREE_OS (self), default_variant);

  GVariant *rollback_variant = NULL; /* Floating */
  if (rollback_deployment)
    {
      rollback_variant =
        rpmostreed_deployment_generate_variant (ot_sysroot, rollback_deployment, booted_id,
                                                ot_repo, TRUE, error);
      if (!rollback_variant)
        return FALSE;
    }
  else
    rollback_variant = rpmostreed_deployment_generate_blank_variant ();
  rpmostree_os_set_rollback_deployment (RPMOSTREE_OS (self), rollback_variant);

  if (!refresh_cached_update (self, error))
    return FALSE;

  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (self));

  return TRUE;
}

static void
rpmostreed_os_iface_init (RPMOSTreeOSIface *iface)
{
  iface->handle_automatic_update_trigger   = os_handle_automatic_update_trigger;
  iface->handle_cleanup                    = os_handle_cleanup;
  iface->handle_get_deployment_boot_config = os_handle_get_deployment_boot_config;
  iface->handle_kernel_args                = os_handle_kernel_args;
  iface->handle_refresh_md                 = os_handle_refresh_md;
  iface->handle_modify_yum_repo            = os_handle_modify_yum_repo;
  iface->handle_rollback                   = os_handle_rollback;
  iface->handle_initramfs_etc              = os_handle_initramfs_etc;
  iface->handle_set_initramfs_state        = os_handle_set_initramfs_state;
  iface->handle_update_deployment          = os_handle_update_deployment;
  iface->handle_finalize_deployment        = os_handle_finalize_deployment;
  /* legacy cleanup API; superseded by Cleanup() */
  iface->handle_clear_rollback_target      = os_handle_clear_rollback_target;
  /* legacy deployment change API; superseded by UpdateDeployment() */
  iface->handle_upgrade                    = os_handle_upgrade;
  iface->handle_rebase                     = os_handle_rebase;
  iface->handle_deploy                     = os_handle_deploy;
  iface->handle_pkg_change                 = os_handle_pkg_change;
  /* cache API; used by Cockpit at least */
  iface->handle_get_deployments_rpm_diff   = os_handle_get_deployments_rpm_diff;
  iface->handle_get_cached_update_rpm_diff = os_handle_get_cached_update_rpm_diff;
  iface->handle_get_cached_rebase_rpm_diff = os_handle_get_cached_rebase_rpm_diff;
  iface->handle_get_cached_deploy_rpm_diff = os_handle_get_cached_deploy_rpm_diff;
  iface->handle_download_update_rpm_diff   = os_handle_download_update_rpm_diff;
  iface->handle_download_rebase_rpm_diff   = os_handle_download_rebase_rpm_diff;
  iface->handle_download_deploy_rpm_diff   = os_handle_download_deploy_rpm_diff;
}

/* ---------------------------------------------------------------------------------------------------- */

RPMOSTreeOS *
rpmostreed_os_new (OstreeSysroot *sysroot,
                   OstreeRepo *repo,
                   const char *name)
{
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (name != NULL);

  g_autofree char *path = rpmostreed_generate_object_path (BASE_DBUS_PATH, name, NULL);

  auto obj = (RpmostreedOS *)g_object_new (RPMOSTREED_TYPE_OS, "name", name, NULL);

  /* FIXME - use GInitable */
  { g_autoptr(GError) local_error = NULL;
    if (!rpmostreed_os_load_internals (obj, &local_error))
      g_warning ("%s", local_error->message);
  }

  rpmostreed_daemon_publish (rpmostreed_daemon_get (), path, FALSE, obj);

  return RPMOSTREE_OS (obj);
}
