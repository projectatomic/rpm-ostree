/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
 * Copyright (C) 2016 Red Hat, Inc.
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
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>
#include <libdnf/libdnf.h>

#include "rpmostree-builtins.h"
#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostreed-transaction-types.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include "libsd-locale-util.h"
#include "libsd-time-util.h"

#include <libglnx.h>

#define RPMOSTREE_AUTOMATIC_TIMER_UNIT \
  "rpm-ostreed-automatic.timer"
#define RPMOSTREE_AUTOMATIC_SERVICE_UNIT          \
  "rpm-ostreed-automatic.service"
#define RPMOSTREE_AUTOMATIC_TIMER_OBJPATH \
  "/org/freedesktop/systemd1/unit/rpm_2dostreed_2dautomatic_2etimer"
#define RPMOSTREE_AUTOMATIC_SERVICE_OBJPATH \
  "/org/freedesktop/systemd1/unit/rpm_2dostreed_2dautomatic_2eservice"

#define OSTREE_FINALIZE_STAGED_SERVICE_UNIT \
  "ostree-finalize-staged.service"

#define SD_MESSAGE_UNIT_STOPPED_STR       SD_ID128_MAKE_STR(9d,1a,aa,27,d6,01,40,bd,96,36,54,38,aa,d2,02,86)

static gboolean opt_pretty;
static gboolean opt_verbose;
static gboolean opt_verbose_advisories;
static gboolean opt_json;
static gboolean opt_only_booted;
static const char *opt_jsonpath;
static gboolean opt_pending_exit_77;

static GOptionEntry option_entries[] = {
  { "pretty", 'p', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_pretty, "This option is deprecated and no longer has any effect", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print additional fields (e.g. StateRoot); implies -a", NULL },
  { "advisories", 'a', 0, G_OPTION_ARG_NONE, &opt_verbose_advisories, "Expand advisories listing", NULL },
  { "json", 0, 0, G_OPTION_ARG_NONE, &opt_json, "Output JSON", NULL },
  { "jsonpath", 'J', 0, G_OPTION_ARG_STRING, &opt_jsonpath, "Filter JSONPath expression", "EXPRESSION" },
  { "booted", 'b', 0, G_OPTION_ARG_NONE, &opt_only_booted, "Only print the booted deployment", NULL },
  { "pending-exit-77", 'b', 0, G_OPTION_ARG_NONE, &opt_pending_exit_77, "If pending deployment available, exit 77", NULL },
  { NULL }
};

/* return space available for printing value side of kv */
static guint
get_textarea_width (guint maxkeylen)
{
  const guint columns = glnx_console_columns ();
  /* +2 for initial leading spaces */
  const guint right_side_width = maxkeylen + 2 + strlen (": ");
  if (right_side_width >= columns)
    return G_MAXUINT; /* can't even print keys without wrapping, nothing pretty to do here */
  /* the sha is already 64 chars, so no point in trying to use less */
  return MAX(OSTREE_SHA256_STRING_LEN, columns - right_side_width);
}

static GVariant *
get_active_txn (RPMOSTreeSysroot *sysroot_proxy)
{
  GVariant* txn = rpmostree_sysroot_get_active_transaction (sysroot_proxy);
  const char *a, *b, *c;
  if (txn)
    {
      g_variant_get (txn, "(&s&s&s)", &a, &b, &c);
      if (*a)
        return txn;
    }
  return NULL;
}

static void
print_packages (const char *k, guint max_key_len,
                const char *const* pkgs,
                const char *const* omit_pkgs)
{
  g_autoptr(GPtrArray) packages_sorted = g_ptr_array_new_with_free_func (g_free);
  for (char **iter = (char**) pkgs; iter && *iter; iter++)
    {
      const char *pkg = *iter;
      if (omit_pkgs != NULL && g_strv_contains (omit_pkgs, pkg))
        continue;

      g_autofree char *quoted = rpmostree_maybe_shell_quote (pkg) ?: g_strdup (pkg);
      g_ptr_array_add (packages_sorted, util::move_nullify (quoted));
    }

  const guint n_packages = packages_sorted->len;
  if (n_packages == 0)
    return;

  rpmostree_print_kv_no_newline (k, max_key_len, "");

  /* wrap pkglist output ourselves rather than letting the terminal cut us up */
  const guint area_width = get_textarea_width (max_key_len);
  guint current_width = 0;
  for (guint i = 0; i < n_packages; i++)
    {
      auto pkg = static_cast<const char *>(packages_sorted->pdata[i]);
      const guint pkg_width = strlen (pkg);

      /* first print */
      if (current_width == 0)
        {
          g_print ("%s", pkg);
          current_width += pkg_width;
        }
      else if ((current_width + pkg_width + 1) <= area_width) /* +1 for space separator */
        {
          g_print (" %s", pkg);
          current_width += (pkg_width + 1);
        }
      else
        {
          /* always print at least one per line, even if we overflow */
          putc ('\n', stdout);
          rpmostree_print_kv_no_newline ("", max_key_len, pkg);
          current_width = pkg_width;
        }
    }
  putc ('\n', stdout);
}

static const gchar**
lookup_array_and_canonicalize (GVariantDict *dict,
                               const char   *key)
{
  g_autofree const gchar **ret = NULL;

  if (g_variant_dict_lookup (dict, key, "^a&s", &ret))
    {
      /* Canonicalize length 0 strv to NULL */
      if (!*ret)
        g_clear_pointer (&ret, g_free);
    }

  return util::move_nullify (ret);
}

static void
gv_nevra_to_evr (GString  *buffer,
                 GVariant *gv_nevra)
{
  guint64 epoch;
  const char *version, *release;
  g_variant_get (gv_nevra, "(sst&s&ss)", NULL, NULL, &epoch, &version, &release, NULL);
  rpmostree_custom_nevra (buffer, NULL, epoch, version, release, NULL,
                          PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE);
}

typedef enum {
  AUTO_UPDATE_SDSTATE_TIMER_UNKNOWN,
  AUTO_UPDATE_SDSTATE_TIMER_INACTIVE,
  AUTO_UPDATE_SDSTATE_SERVICE_FAILED,
  AUTO_UPDATE_SDSTATE_SERVICE_RUNNING,
  AUTO_UPDATE_SDSTATE_SERVICE_EXITED,
} AutoUpdateSdState;

static gboolean
get_last_auto_update_run (GDBusConnection   *connection,
                          AutoUpdateSdState *out_state,
                          char             **out_last_run,
                          GCancellable      *cancellable,
                          GError           **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Querying systemd for last auto-update run", error);

  /* Check if the timer is running, otherwise systemd won't even keep timestamp info on dead
   * services. Also good to tell users if the policy is not none, but timer is off (though
   * we don't print it as an error; e.g. the timer might have been explicitly masked). */
  g_autoptr(GDBusProxy) timer_unit_proxy =
    g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                           "org.freedesktop.systemd1", RPMOSTREE_AUTOMATIC_TIMER_OBJPATH,
                           "org.freedesktop.systemd1.Unit", cancellable, error);
  if (!timer_unit_proxy)
    return FALSE;

  g_autoptr(GVariant) timer_state_val =
    g_dbus_proxy_get_cached_property (timer_unit_proxy, "ActiveState");

  /* let's not error out if we can't msg systemd (e.g. bad sepol); just mark as unknown */
  if (timer_state_val == NULL)
    {
      *out_state = AUTO_UPDATE_SDSTATE_TIMER_UNKNOWN;
      return TRUE; /* NB early return */
    }

  const char *timer_state = g_variant_get_string (timer_state_val, NULL);
  if (g_str_equal (timer_state, "inactive"))
    {
      *out_state = AUTO_UPDATE_SDSTATE_TIMER_INACTIVE;
      return TRUE; /* NB early return */
    }

  g_autoptr(GDBusProxy) service_unit_proxy =
    g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                           "org.freedesktop.systemd1", RPMOSTREE_AUTOMATIC_SERVICE_OBJPATH,
                           "org.freedesktop.systemd1.Unit", cancellable, error);
  if (!service_unit_proxy)
    return FALSE;

  g_autoptr(GVariant) service_state_val =
    g_dbus_proxy_get_cached_property (service_unit_proxy, "ActiveState");
  const char *service_state = g_variant_get_string (service_state_val, NULL);
  if (g_str_equal (service_state, "failed"))
    {
      *out_state = AUTO_UPDATE_SDSTATE_SERVICE_FAILED;
      return TRUE; /* NB early return */
    }
  else if (g_str_equal (service_state, "active"))
    {
      *out_state = AUTO_UPDATE_SDSTATE_SERVICE_RUNNING;
      return TRUE; /* NB early return */
    }

  g_autoptr(GDBusProxy) service_proxy =
    g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                           "org.freedesktop.systemd1", RPMOSTREE_AUTOMATIC_SERVICE_OBJPATH,
                           "org.freedesktop.systemd1.Service", cancellable, error);
  if (!service_proxy)
    return FALSE;

  g_autoptr(GVariant) t_val =
    g_dbus_proxy_get_cached_property (service_proxy, "ExecMainExitTimestamp");

  g_autofree char *last_run = NULL;
  if (t_val)
    {
      guint64 t = g_variant_get_uint64 (t_val);
      if (t > 0)
        {
          char time_rel[FORMAT_TIMESTAMP_RELATIVE_MAX] = "";
          libsd_format_timestamp_relative (time_rel, sizeof(time_rel), t);
          last_run = g_strdup (time_rel);
        }
    }

  *out_state = AUTO_UPDATE_SDSTATE_SERVICE_EXITED;
  *out_last_run = util::move_nullify (last_run);
  return TRUE;
}

