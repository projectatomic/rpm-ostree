/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antill <james@and.org>
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
#include <gio/gunixoutputstream.h>
#include <stdio.h>
#include <json-glib/json-glib.h>

#include <pwd.h>
#include <grp.h>

#include "libglnx.h"
#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

/* Recursively search a directory for a subpath
 * owned by a uid/gid.
 */
static gboolean
dir_contains_uid_or_gid (int            rootfs_fd,
                         const char    *path,
                         guint32        id,
                         gboolean       is_uid,
                         gboolean      *out_found_match,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean found_match = FALSE;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (rootfs_fd, path, FALSE, &dfd_iter, error))
    return FALSE;

  /* Examine the owner of the directory */
  struct stat stbuf;
  if (!glnx_fstat (dfd_iter.fd, &stbuf, error))
    return FALSE;

  if (is_uid)
    found_match = (id == stbuf.st_uid);
  else
    found_match = (id == stbuf.st_gid);

  /* Early return if we found a match */
  if (found_match)
    {
      *out_found_match = TRUE;
      return TRUE;
    }

  /* Loop over the directory contents */
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        {
          if (!dir_contains_uid_or_gid (dfd_iter.fd, dent->d_name,
                                        id, is_uid, out_found_match,
                                        cancellable, error))
            return FALSE;
        }
      else
        {
          if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          if (is_uid)
            found_match = (id == stbuf.st_uid);
          else
            found_match = (id == stbuf.st_gid);
        }

      if (found_match)
        break;
    }

  *out_found_match = found_match;
  return TRUE;
}

static gboolean
dir_contains_uid (int              rootfs_fd,
                  uid_t            uid,
                  gboolean        *out_found_match,
                  GCancellable    *cancellable,
                  GError         **error)
{
  *out_found_match = FALSE;
  return dir_contains_uid_or_gid (rootfs_fd, ".", uid, TRUE,
                                  out_found_match, cancellable, error);
}

static gboolean
dir_contains_gid (int              rootfs_fd,
                  gid_t            gid,
                  gboolean        *out_found_match,
                  GCancellable    *cancellable,
                  GError         **error)
{
  *out_found_match = FALSE;
  return dir_contains_uid_or_gid (rootfs_fd, ".", gid, FALSE,
                                  out_found_match, cancellable, error);
}

static void
conv_passwd_ent_free (void *vptr)
{
  struct conv_passwd_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr->pw_gecos);
  g_free (ptr->pw_dir);
  g_free (ptr->pw_shell);
  g_free (ptr);
}

GPtrArray *
rpmostree_passwd_data2passwdents (const char *data)
{
  struct passwd *ent = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_passwd_ent_free);

  g_return_val_if_fail (data != NULL, NULL);

  g_autoptr(FILE) mf = fmemopen ((void *)data, strlen (data), "r");

  while ((ent = fgetpwent (mf)))
    {
      struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);

      convent->name = g_strdup (ent->pw_name);
      convent->uid  = ent->pw_uid;
      convent->gid  = ent->pw_gid;
      convent->pw_gecos = g_strdup (ent->pw_gecos);
      convent->pw_dir = g_strdup (ent->pw_dir);
      convent->pw_shell = g_strdup (ent->pw_shell);
      g_ptr_array_add (ret, convent);
    }

  return ret;
}

static int
compare_passwd_ents (gconstpointer a, gconstpointer b)
{
  const struct conv_passwd_ent **sa = (const struct conv_passwd_ent **)a;
  const struct conv_passwd_ent **sb = (const struct conv_passwd_ent **)b;

  return strcmp ((*sa)->name, (*sb)->name);
}

static void
conv_group_ent_free (void *vptr)
{
  struct conv_group_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr);
}

static void
sysuser_ent_free (void *vptr)
{
  struct sysuser_ent *ptr = vptr;
  g_free (ptr->name);
  g_free (ptr->id);
  g_free (ptr->gecos);
  g_free (ptr->dir);
  g_free (ptr->shell);
}

GPtrArray *
rpmostree_passwd_data2groupents (const char *data)
{
  g_return_val_if_fail (data != NULL, NULL);

  g_autoptr(FILE) mf = fmemopen ((void *)data, strlen (data), "r");
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_group_ent_free);
  struct group *ent = NULL;
  while ((ent = fgetgrent (mf)))
    {
      struct conv_group_ent *convent = g_new (struct conv_group_ent, 1);

      convent->name = g_strdup (ent->gr_name);
      convent->gid  = ent->gr_gid;
      /* Want to add anymore, like users? */

      g_ptr_array_add (ret, convent);
    }

  return ret;
}

static int
compare_group_ents (gconstpointer a, gconstpointer b)
{
  const struct conv_group_ent **sa = (const struct conv_group_ent **)a;
  const struct conv_group_ent **sb = (const struct conv_group_ent **)b;

  return strcmp ((*sa)->name, (*sb)->name);
}

