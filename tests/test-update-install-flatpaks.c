/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors:
 *  - Sam Spilsbury <sam@endlessm.com>
 */

#include "misc-utils.h"
#include "spawn-utils.h"
#include "ostree-spawn.h"
#include "eos-test-utils.h"
#include "eos-test-convenience.h"

#include <ostree.h>
#include <flatpak.h>

#include <gio/gio.h>
#include <locale.h>
#include <string.h>

typedef struct _FlatpakToInstall {
  const gchar *action;
  const gchar *repository;
  const gchar *app_id;
  const gchar *ref_kind;
} FlatpakToInstall;

static GStrv
flatpaks_to_install_to_strv (const FlatpakToInstall *flatpaks,
                             gsize                   n_flatpaks)
{
  GStrv strv = g_new0 (gchar *, n_flatpaks + 1);
  guint index = 0;

  for (; index < n_flatpaks; ++index)
    {
      strv[index] = g_strdup_printf("%s %s:%s/%s/arch/branch %d",
                                    flatpaks[index].action,
                                    flatpaks[index].repository,
                                    flatpaks[index].ref_kind,
                                    flatpaks[index].app_id,
                                    index);
    }

  return strv;
}

static GStrv
flatpaks_to_install_app_ids_strv (FlatpakToInstall *flatpaks_to_install,
                                  gsize             n_flatpaks_to_install)
{
  GStrv strv = g_new0 (gchar *, n_flatpaks_to_install + 1);
  gsize i = 0;

  for (; i < n_flatpaks_to_install; ++i)
    strv[i] = g_strdup (flatpaks_to_install[i].app_id);

  return strv;
}

static void
autoinstall_flatpaks_files (guint                    commit,
                            const FlatpakToInstall  *flatpaks,
                            gsize                    n_flatpaks,
                            GHashTable             **out_directories_hashtable,
                            GHashTable             **out_files_hashtable)
{
  g_auto (GStrv) flatpaks_as_strv = flatpaks_to_install_to_strv (flatpaks, n_flatpaks);
  gchar *autoinstall_flatpaks_contents = g_strjoinv ("\n", flatpaks_as_strv);
  GStrv directories_to_create_strv = g_new0 (gchar *, 2);
  GPtrArray *files_to_create = g_ptr_array_new_full (1, simple_file_free);

  g_return_if_fail (out_directories_hashtable != NULL);
  g_return_if_fail (out_files_hashtable != NULL);

  if (*out_directories_hashtable == NULL)
    *out_directories_hashtable = g_hash_table_new_full (g_direct_hash,
                                                        g_direct_equal,
                                                        NULL,
                                                        (GDestroyNotify) g_strfreev);
  if (*out_files_hashtable == NULL)
    *out_files_hashtable = g_hash_table_new_full (g_direct_hash,
                                                  g_direct_equal,
                                                  NULL,
                                                  (GDestroyNotify) g_ptr_array_free);

  directories_to_create_strv[0] = g_build_filename ("usr", "share", "eos-application-tools", "flatpak-autoinstall.d", NULL);
  g_ptr_array_add (files_to_create,
                   simple_file_new_steal (g_build_filename ("usr", "share", "eos-application-tools", "flatpak-autoinstall.d", "autoinstall", NULL),
                                          autoinstall_flatpaks_contents));

  g_hash_table_insert (*out_directories_hashtable,
                       GUINT_TO_POINTER (commit),
                       directories_to_create_strv);
  g_hash_table_insert (*out_files_hashtable,
                       GUINT_TO_POINTER (commit),
                       files_to_create);
}

static GStrv
parse_ostree_refs_for_flatpaks (const gchar  *ostree_refs_stdout,
                                GError      **error)
{
  g_auto(GStrv) ostree_refs_stdout_lines = g_strsplit (ostree_refs_stdout, "\n", -1);
  g_auto(GStrv) parsed_out_flatpak_refs = g_new0 (gchar *, g_strv_length (ostree_refs_stdout_lines));
  GStrv ostree_refs_stdout_lines_iter = ostree_refs_stdout_lines;
  g_autoptr(GRegex) flatpak_refs_parser = g_regex_new (".*:.*?\\/(.*?)\\/.*", 0, 0, error);
  guint i = 0;

  if (!flatpak_refs_parser)
    return NULL;

  for (; *ostree_refs_stdout_lines_iter; ++ostree_refs_stdout_lines_iter)
    {
      g_autoptr(GMatchInfo) match_info = NULL;
      gchar *matched_flatpak_name = NULL;

      if (g_strcmp0 (*ostree_refs_stdout_lines_iter, "") == 0)
        continue;

      if (!g_regex_match (flatpak_refs_parser,
                          *ostree_refs_stdout_lines_iter,
                          0,
                          &match_info))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to parse output of OSTree refs: %s", ostree_refs_stdout);
          return FALSE;
        }

      matched_flatpak_name = g_match_info_fetch (match_info, 1);

      if (!matched_flatpak_name)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to parse output of OSTree refs: %s", ostree_refs_stdout);
          return FALSE;
        }

      parsed_out_flatpak_refs[i++] = g_steal_pointer (&matched_flatpak_name);
    }

  return g_steal_pointer (&parsed_out_flatpak_refs);
}