/* Get the ActiveState and StatusText properties of `update_driver_sd_unit`. ActiveState
 * (and StatusText if found) is returned as a single string in `update_driver_state` if
 * ActiveState is not empty. */
static gboolean
get_update_driver_state (RPMOSTreeSysroot *sysroot_proxy,
                         const char       *update_driver_sd_unit,
                         const char      **update_driver_state,
                         GCancellable     *cancellable,
                         GError          **error)
{
  GDBusConnection *connection =
    g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));

  const char *update_driver_objpath = NULL;
  if (!get_sd_unit_objpath (connection, "LoadUnit", g_variant_new ("(s)", update_driver_sd_unit),
                            &update_driver_objpath, cancellable, error))
    return FALSE;

  /* Look up ActiveState property of update driver's systemd unit. */
  g_autoptr(GDBusProxy) update_driver_unit_obj_proxy =
    g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                           "org.freedesktop.systemd1", update_driver_objpath,
                           "org.freedesktop.systemd1.Unit", cancellable, error);
  if (!update_driver_unit_obj_proxy)
    return FALSE;

  g_autoptr(GVariant) active_state_val =
    g_dbus_proxy_get_cached_property (update_driver_unit_obj_proxy, "ActiveState");
  if (!active_state_val)
    return glnx_throw (error, "ActiveState property not found in proxy's cache (%s)",
                       update_driver_objpath);

  const char *active_state = g_variant_get_string (active_state_val, NULL);

  /* Only look up StatusText property if update driver is a service unit. */
  const char *status_text = NULL;
  g_autoptr(GVariant) status_text_val = NULL;
  if (g_str_has_suffix (update_driver_sd_unit, ".service"))
    {
      g_autoptr(GDBusProxy) update_driver_service_obj_proxy =
        g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                               "org.freedesktop.systemd1", update_driver_objpath,
                               "org.freedesktop.systemd1.Service", cancellable, error);
      if (!update_driver_service_obj_proxy)
        return FALSE;

      status_text_val =
        g_dbus_proxy_get_cached_property (update_driver_service_obj_proxy, "StatusText");
      if (!status_text_val)
        return glnx_throw (error, "StatusText property not found in proxy's cache (%s)",
                           update_driver_objpath);

      status_text = g_variant_get_string (status_text_val, NULL);
    }

  if (active_state[0] != '\0')
    {
      /* Only print StatusText if not-NULL (is service unit) and not-empty. */
      if (status_text && status_text[0] != '\0')
        *update_driver_state = g_strdup_printf ("%s; %s", active_state, status_text);
      else
        *update_driver_state = g_strdup (active_state);
    }

  return TRUE;
}

