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

#include <signal.h>
#include <sys/socket.h>
#include <systemd/sd-login.h>

#include <glib-unix.h>
#include <libglnx.h>

#include "rpmostree-types.h"
#include "rpmostree-dbus-helpers.h"
#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-util.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-rust.h"

#define RPMOSTREE_CLI_ID "cli"

void
rpmostree_cleanup_peer (GPid *peer_pid)
{
  if (*peer_pid > 0)
    kill (*peer_pid, SIGTERM);
}

static GDBusConnection*
get_connection_for_path (gchar *sysroot,
                         gboolean force_peer,
                         GPid *out_peer_pid,
                         GBusType *out_bus_type,
                         GCancellable *cancellable,
                         GError **error)
{
  /* This is only intended for use by installed tests.
   * Note that it disregards the 'sysroot' and 'force_peer' options
   * and assumes the service activation command has been configured
   * to use the desired system root path. */
  if (g_getenv ("RPMOSTREE_USE_SESSION_BUS") != NULL)
    {
      if (sysroot != NULL)
        g_warning ("RPMOSTREE_USE_SESSION_BUS set, ignoring --sysroot=%s", sysroot);

      /* NB: as opposed to other early returns, this is _also_ a happy path */
      GDBusConnection *ret = g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, error);
      if (!ret)
        return glnx_prefix_error_null (error, "Connecting to session bus");

      *out_bus_type = G_BUS_TYPE_SESSION;
      return ret;
    }

  if (sysroot == NULL)
    sysroot = "/";

  if (g_strcmp0 ("/", sysroot) == 0 && force_peer == FALSE)
    {
      /* NB: as opposed to other early returns, this is _also_ a happy path */
      GDBusConnection *ret = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
      if (!ret)
        return glnx_prefix_error_null (error, "Connecting to system bus");

      *out_bus_type = G_BUS_TYPE_SYSTEM;
      return ret;
    }

  gchar buffer[16];
  int pair[2];
  const gchar *args[] = {
    "rpm-ostree",
    "start-daemon",
    "--sysroot", sysroot,
    "--dbus-peer", buffer,
    NULL
  };

  g_print ("Running in single user mode. Be sure no other users are modifying the system\n");
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, pair) < 0)
    return glnx_null_throw_errno_prefix (error, "couldn't create socket pair");

  g_snprintf (buffer, sizeof (buffer), "%d", pair[1]);

  g_autoptr(GSocket) socket = g_socket_new_from_fd (pair[0], error);
  if (socket == NULL)
    {
      close (pair[0]);
      close (pair[1]);
      return NULL;
    }

  _cleanup_peer_ GPid peer_pid = 0;
  if (!g_spawn_async (NULL, (gchar **)args, NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &peer_pid, error))
    {
      close (pair[1]);
      return NULL;
    }

  g_autoptr(GSocketConnection) stream =
    g_socket_connection_factory_create_connection (socket);
  g_autoptr(GDBusConnection) connection =
    g_dbus_connection_new_sync (G_IO_STREAM (stream), NULL,
                                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                NULL, cancellable, error);
  if (!connection)
    return NULL;

  *out_peer_pid = peer_pid; peer_pid = 0;
  *out_bus_type = G_BUS_TYPE_NONE;
  return connection;
}

/* Used to close race conditions by ensuring the daemon status is up-to-date */
static void
on_reload_done (GObject      *src,
                GAsyncResult *res,
                gpointer      user_data)
{
  gboolean *donep = user_data;
  *donep = TRUE;
  (void) rpmostree_sysroot_call_reload_finish ((RPMOSTreeSysroot*)src, res, NULL);
}


/* This is an async call so that gdbus handles signals for changed
 * properties. */
static void
await_reload_sync (RPMOSTreeSysroot *sysroot_proxy)
{
  gboolean done = FALSE;
  rpmostree_sysroot_call_reload (sysroot_proxy, NULL, on_reload_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

/**
* rpmostree_load_sysroot
* @sysroot: sysroot path
* @force_peer: Force a peer connection
* @cancellable: A GCancellable
* @out_sysroot: (out) Return location for sysroot
* @error: A pointer to a GError pointer.
*
* Returns: True on success
**/
gboolean
rpmostree_load_sysroot (gchar *sysroot,
                        gboolean force_peer,
                        GCancellable *cancellable,
                        RPMOSTreeSysroot **out_sysroot_proxy,
                        GPid *out_peer_pid,
                        GBusType *out_bus_type,
                        GError **error)
{
  const char *bus_name = NULL;
  glnx_unref_object GDBusConnection *connection = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  GBusType bus_type;
  connection = get_connection_for_path (sysroot, force_peer, &peer_pid, &bus_type,
                                        cancellable, error);
  if (connection == NULL)
    return FALSE;

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  /* Try to register if we can; it doesn't matter much now since the daemon doesn't
   * auto-exit, though that might change in the future. But only register if we're active or
   * root; the daemon won't allow it otherwise. */
  uid_t uid = getuid ();
  gboolean should_register;
  if (uid == 0)
    should_register = TRUE;
  else
    {
      g_autofree char *state = NULL;
      if (sd_uid_get_state (uid, &state) >= 0)
        should_register = (g_strcmp0 (state, "active") == 0);
      else
        should_register = FALSE;
    }

  /* First, call RegisterClient directly for the well-known name, to
   * cause bus activation and allow race-free idle exit.
   * https://github.com/projectatomic/rpm-ostree/pull/606
   * If we get unlucky and try to talk to the daemon in FLUSHING
   * state, then it won't reply, and we should try again.
   */
  static const char sysroot_objpath[] = "/org/projectatomic/rpmostree1/Sysroot";
  while (should_register)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GVariantBuilder) options_builder =
        g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      const char *clientid = g_getenv ("RPMOSTREE_CLIENT_ID") ?: RPMOSTREE_CLI_ID;
      g_variant_builder_add (options_builder, "{sv}", "id", g_variant_new_string (clientid));
      g_autoptr(GVariant) res =
        g_dbus_connection_call_sync (connection, bus_name, sysroot_objpath,
                                     "org.projectatomic.rpmostree1.Sysroot",
                                     "RegisterClient",
                                     g_variant_new ("(@a{sv})", g_variant_builder_end (options_builder)),
                                     (GVariantType*)"()",
                                     G_DBUS_CALL_FLAGS_NONE, -1,
                                     cancellable, &local_error);
      if (res)
        break;  /* Success! */

      if (g_dbus_error_is_remote_error (local_error))
        {
          g_autofree char *remote_err = g_dbus_error_get_remote_error (local_error);
          /* If this is true, we caught the daemon after it was doing an
           * idle exit, but while it still owned the name. Retry.
           */
          if (g_str_equal (remote_err, "org.freedesktop.DBus.Error.NoReply"))
            continue;
          /* Otherwise, fall through */
        }

      /* Something else went wrong */
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy =
    rpmostree_sysroot_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE,
                                      bus_name, "/org/projectatomic/rpmostree1/Sysroot",
                                      NULL, error);
  if (sysroot_proxy == NULL)
    return FALSE;

  /* TODO: Change RegisterClient to also do a reload and do it async instead */
  await_reload_sync (sysroot_proxy);

  *out_sysroot_proxy = g_steal_pointer (&sysroot_proxy);
  *out_peer_pid = peer_pid; peer_pid = 0;
  if (out_bus_type)
    *out_bus_type = bus_type;
  return TRUE;
}