/* Inspect the underlying OSTree repo for flatpak refs that are
 * in the repository but not necessarily installed. We regex out the names
 * of the flatpaks and return them. */
static GStrv
flatpaks_in_installation_repo (GFile   *flatpak_installation_dir,
                               GError **error)
{
  g_auto(CmdResult) cmd = CMD_RESULT_CLEARED;
  g_autoptr(GFile) flatpak_repo = g_file_get_child (flatpak_installation_dir, "repo");

  if (!ostree_list_refs_in_repo (flatpak_repo, &cmd, error))
    return FALSE;

  return parse_ostree_refs_for_flatpaks (cmd.standard_output, error);
}

static gchar *
concat_refspec (const gchar *remote_name, const gchar *ref)
{
  return g_strjoin (":", remote_name, ref, NULL);
}

static gchar *
get_checksum_for_deploy_repo_dir (GFile        *deployment_repo_dir,
                                  const gchar  *refspec,
                                  GError      **error)
{
  g_autoptr(OstreeRepo) repo = ostree_repo_new (deployment_repo_dir);
  g_autoptr(GHashTable) refs = NULL;
  const gchar *ret_checksum = NULL;

  if (!ostree_repo_open (repo, NULL, error))
    return NULL;

  if (!ostree_repo_list_refs (repo, NULL, &refs, NULL, error))
    return NULL;

  ret_checksum = g_hash_table_lookup (refs, refspec);

  if (!ret_checksum)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to lookup ref %s", refspec);
      return NULL;
    }

  return g_strdup (ret_checksum);
}

/* Insert an empty list of flatpaks to automatically install on the commit
 * and ensure that the update still succeeds */
static void
test_update_install_no_flatpaks (EosUpdaterFixture *fixture,
                                 gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
  };
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install no flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);
}

/* Insert a list of flatpaks to automatically install on the commit
 * and ensure that they are pulled into the local repo once the
 * system update has completed. */
static void
test_update_install_flatpaks_in_repo (EosUpdaterFixture *fixture,
                                      gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) flatpaks_in_repo = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);

  /* Assert that our flatpaks were pulled into the local repo */
  flatpaks_in_repo = flatpaks_in_installation_repo (flatpak_user_installation_dir,
                                                    &error);
  g_assert_no_error (error);

  g_assert (g_strv_contains ((const gchar * const *) flatpaks_in_repo, flatpaks_to_install[0].app_id));
}

/* Have flatpaks that are pending deployment but induce a failure in
 * the sysroot deployment. It should be the case that the flatpak refs
 * stay on the local system repo. */