static gboolean
print_daemon_state (RPMOSTreeSysroot *sysroot_proxy,
                    GCancellable     *cancellable,
                    GError          **error)
{
  glnx_unref_object RPMOSTreeTransaction *txn_proxy = NULL;
  if (!rpmostree_transaction_connect_active (sysroot_proxy, NULL, &txn_proxy,
                                             cancellable, error))
    return FALSE;

  const char *policy = rpmostree_sysroot_get_automatic_update_policy (sysroot_proxy);

  g_print ("State: %s\n", txn_proxy ? "busy" : "idle");

  rpmostreecxx::journal_print_staging_failure ();

  g_autofree char *update_driver_sd_unit = NULL;
  g_autofree char *update_driver_name = NULL;
  if (!get_driver_info (&update_driver_name, &update_driver_sd_unit, error))
    return FALSE;

  if (update_driver_name && update_driver_sd_unit)
    {
      if (opt_verbose)
        g_print ("AutomaticUpdatesDriver: %s (%s)\n", 
                 update_driver_name, update_driver_sd_unit);
      else
        g_print ("AutomaticUpdatesDriver: %s\n", update_driver_name);

      /* only try to get unit's StatusText if we're on the system bus */
      g_autofree const char *update_driver_state = NULL;
      g_autoptr(GError) local_error = NULL;
      if (!get_update_driver_state (sysroot_proxy, update_driver_sd_unit,
                                    &update_driver_state, cancellable, &local_error))
        g_printerr ("%s", local_error->message);
      else if (update_driver_state)
        g_print ("  DriverState: %s\n", update_driver_state);
    }
  else if (g_str_equal (policy, "none"))
    {
      /* https://github.com/coreos/fedora-coreos-tracker/issues/271
       * https://github.com/coreos/rpm-ostree/issues/1747
       */
      if (opt_verbose)
        g_print ("AutomaticUpdates: disabled\n");
    }
  else
    {
      g_print ("AutomaticUpdates: %s", policy);

      AutoUpdateSdState state;
      g_autofree char *last_run = NULL;
      g_print ("; ");
      GDBusConnection *connection =
        g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));
      if (!get_last_auto_update_run (connection, &state, &last_run, cancellable, error))
        return FALSE;
      switch (state)
        {
        case AUTO_UPDATE_SDSTATE_TIMER_UNKNOWN:
          {
            g_print ("%s: unknown state\n", RPMOSTREE_AUTOMATIC_TIMER_UNIT);
            break;
          }
        case AUTO_UPDATE_SDSTATE_TIMER_INACTIVE:
          {
            g_print ("%s: inactive\n", RPMOSTREE_AUTOMATIC_TIMER_UNIT);
            break;
          }
        case AUTO_UPDATE_SDSTATE_SERVICE_FAILED:
          {
            g_print ("%s: %s%slast run failed%s%s\n",
                     RPMOSTREE_AUTOMATIC_SERVICE_UNIT,
                     get_red_start (), get_bold_start (),
                     get_bold_end (), get_red_end ());
            break;
          }
        case AUTO_UPDATE_SDSTATE_SERVICE_RUNNING:
          {
            g_print ("%s: running\n", RPMOSTREE_AUTOMATIC_SERVICE_UNIT);
            break;
          }
        case AUTO_UPDATE_SDSTATE_SERVICE_EXITED:
          {
            if (last_run)
              /* e.g. "last run 4h 32min ago" */
              g_print ("%s: last run %s\n", RPMOSTREE_AUTOMATIC_TIMER_UNIT, last_run);
            else
              g_print ("%s: no runs since boot\n", RPMOSTREE_AUTOMATIC_TIMER_UNIT);
            break;
          }
        default:
          {
            g_assert_not_reached ();
          }
        }
    }

  if (txn_proxy)
    {
      const char *title = rpmostree_transaction_get_title (txn_proxy);
      g_print ("Transaction: %s\n", title);
      const char *client = rpmostree_transaction_get_initiating_client_description (txn_proxy);
      if (client && *client)
        {
          g_print ("  Initiator: %s\n", client);
        }
    }

  return TRUE;
}

/* Print the result of rpmostree_context_get_rpmmd_repo_commit_metadata() */
static void
print_origin_repos (gboolean host_endian,
                    guint maxkeylen, GVariantDict *commit_meta)
{
  g_autoptr(GVariant) reposdata =
    g_variant_dict_lookup_value (commit_meta, "rpmostree.rpmmd-repos", G_VARIANT_TYPE ("aa{sv}"));

  if (!reposdata)
    return;

  const guint n = g_variant_n_children (reposdata);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr(GVariant) child = g_variant_get_child_value (reposdata, i);
      g_autoptr(GVariantDict) cdict = g_variant_dict_new (child);

      const char *id = NULL;
      if (!g_variant_dict_lookup (cdict, "id", "&s", &id))
        continue;
      guint64 ts;
      if (!g_variant_dict_lookup (cdict, "timestamp", "t", &ts))
        continue;
      /* `compose tree` commits are canonicalized to BE, but client-side commits
       * are not.  Whee.
       */
      if (!host_endian)
        ts = GUINT64_FROM_BE (ts);
      g_autofree char *timestamp_string = rpmostree_timestamp_str_from_unix_utc (ts);
      g_print ("  %*s%s %s (%s)\n", maxkeylen + 2, " ",
               libsd_special_glyph (i == (n-1) ? TREE_RIGHT : TREE_BRANCH),
               id, timestamp_string);
    }
}

static gboolean
print_live_pkgdiff (const char *live_target, RpmOstreeDiffPrintFormat format, guint max_key_len, GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return FALSE;
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  g_assert (booted_deployment);

  const char *from_rev = ostree_deployment_get_csum (booted_deployment);

  gboolean have_target = FALSE;
  if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, live_target, &have_target, NULL, error))
    return FALSE;
  /* It might happen that the live target commit was GC'd somehow; we're not writing
   * an explicit ref for it.  In that case skip the diff.
   */
  if (!have_target)
    return TRUE;

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;
  if (!rpm_ostree_db_diff (repo, from_rev, live_target,
                           &removed, &added, &modified_old, &modified_new,
                           cancellable, error))
    return FALSE;
  rpmostree_diff_print_formatted (format, "Live", max_key_len,
                                  removed, added, modified_old, modified_new);
  return TRUE;
}

