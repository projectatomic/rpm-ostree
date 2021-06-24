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

#pragma once

// C++ includes
#include <string>
#include <exception>
#include <sstream>
// C includes
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ostree.h>
#include <libdnf/libdnf.h>

#include "libglnx.h"
#include "rpmostree.h"
#include "rpmostree-types.h"
#include "rust/cxx.h"

// C++ code here
namespace util {
// Sadly std::move() doesn't do anything for raw pointer types by default.
// This is our C++ equivalent of g_steal_pointer().
template<typename T>
  T move_nullify(T& v) noexcept
  {
    auto p = v;
    v = nullptr;
    return p;
  }

// In rpm-ostree we always use C++ exceptions as a "fatal error, unwind the stack"
// tool.  We don't try to catch specific exception types, they just carry
// a string that will be displayed to a human.  This helper encapsulates a common
// pattern of adding a "prefix" to the string.  When used multiple times it's
// a bit like a human-friendly stack trace subset.
template<typename T>
  [[ noreturn ]] inline void rethrow_prefixed (std::exception& e, T prefix)
  {
    std::ostringstream msg;
    msg << prefix << ": " << e.what();
    throw std::runtime_error(msg.str());
  }

// Convert a GError into a C++ exception.  Takes ownership of the error.
[[ noreturn ]] static inline void
throw_gerror (GError *&error)
{
  auto s = std::string (error->message);
  g_error_free (error);
  error = NULL;
  throw std::runtime_error (s);
}

// Duplicate a non-empty Rust Str to a NUL-terminated C string.
// The empty string is converted to a NULL pointer.
// This method should be viewed as a workaround for the lack
// of Option<> binding in cxx-rs.
static inline char *
ruststr_dup_c_optempty(const rust::Str s) {
  if (s.length() > 0)
    return g_strndup(s.data(), s.length());
  return NULL;
}

// Return a Rust string pointer from a possibly-NULL C string.
// The NULL C string is converted to the empty string.
// This method should be viewed as a workaround for the lack
// of Option<> binding in cxx-rs.
static inline rust::Str
ruststr_or_empty (const char *c_str) {
  return rust::Str(c_str ?: "");
}

}

namespace rpmostreecxx {
rust::String
util_next_version (rust::Str auto_version_prefix,
                   rust::Str version_suffix,
                   rust::Str last_version);

int
testutil_validate_cxxrs_passthrough(OstreeRepo &repo) noexcept;

}

// Below here is C code
G_BEGIN_DECLS

#define _N(single, plural, n) ( (n) == 1 ? (single) : (plural) )
#define _NS(n) _N("", "s", n)

#define VAR_SELINUX_TARGETED_PATH "var/lib/selinux/targeted/"

int
rpmostree_ptrarray_sort_compare_strings (gconstpointer ap,
                                         gconstpointer bp);

GVariant *_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                                    const char *key,
                                                    const GVariantType *fmt,
                                                    GError     **error);

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           rootfs_dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error);

gboolean
rpmostree_pkg_is_local (DnfPackage *pkg);

char *
rpmostree_pkg_get_local_path (DnfPackage *pkg);

gboolean
rpmostree_check_size_within_limit (guint64     actual,
                                   guint64     limit,
                                   const char *subject,
                                   GError    **error);

char*
rpmostree_translate_path_for_ostree (const char *path);

char *
rpmostree_str_replace (const char  *buf,
                       const char  *old,
                       const char  *newval,
                       GError     **error);

char *
rpmostree_checksum_version (GVariant *checksum);

gboolean
rpmostree_pull_content_only (OstreeRepo  *dest,
                             OstreeRepo  *src,
                             const char  *src_commit,
                             GCancellable *cancellable,
                             GError      **error);
const char *
rpmostree_file_get_path_cached (GFile *file);

static inline
const char *
gs_file_get_path_cached (GFile *file)
{
  return rpmostree_file_get_path_cached (file);
}

char*
rpmostree_generate_diff_summary (guint upgraded,
                                 guint downgraded,
                                 guint removed,
                                 guint added);