gboolean
rpmostree_load_os_proxies (RPMOSTreeSysroot *sysroot_proxy,
                           gchar *opt_osname,
                           GCancellable *cancellable,
                           RPMOSTreeOS **out_os_proxy,
                           RPMOSTreeOSExperimental **out_osexperimental_proxy,
                           GError **error)
{
  g_autofree char *os_object_path = NULL;
  if (opt_osname == NULL)
    os_object_path = rpmostree_sysroot_dup_booted (sysroot_proxy);
  if (os_object_path == NULL)
    {
      /* Usually if opt_osname is null and the property isn't
         populated that means the daemon isn't listen on the bus
         make the call anyways to get the standard error.
      */
      if (!opt_osname)
        opt_osname = "";

      if (!rpmostree_sysroot_call_get_os_sync (sysroot_proxy,
                                               opt_osname,
                                               &os_object_path,
                                               cancellable,
                                               error))
        return FALSE;
    }

  /* owned by sysroot_proxy */
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));
  const char *bus_name = NULL;
  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  glnx_unref_object RPMOSTreeOS *os_proxy =
    rpmostree_os_proxy_new_sync (connection,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 bus_name,
                                 os_object_path,
                                 cancellable,
                                 error);
  if (os_proxy == NULL)
    return FALSE;

  glnx_unref_object RPMOSTreeOSExperimental *ret_osexperimental_proxy = NULL;
  if (out_osexperimental_proxy)
    {
      ret_osexperimental_proxy =
        rpmostree_osexperimental_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 bus_name,
                                                 os_object_path,
                                                 cancellable,
                                                 error);
      if (!ret_osexperimental_proxy)
        return FALSE;
    }

  *out_os_proxy = g_steal_pointer (&os_proxy);
  if (out_osexperimental_proxy)
    *out_osexperimental_proxy = g_steal_pointer (&ret_osexperimental_proxy);
  return TRUE;
}

gboolean
rpmostree_load_os_proxy (RPMOSTreeSysroot *sysroot_proxy,
                         gchar *opt_osname,
                         GCancellable *cancellable,
                         RPMOSTreeOS **out_os_proxy,
                         GError **error)
{
  return rpmostree_load_os_proxies (sysroot_proxy, opt_osname, cancellable,
                                    out_os_proxy, NULL, error);
}


/**
* transaction_console_get_progress_line
*
* Similar to ostree_repo_pull_default_console_progress_changed
*
* Displays outstanding fetch progress in bytes/sec,
* or else outstanding content or metadata writes to the repository in
* number of objects.
**/
static gchar *
transaction_get_progress_line (guint64 start_time,
                               guint64 elapsed_secs,
                               guint outstanding_fetches,
                               guint outstanding_writes,
                               guint n_scanned_metadata,
                               guint metadata_fetched,
                               guint outstanding_metadata_fetches,
                               guint total_delta_parts,
                               guint fetched_delta_parts,
                               guint total_delta_superblocks,
                               guint64 total_delta_part_size,
                               guint fetched,
                               guint requested,
                               guint64 bytes_transferred,
                               guint64 bytes_sec)
{
  GString *buf;

  buf = g_string_new ("");

  if (outstanding_fetches)
    {
      g_autofree gchar *formatted_bytes_transferred = g_format_size_full (bytes_transferred, 0);
      g_autofree gchar *formatted_bytes_sec = NULL;

      if (!bytes_sec)
        formatted_bytes_sec = g_strdup ("-");
      else
        formatted_bytes_sec = g_format_size (bytes_sec);

      if (total_delta_parts > 0)
        {
          g_autofree gchar *formatted_total = g_format_size (total_delta_part_size);
          g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/s %s/%s",
                                  fetched_delta_parts, total_delta_parts,
                                  formatted_bytes_sec, formatted_bytes_transferred,
                                  formatted_total);
        }
      else if (outstanding_metadata_fetches)
        {
          g_string_append_printf (buf, "Receiving metadata objects: %u/(estimating) %s/s %s",
                                  metadata_fetched, formatted_bytes_sec, formatted_bytes_transferred);
        }
      else
        {
          g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                                  (guint)((((double)fetched) / requested) * 100),
                                  fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
        }
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  return g_string_free (buf, FALSE);
}


