/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#include "rpmostree-builtins.h"
#include "rpmostree-treepkgdiff.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static gboolean opt_pretty;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "pretty", 'p', 0, G_OPTION_ARG_NONE, &opt_pretty, "Display status in formatted rows", NULL },
  { NULL }
};

static void 
printchar (char *s, int n)
{
  int i;
  for (i=0; i < n; i++)
    g_print ("%s",s);
  g_print ("\n");
}

/* FIXME: This is a copy of ot_admin_checksum_version */
static char *
checksum_version (GVariant *checksum)
{
  gs_unref_variant GVariant *metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

static char *
version_of_commit (OstreeRepo *repo, const char *checksum)
{
  gs_unref_variant GVariant *variant = NULL;
  
  /* Shouldn't fail, but if it does, we ignore it */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, NULL))
    goto out;

  return checksum_version (variant);
 out:
  return NULL;
}

static gboolean
deployment_get_gpg_verify (OstreeDeployment *deployment,
                           OstreeRepo *repo)
{
  /* XXX Something like this could be added to the OstreeDeployment
   *     API in libostree if the OstreeRepo parameter is acceptable. */

  GKeyFile *origin;
  gs_free char *refspec = NULL;
  gs_free char *remote = NULL;
  gboolean gpg_verify = FALSE;

  origin = ostree_deployment_get_origin (deployment);

  if (origin == NULL)
    goto out;

  refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);

  if (refspec == NULL)
    goto out;

  if (!ostree_parse_refspec (refspec, &remote, NULL, NULL))
    goto out;

  (void) ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, NULL);

out:
  return gpg_verify;
}