static gboolean
print_one_deployment (RPMOSTreeSysroot *sysroot_proxy,
                      GVariant         *child,
                      gboolean          first,
                      gboolean          have_any_live_overlay,
                      gboolean          have_multiple_stateroots,
                      const char       *booted_osname,
                      const char       *cached_update_deployment_id,
                      GVariant         *cached_update,
                      gboolean         *out_printed_cached_update,
                      GError          **error)
{
  /* Add the long keys here */
  const guint max_key_len = MAX (strlen ("InactiveBaseReplacements"),
                                 strlen ("InterruptedLiveCommit"));


  g_autoptr(GVariantDict) dict = g_variant_dict_new (child);

  /* osname should always be present. */
  const gchar *os_name;
  const gchar *id;
  int serial;
  const gchar *checksum;
  g_assert (g_variant_dict_lookup (dict, "osname", "&s", &os_name));
  g_assert (g_variant_dict_lookup (dict, "id", "&s", &id));
  g_assert (g_variant_dict_lookup (dict, "serial", "i", &serial));
  g_assert (g_variant_dict_lookup (dict, "checksum", "&s", &checksum));

  gboolean is_booted;
  if (!g_variant_dict_lookup (dict, "booted", "b", &is_booted))
    is_booted = FALSE;

  if (!is_booted && opt_only_booted)
    return TRUE;

  const gchar *origin_refspec;
  g_autofree const gchar **origin_packages = NULL;
  g_autofree const gchar **origin_modules = NULL;
  g_autofree const gchar **origin_modules_enabled = NULL;
  g_autofree const gchar **origin_requested_packages = NULL;
  g_autofree const gchar **origin_requested_modules = NULL;
  g_autofree const gchar **origin_requested_local_packages = NULL;
  g_autoptr(GVariant) origin_base_removals = NULL;
  g_autofree const gchar **origin_requested_base_removals = NULL;
  g_autoptr(GVariant) origin_base_local_replacements = NULL;
  g_autofree const gchar **origin_requested_base_local_replacements = NULL;
  if (g_variant_dict_lookup (dict, "origin", "&s", &origin_refspec))
    {
      origin_packages =
        lookup_array_and_canonicalize (dict, "packages");
      origin_modules =
        lookup_array_and_canonicalize (dict, "modules");
      origin_modules_enabled =
        lookup_array_and_canonicalize (dict, "modules-enabled");
      origin_requested_packages =
        lookup_array_and_canonicalize (dict, "requested-packages");
      origin_requested_modules =
        lookup_array_and_canonicalize (dict, "requested-modules");
      origin_requested_local_packages =
        lookup_array_and_canonicalize (dict, "requested-local-packages");
      origin_base_removals =
        g_variant_dict_lookup_value (dict, "base-removals", G_VARIANT_TYPE ("av"));
      origin_requested_base_removals =
        lookup_array_and_canonicalize (dict, "requested-base-removals");
      origin_base_local_replacements =
        g_variant_dict_lookup_value (dict, "base-local-replacements",
                                     G_VARIANT_TYPE ("a(vv)"));
      origin_requested_base_local_replacements =
        lookup_array_and_canonicalize (dict, "requested-base-local-replacements");
    }
  else
    origin_refspec = NULL;

  const gchar *version_string;
  if (!g_variant_dict_lookup (dict, "version", "&s", &version_string))
    version_string = NULL;
  const gchar *unlocked;
  if (!g_variant_dict_lookup (dict, "unlocked", "&s", &unlocked))
    unlocked = NULL;

  gboolean regenerate_initramfs;
  if (!g_variant_dict_lookup (dict, "regenerate-initramfs", "b", &regenerate_initramfs))
    regenerate_initramfs = FALSE;

  g_autoptr(GVariant) signatures =
    g_variant_dict_lookup_value (dict, "signatures", G_VARIANT_TYPE ("av"));

  if (!first && !opt_only_booted)
    g_print ("\n");

  g_print ("%s ", is_booted ? libsd_special_glyph (BLACK_CIRCLE) : " ");

  RpmOstreeRefspecType refspectype = RPMOSTREE_REFSPEC_TYPE_OSTREE;
  const char *custom_origin_url = NULL;
  const char *custom_origin_description = NULL;
  if (origin_refspec)
    {
      const char *refspec_data;
      if (!rpmostree_refspec_classify (origin_refspec, &refspectype, &refspec_data, error))
        return FALSE;
      g_autofree char *canonrefspec = rpmostree_refspec_to_string (refspectype, refspec_data);
      switch (refspectype)
        {
        case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
          {
            g_variant_dict_lookup (dict, "custom-origin", "(&s&s)",
                                   &custom_origin_url,
                                   &custom_origin_description);
            /* Canonicalize the empty string to NULL */
            if (custom_origin_url && !*custom_origin_url)
              custom_origin_url = NULL;
            if (custom_origin_url)
              {
                g_assert (custom_origin_description && *custom_origin_description);
                g_print ("%s", custom_origin_url);
              }
            else
              g_print ("%s", canonrefspec);
          }
          break;
        case RPMOSTREE_REFSPEC_TYPE_OSTREE:
          {
            g_print ("%s", canonrefspec);
          }
          break;
        }
    }
  else
    g_print ("%s", checksum);
  g_print ("\n");

  if (custom_origin_description)
    rpmostree_print_kv ("CustomOrigin", max_key_len, custom_origin_description);

  const char *remote_not_found = NULL;
  g_variant_dict_lookup (dict, "remote-error", "s", &remote_not_found);
  if (remote_not_found)
    {
      g_print ("%s%s", get_red_start (), get_bold_start ());
      rpmostree_print_kv ("OstreeRemoteStatus", max_key_len, remote_not_found);
      g_print ("%s%s", get_bold_end (), get_red_end ());
    }

  const char *base_checksum = NULL;
  g_variant_dict_lookup (dict, "base-checksum", "&s", &base_checksum);
  gboolean is_locally_assembled = FALSE;
  if (base_checksum != NULL)
    is_locally_assembled = TRUE;

  /* Load the commit metadata into a dict */
  g_autoptr(GVariantDict) commit_meta_dict =
    ({ g_autoptr(GVariant) commit_meta_v = NULL;
      g_assert (g_variant_dict_lookup (dict, "base-commit-meta", "@a{sv}", &commit_meta_v));
      g_variant_dict_new (commit_meta_v); });
  g_autoptr(GVariantDict) layered_commit_meta_dict = NULL;
  if (is_locally_assembled)
    {
      g_autoptr(GVariant) layered_commit_meta_v = NULL;
      g_assert (g_variant_dict_lookup (dict, "layered-commit-meta", "@a{sv}", &layered_commit_meta_v));
      layered_commit_meta_dict = g_variant_dict_new (layered_commit_meta_v);
    }

  const gchar *source_title = NULL;
  g_variant_dict_lookup (commit_meta_dict, OSTREE_COMMIT_META_KEY_SOURCE_TITLE, "&s", &source_title);
  if (source_title)
    g_print ("  %s %s\n", libsd_special_glyph (TREE_RIGHT), source_title);

  guint64 t = 0;
  if (is_locally_assembled)
    g_assert (g_variant_dict_lookup (dict, "base-timestamp", "t", &t));
  else
    g_assert (g_variant_dict_lookup (dict, "timestamp", "t", &t));
  g_autofree char *timestamp_string = rpmostree_timestamp_str_from_unix_utc (t);

  rpmostree_print_timestamp_version (version_string, timestamp_string, max_key_len);

  const gchar *live_inprogress;
  const gchar *live_replaced;
  if (!g_variant_dict_lookup (dict, "live-inprogress", "&s", &live_inprogress))
    live_inprogress = NULL;
  if (!g_variant_dict_lookup (dict, "live-replaced", "&s", &live_replaced))
    live_replaced = NULL;
  const gboolean have_live_changes = live_inprogress || live_replaced;

  const gboolean is_ostree_or_verbose = opt_verbose || refspectype == RPMOSTREE_REFSPEC_TYPE_OSTREE;
  const RpmOstreeDiffPrintFormat diff_format =
        opt_verbose ? RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED
                    : RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY;

  if (is_locally_assembled && is_ostree_or_verbose)
    {
      if (have_live_changes)
        rpmostree_print_kv ("BootedBaseCommit", max_key_len, base_checksum);
      else
        rpmostree_print_kv ("BaseCommit", max_key_len, base_checksum);
      if (opt_verbose)
        print_origin_repos (FALSE, max_key_len, commit_meta_dict);
      if (opt_verbose || have_any_live_overlay)
        rpmostree_print_kv ("Commit", max_key_len, checksum);
      if (opt_verbose)
        print_origin_repos (TRUE, max_key_len, layered_commit_meta_dict);
    }
  else if (is_ostree_or_verbose)
    {
      if (have_live_changes)
        rpmostree_print_kv ("BootedCommit", max_key_len, checksum);
      if (!have_live_changes || opt_verbose)
        rpmostree_print_kv ("Commit", max_key_len, checksum);
      if (opt_verbose)
        print_origin_repos (FALSE, max_key_len, commit_meta_dict);
    }

  if (live_inprogress)
    {
      if (is_booted)
        g_print ("%s%s", get_red_start (), get_bold_start ());
      rpmostree_print_kv ("InterruptedLiveCommit", max_key_len, live_inprogress);
      if (is_booted)
        g_print ("%s%s", get_bold_end (), get_red_end ());
    }
  if (live_replaced)
    {
      if (is_booted)
        g_print ("%s%s", get_red_start (), get_bold_start ());
      rpmostree_print_kv ("LiveCommit", max_key_len, live_replaced);
      if (is_booted)
        g_print ("%s%s", get_bold_end (), get_red_end ());
      if (!print_live_pkgdiff (live_replaced, diff_format, max_key_len, NULL, error))
        return FALSE;
    }

  gboolean is_staged = FALSE;
  g_variant_dict_lookup (dict, "staged", "b", &is_staged);

  if (opt_verbose && (is_staged || first))
    rpmostree_print_kv ("Staged", max_key_len, is_staged ? "yes" : "no");

  /* This used to be OSName; see https://github.com/ostreedev/ostree/pull/794 */
  if (opt_verbose || have_multiple_stateroots)
    rpmostree_print_kv ("StateRoot", max_key_len, os_name);

  gboolean gpg_enabled;
  if (!g_variant_dict_lookup (dict, "gpg-enabled", "b", &gpg_enabled))
    gpg_enabled = FALSE;

  if (gpg_enabled)
    rpmostree_print_gpg_info (signatures, opt_verbose, max_key_len);

  gboolean is_pending_deployment =
    (first && !is_booted && g_strcmp0 (os_name, booted_osname) == 0);

  /* Print rpm diff and advisories summary if this is a pending deployment matching the
   * deployment on which the cached update is based. */
  if (is_pending_deployment && g_strcmp0 (id, cached_update_deployment_id) == 0)
    {
      g_auto(GVariantDict) dict;
      g_variant_dict_init (&dict, cached_update);
      g_autoptr(GVariant) rpm_diff =
        g_variant_dict_lookup_value (&dict, "rpm-diff", G_VARIANT_TYPE ("a{sv}"));
      g_autoptr(GVariant) advisories =
        g_variant_dict_lookup_value (&dict, "advisories", G_VARIANT_TYPE ("a(suuasa{sv})"));
      if (!rpmostree_print_diff_advisories (rpm_diff, advisories, opt_verbose,
                                            opt_verbose_advisories, max_key_len, error))
        return FALSE;
      if (out_printed_cached_update)
        *out_printed_cached_update= TRUE;
    }
  else if (is_pending_deployment && sysroot_proxy)
    {
      /* No cached update, but we can still print a diff summary */
      const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
      rpmostreecxx::print_treepkg_diff_from_sysroot_path (rust::Str(sysroot_path), diff_format,
                                                          max_key_len, NULL);
    }

  /* print base overrides before overlays */
  g_autoptr(GPtrArray) active_removals = g_ptr_array_new_with_free_func (g_free);
  if (origin_base_removals)
    {
      g_autoptr(GString) str = g_string_new ("");
      g_autoptr(GHashTable) grouped_evrs =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                               (GDestroyNotify)g_ptr_array_unref);

      const guint n = g_variant_n_children (origin_base_removals);
      for (guint i = 0; i < n; i++)
        {
          g_autoptr(GVariant) gv_nevra;
          g_variant_get_child (origin_base_removals, i, "v", &gv_nevra);
          const char *name;
          g_variant_get_child (gv_nevra, 1, "&s", &name);
          g_ptr_array_add (active_removals, g_strdup (name));

          gv_nevra_to_evr (str, gv_nevra);
          const char *evr = str->str;
          auto pkgs = static_cast<GPtrArray *>(g_hash_table_lookup (grouped_evrs, evr));
          if (!pkgs)
            {
              pkgs = g_ptr_array_new_with_free_func (g_free);
              g_hash_table_insert (grouped_evrs, g_strdup (evr), pkgs);
            }
          g_ptr_array_add (pkgs, g_strdup (name));
          g_string_erase (str, 0, -1);
        }
      g_ptr_array_add (active_removals, NULL);

      GLNX_HASH_TABLE_FOREACH_KV (grouped_evrs, const char*, evr, GPtrArray*, pkgs)
        {
          if (str->len)
            g_string_append (str, ", ");

          for (guint i = 0, n = pkgs->len; i < n; i++)
            {
              auto pkgname = static_cast<const char *>(g_ptr_array_index (pkgs, i));
              if (i > 0)
                g_string_append_c (str, ' ');
              g_string_append (str, pkgname);
            }
          g_string_append_printf (str, " %s", evr);
        }

      if (str->len)
        rpmostree_print_kv ("RemovedBasePackages", max_key_len, str->str);
    }

  /* only print inactive base removal requests in verbose mode */
  if (origin_requested_base_removals && opt_verbose)
    print_packages ("InactiveBaseRemovals", max_key_len,
                    origin_requested_base_removals,
                    (const char *const*)active_removals->pdata);

  g_autoptr(GPtrArray) active_replacements = g_ptr_array_new_with_free_func (g_free);
  if (origin_base_local_replacements)
    {
      g_autoptr(GString) str = g_string_new ("");
      g_autoptr(GHashTable) grouped_diffs =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                               (GDestroyNotify)g_ptr_array_unref);

      const guint n = g_variant_n_children (origin_base_local_replacements);
      for (guint i = 0; i < n; i++)
        {
          g_autoptr(GVariant) gv_nevra_new;
          g_autoptr(GVariant) gv_nevra_old;
          g_variant_get_child (origin_base_local_replacements, i, "(vv)",
                               &gv_nevra_new, &gv_nevra_old);
          const char *nevra_new, *name_new, *name_old;
          g_variant_get_child (gv_nevra_new, 0, "&s", &nevra_new);
          g_variant_get_child (gv_nevra_new, 1, "&s", &name_new);
          g_variant_get_child (gv_nevra_old, 1, "&s", &name_old);

          /* if pkgnames match, print a nicer version like treediff */
          if (g_str_equal (name_new, name_old))
            {
              /* let's just use str as a scratchpad to avoid excessive mallocs; the str
               * needs to be stretched anyway for the final output */
              gsize original_size = str->len;
              gv_nevra_to_evr (str, gv_nevra_old);
              g_string_append (str, " -> ");
              gv_nevra_to_evr (str, gv_nevra_new);
              const char *diff = str->str + original_size;
              auto pkgs = static_cast<GPtrArray *>(g_hash_table_lookup (grouped_diffs, diff));
              if (!pkgs)
                {
                  pkgs = g_ptr_array_new_with_free_func (g_free);
                  g_hash_table_insert (grouped_diffs, g_strdup (diff), pkgs);
                }
              g_ptr_array_add (pkgs, g_strdup (name_new));
              g_string_truncate (str, original_size);
            }
          else
            {
              if (str->len)
                g_string_append (str, ", ");

              const char *nevra_old;
              g_variant_get_child (gv_nevra_old, 0, "&s", &nevra_old);
              g_string_append_printf (str, "%s -> %s", nevra_old, nevra_new);
            }
          g_ptr_array_add (active_replacements, g_strdup (nevra_new));
        }

      GLNX_HASH_TABLE_FOREACH_KV (grouped_diffs, const char*, diff, GPtrArray*, pkgs)
        {
          if (str->len)
            g_string_append (str, ", ");
          for (guint i = 0, n = pkgs->len; i < n; i++)
            {
              auto pkgname = static_cast<const char *>(g_ptr_array_index (pkgs, i));
              if (i > 0)
                g_string_append_c (str, ' ');
              g_string_append (str, pkgname);
            }
          g_string_append_c (str, ' ');
          g_string_append (str, diff);
        }

      g_ptr_array_add (active_replacements, NULL);

      if (str->len)
        rpmostree_print_kv ("ReplacedBasePackages", max_key_len, str->str);
    }

  if (origin_requested_base_local_replacements && opt_verbose)
    print_packages ("InactiveBaseReplacements", max_key_len,
                    origin_requested_base_local_replacements,
                    (const char *const*)active_replacements->pdata);

  /* only print inactive layering requests in verbose mode */
  if (origin_requested_packages && opt_verbose)
    /* requested-packages - packages = inactive (i.e. dormant requests) */
    print_packages ("InactiveRequests", max_key_len,
                    origin_requested_packages, origin_packages);
  if (origin_requested_modules && opt_verbose)
    /* requested-modules - modules = inactive (i.e. dormant requests) */
    /* note the core doesn't support inactive modules yet, but could in the future */
    print_packages ("InactiveModuleRequests", max_key_len,
                    origin_requested_modules, origin_modules);

  if (origin_packages)
    print_packages ("LayeredPackages", max_key_len,
                    origin_packages, NULL);
  if (origin_modules)
    print_packages ("LayeredModules", max_key_len,
                    origin_modules, NULL);
  if (origin_modules_enabled)
    print_packages ("EnabledModules", max_key_len,
                    origin_modules_enabled, NULL);

  if (origin_requested_local_packages)
    print_packages ("LocalPackages", max_key_len,
                    origin_requested_local_packages, NULL);

  if (regenerate_initramfs)
    {
      g_autoptr(GString) buf = g_string_new ("");
      g_autofree char **initramfs_args = NULL;

      g_variant_dict_lookup (dict, "initramfs-args", "^a&s", &initramfs_args);

      for (char **iter = initramfs_args; iter && *iter; iter++)
        {
          const char *arg = *iter;
          g_autofree char *quoted = rpmostree_maybe_shell_quote (arg);
          g_string_append (buf, quoted ?: arg);
          g_string_append_c (buf, ' ');
        }
      if (buf->len == 0)
        g_string_append (buf, "regenerate");
      rpmostree_print_kv ("Initramfs", max_key_len, buf->str);
    }
  
  gboolean cliwrap = FALSE;
  if (g_variant_dict_lookup (dict, "cliwrap", "b", &cliwrap) && cliwrap)
    rpmostree_print_kv ("Cliwrap", max_key_len, "enabled");

  g_autofree char **initramfs_etc_files = NULL;
  g_variant_dict_lookup (dict, "initramfs-etc", "^a&s", &initramfs_etc_files);
  if (initramfs_etc_files && *initramfs_etc_files)
    /* XXX: not really packages but it works... should just rename that function */
    print_packages ("InitramfsEtc", max_key_len, (const char**)initramfs_etc_files, NULL);

  gboolean pinned = FALSE;
  g_variant_dict_lookup (dict, "pinned", "b", &pinned);
  if (pinned)
    rpmostree_print_kv ("Pinned", max_key_len, "yes");

  if (unlocked && g_strcmp0 (unlocked, "none") != 0)
    {
      g_print ("%s%s", get_red_start (), get_bold_start ());
      rpmostree_print_kv ("Unlocked", max_key_len, unlocked);
      g_print ("%s%s", get_bold_end (), get_red_end ());
    }
  const char *end_of_life_string = NULL;
  /* look for endoflife attribute in the deployment */
  g_variant_dict_lookup (dict, "endoflife", "&s", &end_of_life_string);

  if (end_of_life_string)
    {
      g_print ("%s%s", get_red_start (), get_bold_start ());
      rpmostree_print_kv ("EndOfLife", max_key_len, end_of_life_string);
      g_print ("%s%s", get_bold_end (), get_red_end ());
    }

  return TRUE;
}