typedef struct
{
  gboolean progress;
  GError *error;
  GMainLoop *loop;
  gboolean complete;
} TransactionProgress;


static TransactionProgress *
transaction_progress_new (void)
{
  TransactionProgress *self;

  self = g_slice_new0 (TransactionProgress);
  self->loop = g_main_loop_new (NULL, FALSE);

  return self;
}

static void
transaction_progress_free (TransactionProgress *self)
{
  g_main_loop_unref (self->loop);
  g_slice_free (TransactionProgress, self);
}

static void
transaction_progress_end (TransactionProgress *self)
{
  if (self->progress)
    {
      ror_progress_end (NULL);
      self->progress = FALSE;
    }
  g_main_loop_quit (self->loop);
}

static void
on_transaction_progress (GDBusProxy *proxy,
                         gchar *sender_name,
                         gchar *signal_name,
                         GVariant *parameters,
                         gpointer user_data)
{
  TransactionProgress *tp = user_data;

  if (g_strcmp0 (signal_name, "SignatureProgress") == 0)
    {
      /* We used to print the signature here, but doing so interferes with the
       * libostree HTTP progress, and it gets really, really verbose when doing
       * a deploy. Let's follow the Unix philosophy here: silence is success.
       */
    }
  else if (g_strcmp0 (signal_name, "Message") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_print ("%s\n", message);
    }
  else if (g_strcmp0 (signal_name, "TaskBegin") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      tp->progress = TRUE;
      ror_progress_begin_task (message);
    }
  else if (g_strcmp0 (signal_name, "TaskEnd") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      if (tp->progress)
        {
          g_assert (tp->progress);
          ror_progress_end (message);
          tp->progress = FALSE;
        }
    }
  else if (g_strcmp0 (signal_name, "ProgressEnd") == 0)
    {
      if (tp->progress)
        {
          g_assert (tp->progress);
          ror_progress_end (NULL);
          tp->progress = FALSE;
        }
    }
  else if (g_strcmp0 (signal_name, "PercentProgress") == 0)
    {
      const gchar *message = NULL;
      guint32 percentage;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_variant_get_child (parameters, 1, "u", &percentage);
      if (!tp->progress)
        {
          tp->progress = TRUE;
          ror_progress_begin_percent (message);
        }
      ror_progress_update (percentage);
    }
  else if (g_strcmp0 (signal_name, "DownloadProgress") == 0)
    {
      guint64 start_time;
      guint64 elapsed_secs;
      guint outstanding_fetches;
      guint outstanding_writes;
      guint n_scanned_metadata;
      guint metadata_fetched;
      guint outstanding_metadata_fetches;
      guint total_delta_parts;
      guint fetched_delta_parts;
      guint total_delta_superblocks;
      guint64 total_delta_part_size;
      guint fetched;
      guint requested;
      guint64 bytes_transferred;
      guint64 bytes_sec;
      g_variant_get (parameters, "((tt)(uu)(uuu)(uuut)(uu)(tt))",
                     &start_time, &elapsed_secs,
                     &outstanding_fetches, &outstanding_writes,
                     &n_scanned_metadata, &metadata_fetched,
                     &outstanding_metadata_fetches,
                     &total_delta_parts, &fetched_delta_parts,
                     &total_delta_superblocks, &total_delta_part_size,
                     &fetched, &requested, &bytes_transferred, &bytes_sec);

      g_autofree gchar *line =
             transaction_get_progress_line (start_time, elapsed_secs,
                                            outstanding_fetches,
                                            outstanding_writes,
                                            n_scanned_metadata,
                                            metadata_fetched,
                                            outstanding_metadata_fetches,
                                            total_delta_parts,
                                            fetched_delta_parts,
                                            total_delta_superblocks,
                                            total_delta_part_size,
                                            fetched,
                                            requested,
                                            bytes_transferred,
                                            bytes_sec);
      if (!tp->progress)
        {
          tp->progress = TRUE;
          ror_progress_begin_task (line);
        }
      else
        ror_progress_set_message (line);
    }
  else if (g_strcmp0 (signal_name, "Finished") == 0)
    {
      if (tp->error == NULL)
        {
          g_autofree char *error_message = NULL;
          gboolean success = FALSE;

          g_variant_get (parameters, "(bs)", &success, &error_message);

          if (!success)
            {
              tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                                           error_message);
            }
        }

      transaction_progress_end (tp);
    }
}

static void
on_owner_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  /* Owner shouldn't change durning a transaction
   * that messes with notifications, abort, abort.
   */
  TransactionProgress *tp = user_data;
  tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                               "Bus owner changed, aborting.");
  transaction_progress_end (tp);
}

static void
cancelled_handler (GCancellable *cancellable,
                   gpointer user_data)
{
  RPMOSTreeTransaction *transaction = user_data;
  rpmostree_transaction_call_cancel_sync (transaction, NULL, NULL);
}

static gboolean
on_sigint (gpointer user_data)
{
  GCancellable *cancellable = user_data;
  if (!g_cancellable_is_cancelled (cancellable))
    {
      g_printerr ("Caught SIGINT, cancelling transaction\n");
      g_cancellable_cancel (cancellable);
    }
  else
    {
      g_printerr ("Awaiting transaction cancellation...\n");
    }
  return TRUE;
}

