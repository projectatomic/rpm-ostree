/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include "ostree.h"
#include "rpmostree-builtin-types.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-cxxrs.h"

G_BEGIN_DECLS

#define BUILTINPROTO(name) gboolean rpmostree_builtin_ ## name (int argc, char **argv, \
                                                                RpmOstreeCommandInvocation *invocation, \
                                                                GCancellable *cancellable, GError **error)

BUILTINPROTO(compose);
BUILTINPROTO(countme);
BUILTINPROTO(upgrade);
BUILTINPROTO(reload);
BUILTINPROTO(usroverlay);
BUILTINPROTO(deploy);
BUILTINPROTO(rebase);
BUILTINPROTO(cancel);
BUILTINPROTO(cleanup);
BUILTINPROTO(rollback);
BUILTINPROTO(initramfs);
BUILTINPROTO(status);
BUILTINPROTO(refresh_md);
BUILTINPROTO(db);
BUILTINPROTO(internals);
BUILTINPROTO(container);
BUILTINPROTO(install);
BUILTINPROTO(uninstall);
BUILTINPROTO(override);
BUILTINPROTO(kargs);
BUILTINPROTO(reset);
BUILTINPROTO(start_daemon);
BUILTINPROTO(shlib_backend);
BUILTINPROTO(coreos_rootfs);
BUILTINPROTO(testutils);
BUILTINPROTO(ex);
BUILTINPROTO(finalize_deployment);

#undef BUILTINPROTO

gboolean
rpmostree_option_context_parse (GOptionContext *context,
                                const GOptionEntry *main_entries,
                                int *argc,
                                char ***argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable *cancellable,
                                const char *const* *out_install_pkgs,
                                const char *const* *out_uninstall_pkgs,
                                RPMOSTreeSysroot **out_sysroot_proxy,
                                GError **error);

int
rpmostree_handle_subcommand (int argc, char **argv,
                             RpmOstreeCommand *subcommands,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error);

G_END_DECLS