static void
test_update_deploy_fail_flatpaks_stay_in_repo (EosUpdaterFixture *fixture,
                                              gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  DownloadSource main_source = DOWNLOAD_MAIN;
  g_auto(CmdAsyncResult) updater_cmd = CMD_ASYNC_RESULT_CLEARED;
  g_auto(CmdResult) reaped_updater = CMD_RESULT_CLEARED;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) flatpaks_in_repo = NULL;
  g_autofree gchar *remote_repo_directory_relative_path = g_build_filename ("main",
                                                                            "served",
                                                                            default_ostree_path,
                                                                            NULL);
  g_autoptr(GFile) remote_repo_directory = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autofree gchar *expected_directory_relative_path = NULL;
  g_autoptr(GFile) expected_directory = NULL;
  g_autoptr(GFile) expected_directory_child = NULL;
  g_autoptr(GFile) autoupdater_root = NULL;
  g_autoptr(EosTestAutoupdater) autoupdater = NULL;
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *deployment_id = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);

  /* Before updating the client, write a directory to a location of one of the
   * files that ostree_sysroot_deploy_tree will want to write to. This relies on
   * implementation details of ostree_sysroot_deploy_tree, but essentially it
   * puts a nonempty directory where the origin file should be.
   *
   * ostree_sysroot_deploy_tree will call glnx_file_replace_contents_at
   * which will only replace the contents of the file if it is a file
   * or a nonempty directory and return an error otherwise.
   *
   * When the error occurrs, the updater should catch it and revert the
   * operations done to pre-install flatpaks. */  
  remote_repo_directory = g_file_get_child (data->fixture->tmpdir,
                                            remote_repo_directory_relative_path);
  deployment_csum = get_checksum_for_deploy_repo_dir (remote_repo_directory,
                                                      default_ref,
                                                      &error);
  deployment_id = g_strjoin (".", deployment_csum, "0", "origin", NULL);

  expected_directory_relative_path = g_build_filename ("sysroot",
                                                       "ostree",
                                                       "deploy",
                                                       default_remote_name,
                                                       "deploy",
                                                       deployment_id,
                                                       NULL);
  expected_directory = g_file_get_child (data->client->root,
                                         expected_directory_relative_path);
  expected_directory_child = g_file_get_child (expected_directory, "child");

  g_file_make_directory_with_parents (expected_directory, NULL, &error);
  g_assert_no_error (error);

  g_file_set_contents (g_file_get_path (expected_directory_child), "", -1, &error);
  g_assert_no_error (error);

  /* Attempt to update client - run updater daemon */
  eos_test_client_run_updater (data->client,
                               &main_source,
                               1,
                               NULL,
                               &updater_cmd,
                               NULL);
  g_assert_no_error (error);

  /* Trigger update */
  autoupdater_root = g_file_get_child (data->fixture->tmpdir, "autoupdater");
  autoupdater = eos_test_autoupdater_new (autoupdater_root,
                                          UPDATE_STEP_APPLY,
                                          1,
                                          TRUE,
                                          &error);
  g_assert_no_error (error);

  /* Done with update, reap updater server */
  eos_test_client_reap_updater (data->client,
                                &updater_cmd,
                                &reaped_updater,
                                &error);
  g_assert_no_error (error);

  /* Should have been an error on the autoupdater, since the update would
   * have failed. */
  g_assert (g_spawn_check_exit_status (autoupdater->cmd->exit_status, NULL) == FALSE);

  /* Assert that our flatpaks are in the installation repo */
  flatpaks_in_repo = flatpaks_in_installation_repo (flatpak_user_installation_dir,
                                                    &error);
  g_assert_no_error (error);

  g_assert (g_strv_contains ((const gchar * const *) flatpaks_in_repo, flatpaks_to_install[0].app_id));
}

/* Have flatpaks that are pending deployment but induce a failure in
 * the sysroot deployment. It should be the case that the flatpaks are
 * not deployed on reboot */