static gboolean
set_variable_false (gpointer data)
{
  gboolean *donep = data;
  *donep = TRUE;
  g_main_context_wakeup (NULL);
  return FALSE;
}

/* We explicitly run the loop so we receive DBus messages,
 * in particular notification of a new txn.
 */
static void
spin_mainloop_for_a_second (void)
{
  gboolean done = FALSE;

  g_timeout_add_seconds (1, set_variable_false, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

static RPMOSTreeTransaction *
transaction_connect (const char *transaction_address,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(GDBusConnection) peer_connection =
    g_dbus_connection_new_for_address_sync (transaction_address,
                                            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                            NULL, cancellable, error);

  if (peer_connection == NULL)
    return NULL;

  return rpmostree_transaction_proxy_new_sync (peer_connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               NULL, "/", cancellable, error);
}

/* Connect to the active transaction if one exists.  Because this is
 * currently racy, we use a retry loop for up to ~5 seconds.
 */
gboolean
rpmostree_transaction_connect_active (RPMOSTreeSysroot *sysroot_proxy,
                                      char                 **out_path,
                                      RPMOSTreeTransaction **out_txn,
                                      GCancellable *cancellable,
                                      GError      **error)
{
  /* We don't want to loop infinitely if something is going wrong with e.g.
   * permissions.
   */
  guint n_tries = 0;
  const guint max_tries = 5;
  g_autoptr(GError) txn_connect_error = NULL;

  for (n_tries = 0; n_tries < max_tries; n_tries++)
    {
      const char *txn_path = rpmostree_sysroot_get_active_transaction_path (sysroot_proxy);
      if (!txn_path || !*txn_path)
        {
          /* No active txn?  We're done */
          if (out_path)
            *out_path = NULL;
          *out_txn = NULL;
          return TRUE;
        }

      /* Keep track of the last error so we have something to return */
      g_clear_error (&txn_connect_error);
      RPMOSTreeTransaction *txn =
        transaction_connect (txn_path, cancellable, &txn_connect_error);
      if (txn)
        {
          if (out_path)
            *out_path = g_strdup (txn_path);
          *out_txn = txn;
          return TRUE;
        }
      else
        spin_mainloop_for_a_second ();
    }

  g_propagate_error (error, g_steal_pointer (&txn_connect_error));
  return FALSE;
}

/* Transactions need an explicit Start call so we can set up watches for signals
 * beforehand and avoid losing information.  We monitor the transaction,
 * printing output it sends, and handle Ctrl-C, etc.
 */
gboolean
rpmostree_transaction_get_response_sync (RPMOSTreeSysroot *sysroot_proxy,
                                         const char *transaction_address,
                                         GCancellable *cancellable,
                                         GError **error)
{
  guint sigintid = 0;
  GDBusConnection *connection;
  glnx_unref_object GDBusObjectManager *object_manager = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;

  TransactionProgress *tp = transaction_progress_new ();

  const char *bus_name = NULL;
  gint cancel_handler;
  gulong signal_handler = 0;
  gboolean success = FALSE;
  gboolean just_started = FALSE;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  /* If we are on the message bus, setup object manager connection
   * to notify if the owner changes. */
  if (bus_name != NULL)
    {
      object_manager = rpmostree_object_manager_client_new_sync (connection,
                                                          G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                          bus_name,
                                                          "/org/projectatomic/rpmostree1",
                                                          cancellable,
                                                          error);

      if (object_manager == NULL)
        goto out;

      g_signal_connect (object_manager,
                        "notify::name-owner",
                        G_CALLBACK (on_owner_changed),
                        tp);
    }

  transaction = transaction_connect (transaction_address, cancellable, error);
  if (!transaction)
    goto out;

  sigintid = g_unix_signal_add (SIGINT, on_sigint, cancellable);

  /* setup cancel handler */
  cancel_handler = g_cancellable_connect (cancellable,
                                          G_CALLBACK (cancelled_handler),
                                          transaction, NULL);

  signal_handler = g_signal_connect (transaction, "g-signal",
                                     G_CALLBACK (on_transaction_progress),
                                     tp);

  /* Tell the server we're ready to receive signals. */
  if (!rpmostree_transaction_call_start_sync (transaction,
                                              &just_started,
                                              cancellable,
                                              error))
    goto out;

  /* FIXME Use the 'just_started' flag to determine whether to print
   *       a message about reattaching to an in-progress transaction,
   *       like:
   *
   *       Existing upgrade in progress, reattaching.  Control-C to cancel.
   *
   *       But that requires having a printable description of the
   *       operation.  Maybe just add a string arg to this function?
   */
  g_main_loop_run (tp->loop);

  g_cancellable_disconnect (cancellable, cancel_handler);

  if (!g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      if (tp->error)
        {
          g_propagate_error (error, tp->error);
        }
      else
        {
          success = TRUE;
        }
    }

  /* On success, call Reload() as a way to sync with the daemon. Do this in async mode so
   * that gdbus handles signals for changed properties. */
  if (success)
    await_reload_sync (sysroot_proxy);

out:
  if (sigintid)
    g_source_remove (sigintid);
  if (signal_handler)
    g_signal_handler_disconnect (transaction, signal_handler);

  transaction_progress_free (tp);
  return success;
}

/* Handles client-side processing for most command line tools
 * after a transaction has been started.  Wraps invocation
 * of rpmostree_transaction_get_response_sync().
 */
gboolean
rpmostree_transaction_client_run (RpmOstreeCommandInvocation *invocation,
                                  RPMOSTreeSysroot    *sysroot_proxy,
                                  RPMOSTreeOS         *os_proxy,
                                  GVariant            *options,
                                  gboolean             exit_unchanged_77,
                                  const char          *transaction_address,
                                  RpmOstreeDeployment *previous_deployment,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  /* Wait for the txn to complete */
  if (!rpmostree_transaction_get_response_sync (sysroot_proxy, transaction_address,
                                                cancellable, error))
    return FALSE;

  /* Process the result of the txn and our options */

  g_auto(GVariantDict) optdict = G_VARIANT_DICT_INIT (options);
  /* Parse back the options variant */
  gboolean opt_reboot = FALSE;
  g_variant_dict_lookup (&optdict, "reboot", "b", &opt_reboot);
  gboolean opt_dry_run = FALSE;
  g_variant_dict_lookup (&optdict, "dry-run", "b", &opt_dry_run);

  if (opt_dry_run)
    {
      g_print ("Exiting because of '--dry-run' option\n");
    }
  else if (!opt_reboot)
    {
      gboolean changed;
      if (!rpmostree_print_diff_from_deployment (previous_deployment, &changed,
                                                 cancellable, error))
        return FALSE;

      if (!changed)
        {
          if (exit_unchanged_77)
            invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
        }
      else
        g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}

static void
rpmostree_print_signatures (GVariant *variant,
                            const gchar *sep,
                            gboolean verbose)
{
  const guint n_sigs = g_variant_n_children (variant);
  g_autoptr(GString) sigs_buffer = g_string_sized_new (256);

  for (guint i = 0; i < n_sigs; i++)
    {
      g_autoptr(GVariant) v = NULL;
      if (i != 0)
        g_string_append_c (sigs_buffer, '\n');
      g_variant_get_child (variant, i, "v", &v);
      if (verbose)
        ostree_gpg_verify_result_describe_variant (v, sigs_buffer, sep,
                                                   OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
      else
        {
          if (i != 0)
            g_string_append (sigs_buffer, sep);

          gboolean is_key_missing;
          g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING, "b", &is_key_missing);

          const char *fingerprint;
          g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT, "&s", &fingerprint);

          if (is_key_missing)
            {
              g_string_append_printf (sigs_buffer, "Can't check signature: public key %s not found\n", fingerprint);
            }
          else
            {
              gboolean valid;
              g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_VALID, "b", &valid);

              g_string_append_printf (sigs_buffer, "%s signature by %s\n", valid ? "Valid" : "Invalid",
                                      fingerprint);
            }
        }
    }

  g_print ("%s", sigs_buffer->str);
}

void
rpmostree_print_gpg_info (GVariant  *signatures,
                          gboolean   verbose,
                          guint      max_key_len)
{
  if (signatures)
    {
      /* +2 for initial leading spaces */
      const guint gpgpad = max_key_len + 2 + strlen (": ");
      char gpgspaces[gpgpad+1];
      memset (gpgspaces, ' ', gpgpad);
      gpgspaces[gpgpad] = '\0';

      if (verbose)
        {
          const guint n_sigs = g_variant_n_children (signatures);
          g_autofree char *gpgheader =
            g_strdup_printf ("%u signature%s", n_sigs,
                             n_sigs == 1 ? "" : "s");
          rpmostree_print_kv ("GPGSignature", max_key_len, gpgheader);
        }
      else
        rpmostree_print_kv_no_newline ("GPGSignature", max_key_len, "");
      rpmostree_print_signatures (signatures, gpgspaces, verbose);
    }
  else
    {
      rpmostree_print_kv ("GPGSignature", max_key_len, "(unsigned)");
    }
}

static gint
pkg_diff_variant_compare (gconstpointer a,
                          gconstpointer b,
                          gpointer unused)
{
  const char *pkg_name_a = NULL;
  const char *pkg_name_b = NULL;

  g_variant_get_child ((GVariant *) a, 0, "&s", &pkg_name_a);
  g_variant_get_child ((GVariant *) b, 0, "&s", &pkg_name_b);

  /* XXX Names should be unique since we're comparing packages
   *     from two different trees... right? */

  return g_strcmp0 (pkg_name_a, pkg_name_b);
}

static void
pkg_diff_variant_print (GVariant *variant)
{
  g_autoptr(GVariant) details = NULL;
  const char *old_name, *old_evr, *old_arch;
  const char *new_name, *new_evr, *new_arch;
  gboolean have_old = FALSE;
  gboolean have_new = FALSE;

  details = g_variant_get_child_value (variant, 2);
  g_return_if_fail (details != NULL);

  have_old = g_variant_lookup (details,
                               "PreviousPackage", "(&s&s&s)",
                               &old_name, &old_evr, &old_arch);

  have_new = g_variant_lookup (details,
                               "NewPackage", "(&s&s&s)",
                               &new_name, &new_evr, &new_arch);

  if (have_old && have_new)
    {
      g_print ("!%s-%s-%s\n", old_name, old_evr, old_arch);
      g_print ("=%s-%s-%s\n", new_name, new_evr, new_arch);
    }
  else if (have_old)
    {
      g_print ("-%s-%s-%s\n", old_name, old_evr, old_arch);
    }
  else if (have_new)
    {
      g_print ("+%s-%s-%s\n", new_name, new_evr, new_arch);
    }
}

void
rpmostree_print_package_diffs (GVariant *variant)
{
  GQueue queue = G_QUEUE_INIT;
  GVariantIter iter;
  GVariant *child;

  /* GVariant format should be a(sua{sv}) */

  g_return_if_fail (variant != NULL);

  g_variant_iter_init (&iter, variant);

  /* Queue takes ownership of the child variant. */
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    g_queue_insert_sorted (&queue, child, pkg_diff_variant_compare, NULL);

  while (!g_queue_is_empty (&queue))
    {
      child = g_queue_pop_head (&queue);
      pkg_diff_variant_print (child);
      g_variant_unref (child);
    }
}

/* swiss-army knife: takes an strv of pkgspecs destined for
 * install, and splits it into repo pkgs, and for local
 * pkgs, an fd list & idx variant. */
gboolean
rpmostree_sort_pkgs_strv (const char *const* pkgs,
                          GUnixFDList  *fd_list,
                          GPtrArray   **out_repo_pkgs,
                          GVariant    **out_fd_idxs,
                          GError      **error)
{
  g_autoptr(GPtrArray) repo_pkgs = g_ptr_array_new_with_free_func (g_free);
  g_auto(GVariantBuilder) builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ah"));
  for (const char *const* pkg = pkgs; pkg && *pkg; pkg++)
    {
      if (g_str_has_prefix (*pkg, "http://") ||
          g_str_has_prefix (*pkg, "https://"))
        {
          g_print ("Downloading '%s'... ", *pkg);
          glnx_autofd int fd = ror_download_to_fd (*pkg, error);
          if (fd < 0)
            {
              g_print ("failed!\n");
              return FALSE;
            }
          g_print ("done!\n");

          int idx = g_unix_fd_list_append (fd_list, fd, error);
          if (idx < 0)
            return FALSE;

          g_variant_builder_add (&builder, "h", idx);
        }
      else if (!g_str_has_suffix (*pkg, ".rpm"))
        g_ptr_array_add (repo_pkgs, g_strdup (*pkg));
      else
        {
          glnx_autofd int fd = -1;
          if (!glnx_openat_rdonly (AT_FDCWD, *pkg, TRUE, &fd, error))
            return FALSE;

          int idx = g_unix_fd_list_append (fd_list, fd, error);
          if (idx < 0)
            return FALSE;

          g_variant_builder_add (&builder, "h", idx);
        }
    }

  *out_fd_idxs = g_variant_ref_sink (g_variant_new ("ah", &builder));
  *out_repo_pkgs = g_steal_pointer (&repo_pkgs);
  return TRUE;
}

static void
vardict_insert_strv (GVariantDict *dict,
                     const char   *key,
                     const char *const* strv)
{
  if (strv && *strv)
    g_variant_dict_insert (dict, key, "^as", (char**)strv);
}

static gboolean
vardict_sort_and_insert_pkgs (GVariantDict *dict,
                              const char   *key_prefix,
                              GUnixFDList  *fd_list,
                              const char *const* pkgs,
                              GError      **error)
{
  g_autoptr(GVariant) fd_idxs = NULL;
  g_autoptr(GPtrArray) repo_pkgs = NULL;

  if (!rpmostree_sort_pkgs_strv (pkgs, fd_list, &repo_pkgs, &fd_idxs, error))
    return FALSE;

  /* for grep: here we insert install-packages/override-replace-packages */
  if (repo_pkgs != NULL && repo_pkgs->len > 0)
    g_variant_dict_insert_value (dict, glnx_strjoina (key_prefix, "-packages"),
      g_variant_new_strv ((const char *const*)repo_pkgs->pdata,
                                              repo_pkgs->len));

  /* for grep: here we insert install-local-packages/override-replace-local-packages */
  if (fd_idxs != NULL)
    g_variant_dict_insert_value (dict, glnx_strjoina (key_prefix, "-local-packages"),
                                 fd_idxs);
  return TRUE;
}

static gboolean
get_modifiers_variant (const char   *set_refspec,
                       const char   *set_revision,
                       const char *const* install_pkgs,
                       const char *const* uninstall_pkgs,
                       const char *const* override_replace_pkgs,
                       const char *const* override_remove_pkgs,
                       const char *const* override_reset_pkgs,
                       const char   *local_repo_remote,
                       GVariant    **out_modifiers,
                       GUnixFDList **out_fd_list,
                       GError      **error)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new ();

  if (install_pkgs)
    {
      if (!vardict_sort_and_insert_pkgs (&dict, "install", fd_list,
                                         install_pkgs, error))
        return FALSE;
    }

  if (override_replace_pkgs)
    {
      if (!vardict_sort_and_insert_pkgs (&dict, "override-replace", fd_list,
                                         override_replace_pkgs, error))
        return FALSE;
    }

  if (set_refspec)
    g_variant_dict_insert (&dict, "set-refspec", "s", set_refspec);
  if (set_revision)
    g_variant_dict_insert (&dict, "set-revision", "s", set_revision);

  vardict_insert_strv (&dict, "uninstall-packages", uninstall_pkgs);
  vardict_insert_strv (&dict, "override-remove-packages", override_remove_pkgs);
  vardict_insert_strv (&dict, "override-reset-packages", override_reset_pkgs);

  if (local_repo_remote)
    {
      /* Unfortunately, we can't pass an fd to a dir through D-Bus on el7 right now. So
       * there, we just pass the path. Once that's fixed (or we no longer care about
       * supporting this feature on el7), we can drop this buildopt. See:
       * https://bugzilla.redhat.com/show_bug.cgi?id=1672404 */
      glnx_fd_close int repo_dfd = -1;
      if (!glnx_opendirat (AT_FDCWD, local_repo_remote, TRUE, &repo_dfd, error))
        return FALSE;

      int idx = g_unix_fd_list_append (fd_list, repo_dfd, error);
      if (idx < 0)
        return FALSE;

      g_variant_dict_insert (&dict, "ex-local-repo-remote", "h", idx);
    }

  *out_fd_list = g_steal_pointer (&fd_list);
  *out_modifiers = g_variant_ref_sink (g_variant_dict_end (&dict));
  return TRUE;
}