static int
compare_sysuser_ents (gconstpointer a, gconstpointer b)
{
  const struct sysuser_ent **sa = (const struct sysuser_ent **)a;
  const struct sysuser_ent **sb = (const struct sysuser_ent **)b;

  /* g > u > m */
  if (!g_str_equal ((*sa)->type, (*sb)->type))
    {
      gboolean is_group_type = g_str_equal ((*sa)->type, "g") || g_str_equal ((*sa)->type, "g");
      if (is_group_type)
        return !strcmp ((*sa)->type, (*sb)->type); /* g is smaller than m and u, we want g > other type here*/

      gboolean is_user_type = g_str_equal ((*sa)->type, "u") || g_str_equal ((*sb)->type, "u");
      if (is_user_type)
        return strcmp ((*sa)->type, (*sb)->type); /* u > m */
    }
  /* We sort the entry name if type happens to be the same */
  if (!g_str_equal ((*sa)->name, (*sb)->name))
      return strcmp ((*sa)->name, (*sb)->name);

  /* This is a collision, when both name and type matches, could error out here */
  return 0;
}

gboolean
rpmostree_passwdents2sysusers (GPtrArray  *passwd_ents,
                               GPtrArray  **out_sysusers_entries,
                               GError     **error)
{
  /* Do the assignment inside the function so we don't need to expose sysuser_ent
   * to other files */
  GPtrArray *sysusers_array = NULL;
  sysusers_array = *out_sysusers_entries ?: g_ptr_array_new_with_free_func (sysuser_ent_free);

  for (int counter = 0; counter < passwd_ents->len; counter++)
    {
      struct conv_passwd_ent *convent = passwd_ents->pdata[counter];
      struct sysuser_ent *sysent = g_new (struct sysuser_ent, 1);

      /* Systemd-sysusers also supports uid:gid format. That case was used
       * when creating user and group pairs with different numeric UID and GID values.*/
      if (convent->uid != convent->gid)
        sysent->id = g_strdup_printf ("%u:%u", convent->uid, convent->gid);
      else
        sysent->id = g_strdup_printf ("%u", convent->uid);

      sysent->type = "u";
      sysent->name = g_strdup (convent->name);

      /* Gecos may contain multiple words, thus adding a quote here to make it as a 'word' */
      sysent->gecos = (g_str_equal (convent->pw_gecos, "")) ? NULL :
                       g_strdup_printf ("\"%s\"", convent->pw_gecos);
      sysent->dir = (g_str_equal (convent->pw_dir, ""))? NULL :
                    g_steal_pointer (&convent->pw_dir);
      sysent->shell = g_steal_pointer (&convent->pw_shell);

      g_ptr_array_add (sysusers_array, sysent);
    }
  /* Do the assignment at the end if the sysusers_table was not initialized */
  if (*out_sysusers_entries == NULL)
    *out_sysusers_entries = g_steal_pointer (&sysusers_array);

  return TRUE;
}

gboolean
rpmostree_groupents2sysusers (GPtrArray  *group_ents,
                              GPtrArray  **out_sysusers_entries,
                              GError     **error)
{
  /* Similar to converting passwd to sysusers, we do assignment inside the function */
  GPtrArray *sysusers_array = NULL;
  sysusers_array  = *out_sysusers_entries ?: g_ptr_array_new_with_free_func (sysuser_ent_free);

  for (int counter = 0; counter < group_ents->len; counter++)
    {
      struct conv_group_ent *convent = group_ents->pdata[counter];
      struct sysuser_ent *sysent = g_new (struct sysuser_ent, 1);

      sysent->type = "g";
      sysent->name = g_steal_pointer (&convent->name);
      sysent->id = g_strdup_printf ("%u", convent->gid);
      sysent->gecos = NULL;
      sysent->dir = NULL;
      sysent->shell = NULL;

      g_ptr_array_add (sysusers_array, sysent);
    }
  /* Do the assignment at the end if the sysusers_array was not initialized */
  if (*out_sysusers_entries == NULL)
    *out_sysusers_entries = g_steal_pointer (&sysusers_array);

  return TRUE;
}

