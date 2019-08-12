#include <mongoc/mongoc.h>

#include "mongoc/mongoc-collection-private.h"

#include "json-test.h"
#include "test-libmongoc.h"
#include "mock_server/mock-rs.h"
#include "mock_server/future.h"
#include "mock_server/future-functions.h"
#include "json-test-operations.h"

static bool
retryable_reads_test_run_operation (json_test_ctx_t *ctx,
                                    const bson_t *test,
                                    const bson_t *operation)
{
   bool *explicit_session = (bool *) ctx->config->ctx;
   bson_t reply;
   bool res;

   res = json_test_operation (ctx,
                              test,
                              operation,
                              ctx->collection,
                              *explicit_session ? ctx->sessions[0] : NULL,
                              &reply);

   // bson_destroy (&reply);

   return res;
}


/* Callback for JSON tests from Retryable Reads Spec */
static void
test_retryable_reads_cb (bson_t *scenario)
{
   bool explicit_session;
   json_test_config_t config = JSON_TEST_CONFIG_INIT;

   /* use the context pointer to send "explicit_session" to the callback */
   config.ctx = &explicit_session;
   config.run_operation_cb = retryable_reads_test_run_operation;
   config.scenario = scenario;
   config.command_started_events_only = true;
   explicit_session = true;
   run_json_general_test (&config);
   explicit_session = false;
   run_json_general_test (&config);
}


/* Test code paths for _mongoc_client_command_with_opts */
static void
test_command_with_opts (void *ctx)
{
   mongoc_uri_t *uri;
   mongoc_client_t *client;
   uint32_t server_id;
   mongoc_collection_t *collection;
   bson_t *cmd;
   bson_t reply;
   bson_error_t error;
   bson_iter_t iter;

   uri = test_framework_get_uri ();
   mongoc_uri_set_option_as_bool (uri, "retryReads", true);

   client = mongoc_client_new_from_uri (uri);
   test_framework_set_ssl_opts (client);
   mongoc_uri_destroy (uri);

   /* clean up in case a previous test aborted */
   server_id = mongoc_topology_select_server_id (
      client->topology, MONGOC_SS_WRITE, NULL, &error);
   ASSERT_OR_PRINT (server_id, error);
   deactivate_fail_points (client, server_id);

   collection = get_test_collection (client, "retryable_reads");

   if (!mongoc_collection_drop (collection, &error)) {
      if (strcmp (error.message, "ns not found")) {
         /* an error besides ns not found */
         ASSERT_OR_PRINT (false, error);
      }
   }

   ASSERT_OR_PRINT (mongoc_collection_insert_one (
                       collection, tmp_bson ("{'_id': 0}"), NULL, NULL, &error),
                    error);
   ASSERT_OR_PRINT (mongoc_collection_insert_one (
                       collection, tmp_bson ("{'_id': 1}"), NULL, NULL, &error),
                    error);

   cmd = tmp_bson ("{'configureFailPoint': 'failCommand',"
                   " 'mode': {'times': 1},"
                   " 'data': {'errorCode': 10107, 'failCommands': ['count']}}");

   ASSERT_OR_PRINT (mongoc_client_command_simple_with_server_id (
                       client, "admin", cmd, NULL, server_id, NULL, &error),
                    error);

   cmd = tmp_bson ("{'count': 'coll'}", collection->collection);

   ASSERT_OR_PRINT (mongoc_collection_read_command_with_opts (
                       collection, cmd, NULL, NULL, &reply, &error),
                    error);

   bson_iter_init_find (&iter, &reply, "n");
   ASSERT (bson_iter_int32 (&iter) == 2);

   deactivate_fail_points (client, server_id);

   ASSERT_OR_PRINT (mongoc_collection_drop (collection, &error), error);

   bson_destroy (&reply);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
}

static void
test_retry_reads_off (void *ctx)
{
   mongoc_uri_t *uri;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   uint32_t server_id;
   bson_t *cmd;
   bson_error_t error;
   bool res;

   uri = test_framework_get_uri ();
   mongoc_uri_set_option_as_bool (uri, "retryreads", false);
   client = mongoc_client_new_from_uri (uri);

   /* clean up in case a previous test aborted */
   server_id = mongoc_topology_select_server_id (
      client->topology, MONGOC_SS_WRITE, NULL, &error);
   ASSERT_OR_PRINT (server_id, error);
   deactivate_fail_points (client, server_id);

   collection = get_test_collection (client, "retryable_reads");

   cmd = tmp_bson ("{'configureFailPoint': 'failCommand',"
                   " 'mode': {'times': 1},"
                   " 'data': {'errorCode': 10107, 'failCommands': ['count']}}");
   ASSERT_OR_PRINT (mongoc_client_command_simple_with_server_id (
                       client, "admin", cmd, NULL, server_id, NULL, &error),
                    error);

   cmd = tmp_bson ("{'count': 'coll'}", collection->collection);

   res = mongoc_collection_read_command_with_opts (
      collection, cmd, NULL, NULL, NULL, &error);
   ASSERT (!res);
   ASSERT_CONTAINS (error.message,
                    "Failing command due to 'failCommand' failpoint");

   deactivate_fail_points (client, server_id);

   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
}

/*
 *-----------------------------------------------------------------------
 *
 * Runner for the JSON tests for retryable reads.
 *
 *-----------------------------------------------------------------------
 */
static void
test_all_spec_tests (TestSuite *suite)
{
   char resolved[PATH_MAX];

   test_framework_resolve_path (JSON_DIR "/retryable_reads", resolved);
   install_json_test_suite_with_check (
      suite, resolved, test_retryable_reads_cb);
}

void
test_retryable_reads_install (TestSuite *suite)
{
   test_all_spec_tests (suite);
   TestSuite_AddFull (suite,
                      "/retryable_reads/command_with_opts",
                      test_command_with_opts,
                      NULL,
                      NULL,
                      test_framework_skip_if_not_rs_version_6);
   TestSuite_AddFull (suite,
                      "/retryable_reads/retry_off",
                      test_retry_reads_off,
                      NULL,
                      NULL,
                      test_framework_skip_if_not_rs_version_6);
}