gboolean
rpmostree_update_deployment (RPMOSTreeOS  *os_proxy,
                             const char   *set_refspec,
                             const char   *set_revision,
                             const char *const* install_pkgs,
                             const char *const* uninstall_pkgs,
                             const char *const* override_replace_pkgs,
                             const char *const* override_remove_pkgs,
                             const char *const* override_reset_pkgs,
                             const char   *local_repo_remote,
                             GVariant     *options,
                             char        **out_transaction_address,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GVariant) modifiers = NULL;
  glnx_unref_object GUnixFDList *fd_list = NULL;
  if (!get_modifiers_variant (set_refspec, set_revision,
                              install_pkgs, uninstall_pkgs,
                              override_replace_pkgs,
                              override_remove_pkgs,
                              override_reset_pkgs,
                              local_repo_remote,
                              &modifiers, &fd_list, error))
    return FALSE;

  return rpmostree_os_call_update_deployment_sync (os_proxy,
                                                   modifiers,
                                                   options,
                                                   fd_list,
                                                   out_transaction_address,
                                                   NULL,
                                                   cancellable,
                                                   error);
}

static void
append_to_summary (GString     *summary,
                   const char  *type,
                   guint        n)
{
  if (n == 0)
    return;
  if (summary->len > 0)
    g_string_append (summary, ", ");
  g_string_append_printf (summary, "%u %s", n, type);
}