static void
test_update_deploy_fail_flatpaks_not_deployed (EosUpdaterFixture *fixture,
                                              gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  DownloadSource main_source = DOWNLOAD_MAIN;
  g_auto(CmdAsyncResult) updater_cmd = CMD_ASYNC_RESULT_CLEARED;
  g_auto(CmdResult) reaped_updater = CMD_RESULT_CLEARED;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "com.endlessm.TestInstallFlatpaksCollection", "org.test.Test", "app", FLATPAK_TO_INSTALL_FLAGS_NONE }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autofree gchar *remote_repo_directory_relative_path = g_build_filename ("main",
                                                                            "served",
                                                                            default_ostree_path,
                                                                            NULL);
  g_autoptr(GFile) remote_repo_directory = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autofree gchar *expected_directory_relative_path = NULL;
  g_autoptr(GFile) expected_directory = NULL;
  g_autoptr(GFile) expected_directory_child = NULL;
  g_autoptr(GFile) autoupdater_root = NULL;
  g_autoptr(EosTestAutoupdater) autoupdater = NULL;
  g_autofree gchar *deployment_repo_relative_path = g_build_filename ("sysroot", "ostree", "repo", NULL);
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *anticipated_deployment_csum = NULL;
  g_autofree gchar *deployment_id = NULL;
  g_autofree gchar *refspec = concat_refspec (default_remote_name, default_ref);
  g_autoptr(GFile) deployment_repo_dir = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               "com.endlessm.TestInstallFlatpaksCollection",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);

  /* Before updating the client, write a directory to a location of one of the
   * files that ostree_sysroot_deploy_tree will want to write to. This relies on
   * implementation details of ostree_sysroot_deploy_tree, but essentially it
   * puts a nonempty directory where the origin file should be.
   *
   * ostree_sysroot_deploy_tree will call glnx_file_replace_contents_at
   * which will only replace the contents of the file if it is a file
   * or a nonempty directory and return an error otherwise.
   *
   * When the error occurrs, the updater should catch it and revert the
   * operations done to pre-install flatpaks. */  
  remote_repo_directory = g_file_get_child (data->fixture->tmpdir,
                                            remote_repo_directory_relative_path);
  anticipated_deployment_csum = get_checksum_for_deploy_repo_dir (remote_repo_directory,
                                                                  default_ref,
                                                                  &error);
  deployment_id = g_strjoin (".", anticipated_deployment_csum, "0", "origin", NULL);

  expected_directory_relative_path = g_build_filename ("sysroot",
                                                       "ostree",
                                                       "deploy",
                                                       default_remote_name,
                                                       "deploy",
                                                       deployment_id,
                                                       NULL);
  expected_directory = g_file_get_child (data->client->root,
                                         expected_directory_relative_path);
  expected_directory_child = g_file_get_child (expected_directory, "child");

  g_file_make_directory_with_parents (expected_directory, NULL, &error);
  g_assert_no_error (error);

  g_file_set_contents (g_file_get_path (expected_directory_child), "", -1, &error);
  g_assert_no_error (error);

  /* Attempt to update client - run updater daemon */
  eos_test_client_run_updater (data->client,
                               &main_source,
                               1,
                               NULL,
                               &updater_cmd,
                               NULL);
  g_assert_no_error (error);

  /* Trigger update */
  autoupdater_root = g_file_get_child (data->fixture->tmpdir, "autoupdater");
  autoupdater = eos_test_autoupdater_new (autoupdater_root,
                                          UPDATE_STEP_APPLY,
                                          1,
                                          TRUE,
                                          &error);
  g_assert_no_error (error);

  /* Done with update, reap updater server */
  eos_test_client_reap_updater (data->client,
                                &updater_cmd,
                                &reaped_updater,
                                &error);
  g_assert_no_error (error);

  /* Now simulate a reboot by running eos-updater-flatpak-installer */
  deployment_repo_dir = g_file_get_child (data->client->root,
                                          deployment_repo_relative_path);
  deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                      refspec,
                                                      &error);
  g_assert_no_error (error);

  eos_test_run_flatpak_installer (data->client->root,
                                  deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Assert that our flatpak was not installed */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (!g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[0].app_id));
}

/* Insert a list of flatpaks to automatically install on the commit
 * and ensure that they are not installed before reboot */
static void
test_update_install_flatpaks_not_deployed (EosUpdaterFixture *fixture,
                                           gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);

  /* Get the currently deployed flatpaks and ensure we are not one of them */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (!g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[0].app_id));
}

/* Insert a list of flatpaks to automatically install on the commit
 * and simulate a reboot by running eos-updater-flatpak-installer. This
 * should check the deployment for a list of flatpaks to install and
 * install them from the local repo into the installation. Verify that
 * the flatpaks are installed and deployed once this has completed. */
static void
test_update_deploy_flatpaks_on_reboot (EosUpdaterFixture *fixture,
                                       gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autofree gchar *deployment_repo_relative_path = g_build_filename ("sysroot", "ostree", "repo", NULL);
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *refspec = concat_refspec (default_remote_name, default_ref);
  g_autoptr(GFile) deployment_repo_dir = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  deployment_repo_dir = g_file_get_child (data->client->root,
                                          deployment_repo_relative_path);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);

  /* Now simulate a reboot by running eos-updater-flatpak-installer */
  deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                      refspec,
                                                      &error);
  g_assert_no_error (error);

  eos_test_run_flatpak_installer (data->client->root,
                                  deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Assert that our flatpak was installed */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[0].app_id));
}

/* Insert a list of flatpaks to automatically install on the commit
 * and simulate a reboot by running eos-updater-flatpak-installer. Then
 * uninstall the flatpak and update again with the same list of actions. This
 * should not reinstall the flatpak that was previously removed. */ 