/* We will have an optimized path for the case where there are just
 * two deployments, this code will be the generic fallback.
 */
static gboolean
print_deployments (RPMOSTreeSysroot *sysroot_proxy,
                   GVariant         *deployments,
                   GVariant         *cached_update,
                   gboolean         *out_printed_cached_update,
                   GCancellable     *cancellable,
                   GError          **error)
{
  GVariantIter iter;

  /* First, gather global state */
  const char *booted_osname = NULL;
  gboolean have_any_live_overlay = FALSE;
  gboolean have_multiple_stateroots = FALSE;
  const char *last_osname = NULL;
  g_variant_iter_init (&iter, deployments);
  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);

      if (!child)
        break;

      g_autoptr(GVariantDict) dict = g_variant_dict_new (child);

      const gchar *live_inprogress;
      if (!g_variant_dict_lookup (dict, "live-inprogress", "&s", &live_inprogress))
        live_inprogress = NULL;
      const gchar *live_replaced;
      if (!g_variant_dict_lookup (dict, "live-replaced", "&s", &live_replaced))
        live_replaced = NULL;
      const gboolean have_live_changes = live_inprogress || live_replaced;

      have_any_live_overlay = have_any_live_overlay || have_live_changes;

      const char *osname = NULL;
      g_assert (g_variant_dict_lookup (dict, "osname", "&s", &osname));
      if (!last_osname)
        last_osname = osname;
      else if (!g_str_equal (osname, last_osname))
        have_multiple_stateroots = TRUE;

      gboolean is_booted;
      if (!g_variant_dict_lookup (dict, "booted", "b", &is_booted))
        is_booted = FALSE;

      if (is_booted)
        booted_osname = osname;
    }

  if (opt_only_booted)
    g_print ("BootedDeployment:\n");
  else
    g_print ("Deployments:\n");

  /* just unpack this so that each iteration doesn't have to dig for it */
  const char *cached_update_deployment_id = NULL;
  if (cached_update)
    {
      g_auto(GVariantDict) dict;
      g_variant_dict_init (&dict, cached_update);
      g_variant_dict_lookup (&dict, "deployment", "&s", &cached_update_deployment_id);
    }

  g_variant_iter_init (&iter, deployments);

  gboolean first = TRUE;
  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
      if (child == NULL)
        break;

      if (!print_one_deployment (sysroot_proxy, child, first, have_any_live_overlay,
                                 have_multiple_stateroots, booted_osname,
                                 cached_update_deployment_id, cached_update,
                                 out_printed_cached_update, error))
        return FALSE;
      if (first)
        first = FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_builtin_status (int             argc,
                          char          **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable   *cancellable,
                          GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       error))
    return FALSE;

  if (opt_json && opt_jsonpath)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --json and --jsonpath");
      return FALSE;
    }

  if (!rpmostree_load_os_proxy (sysroot_proxy, NULL, cancellable, &os_proxy, error))
    return FALSE;

  g_autoptr(GVariant) deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);
  g_assert (deployments);
  g_autoptr(GVariant) cached_update = NULL;
  if (rpmostree_os_get_has_cached_update_rpm_diff (os_proxy))
    cached_update = rpmostree_os_dup_cached_update (os_proxy);
  g_autoptr(GVariant) driver_info = NULL;
  if (!get_driver_g_variant (&driver_info, error))
    return FALSE;

  if (opt_json || opt_jsonpath)
    {
      glnx_unref_object JsonBuilder *builder = json_builder_new ();
      json_builder_begin_object (builder);

      json_builder_set_member_name (builder, "deployments");
      json_builder_add_value (builder, json_gvariant_serialize (deployments));
      json_builder_set_member_name (builder, "transaction");
      GVariant *txn = get_active_txn (sysroot_proxy);
      JsonNode *txn_node =
        txn ? json_gvariant_serialize (txn) : json_node_new (JSON_NODE_NULL);
      json_builder_add_value (builder, txn_node);
      json_builder_set_member_name (builder, "cached-update");
      JsonNode *cached_update_node;
      if (cached_update)
        cached_update_node = json_gvariant_serialize (cached_update);
      else
        cached_update_node = json_node_new (JSON_NODE_NULL);
      json_builder_add_value (builder, cached_update_node);
      json_builder_set_member_name (builder, "update-driver");
      JsonNode *update_driver_node =
        driver_info ? json_gvariant_serialize (driver_info) : json_node_new (JSON_NODE_NULL);
      json_builder_add_value (builder, update_driver_node);
      json_builder_end_object (builder);

      JsonNode *json_root = json_builder_get_root (builder);
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_pretty (generator, TRUE);

      if (opt_json)
        json_generator_set_root (generator, json_root);
      else
        {
          JsonNode *result = json_path_query (opt_jsonpath, json_root, error);
          if (!result)
            {
              g_prefix_error (error, "While compiling jsonpath: ");
              return FALSE;
            }
          json_generator_set_root (generator, result);
          json_node_free (result);
        }
      json_node_free (json_root);

      glnx_unref_object GOutputStream *stdout_gio = g_unix_output_stream_new (1, FALSE);
      /* NB: watch out for the misleading API docs */
      if (json_generator_to_stream (generator, stdout_gio, NULL, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;
    }
  else
    {
      if (!print_daemon_state (sysroot_proxy, cancellable, error))
        return FALSE;

      gboolean printed_cached_update = FALSE;
      if (!print_deployments (sysroot_proxy, deployments, cached_update,
                              &printed_cached_update, cancellable, error))
        return FALSE;

      const char *policy = rpmostree_sysroot_get_automatic_update_policy (sysroot_proxy);
      gboolean auto_updates_enabled = (!g_str_equal (policy, "none"));
      if (cached_update && !printed_cached_update && auto_updates_enabled)
        {
          g_print ("\n");
          if (!rpmostree_print_cached_update (cached_update, opt_verbose,
                                              opt_verbose_advisories, cancellable, error))
            return FALSE;
        }
    }

  if (opt_pending_exit_77)
    {
      if (g_variant_n_children (deployments) > 1)
        {
          g_autoptr(GVariant) pending = g_variant_get_child_value (deployments, 0);
          g_auto(GVariantDict) dict;
          g_variant_dict_init (&dict, pending);
          gboolean is_booted;
          if (g_variant_dict_lookup (&dict, "booted", "b", &is_booted) && !is_booted)
            invocation->exit_code = RPM_OSTREE_EXIT_PENDING;
        }
    }

  return TRUE;
}

/* XXX
static gboolean opt_no_pager;
static gboolean opt_deployments;
*/
static int opt_limit = 3;
static gboolean opt_all;

static GOptionEntry history_option_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print additional fields (e.g. StateRoot)", NULL },
  { "json", 0, 0, G_OPTION_ARG_NONE, &opt_json, "Output JSON", NULL },
  { "limit", 'n', 0, G_OPTION_ARG_INT, &opt_limit, "Limit number of entries to output (default: 3)", "N" },
  { "all", 0, 0, G_OPTION_ARG_NONE, &opt_all, "Output all entries", NULL },
  /* XXX { "deployments", 0, 0, G_OPTION_ARG_NONE, &opt_deployments, "Print all deployments, not just those booted into", NULL }, */
  /* XXX { "no-pager", 0, 0, G_OPTION_ARG_NONE, &opt_no_pager, "Don't use a pager to display output", NULL }, */
  { NULL }
};