static int
compare_sec_advisories (gconstpointer ap,
                        gconstpointer bp)
{
  GVariant *a = *((GVariant**)ap);
  GVariant *b = *((GVariant**)bp);

  RpmOstreeAdvisorySeverity asev;
  g_variant_get_child (a, 2, "u", &asev);

  RpmOstreeAdvisorySeverity bsev;
  g_variant_get_child (b, 2, "u", &bsev);

  if (asev != bsev)
    return asev - bsev;

  const char *aid;
  g_variant_get_child (a, 0, "&s", &aid);

  const char *bid;
  g_variant_get_child (b, 0, "&s", &bid);

  return strcmp (aid, bid);
}

static const char*
severity_to_str (RpmOstreeAdvisorySeverity severity)
{
  switch (severity)
    {
    case RPM_OSTREE_ADVISORY_SEVERITY_LOW:
      return "Low";
    case RPM_OSTREE_ADVISORY_SEVERITY_MODERATE:
      return "Moderate";
    case RPM_OSTREE_ADVISORY_SEVERITY_IMPORTANT:
      return "Important";
    case RPM_OSTREE_ADVISORY_SEVERITY_CRITICAL:
      return "Critical";
    default: /* including NONE */
      return "Unknown";
    }
}

static void
print_advisories (GVariant *advisories,
                  gboolean  verbose,
                  guint     max_key_len)
{
  /* counters for none/unknown, low, moderate, important, critical advisories */
  guint n_sev[RPM_OSTREE_ADVISORY_SEVERITY_LAST] = {0,};

  /* we only display security advisories for now */
  g_autoptr(GPtrArray) sec_advisories =
    g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  guint max_id_len = 0;

  GVariantIter iter;
  g_variant_iter_init (&iter, advisories);
  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
      if (!child)
        break;

      DnfAdvisoryKind kind;
      g_variant_get_child (child, 1, "u", &kind);

      /* we only display security advisories for now */
      if (kind != DNF_ADVISORY_KIND_SECURITY)
        continue;

      const char *id;
      g_variant_get_child (child, 0, "&s", &id);
      max_id_len = MAX (max_id_len, strlen (id));

      RpmOstreeAdvisorySeverity severity;
      g_variant_get_child (child, 2, "u", &severity);
      /* just make sure it's capped at LAST */
      if (severity < RPM_OSTREE_ADVISORY_SEVERITY_LAST)
        n_sev[severity]++;
      else /* bad val; count as unknown */
        n_sev[0]++;

      g_ptr_array_add (sec_advisories, g_variant_ref (child));
    }

  if (sec_advisories->len == 0)
    return;

  g_print ("%s%s", get_red_start (), get_bold_start ());
  rpmostree_print_kv_no_newline ("SecAdvisories", max_key_len, "");

  if (!verbose)
    {
      /* just spell out "severity" for the unknown case, because e.g.
       * "SecAdvisories: 1 unknown" on its own is cryptic and scary */
      g_autoptr(GString) advisory_summary = g_string_new (NULL);
      const char *sev_str[] = {"unknown severity", "low", "moderate", "important", "critical"};
      g_assert_cmpint (G_N_ELEMENTS (n_sev), ==, G_N_ELEMENTS (sev_str));
      for (guint i = 0; i < G_N_ELEMENTS (sev_str); i++)
        append_to_summary (advisory_summary, sev_str[i], n_sev[i]);
      g_print ("%s\n", advisory_summary->str);
    }

  g_print ("%s%s", get_bold_end (), get_red_end ());
  if (!verbose)
    return;

  const guint max_sev_len = strlen ("Important");

  /* sort by severity */
  g_ptr_array_sort (sec_advisories, compare_sec_advisories);

  for (guint i = 0; i < sec_advisories->len; i++)
    {
      GVariant *advisory = sec_advisories->pdata[i];

      const char *id;
      g_variant_get_child (advisory, 0, "&s", &id);

      DnfAdvisoryKind kind;
      g_variant_get_child (advisory, 1, "u", &kind);

      RpmOstreeAdvisorySeverity severity;
      g_variant_get_child (advisory, 2, "u", &severity);

      g_autoptr(GVariant) pkgs = g_variant_get_child_value (advisory, 3);

      const char *severity_str = severity_to_str (severity);
      const guint n_pkgs = g_variant_n_children (pkgs);
      for (guint j = 0; j < n_pkgs; j++)
        {
          const char *nevra;
          g_variant_get_child (pkgs, j, "&s", &nevra);

          if (i == 0 && j == 0) /* we're on the same line as SecAdvisories */
            g_print ("%-*s  %-*s  %s\n", max_id_len, id, max_sev_len, severity_str, nevra);
          else
            g_print ("  %*s  %-*s  %-*s  %s\n", max_key_len, "", max_id_len, id,
                     max_sev_len, severity_str, nevra);
        }

      g_autoptr(GVariant) additional_info = g_variant_get_child_value (advisory, 4);
      g_auto(GVariantDict) dict;
      g_variant_dict_init (&dict, additional_info);

      g_autoptr(GVariant) refs =
        g_variant_dict_lookup_value (&dict, "cve_references", G_VARIANT_TYPE ("a(ss)"));

      /* for backwards compatibility with cached metadata from older versions */
      if (!refs)
        continue;

      const guint n_refs = g_variant_n_children (refs);
      for (guint j = 0; j < n_refs; j++)
        {
          const char *url, *title;
          g_variant_get_child (refs, j, "(&s&s)", &url, &title);
          g_print ("  %*s    %s\n", max_key_len, "", title);
          g_print ("  %*s    %s\n", max_key_len, "", url);
        }
    }
}