static void
test_update_no_deploy_flatpaks_twice (EosUpdaterFixture *fixture,
                                      gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autofree gchar *deployment_repo_relative_path = g_build_filename ("sysroot", "ostree", "repo", NULL);
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *second_deployment_csum = NULL;
  g_autofree gchar *refspec = concat_refspec (default_remote_name, default_ref);
  g_autoptr(GFile) deployment_repo_dir = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Commit number 2 has the same list of actions to apply
   */
  autoinstall_flatpaks_files (2,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  deployment_repo_dir = g_file_get_child (data->client->root,
                                          deployment_repo_relative_path);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);

  deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                      refspec,
                                                      &error);
  g_assert_no_error (error);

  /* First reboot, should install flatpaks */
  eos_test_run_flatpak_installer (data->client->root,
                                  deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Now, uninstall the flatpak */
  flatpak_uninstall (updater_directory,
                     "org.test.Test",
                     &error);
  g_assert_no_error (error);

  /* Update the server, so it has a new commit (2).
   */
  etc_update_server (data, 2);
  /* Update the client, so it also has a new commit (2); and, at this
   * point, three deployments.
   */
  etc_update_client (data);

  second_deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                             refspec,
                                                             &error);
  g_assert_no_error (error);

  /* Reboot #2. Should not reinstall the same flatpak */
  eos_test_run_flatpak_installer (data->client->root,
                                  second_deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Assert that our flatpak was not installed */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (!g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[0].app_id));
}

/* Insert a list of flatpaks to automatically install on the commit
 * and simulate a reboot by running eos-updater-flatpak-installer. Then
 * uninstall the flatpak and update again with a new list of actions containing
 * a new install command. This should reinstall the flatpak. */
static void
test_update_force_reinstall_flatpak (EosUpdaterFixture *fixture,
                                     gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" }
  };
  FlatpakToInstall next_flatpaks_to_install[] = {
    { "install", "test-repo", "org.test.Test", "app" },
    { "install", "test-repo", "org.test.Test", "app" }
  };
  g_autofree gchar *flatpak_user_installation = NULL;
  g_autoptr(GFile) flatpak_user_installation_dir = NULL;
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install,
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autofree gchar *deployment_repo_relative_path = g_build_filename ("sysroot", "ostree", "repo", NULL);
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *second_deployment_csum = NULL;
  g_autofree gchar *refspec = concat_refspec (default_remote_name, default_ref);
  g_autoptr(GFile) deployment_repo_dir = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Commit number 1 will install some flatpaks
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install,
                              G_N_ELEMENTS (flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Commit number 2 has an updated list of actions to apply
   */
  autoinstall_flatpaks_files (2,
                              next_flatpaks_to_install,
                              G_N_ELEMENTS (next_flatpaks_to_install),
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  flatpak_user_installation = g_build_filename (g_file_get_path (updater_directory),
                                                "flatpak-user",
                                                NULL);
  flatpak_user_installation_dir = g_file_new_for_path (flatpak_user_installation);
  deployment_repo_dir = g_file_get_child (data->client->root,
                                          deployment_repo_relative_path);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (1).
   */
  etc_update_server (data, 1);
  /* Update the client, so it also has a new commit (1); and, at this
   * point, two deployments - old one pointing to commit 0 and a new
   * one pointing to commit 1.
   */
  etc_update_client (data);

  deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                      refspec,
                                                      &error);
  g_assert_no_error (error);

  /* First reboot, should install flatpaks */
  eos_test_run_flatpak_installer (data->client->root,
                                  deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Now, uninstall the flatpak */
  flatpak_uninstall (updater_directory,
                     "org.test.Test",
                     &error);
  g_assert_no_error (error);

  /* Update the server, so it has a new commit (2).
   */
  etc_update_server (data, 2);
  /* Update the client, so it also has a new commit (2); and, at this
   * point, three deployments.
   */
  etc_update_client (data);

  second_deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                             refspec,
                                                             &error);
  g_assert_no_error (error);

  /* Reboot #2. Should reinstall the same flatpak */
  eos_test_run_flatpak_installer (data->client->root,
                                  second_deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Assert that our flatpak was installed */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[0].app_id));
}

/* Over the course of three revisions, install, remove, then install a flatpak.
 * The result should be that the flatpak is installed (overall) */