// note this enum is shared with Rust via the cxxbridge
typedef enum : std::uint8_t {
  RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY,       /* Diff: 1 upgraded, 2 downgraded, 3 removed, 4 added */
  RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED,  /* Upgraded: ...
                                                          ...
                                                   Added: ...
                                                          ...  */
  RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE /* Upgraded:
                                                   ...
                                                   ...
                                                 Added:
                                                   ...
                                                   ...  */
} RpmOstreeDiffPrintFormat;

namespace rpmostreecxx {
  using RpmOstreeDiffPrintFormat = ::RpmOstreeDiffPrintFormat;
}

void
rpmostree_diff_print_formatted (RpmOstreeDiffPrintFormat format,
                                const char *prefix,
                                guint      max_key_len,
                                GPtrArray *removed,
                                GPtrArray *added,
                                GPtrArray *modified_old,
                                GPtrArray *modified_new);

void
rpmostree_variant_diff_print_formatted (guint     max_key_len,
                                        GVariant *upgraded,
                                        GVariant *downgraded,
                                        GVariant *removed,
                                        GVariant *added);

void
rpmostree_diff_print (GPtrArray *removed,
                      GPtrArray *added,
                      GPtrArray *modified_old,
                      GPtrArray *modified_new);

gboolean
rpmostree_str_has_prefix_in_strv (const char *str,
                                  char      **strv,
                                  int         n);

gboolean
rpmostree_str_has_prefix_in_ptrarray (const char *str,
                                      GPtrArray  *prefixes);

gboolean
rpmostree_str_ptrarray_contains (GPtrArray  *strs,
                                 const char *str);

/* XXX: just return a struct out of these */
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
                                       GError           **error);

/* simpler version of the above */
gboolean
rpmostree_deployment_get_base_layer (OstreeRepo        *repo,
                                     OstreeDeployment  *deployment,
                                     char             **out_base_layer,
                                     GError           **error);

gboolean
rpmostree_migrate_pkgcache_repo (OstreeRepo   *repo,
                                 GCancellable *cancellable,
                                 GError      **error);

gboolean
rpmostree_decompose_sha256_nevra (const char **nevra,
                                  char       **sha256,
                                  GError     **error);

char*
rpmostree_get_deployment_root (OstreeSysroot     *sysroot,
                               OstreeDeployment *deployment);

char *
rpmostree_commit_content_checksum (GVariant *commit);

/* https://github.com/ostreedev/ostree/pull/1132 */
typedef struct {
  gboolean initialized;
  gboolean commit_on_failure;
  OstreeRepo *repo;
} RpmOstreeRepoAutoTransaction;

static inline void
rpmostree_repo_auto_transaction_cleanup (void *p)
{
  RpmOstreeRepoAutoTransaction *autotxn = (RpmOstreeRepoAutoTransaction *)p;
  if (!autotxn->initialized)
    return;

  /* If e.g. package downloads are cancelled, we still want to (try to) commit
   * to avoid redownloading.
   */
  if (autotxn->commit_on_failure)
    (void) ostree_repo_commit_transaction (autotxn->repo, NULL, NULL, NULL);
  else
    (void) ostree_repo_abort_transaction (autotxn->repo, NULL, NULL);
}

static inline gboolean
rpmostree_repo_auto_transaction_start (RpmOstreeRepoAutoTransaction     *autotxn,
                                       OstreeRepo                       *repo,
                                       gboolean                          commit_on_failure,
                                       GCancellable                     *cancellable,
                                       GError                          **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;
  autotxn->commit_on_failure = commit_on_failure;
  autotxn->repo = repo;
  autotxn->initialized = TRUE;
  return TRUE;
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmOstreeRepoAutoTransaction, rpmostree_repo_auto_transaction_cleanup)

gboolean
rpmostree_variant_bsearch_str (GVariant   *array,
                               const char *str,
                               int        *out_pos);

const char*
rpmostree_auto_update_policy_to_str (RpmostreedAutomaticUpdatePolicy policy,
                                     GError **error);

gboolean
rpmostree_str_to_auto_update_policy (const char *str,
                                     RpmostreedAutomaticUpdatePolicy *out_policy,
                                     GError **error);

char*
rpmostree_timestamp_str_from_unix_utc (guint64 t);

char*
rpmostree_maybe_shell_quote (const char *s);

void
rpmostree_journal_error (GError *error);

void
rpmostree_variant_be_to_native (GVariant **v);

void
rpmostree_variant_native_to_be (GVariant **v);

G_END_DECLS