/* Read from history db, sets @out_deployment to NULL on ENOENT. */
static gboolean
fetch_history_deployment_gvariant (const rpmostreecxx::HistoryEntry &entry,
                                   GVariant        **out_deployment,
                                   GError          **error)
{
  g_autofree char *fn =
    g_strdup_printf ("%s/%" PRIu64, RPMOSTREE_HISTORY_DIR, entry.deploy_timestamp);

  *out_deployment = NULL;

  glnx_autofd int fd = -1;
  g_autoptr(GError) local_error = NULL;
  if (!glnx_openat_rdonly (AT_FDCWD, fn, TRUE, &fd, &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return g_propagate_error (error, util::move_nullify (local_error)), FALSE;
      return TRUE; /* Note early return */
    }

  g_autoptr(GBytes) data = glnx_fd_readall_bytes (fd, NULL, error);
  if (!data)
    return FALSE;

  *out_deployment =
    g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT, data, FALSE));
  return TRUE;
}

static void
print_timestamp_and_relative (const char* key, guint64 t)
{
  g_autofree char *ts = rpmostree_timestamp_str_from_unix_utc (t);
  char time_rel[FORMAT_TIMESTAMP_RELATIVE_MAX] = "";
  libsd_format_timestamp_relative (time_rel, sizeof(time_rel), t * USEC_PER_SEC);
  if (key)
    g_print ("%s: ", key);
  g_print ("%s (%s)\n", ts, time_rel);
}