static void
test_update_install_through_squashed_list (EosUpdaterFixture *fixture,
                                           gconstpointer      user_data)
{
  g_auto(EtcData) real_data = { NULL, };
  EtcData *data = &real_data;
  FlatpakToInstall flatpaks_to_install[][3] = {
    {
      { "install", "test-repo", "org.test.Test", "app" }
    },
    {
      { "install", "test-repo", "org.test.Test", "app" },
      { "uninstall", "test-repo", "org.test.Test", "app" }
    },
    {
      { "install", "test-repo", "org.test.Test", "app" },
      { "uninstall", "test-repo", "org.test.Test", "app" },
      { "install", "test-repo", "org.test.Test", "app" }
    },
  };
  g_auto(GStrv) wanted_flatpaks = flatpaks_to_install_app_ids_strv (flatpaks_to_install[0],
                                                                    G_N_ELEMENTS (flatpaks_to_install));
  g_auto(GStrv) deployed_flatpaks = NULL;
  g_autofree gchar *deployment_repo_relative_path = g_build_filename ("sysroot", "ostree", "repo", NULL);
  g_autofree gchar *deployment_csum = NULL;
  g_autofree gchar *refspec = concat_refspec (default_remote_name, default_ref);
  g_autoptr(GFile) deployment_repo_dir = NULL;
  g_autoptr(GFile) updater_directory = NULL;
  g_autoptr(GError) error = NULL;

  g_test_bug ("T16682");

  etc_data_init (data, fixture);

  /* Note that since we had to hardcode the sub-array size above in the
   * flatpaks_to_install declaration in order to keep the compiler happy, we
   * cannot use G_N_ELEMENTS to work out the sub array sizes. Just use
   * hardcoded sizes instead. */

  /* Commit number 1 will install a flatpak
   */
  autoinstall_flatpaks_files (1,
                              flatpaks_to_install[0],
                              1,
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Commit number 1 will remove that flatpak
   */
  autoinstall_flatpaks_files (2,
                              flatpaks_to_install[1],
                              2,
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Commit number 3 will install it again
   */
  autoinstall_flatpaks_files (3,
                              flatpaks_to_install[2],
                              3,
                              &data->additional_directories_for_commit,
                              &data->additional_files_for_commit);

  /* Create and set up the server with the commit 0.
   */
  etc_set_up_server (data);
  /* Create and set up the client, that pulls the update from the
   * server, so it should have also a commit 0 and a deployment based
   * on this commit.
   */
  etc_set_up_client_synced_to_server (data);

  updater_directory = g_file_get_child (data->client->root, "updater");
  deployment_repo_dir = g_file_get_child (data->client->root,
                                          deployment_repo_relative_path);
  eos_test_setup_flatpak_repo (updater_directory,
                               "test-repo",
                               (const gchar **) wanted_flatpaks,
                               &error);

  /* Update the server, so it has a new commit (3).
   */
  etc_update_server (data,3);
  /* Update the client to commit 3, skipping 2.
   */
  etc_update_client (data);

  deployment_csum = get_checksum_for_deploy_repo_dir (deployment_repo_dir,
                                                      refspec,
                                                      &error);
  g_assert_no_error (error);

  /* Reboot and install flatpaks */
  eos_test_run_flatpak_installer (data->client->root,
                                  deployment_csum,
                                  default_remote_name,
                                  &error);
  g_assert_no_error (error);

  /* Assert that our flatpak was not installed */
  deployed_flatpaks = eos_test_get_installed_flatpaks (updater_directory, &error);
  g_assert_no_error (error);

  g_assert (g_strv_contains ((const gchar * const *) deployed_flatpaks, flatpaks_to_install[2][2].app_id));
}

int
main (int argc,
      char **argv)
{
  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);
  g_test_bug_base ("https://phabricator.endlessm.com/");

  eos_test_add ("/updater/install-no-flatpaks", NULL, test_update_install_no_flatpaks);
  eos_test_add ("/updater/install-flatpaks-pull-to-repo", NULL, test_update_install_flatpaks_in_repo);
  eos_test_add ("/updater/install-flatpaks-not-deployed", NULL, test_update_install_flatpaks_not_deployed);
  eos_test_add ("/updater/install-flatpaks-deploy-on-reboot", NULL, test_update_deploy_flatpaks_on_reboot);
  eos_test_add ("/updater/no-deploy-same-action-twice", NULL, test_update_no_deploy_flatpaks_twice);
  eos_test_add ("/updater/reinstall-flatpak-if-counter-is-later", NULL, test_update_force_reinstall_flatpak);
  eos_test_add ("/updater/update-deploy-fail-flatpaks-stay-in-repo", NULL, test_update_deploy_fail_flatpaks_stay_in_repo);
  eos_test_add ("/updater/update-deploy-fail-flatpaks-not-deployed", NULL, test_update_deploy_fail_flatpaks_not_deployed);
  eos_test_add ("/updater/update-install-through-squashed-list", NULL, test_update_install_through_squashed_list);

  return g_test_run ();
}