/* print "rpm-diff" and "advisories" GVariants from a cached update */
gboolean
rpmostree_print_diff_advisories (GVariant         *rpm_diff,
                                 GVariant         *advisories,
                                 gboolean          verbose,
                                 gboolean          verbose_advisories,
                                 guint             max_key_len,
                                 GError          **error)
{
  if (!rpm_diff)
    return TRUE; /* Nothing to 🖨️ */

  if (advisories)
    print_advisories (advisories, verbose || verbose_advisories, max_key_len);

  g_auto(GVariantDict) rpm_diff_dict;
  g_variant_dict_init (&rpm_diff_dict, rpm_diff);

  g_autoptr(GVariant) upgraded =
    _rpmostree_vardict_lookup_value_required (&rpm_diff_dict, "upgraded",
                                              RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT,
                                              error);
  if (!upgraded)
    return FALSE;

  g_autoptr(GVariant) downgraded =
    _rpmostree_vardict_lookup_value_required (&rpm_diff_dict, "downgraded",
                                              RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT,
                                              error);
  if (!downgraded)
    return FALSE;

  g_autoptr(GVariant) removed =
    _rpmostree_vardict_lookup_value_required (&rpm_diff_dict, "removed",
                                              RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT,
                                              error);
  if (!removed)
    return FALSE;

  g_autoptr(GVariant) added =
    _rpmostree_vardict_lookup_value_required (&rpm_diff_dict, "added",
                                              RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT,
                                              error);
  if (!added)
    return FALSE;

  if (verbose)
    rpmostree_variant_diff_print_formatted (max_key_len,
                                            upgraded, downgraded, removed, added);
  else
    {
      g_autofree char *diff_summary =
        rpmostree_generate_diff_summary (g_variant_n_children (upgraded),
                                         g_variant_n_children (downgraded),
                                         g_variant_n_children (removed),
                                         g_variant_n_children (added));
      if (strlen (diff_summary) > 0) /* only print if we have something to print */
        rpmostree_print_kv ("Diff", max_key_len, diff_summary);
    }

  return TRUE;
}