gboolean
rpmostree_passwd_sysusers2char (GPtrArray *sysusers_entries,
                                char      **out_content,
                                GError    **error)
{

  GString* sysuser_content = g_string_new (NULL);
  /* We do the sorting before conversion */
  g_ptr_array_sort (sysusers_entries, compare_sysuser_ents);
  for (int counter = 0; counter < sysusers_entries->len; counter++)
    {
      struct sysuser_ent *sysent = sysusers_entries->pdata[counter];
      const char *shell = sysent->shell ?: "-";
      const char *gecos = sysent->gecos ?: "-";
      const char *dir = sysent->dir ?: "-";
      g_autofree gchar* line_content = g_strjoin (" ", sysent->type, sysent->name,
                                                  sysent->id, gecos, dir, shell, NULL);
      g_string_append_printf (sysuser_content, "%s\n", line_content);
    }
  if (out_content)
    *out_content = g_string_free(sysuser_content, FALSE);

  return TRUE;
}

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
*/
static gboolean
rpmostree_check_passwd_groups (gboolean         passwd,
                               OstreeRepo      *repo,
                               int              rootfs_fd,
                               RORTreefile     *treefile_rs,
                               JsonObject      *treedata,
                               const char      *previous_commit,
                               GCancellable    *cancellable,
                               GError         **error)
{
  const char *chk_type = "previous";
  const char *commit_filepath = passwd ? "usr/lib/passwd" : "usr/lib/group";
  const char *json_conf_name  = passwd ? "check-passwd" : "check-groups";
  const char *json_conf_ign   = passwd ? "ignore-removed-users" : "ignore-removed-groups";
  g_autoptr(GPtrArray) ignore_removed_ents = NULL;
  gboolean ignore_all_removed = FALSE;
  g_autoptr(GPtrArray) old_ents = NULL;
  g_autoptr(GPtrArray) new_ents = NULL;

  if (json_object_has_member (treedata, json_conf_name))
    {
      JsonObject *chk = json_object_get_object_member (treedata,json_conf_name);
      if (!chk)
        return glnx_throw (error, "%s is not an object", json_conf_name);

      chk_type = _rpmostree_jsonutil_object_require_string_member (chk, "type", error);
      if (!chk_type)
        return FALSE;

      if (g_str_equal (chk_type, "none"))
        return TRUE; /* Note early return */
      else if (g_str_equal (chk_type, "previous"))
        ; /* Handled below */
      else if (g_str_equal (chk_type, "file"))
        ; /* Handled below */
      else if (g_str_equal (chk_type, "data"))
        {
          JsonNode *ents_node = json_object_get_member (chk, "entries");
          JsonObject *ents_obj = NULL;
          GList *ents;
          GList *iter;

          if (!ents_node)
            return glnx_throw (error, "No entries member for data in %s", json_conf_name);

          ents_obj = json_node_get_object (ents_node);

          if (passwd)
            old_ents = g_ptr_array_new_with_free_func (conv_passwd_ent_free);
          else
            old_ents = g_ptr_array_new_with_free_func (conv_group_ent_free);

          ents = json_object_get_members (ents_obj);
          for (iter = ents; iter; iter = iter->next)
            if (passwd)
            {
              const char *name = iter->data;
              JsonNode *val = json_object_get_member (ents_obj, name);
              JsonNodeType child_type = json_node_get_node_type (val);
              gint64 uid = 0;
              gint64 gid = 0;

              if (child_type != JSON_NODE_ARRAY)
                {
                  if (!_rpmostree_jsonutil_object_require_int_member (ents_obj, name, &uid, error))
                    return FALSE;
                  gid = uid;
                }
              else
                {
                  JsonArray *child_array = json_node_get_array (val);
                  guint len = json_array_get_length (child_array);

                  if (!len || (len > 2))
                    return glnx_throw (error, "Array %s is only for uid and gid. Has length %u",
                                       name, len);
                  if (!_rpmostree_jsonutil_array_require_int_element (child_array, 0, &uid, error))
                    return FALSE;
                  if (len == 1)
                    gid = uid;
                  else if (!_rpmostree_jsonutil_array_require_int_element (child_array, 1, &gid, error))
                    return FALSE;
                }

              struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);
              convent->name = g_strdup (name);
              convent->uid  = uid;
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
            else
            {
              const char *name = iter->data;
              gint64 gid = 0;

              if (!_rpmostree_jsonutil_object_require_int_member (ents_obj, name, &gid, error))
                return FALSE;

              struct conv_group_ent *convent = g_new (struct conv_group_ent, 1);
              convent->name = g_strdup (name);
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
        }
      else
        return glnx_throw (error, "Invalid %s type '%s'", json_conf_name, chk_type);
    }

  g_autoptr(GFile) old_path = NULL;
  g_autofree char *old_contents = NULL;
  if (g_str_equal (chk_type, "previous"))
    {
      if (previous_commit != NULL)
        {
          g_autoptr(GFile) root = NULL;

          if (!ostree_repo_read_commit (repo, previous_commit, &root, NULL, NULL, error))
            return FALSE;

          old_path = g_file_resolve_relative_path (root, commit_filepath);
          /* Note this one can't be ported to glnx_file_get_contents_utf8_at() because
           * we're loading from ostree via `OstreeRepoFile`.
           */
          if (!g_file_load_contents (old_path, cancellable, &old_contents, NULL, NULL, error))
            return FALSE;
        }
      else
        {
          /* Early return */
          return TRUE;
        }
    }
  else if (g_str_equal (chk_type, "file"))
    {
      int fd = passwd ? ror_treefile_get_passwd_fd (treefile_rs) :
        ror_treefile_get_group_fd (treefile_rs);
      old_contents = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
      if (!old_contents)
        return FALSE;
    }

  if (g_str_equal (chk_type, "previous") || g_str_equal (chk_type, "file"))
    {
      if (passwd)
        old_ents = rpmostree_passwd_data2passwdents (old_contents);
      else
        old_ents = rpmostree_passwd_data2groupents (old_contents);
    }
  g_assert (old_ents);

  if (passwd)
    g_ptr_array_sort (old_ents, compare_passwd_ents);
  else
    g_ptr_array_sort (old_ents, compare_group_ents);

  g_autofree char *new_contents =
    glnx_file_get_contents_utf8_at (rootfs_fd, commit_filepath, NULL,
                                    cancellable, error);
  if (!new_contents)
      return FALSE;

  if (json_object_has_member (treedata, json_conf_ign))
    {
      ignore_removed_ents = g_ptr_array_new ();
      if (!_rpmostree_jsonutil_append_string_array_to (treedata, json_conf_ign,
                                                       ignore_removed_ents,
                                                       error))
        return FALSE;
    }
  ignore_all_removed = rpmostree_str_ptrarray_contains (ignore_removed_ents, "*");

  if (passwd)
    {
      new_ents = rpmostree_passwd_data2passwdents (new_contents);
      g_ptr_array_sort (new_ents, compare_passwd_ents);
    }
  else
    {
      new_ents = rpmostree_passwd_data2groupents (new_contents);
      g_ptr_array_sort (new_ents, compare_group_ents);
    }

  unsigned int oiter = 0;
  unsigned int niter = 0;
  while ((oiter < old_ents->len) && (niter < new_ents->len))
    if (passwd)
    {
      struct conv_passwd_ent *odata = old_ents->pdata[oiter];
      struct conv_passwd_ent *ndata = new_ents->pdata[niter];
      int cmp = 0;

      cmp = g_strcmp0 (odata->name, ndata->name);

      if (cmp == 0)
        {
          if (odata->uid != ndata->uid)
            return glnx_throw (error, "passwd UID changed: %s (%u to %u)",
                               odata->name, (guint)odata->uid, (guint)ndata->uid);
          if (odata->gid != ndata->gid)
            return glnx_throw (error, "passwd GID changed: %s (%u to %u)",
                               odata->name, (guint)odata->gid, (guint)ndata->gid);

          ++oiter;
          ++niter;
        }
      else if (cmp < 0) /* Missing value from new passwd */
        {
          gboolean found_matching_uid;

          if (ignore_all_removed ||
              rpmostree_str_ptrarray_contains (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored user missing from new passwd file: %s\n",
                       odata->name);
            }
          else
            {
              if (!dir_contains_uid (rootfs_fd, odata->uid, &found_matching_uid,
                                     cancellable, error))
                return FALSE;

              if (found_matching_uid)
                return glnx_throw (error, "User missing from new passwd file: %s", odata->name);
              else
                g_print ("User removed from new passwd file: %s\n",
                         odata->name);
            }

          ++oiter;
        }
      else
        {
          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }
    else
    {
      struct conv_group_ent *odata = old_ents->pdata[oiter];
      struct conv_group_ent *ndata = new_ents->pdata[niter];
      int cmp = 0;

      cmp = g_strcmp0 (odata->name, ndata->name);

      if (cmp == 0)
        {
          if (odata->gid != ndata->gid)
            return glnx_throw (error, "group GID changed: %s (%u to %u)",
                               odata->name, (guint)odata->gid, (guint)ndata->gid);

          ++oiter;
          ++niter;
          continue;
        }
      else if (cmp < 0) /* Missing value from new group */
        {

          if (ignore_all_removed ||
              rpmostree_str_ptrarray_contains (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored group missing from new group file: %s\n",
                       odata->name);
            }
          else
            {
              gboolean found_gid;

              if (!dir_contains_gid (rootfs_fd, odata->gid, &found_gid,
                                     cancellable, error))
                return FALSE;

              if (found_gid)
                return glnx_throw (error, "Group missing from new group file: %s", odata->name);
              else
                g_print ("Group removed from new passwd file: %s\n",
                         odata->name);
            }

          ++oiter;
        }
      else
        {
          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

  if (passwd)
    {
      if (oiter < old_ents->len)
        {
          struct conv_passwd_ent *odata = old_ents->pdata[oiter];

          return glnx_throw (error, "User missing from new passwd file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          struct conv_passwd_ent *ndata = new_ents->pdata[niter];

          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }
  else
    {
      if (oiter < old_ents->len)
        {
          struct conv_group_ent *odata = old_ents->pdata[oiter];

          return glnx_throw (error, "Group missing from new group file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          struct conv_group_ent *ndata = new_ents->pdata[niter];

          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

  return TRUE;
}

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
   If sysusers is enabled, this delegates to that.
*/
gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        int              rootfs_fd,
                        RORTreefile     *treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  if (ror_treefile_get_sysusers (treefile_rs))
    {
      return ror_sysusers_final_postprocess (rootfs_fd, error);
    }
  else
    {
      return rpmostree_check_passwd_groups (TRUE, repo, rootfs_fd, treefile_rs,
                                            treedata, previous_commit,
                                            cancellable, error);
    }
}

/* See "man 5 group" We just need to make sure the name and gid match,
   and that none are missing. Don't care about users.
 */
gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        int              rootfs_fd,
                        RORTreefile     *treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (FALSE, repo, rootfs_fd, treefile_rs,
                                        treedata, previous_commit,
                                        cancellable, error);
}

/* Like glnx_openat_rdonly() but returns a FILE*.
 * Or, like fopen() but fd-relative.
 *
 * (And in both cases use GError)
 */
static FILE *
open_file_stream_read_at (int dfd,
                          const char *path,
                          GError **error)
{
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (dfd, path, TRUE, &fd, error))
    return FALSE;
  FILE *ret = fdopen (fd, "r");
  if (!ret)
    return glnx_null_throw_errno_prefix (error, "fdopen");
  /* fdopen() steals ownership of fd */
  fd = -1;
  return ret;
}

/* Like fopen() but fd-relative and with GError. */
static FILE *
open_file_stream_write_at (int dfd,
                           const char *path,
                           const char *mode,
                           GError **error)
{
  /* Explicitly use 0664 rather than 0666 as fopen() does since IMO if one wants
   * a world-writable file, do it explicitly.
   */
  glnx_autofd int fd = openat (dfd, path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY, 0664);
  if (fd < 0)
    return glnx_null_throw_errno_prefix (error, "openat(%s)", path);
  FILE *ret = fdopen (fd, mode);
  if (!ret)
    return glnx_null_throw_errno_prefix (error, "fdopen");
  /* fdopen() steals ownership of fd */
  fd = -1;
  return ret;
}

/*
 * This function is taking the /etc/passwd generated in the install root (really
 * in /usr/etc at this point), and splitting it into two streams: a new
 * /etc/passwd that just contains the root entry, and /usr/lib/passwd which
 * contains everything else.
 *
 * The implementation is kind of horrible because I wanted to avoid
 * duplicating the user/group code.
 */
gboolean
rpmostree_passwd_migrate_except_root (int            rootfs_dfd,
                                      RpmOstreePasswdMigrateKind    kind,
                                      GHashTable    *preserve,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  GLNX_AUTO_PREFIX_ERROR ("passwd migration", error);
  const char *name = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";

  const char *src_path = glnx_strjoina ("usr/etc/", name);
  g_autoptr(FILE) src_stream = open_file_stream_read_at (rootfs_dfd, src_path, error);
  if (!src_stream)
    return FALSE;

  const char *etctmp_path = glnx_strjoina ("usr/etc/", name, ".tmp");
  g_autoptr(FILE) etcdest_stream = open_file_stream_write_at (rootfs_dfd, etctmp_path, "w", error);
  if (!etcdest_stream)
    return FALSE;

  const char *usrdest_path = glnx_strjoina ("usr/lib/", name);
  g_autoptr(FILE) usrdest_stream = open_file_stream_write_at (rootfs_dfd, usrdest_path, "a", error);
  if (!usrdest_stream)
    return FALSE;

  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;

      if (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetpwent");
          else
            break;
        }

      guint32 id;
      const char *name;
      if (pw)
        {
          id = pw->pw_uid;
          name = pw->pw_name;
        }
      else
        {
          id = gr->gr_gid;
          name = gr->gr_name;
        }

      FILE *deststream;
      if (id == 0)
        deststream = etcdest_stream;
      else
        deststream = usrdest_stream;

      int r;
      if (pw)
        r = putpwent (pw, deststream);
      else
        r = putgrent (gr, deststream);

      /* If it's marked in the preserve group, we need to write to
       * *both* /etc and /usr/lib in order to preserve semantics for
       * upgraded systems from before we supported the preserve
       * concept.
       */
      if (preserve && g_hash_table_contains (preserve, name))
        {
          /* We should never be trying to preserve the root entry, it
           * should always be only in /etc.
           */
          g_assert (deststream == usrdest_stream);
          if (pw)
            r = putpwent (pw, etcdest_stream);
          else
            r = putgrent (gr, etcdest_stream);
        }

      if (r == -1)
        return glnx_throw_errno_prefix (error, "putpwent");
    }

  if (!glnx_stdio_file_flush (etcdest_stream, error))
    return FALSE;
  if (!glnx_stdio_file_flush (usrdest_stream, error))
    return FALSE;

  if (!glnx_renameat (rootfs_dfd, etctmp_path, rootfs_dfd, src_path, error))
    return FALSE;

  return TRUE;
}

static gboolean
_rpmostree_gfile2stdio (GFile         *source,
                        char         **storage_buf,
                        FILE         **ret_src_stream,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gsize len;

  /* We read the file into memory using Gio (which talks
   * to libostree), then memopen it, which works with libc.
   */
  if (!g_file_load_contents (source, cancellable,
                             storage_buf, &len, NULL, error))
    return FALSE;

  if (len == 0)
    {
      *ret_src_stream = NULL;
      return TRUE; /* Early return */
    }

  FILE *src_stream = fmemopen (*storage_buf, len, "r");
  if (!src_stream)
    return glnx_throw_errno_prefix (error, "fmemopen");

  *ret_src_stream = g_steal_pointer (&src_stream);
  return TRUE;
}

static gboolean
concat_entries (FILE    *src_stream,
                FILE    *dest_stream,
                RpmOstreePasswdMigrateKind kind,
                GHashTable *seen_names,
                GError **error)
{
  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;
      int r;
      const char *name;

      if (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetpwent");
          else
            break;
        }

      if (pw)
        name = pw->pw_name;
      else
        name = gr->gr_name;

      /* Deduplicate */
      if (g_hash_table_lookup (seen_names, name))
        continue;
      g_hash_table_add (seen_names, g_strdup (name));

      if (pw)
        r = putpwent (pw, dest_stream);
      else
        r = putgrent (gr, dest_stream);

      if (r == -1)
        return glnx_throw_errno_prefix (error, "putpwent");
    }

  return TRUE;
}

static gboolean
concat_passwd_file (int              rootfs_fd,
                    GFile           *previous_commit,
                    RpmOstreePasswdMigrateKind kind,
                    GCancellable    *cancellable,
                    GError         **error)
{
  const char *filename = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";
  g_autofree char *usretc_subpath = g_strconcat ("usr/etc/", filename, NULL);
  g_autofree char *usrlib_subpath = g_strconcat ("usr/lib/", filename, NULL);
  g_autoptr(GFile) orig_etc_content =
    g_file_resolve_relative_path (previous_commit, usretc_subpath);
  g_autoptr(GFile) orig_usrlib_content =
    g_file_resolve_relative_path (previous_commit, usrlib_subpath);
  g_autoptr(GHashTable) seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  GFile *sources[] = { orig_etc_content, orig_usrlib_content };

  gboolean have_etc = g_file_query_exists (orig_etc_content, NULL);
  gboolean have_usr = g_file_query_exists (orig_usrlib_content, NULL);

  /* This could actually happen after we transition to
   * systemd-sysusers; we won't have a need for preallocated user data
   * in the tree.
   */
  if (!(have_etc || have_usr))
    return TRUE;

  const char *target_etc_filename = glnx_strjoina ("etc/", filename);
  g_autoptr(FILE) dest_stream = open_file_stream_write_at (rootfs_fd, target_etc_filename, "w", error);
  if (!dest_stream)
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (sources); i++)
    {
      GFile *source = sources[i];
      g_autoptr(FILE) src_stream = NULL;

      g_autofree char *contents = NULL;
      if (!_rpmostree_gfile2stdio (source, &contents, &src_stream,
                                   cancellable, error))
        return FALSE;

      if (!src_stream)
        continue;

      if (!concat_entries (src_stream, dest_stream, kind,
                           seen_names, error))
        return FALSE;
    }

  if (!glnx_stdio_file_flush (dest_stream, error))
    return FALSE;

  return TRUE;
}

static gboolean
_data_from_json (int              rootfs_dfd,
                 const char      *dest,
                 RORTreefile     *treefile_rs,
                 JsonObject      *treedata,
                 RpmOstreePasswdMigrateKind kind,
                 gboolean        *out_found,
                 GCancellable    *cancellable,
                 GError         **error)
{
  const gboolean passwd = (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD);
  const char *json_conf_name = passwd ? "check-passwd" : "check-groups";
  *out_found = FALSE;
  if (!json_object_has_member (treedata, json_conf_name))
    return TRUE;

  JsonObject *chk = json_object_get_object_member (treedata,json_conf_name);
  if (!chk)
    return TRUE;

  const char *chk_type =
    _rpmostree_jsonutil_object_require_string_member (chk, "type", error);
  if (!chk_type)
    return FALSE;

  if (!g_str_equal (chk_type, "file"))
    return TRUE;

  const char *filename =
    _rpmostree_jsonutil_object_require_string_member (chk, "filename", error);
  if (!filename)
    return FALSE;

  /* migrate the check data from the specified file to /etc */
  int fd = passwd ? ror_treefile_get_passwd_fd (treefile_rs) :
    ror_treefile_get_group_fd (treefile_rs);
  size_t len = 0;
  g_autofree char *contents = glnx_fd_readall_utf8 (fd, &len, cancellable, error);
  if (!contents)
    return FALSE;
  g_autoptr(FILE) src_stream = fmemopen (contents, len, "r");
  if (!src_stream)
    return glnx_throw_errno_prefix (error, "fmemopen");

  /* no matter what we've used the data now */
  *out_found = TRUE;

  const char *filebasename = passwd ? "passwd" : "group";
  const char *target_etc_filename = glnx_strjoina (dest, filebasename);
  g_autoptr(FILE) dest_stream = open_file_stream_write_at (rootfs_dfd, target_etc_filename, "w", error);
  if (!dest_stream)
    return FALSE;

  g_autoptr(GHashTable) seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  if (!concat_entries (src_stream, dest_stream, kind, seen_names, error))
    return FALSE;

  return TRUE;
}

/* For composes/treefiles, we support various passwd/group handling.  This
 * function is primarily responsible for handling the "previous" and "file"
 * paths; in both cases we inject data into the tree before even laying
 * down any files, and notably before running RPM `useradd` etc.
 */
gboolean
rpmostree_passwd_compose_prep (int              rootfs_dfd,
                               OstreeRepo      *repo,
                               gboolean         unified_core,
                               RORTreefile     *treefile_rs,
                               JsonObject      *treedata,
                               const char      *previous_checksum,
                               GCancellable    *cancellable,
                               GError         **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Preparing passwd/group", error);
  gboolean generate_from_previous = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata, "preserve-passwd",
                                                               &generate_from_previous, error))
    return FALSE;

  if (!generate_from_previous)
    return TRUE; /* Nothing to do */

  const char *dest = (unified_core) ? "usr/etc/" : "etc/";

  /* Create /etc in the target root; FIXME - should ensure we're using
   * the right permissions from the filesystem RPM.  Doing this right
   * is really hard because filesystem depends on setup which installs
   * the files...
   */
  if (!glnx_shutil_mkdir_p_at (rootfs_dfd, dest, 0755, cancellable, error))
    return FALSE;

  gboolean found_passwd_data = FALSE;
  if (!_data_from_json (rootfs_dfd, dest, treefile_rs,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                        &found_passwd_data, cancellable, error))
    return FALSE;

  gboolean found_groups_data = FALSE;
  if (!_data_from_json (rootfs_dfd, dest, treefile_rs,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                        &found_groups_data, cancellable, error))
    return FALSE;

  /* We should error if we are getting passwd data from JSON and group from
   * previous commit, or vice versa, as that'll confuse everyone when it goes
   * wrong. */
  if ( found_passwd_data && !found_groups_data)
    return glnx_throw (error, "Configured to migrate passwd data from JSON, and group data from commit");
  if (!found_passwd_data &&  found_groups_data)
    return glnx_throw (error, "Configured to migrate passwd data from commit, and group data from JSON");

  if (found_passwd_data || !previous_checksum)
    return TRUE; /* Nothing to do */

  g_assert (repo);
  g_autoptr(GFile) previous_root = NULL;
  if (!ostree_repo_read_commit (repo, previous_checksum, &previous_root, NULL,
                                cancellable, error))
    return FALSE;

  if (!concat_passwd_file (rootfs_dfd, previous_root, RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                          cancellable, error))
    return FALSE;


  if (!concat_passwd_file (rootfs_dfd, previous_root, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                           cancellable, error))
    return FALSE;

  return TRUE;
}

static const char *usrlib_pwgrp_files[] = { "passwd", "group" };
/* Lock/backup files that should not be in the base commit (TODO fix) */
static const char *pwgrp_lock_and_backup_files[] = { ".pwd.lock", "passwd-", "group-",
                                                     "shadow-", "gshadow-",
                                                     "subuid-", "subgid-" };
static const char *pwgrp_shadow_files[] = { "shadow", "gshadow",
                                            "subuid", "subgid"};

static gboolean
rootfs_has_usrlib_passwd (int rootfs_dfd,
                       gboolean *out_have_passwd,
                       GError **error)
{
  /* Does this rootfs have a usr/lib/passwd?  We might be doing a
   * container or something else.
   */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, "usr/lib/passwd", NULL, 0, error))
    return FALSE;
  *out_have_passwd = (errno == 0);
  return TRUE;
}