static gboolean
print_history_entry (const rpmostreecxx::HistoryEntry &entry,
                     GError          **error)
{
  g_autoptr(GVariant) deployment = NULL;
  if (!fetch_history_deployment_gvariant (entry, &deployment, error))
    return FALSE;

  if (!opt_json)
    {
      print_timestamp_and_relative ("BootTimestamp", entry.last_boot_timestamp);
      if (entry.boot_count > 1)
        {
          g_print ("%s BootCount: %" PRIu64 "; first booted on ",
                   libsd_special_glyph (TREE_RIGHT), entry.boot_count);
          print_timestamp_and_relative (NULL, entry.first_boot_timestamp);
        }

      print_timestamp_and_relative ("CreateTimestamp", entry.deploy_timestamp);
      if (entry.deploy_cmdline.length() > 0)
        {
          // Copy so we can use c_str()
          auto cmdline_copy = std::string(entry.deploy_cmdline);
          g_print ("CreateCommand: %s%s%s\n",
                   get_bold_start (), cmdline_copy.c_str(), get_bold_end ());
        }
      if (!deployment)
        /* somehow we're missing an entry? XXX: just fallback to checksum, version, refspec
         * from journal entry in this case */
        g_print ("  << Missing history information >>\n");

      /* XXX: factor out interesting bits from print_one_deployment() */
      else if (!print_one_deployment (NULL, deployment, TRUE, FALSE, FALSE,
                                      NULL, NULL, NULL, NULL, error))
        return FALSE;
    }
  else
    {
      /* NB: notice we implicitly print as a stream of objects rather than an array */

      glnx_unref_object JsonBuilder *builder = json_builder_new ();
      json_builder_begin_object (builder);

      if (deployment)
        {
          json_builder_set_member_name (builder, "deployment");
          json_builder_add_value (builder, json_gvariant_serialize (deployment));
        }

      json_builder_set_member_name (builder, "deployment-create-timestamp");
      json_builder_add_int_value (builder, entry.deploy_timestamp);
      if (entry.deploy_cmdline.length() > 0)
        {
          json_builder_set_member_name (builder, "deployment-create-command-line"); 
          // Copy so we can use c_str()
          auto cmdline_copy = std::string(entry.deploy_cmdline);
          json_builder_add_string_value (builder, cmdline_copy.c_str()); 
        }
      json_builder_set_member_name (builder, "boot-count");
      json_builder_add_int_value (builder, entry.boot_count);
      json_builder_set_member_name (builder, "first-boot-timestamp");
      json_builder_add_int_value (builder, entry.first_boot_timestamp);
      json_builder_set_member_name (builder, "last-boot-timestamp");
      json_builder_add_int_value (builder, entry.last_boot_timestamp);
      json_builder_end_object (builder);

      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_pretty (generator, TRUE);
      json_generator_set_root (generator, json_builder_get_root (builder));
      glnx_unref_object GOutputStream *stdout_gio = g_unix_output_stream_new (1, FALSE);
      /* NB: watch out for the misleading API docs */
      if (json_generator_to_stream (generator, stdout_gio, NULL, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;
    }

  g_print ("\n");
  return TRUE;
}

/* The `history` command also lives here since the printing bits re-use a lot of the
 * `status` machinery. */
gboolean
rpmostree_ex_builtin_history (int             argc,
                              char          **argv,
                              RpmOstreeCommandInvocation *invocation,
                              GCancellable   *cancellable,
                              GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  if (!rpmostree_option_context_parse (context,
                                       history_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (opt_limit <= 0)
    return glnx_throw (error, "Limit must be positive integer");

  /* initiate a history context, then iterate over each (boot time, deploy time), then print */

  /* XXX: enhance with option for going in reverse (oldest first) */
  auto history_ctx = rpmostreecxx::history_ctx_new ();

  /* XXX: use pager here */

  gboolean at_least_one = FALSE;
  while (opt_all || opt_limit--)
    {
      auto entry = history_ctx->next_entry();
      if (entry.eof)
        break;
      if (!print_history_entry (entry, error))
        return FALSE;
      at_least_one = TRUE;
    }

  if (!at_least_one)
    g_print ("<< No entries found >>\n");

  return TRUE;
}