/* this is used by both `status` and `upgrade --check/--preview` */
gboolean
rpmostree_print_cached_update (GVariant         *cached_update,
                               gboolean          verbose,
                               gboolean          verbose_advisories,
                               GCancellable     *cancellable,
                               GError          **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Retrieving cached update", error);

  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, cached_update);

  /* let's just extract 📤 all the info ahead of time */

  const char *checksum;
  if (!g_variant_dict_lookup (&dict, "checksum", "&s", &checksum))
    return glnx_throw (error, "Missing \"checksum\" key");

  const char *version;
  if (!g_variant_dict_lookup (&dict, "version", "&s", &version))
    version= NULL;

  g_autofree char *timestamp = NULL;
  { guint64 t;
    if (!g_variant_dict_lookup (&dict, "timestamp", "t", &t))
      t = 0;
    timestamp = rpmostree_timestamp_str_from_unix_utc (t);
  }

  gboolean gpg_enabled;
  if (!g_variant_dict_lookup (&dict, "gpg-enabled", "b", &gpg_enabled))
    gpg_enabled = FALSE;

  g_autoptr(GVariant) signatures =
    g_variant_dict_lookup_value (&dict, "signatures", G_VARIANT_TYPE ("av"));

  gboolean is_new_checksum;
  g_assert (g_variant_dict_lookup (&dict, "ref-has-new-commit", "b", &is_new_checksum));

  g_autoptr(GVariant) rpm_diff =
    g_variant_dict_lookup_value (&dict, "rpm-diff", G_VARIANT_TYPE ("a{sv}"));

  g_autoptr(GVariant) advisories =
    g_variant_dict_lookup_value (&dict, "advisories", G_VARIANT_TYPE ("a(suuasa{sv})"));

  /* and now we can print 🖨️ things! */

  g_print ("AvailableUpdate:\n");

  /* add the long keys here */
  const guint max_key_len = MAX (strlen ("SecAdvisories"),
                                 strlen ("GPGSignature"));

  if (is_new_checksum)
    {
      rpmostree_print_timestamp_version (version, timestamp, max_key_len);
      rpmostree_print_kv ("Commit", max_key_len, checksum);
      if (gpg_enabled)
        rpmostree_print_gpg_info (signatures, verbose, max_key_len);
    }

  if (!rpmostree_print_diff_advisories (rpm_diff, advisories, verbose,
                                        verbose_advisories, max_key_len, error))
    return FALSE;

  return TRUE;
}