gboolean
rpmostree_builtin_status (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;  // list of all depoyments
  OstreeDeployment *booted_deployment = NULL;   // current booted deployment
  GOptionContext *context = g_option_context_new ("- Get the version of the booted system");
  const guint CSUM_DISP_LEN = 10; // number of checksum characters to display
  guint i, j;
  guint max_timestamp_len = 19; // length of timestamp "YYYY-MM-DD HH:MM:SS"
  guint max_id_len = CSUM_DISP_LEN; // length of checksum ID
  guint max_osname_len = 0; // maximum length of osname - determined in conde
  guint max_refspec_len = 0; // maximum length of refspec - determined in code
  guint max_version_len = 0; // maximum length of version - determined in code
  guint buffer = 5; // minimum space between end of one entry and new column
  gs_free char *booted_csum = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  deployments = ostree_sysroot_get_deployments (sysroot);

  /* find lengths for use in column output */
  if(!opt_pretty)
    {
      /* find max lengths of osname and refspec */
      for (j = 0; j < deployments->len; j++) 
        {
          const char *csum = ostree_deployment_get_csum (deployments->pdata[j]);
          GKeyFile *origin;
          gs_free char *origin_refspec = NULL;
          OstreeDeployment *deployment = deployments->pdata[j];
          gs_free char *version_string = NULL;

          max_osname_len = MAX (max_osname_len, strlen (ostree_deployment_get_osname (deployment)));

          origin = ostree_deployment_get_origin (deployment);
          if (!origin)
            origin_refspec = "none";
          else
            {
              origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
              if (!origin_refspec)
                origin_refspec = g_strdup ("<unknown origin type>");
            }
          max_refspec_len = MAX (max_refspec_len, strlen (origin_refspec));

          version_string = version_of_commit (repo, csum);
          if (version_string)
            max_version_len = MAX (max_version_len, strlen (version_string));
        }
      /* print column headers */
      g_print ("  %-*s", max_timestamp_len+buffer,"TIMESTAMP (UTC)");
      if (max_version_len)
        g_print ("%-*s", max_version_len+buffer,"VERSION");
      g_print ("%-*s%-*s%-*s\n",
               max_id_len+buffer, "ID",
               max_osname_len+buffer, "OSNAME",
               max_refspec_len+buffer, "REFSPEC");
    }
  /* header for "pretty" row output */
  else
    printchar ("=", 60);

  /* print entries for each deployment */
  for (i=0; i<deployments->len; i++)
    {
      gs_unref_variant GVariant *commit = NULL;
      const char *csum = ostree_deployment_get_csum (deployments->pdata[i]);
      OstreeDeployment *deployment = deployments->pdata[i];
      GKeyFile *origin;
      gs_free char *origin_refspec = NULL;
      GDateTime *timestamp = NULL;
      gs_free char *timestamp_string = NULL;
      char *truncated_csum = NULL;
      gs_free char *version_string = NULL;

      /* get commit for timestamp */
      if (!ostree_repo_load_variant (repo,
                                   OSTREE_OBJECT_TYPE_COMMIT,
                                   csum,
                                   &commit,
                                   error))
        goto out;

      /* format timestamp*/
      timestamp = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (commit));
      g_assert (timestamp);
      timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
      g_date_time_unref (timestamp);
      version_string = checksum_version (commit);

      /* get origin refspec */
      origin = ostree_deployment_get_origin (deployment);
      if (!origin)
        origin_refspec = g_strdup ("none");
      else
        {
          origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
          if (!origin_refspec)
            origin_refspec = g_strdup ("<unknown origin type>");
        }

      /* truncate checksum */
      truncated_csum = g_strndup (csum, CSUM_DISP_LEN);

      /* print deployment info column */
      if (!opt_pretty)
        {
          if (deployment == booted_deployment)
            {
              /* Stash this for printing signatures later. */
              if (deployment_get_gpg_verify (deployment, repo))
                booted_csum = g_strdup (csum);
            }

          g_print ("%c %-*s",
                   deployment == booted_deployment ? '*' : ' ',
                   max_timestamp_len+buffer, timestamp_string);

          if (max_version_len)
            g_print ("%-*s",
                     max_version_len+buffer, version_string ? version_string : "");
          g_print ("%-*s%-*s%-*s\n",
                   max_id_len+buffer, truncated_csum,
                   max_osname_len+buffer, ostree_deployment_get_osname (deployment),
                   max_refspec_len+buffer, origin_refspec);
        }

      /* print "pretty" row info */
      else
        {
          gs_unref_object OstreeGpgVerifyResult *result = NULL;
          guint tab = 11;
          char *title = NULL;
          GError *local_error = NULL;

          if (i==0)
            title = "DEFAULT ON BOOT";
          else if (deployment == booted_deployment ||
                  deployments->len <= 2)
            title = "NON-DEFAULT ROLLBACK TARGET";
          else
            title = "NON-DEFAULT DEPLOYMENT";
          g_print ("  %c %s\n",
                  deployment == booted_deployment ? '*' : ' ',
                  title);

          printchar ("-", 40);
          if (version_string)
            g_print ("  %-*s%-*s\n", tab, "version", tab, version_string);
          g_print ("  %-*s%-*s\n  %-*s%-*s.%d\n  %-*s%-*s\n  %-*s%-*s\n",
                  tab, "timestamp", tab, timestamp_string,
                  tab, "id", tab, csum, ostree_deployment_get_deployserial (deployment),
                  tab, "osname", tab, ostree_deployment_get_osname (deployment),
                  tab, "refspec", tab, origin_refspec);

          if (deployment_get_gpg_verify (deployment, repo))
            {
              result = ostree_repo_verify_commit_ext (repo, csum, NULL, NULL,
                                                      cancellable, &local_error);

              /* NOT_FOUND just means the commit is not signed. */
              if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_clear_error (&local_error);
                }
              else if (local_error != NULL)
                {
                  g_propagate_error (error, local_error);
                  goto out;
                }
              else
                {
                  GString *sigs_buffer;
                  guint n_sigs, ii;

                  n_sigs = ostree_gpg_verify_result_count_all (result);

                  sigs_buffer = g_string_sized_new (256);

                  for (ii = 0; ii < n_sigs; ii++)
                    {
                      g_string_append_c (sigs_buffer, '\n');
                      ostree_gpg_verify_result_describe (result, ii, sigs_buffer, "  GPG: ",
                                                         OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
                    }

                  g_print ("%s", sigs_buffer->str);
                  g_string_free (sigs_buffer, TRUE);
                }
            }

          printchar ("=", 60);
        }
    }

  /* Print any signatures for the booted deployment, but only in NON-pretty
   * mode.  We save this for the end to preserve the tabular formatting for
   * deployments. */
  if (booted_csum != NULL)
    {
      gs_unref_object OstreeGpgVerifyResult *result = NULL;
      GError *local_error = NULL;

      result = ostree_repo_verify_commit_ext (repo, booted_csum, NULL, NULL,
                                              cancellable, &local_error);

      /* NOT_FOUND just means the commit is not signed. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }
      else
        {
          GString *sigs_buffer;
          guint n_sigs, ii;

          n_sigs = ostree_gpg_verify_result_count_all (result);

          /* XXX If we ever add internationalization, use ngettext() here. */
          g_print ("\nFound %u signature%s on the booted deployment (*):\n",
                   n_sigs, n_sigs == 1 ? "" : "s");

          sigs_buffer = g_string_sized_new (256);

          for (ii = 0; ii < n_sigs; ii++)
            {
              g_string_append_c (sigs_buffer, '\n');
              ostree_gpg_verify_result_describe (result, ii, sigs_buffer, "  ",
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
            }

          g_print ("%s", sigs_buffer->str);
          g_string_free (sigs_buffer, TRUE);
        }
    }

  ret = TRUE;
  out:
  	return ret;
}