/* This may be leftover in the tree from an older version of rpm-ostree that
 * didn't clean them up at compose time, and having them exist will mean
 * rofiles-fuse will prevent useradd from opening it for write.
 */
gboolean
rpmostree_passwd_cleanup (int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (pwgrp_lock_and_backup_files); i++)
    {
      const char *file = pwgrp_lock_and_backup_files[i];
      if (unlinkat (rootfs_dfd, glnx_strjoina ("usr/etc/", file), 0) < 0)
        {
          if (errno == ENOENT)
            ;
          else
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
    }

  return TRUE;
}

/* We actually want RPM to inject to /usr/lib/passwd - we
 * accomplish this by temporarily renaming /usr/lib/passwd -> /usr/etc/passwd
 * (Which appears as /etc/passwd via our compatibility symlink in the bubblewrap
 *  script runner). We also copy the merge deployment's /etc/passwd to
 *  /usr/lib/passwd, so that %pre scripts are aware of newly added system users
 *  not in the tree's /usr/lib/passwd (through nss-altfiles in the container).
 */
gboolean
rpmostree_passwd_prepare_rpm_layering (int                rootfs_dfd,
                                       const char        *merge_passwd_dir,
                                       gboolean          *out_have_passwd,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
  if (!rpmostree_passwd_cleanup (rootfs_dfd, cancellable, error))
    return FALSE;

  /* Break hardlinks for the shadow files, since shadow-utils currently uses
   * O_RDWR unconditionally.
   */
  for (guint i = 0; i < G_N_ELEMENTS (pwgrp_shadow_files); i++)
    {
      const char *file = pwgrp_shadow_files[i];
      g_autofree char *src = g_strconcat ("etc/", file, NULL);

      if (!glnx_fstatat_allow_noent (rootfs_dfd, src, NULL, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT)
        continue;

      if (!ostree_break_hardlink (rootfs_dfd, src, TRUE,
                                  cancellable, error))
        return FALSE;
    }

  if (!rootfs_has_usrlib_passwd (rootfs_dfd, out_have_passwd, error))
    return FALSE;
  if (!*out_have_passwd)
    return TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (usrlib_pwgrp_files); i++)
    {
      const char *file = usrlib_pwgrp_files[i];
      g_autofree char *usrlibfile = g_strconcat ("usr/lib/", file, NULL);
      g_autofree char *usretcfile = g_strconcat ("etc/", file, NULL);
      g_autofree char *usrlibfiletmp = g_strconcat ("usr/lib/", file, ".tmp", NULL);

      /* Retain the current copies in /etc as backups */
      if (!glnx_renameat (rootfs_dfd, usretcfile, rootfs_dfd,
                          glnx_strjoina (usretcfile, ".rpmostreesave"), error))
        return FALSE;

      /* Copy /usr/lib/{passwd,group} -> /etc (breaking hardlinks) */
      if (!glnx_file_copy_at (rootfs_dfd, usrlibfile, NULL,
                              rootfs_dfd, usretcfile,
                              GLNX_FILE_COPY_NOXATTRS,
                              cancellable, error))
        return FALSE;

      /* Copy the merge's passwd/group to usr/lib (breaking hardlinks) */
      if (merge_passwd_dir)
        {
          if (!glnx_file_copy_at (AT_FDCWD,
                                  glnx_strjoina (merge_passwd_dir, "/", file), NULL,
                                  rootfs_dfd, usrlibfiletmp,
                                  GLNX_FILE_COPY_OVERWRITE | GLNX_FILE_COPY_NOXATTRS,
                                  cancellable, error))
            return FALSE;

          if (!glnx_renameat (rootfs_dfd, usrlibfiletmp, rootfs_dfd, usrlibfile, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
rpmostree_passwd_complete_rpm_layering (int       rootfs_dfd,
                                        GError  **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (usrlib_pwgrp_files); i++)
    {
      const char *file = usrlib_pwgrp_files[i];
      /* And now the inverse: /etc/passwd -> /usr/lib/passwd */
      if (!glnx_renameat (rootfs_dfd, glnx_strjoina ("etc/", file),
                          rootfs_dfd, glnx_strjoina ("usr/lib/", file), error))
        return FALSE;
      /* /etc/passwd.rpmostreesave -> /etc/passwd */
      if (!glnx_renameat (rootfs_dfd, glnx_strjoina ("etc/", file, ".rpmostreesave"),
                          rootfs_dfd, glnx_strjoina ("etc/", file), error))
        return FALSE;
    }
  /* However, we leave the (potentially modified) shadow files in place.
   * In actuality, nothing should change /etc/shadow or /etc/gshadow, so
   * we'll just have to pay the (tiny) cost of re-checksumming.
   */
  return TRUE;
}

struct RpmOstreePasswdDB
{
  GHashTable *users;
  GHashTable *groups;
};

static gboolean
add_passwd_to_hash (int rootfs_dfd, const char *path,
                    GHashTable *users, GError **error)
{
  g_autoptr(FILE) src_stream = open_file_stream_read_at (rootfs_dfd, path, error);
  if (!src_stream)
    return FALSE;

  while (TRUE)
    {
      errno = 0;
      struct passwd *pw = fgetpwent (src_stream);
      if (!pw)
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetpwent");
          break;
        }
      g_hash_table_insert (users, GUINT_TO_POINTER (pw->pw_uid), g_strdup (pw->pw_name));
    }

  return TRUE;
}

static gboolean
add_groups_to_hash (int rootfs_dfd, const char *path,
                   GHashTable *groups, GError **error)
{
  g_autoptr(FILE) src_stream = open_file_stream_read_at (rootfs_dfd, path, error);
  if (!src_stream)
    return FALSE;

  while (TRUE)
    {
      errno = 0;
      struct group *gr = fgetgrent (src_stream);
      if (!gr)
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetgrent");
          break;
        }
      g_hash_table_insert (groups, GUINT_TO_POINTER (gr->gr_gid), g_strdup (gr->gr_name));
    }

  return TRUE;
}

RpmOstreePasswdDB *
rpmostree_passwddb_open (int rootfs, GCancellable *cancellable, GError **error)
{
  g_autoptr(RpmOstreePasswdDB) ret = g_new0 (RpmOstreePasswdDB, 1);
  ret->users = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  ret->groups = g_hash_table_new_full (NULL, NULL, NULL, g_free);

  if (!add_passwd_to_hash (rootfs, "usr/etc/passwd", ret->users, error))
    return NULL;
  if (!glnx_fstatat_allow_noent (rootfs, "usr/lib/passwd", NULL, 0, error))
    return FALSE;
  const gboolean have_usrlib_passwd = (errno == 0);
  if (have_usrlib_passwd)
    {
      if (!add_passwd_to_hash (rootfs, "usr/lib/passwd", ret->users, error))
        return NULL;
    }

  if (!add_groups_to_hash (rootfs, "usr/etc/group", ret->groups, error))
    return NULL;
  if (have_usrlib_passwd)
    {
      if (!add_groups_to_hash (rootfs, "usr/lib/group", ret->groups, error))
        return NULL;
    }

  return g_steal_pointer (&ret);
}

const char *
rpmostree_passwddb_lookup_user (RpmOstreePasswdDB *db, uid_t uid)
{
  return g_hash_table_lookup (db->users, GUINT_TO_POINTER (uid));
}

const char *
rpmostree_passwddb_lookup_group (RpmOstreePasswdDB *db, gid_t gid)
{
  return g_hash_table_lookup (db->groups, GUINT_TO_POINTER (gid));
}

void
rpmostree_passwddb_free (RpmOstreePasswdDB *db)
{
  g_hash_table_unref (db->users);
  g_hash_table_unref (db->groups);
  g_free (db);
}
