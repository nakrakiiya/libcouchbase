/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>

#include "internal.h" /* to look at the internals to check if sasl ok */
#include "server.h"
#include "test.h"

lcb_t session = NULL;
const void *mock = NULL;
struct lcb_io_opt_st *io = NULL;
lcb_error_t global_error = -1;
int total_node_count = -1;


static void error_callback(lcb_t instance,
                           lcb_error_t err,
                           const char *errinfo)
{
    err_exit("Error %s: %s", lcb_strerror(instance, err), errinfo);
}

static void error_callback2(lcb_t instance,
                            lcb_error_t err,
                            const char *errinfo)
{
    global_error = err;
    (void)instance;
    (void)errinfo;
}

static void setup(char **argv, const char *username, const char *password,
                  const char *bucket)
{
    const char *endpoint;
    struct lcb_create_st options;

    assert(session == NULL);
    assert(mock == NULL);
    assert(io == NULL);

    io = get_test_io_opts();
    if (io == NULL) {
        err_exit("Failed to create IO session");
    }

    mock = start_mock_server(argv);
    if (mock == NULL) {
        err_exit("Failed to start mock server");
    }

    endpoint = get_mock_http_server(mock);
    memset(&options, 0, sizeof(options));
    options.v.v0.host = endpoint;
    options.v.v0.user = username;
    options.v.v0.passwd = password;
    options.v.v0.bucket = bucket;
    options.v.v0.io = io;

    if (lcb_create(&session, &options) != LCB_SUCCESS) {
        err_exit("Failed to create libcouchbase session");
    }

    (void)lcb_set_error_callback(session, error_callback);

    if (lcb_connect(session) != LCB_SUCCESS) {
        err_exit("Failed to connect to server");
    }
    lcb_wait(session);
}

static void teardown(void)
{
    lcb_destroy(session);
    session = NULL;
    io = NULL;
    shutdown_mock_server(mock);
    mock = NULL;
}

struct rvbuf {
    lcb_error_t error;
    lcb_storage_t operation;
    const char *key;
    lcb_size_t nkey;
    const char *bytes;
    lcb_size_t nbytes;
    lcb_cas_t cas;
    lcb_uint32_t flags;
    int32_t counter;
    lcb_uint32_t errors;
};

static void store_callback(lcb_t instance,
                           const void *cookie,
                           lcb_storage_t operation,
                           lcb_error_t error,
                           const lcb_store_resp_t *resp)
{
    struct rvbuf *rv = (struct rvbuf *)cookie;
    rv->error = error;
    rv->operation = operation;
    rv->key = resp->v.v0.key;
    rv->nkey = resp->v.v0.nkey;
    rv->cas = resp->v.v0.cas;
    assert(io);
    io->stop_event_loop(io);
    (void)instance;
}

static void mstore_callback(lcb_t instance,
                            const void *cookie,
                            lcb_storage_t operation,
                            lcb_error_t error,
                           const lcb_store_resp_t *resp)
{
    struct rvbuf *rv = (struct rvbuf *)cookie;
    rv->errors |= error;
    rv->operation = operation;
    rv->key = resp->v.v0.key;
    rv->nkey = resp->v.v0.nkey;
    rv->cas = resp->v.v0.cas;
    rv->counter--;
    if (rv->counter <= 0) {
        assert(io);
        io->stop_event_loop(io);
    }
    (void)instance;
}

static void get_callback(lcb_t instance,
                         const void *cookie,
                         lcb_error_t error,
                         const lcb_get_resp_t *resp)
{
    struct rvbuf *rv = (struct rvbuf *)cookie;
    rv->error = error;
    rv->bytes = resp->v.v0.bytes;
    rv->nbytes = resp->v.v0.nbytes;
    rv->key = resp->v.v0.key;
    rv->nkey = resp->v.v0.nkey;
    rv->cas = resp->v.v0.cas;
    rv->flags = resp->v.v0.flags;
    rv->counter--;
    if (rv->counter <= 0) {
        assert(io);
        io->stop_event_loop(io);
    }
    (void)instance;
}

static void touch_callback(lcb_t instance,
                           const void *cookie,
                           lcb_error_t error,
                           const lcb_touch_resp_t *resp)
{
    struct rvbuf *rv = (struct rvbuf *)cookie;
    rv->error = error;
    assert(error == LCB_SUCCESS);
    rv->key = resp->v.v0.key;
    rv->nkey = resp->v.v0.nkey;
    rv->counter--;
    if (rv->counter <= 0) {
        assert(io);
        io->stop_event_loop(io);
    }
    (void)instance;
}

static void version_callback(lcb_t instance,
                             const void *cookie,
                             lcb_error_t error,
                             const lcb_server_version_resp_t *resp)
{
    const char *server_endpoint = resp->v.v0.server_endpoint;
    const char *vstring = resp->v.v0.vstring;
    lcb_size_t nvstring = resp->v.v0.nvstring;
    struct rvbuf *rv = (struct rvbuf *)cookie;
    rv->error = error;
    char *str;

    assert(error == LCB_SUCCESS);

    if (server_endpoint == NULL) {
        assert(rv->counter == 0);
        io->stop_event_loop(io);
        return;
    }

    rv->counter--;

    /*copy the key to an allocated buffer and ensure the key read from vstring
     * will not segfault
     */
    str = malloc(nvstring);
    memcpy(str, vstring, nvstring);
    free(str);

    (void)instance;
}

static void test_set1(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    lcb_store_cmd_t cmd;
    const lcb_store_cmd_t *cmds[] = { &cmd };

    memset(&cmd, 0, sizeof(cmd));
    cmd.v.v0.key = "foo";
    cmd.v.v0.nkey = strlen(cmd.v.v0.key);
    cmd.v.v0.bytes = "bar";
    cmd.v.v0.nbytes = strlen(cmd.v.v0.bytes);
    cmd.v.v0.operation = LCB_SET;
    (void)lcb_set_store_callback(session, store_callback);
    err = lcb_store(session, &rv, 1, cmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);
    assert(rv.operation == LCB_SET);
    assert(memcmp(rv.key, "foo", 3) == 0);
}

static void test_set2(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    lcb_size_t ii;
    lcb_store_cmd_t cmd;
    const lcb_store_cmd_t *cmds[] = { &cmd };

    memset(&cmd, 0, sizeof(cmd));
    cmd.v.v0.key = "foo";
    cmd.v.v0.nkey = strlen(cmd.v.v0.key);
    cmd.v.v0.bytes = "bar";
    cmd.v.v0.nbytes = strlen(cmd.v.v0.bytes);
    cmd.v.v0.operation = LCB_SET;

    (void)lcb_set_store_callback(session, mstore_callback);
    rv.errors = 0;
    rv.counter = 0;
    for (ii = 0; ii < 10; ++ii, ++rv.counter) {
        err = lcb_store(session, &rv, 1, cmds);
        assert(err == LCB_SUCCESS);
    }
    io->run_event_loop(io);
    assert(rv.errors == 0);
}

static void test_get1(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    lcb_store_cmd_t storecmd;
    const lcb_store_cmd_t *storecmds[] = { &storecmd };
    lcb_get_cmd_t getcmd;
    const lcb_get_cmd_t *getcmds[] = { &getcmd };

    memset(&storecmd, 0, sizeof(storecmd));
    storecmd.v.v0.key = "foo";
    storecmd.v.v0.nkey = strlen(storecmd.v.v0.key);
    storecmd.v.v0.bytes = "bar";
    storecmd.v.v0.nbytes = strlen(storecmd.v.v0.bytes);
    storecmd.v.v0.operation = LCB_SET;

    (void)lcb_set_store_callback(session, store_callback);
    (void)lcb_set_get_callback(session, get_callback);

    err = lcb_store(session, &rv, 1, storecmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);

    memset(&rv, 0, sizeof(rv));
    memset(&getcmd, 0, sizeof(getcmd));
    getcmd.v.v0.key = "foo";
    getcmd.v.v0.nkey = strlen(getcmd.v.v0.key);
    err = lcb_get(session, &rv, 1, getcmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);
    assert(rv.nbytes == strlen("bar"));
    assert(memcmp(rv.bytes, "bar", 3) == 0);
}

static void test_get2(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    char *key = "fooX", *val = "bar";
    lcb_size_t nkey = strlen(key), nval = strlen(val);
    char **keys;
    lcb_size_t *nkeys, ii;
    lcb_store_cmd_t storecmd;
    const lcb_store_cmd_t *storecmds[] = { &storecmd };
    lcb_get_cmd_t *getcmds[26];

    (void)lcb_set_store_callback(session, store_callback);
    (void)lcb_set_get_callback(session, get_callback);

    keys = malloc(26 * sizeof(char *));
    nkeys = malloc(26 * sizeof(lcb_size_t));
    if (keys == NULL || nkeys == NULL) {
        err_exit("Failed to allocate memory for keys");
    }
    for (ii = 0; ii < 26; ii++) {
        nkeys[ii] = nkey;
        keys[ii] = strdup(key);
        if (keys[ii] == NULL) {
            err_exit("Failed to allocate memory for key");
        }
        keys[ii][3] = (char)(ii + 'a');
        memset(&storecmd, 0, sizeof(storecmd));
        storecmd.v.v0.key = key;
        storecmd.v.v0.nkey = nkey;
        storecmd.v.v0.bytes = val;
        storecmd.v.v0.nbytes = nval;
        storecmd.v.v0.operation = LCB_SET;
        err = lcb_store(session, &rv, 1, storecmds);
        assert(err == LCB_SUCCESS);
        io->run_event_loop(io);
        assert(rv.error == LCB_SUCCESS);
        memset(&rv, 0, sizeof(rv));

        getcmds[ii] = calloc(1, sizeof(lcb_get_cmd_t));
        if (getcmds[ii] == NULL) {
            err_exit("Failed to allocate memory for get command");
        }
        getcmds[ii]->v.v0.key = key;
        getcmds[ii]->v.v0.nkey = nkey;
    }

    rv.counter = 26;
    err = lcb_get(session, &rv, 26, (const lcb_get_cmd_t *const *)getcmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);
    assert(rv.nbytes == nval);
    assert(memcmp(rv.bytes, "bar", 3) == 0);
    for (ii = 0; ii < 26; ii++) {
        free(keys[ii]);
        free(getcmds[ii]);
    }
    free(keys);
    free(nkeys);
}

static void test_touch1(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    char *key = "fooX", *val = "bar";
    lcb_size_t nkey = strlen(key), nval = strlen(val);
    char **keys;
    lcb_size_t *nkeys, ii;
    lcb_time_t *ttls;
    lcb_store_cmd_t storecmd;
    const lcb_store_cmd_t *storecmds[] = { &storecmd };
    lcb_touch_cmd_t *touchcmds[26];

    (void)lcb_set_store_callback(session, store_callback);
    (void)lcb_set_touch_callback(session, touch_callback);

    keys = malloc(26 * sizeof(char *));
    nkeys = malloc(26 * sizeof(lcb_size_t));
    ttls = malloc(26 * sizeof(lcb_time_t));
    if (keys == NULL || nkeys == NULL || ttls == NULL) {
        err_exit("Failed to allocate memory for keys");
    }
    for (ii = 0; ii < 26; ii++) {
        nkeys[ii] = nkey;
        keys[ii] = strdup(key);
        ttls[ii] = 1;
        if (keys[ii] == NULL) {
            err_exit("Failed to allocate memory for key");
        }
        keys[ii][3] = (char)(ii + 'a');
        memset(&storecmd, 0, sizeof(storecmd));
        storecmd.v.v0.key = key;
        storecmd.v.v0.nkey = nkey;
        storecmd.v.v0.bytes = val;
        storecmd.v.v0.nbytes = nval;
        storecmd.v.v0.operation = LCB_SET;
        err = lcb_store(session, &rv, 1, storecmds);
        assert(err == LCB_SUCCESS);
        io->run_event_loop(io);
        assert(rv.error == LCB_SUCCESS);
        memset(&rv, 0, sizeof(rv));

        touchcmds[ii] = calloc(1, sizeof(lcb_touch_cmd_t));
        if (touchcmds[ii] == NULL) {
            err_exit("Failed to allocate memory for touch command");
        }
        touchcmds[ii]->v.v0.key = key;
        touchcmds[ii]->v.v0.nkey = nkey;
    }

    rv.counter = 26;
    err = lcb_touch(session, &rv, 26, (const lcb_touch_cmd_t * const *)touchcmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);
    for (ii = 0; ii < 26; ii++) {
        free(keys[ii]);
        free(touchcmds[ii]);
    }
    free(keys);
    free(ttls);
    free(nkeys);
}

static lcb_error_t test_connect(char **argv, const char *username,
                                const char *password,
                                const char *bucket)
{
    const char *endpoint;
    lcb_error_t rc;
    struct lcb_create_st options;

    assert(session == NULL);
    assert(mock == NULL);
    assert(io == NULL);

    io = get_test_io_opts();
    if (io == NULL) {
        err_exit("Failed to create IO session");
    }

    mock = start_mock_server(argv);
    if (mock == NULL) {
        err_exit("Failed to start mock server");
    }

    endpoint = get_mock_http_server(mock);

    memset(&options, 0, sizeof(options));
    options.v.v0.host = endpoint;
    options.v.v0.user = username;
    options.v.v0.passwd = password;
    options.v.v0.bucket = bucket;
    options.v.v0.io = io;

    if (lcb_create(&session, &options) != LCB_SUCCESS) {
        err_exit("Failed to create libcouchbase session");
    }

    (void)lcb_set_error_callback(session, error_callback2);

    if (lcb_connect(session) != LCB_SUCCESS) {
        err_exit("Failed to connect to server");
    }
    lcb_wait(session);
    rc = global_error;

    lcb_destroy(session);
    session = NULL;
    io = NULL;
    shutdown_mock_server(mock);
    mock = NULL;

    return rc;
}

RESPONSE_HANDLER old_sasl_auth_response_handler;
lcb_error_t sasl_auth_rc;

static void sasl_auth_response_handler(lcb_server_t *server,
                                       struct lcb_command_data_st *command_data,
                                       protocol_binary_response_header *res)
{
    sasl_auth_rc = ntohs(res->response.status);
    old_sasl_auth_response_handler(server, command_data, res);
}

static void test_set3(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    lcb_store_cmd_t storecmd;
    const lcb_store_cmd_t *storecmds[] = { &storecmd };

    old_sasl_auth_response_handler = session->response_handler[PROTOCOL_BINARY_CMD_SASL_AUTH];
    session->response_handler[PROTOCOL_BINARY_CMD_SASL_AUTH] = sasl_auth_response_handler;
    sasl_auth_rc = -1;

    (void)lcb_set_store_callback(session, store_callback);
    memset(&storecmd, 0, sizeof(storecmd));
    storecmd.v.v0.key = "foo";
    storecmd.v.v0.nkey = strlen(storecmd.v.v0.key);
    storecmd.v.v0.bytes = "bar";
    storecmd.v.v0.nbytes = strlen(storecmd.v.v0.bytes);
    storecmd.v.v0.operation = LCB_SET;
    err = lcb_store(session, &rv, 1, storecmds);
    assert(err == LCB_SUCCESS);
    io->run_event_loop(io);
    assert(rv.error == LCB_SUCCESS);
    assert(rv.operation == LCB_SET);
    assert(memcmp(rv.key, "foo", 3) == 0);
    assert(sasl_auth_rc == LCB_SUCCESS);
    session->response_handler[PROTOCOL_BINARY_CMD_SASL_AUTH] = old_sasl_auth_response_handler;
}

static void test_version1(void)
{
    lcb_error_t err;
    struct rvbuf rv;
    lcb_server_version_cmd_t cmd;
    const lcb_server_version_cmd_t* cmds[] = { &cmd };
    memset(&cmd, 0, sizeof(cmd));

    (void)lcb_set_version_callback(session, version_callback);
    err = lcb_server_versions(session, &rv, 1, cmds);

    assert(err == LCB_SUCCESS);

    rv.counter = total_node_count;

    io->run_event_loop(io);

    /* Ensure all version responses have been received */
    assert(rv.counter == 0);
}

static void test_spurious_saslerr(void)
{
    int iterations = 50;
    struct rvbuf rvs[50];
    int i;
    lcb_error_t err;
    lcb_store_cmd_t storecmd;
    const lcb_store_cmd_t *storecmds[] = { &storecmd };
    lcb_set_store_callback(session, mstore_callback);

    memset(rvs, 0, sizeof(rvs));

    for (i = 0; i < iterations; i++) {
        rvs[i].counter = 999; /*don't trigger a stop_event_loop*/
        memset(&storecmd, 0, sizeof(storecmd));
        storecmd.v.v0.key = "KEY";
        storecmd.v.v0.nkey = strlen(storecmd.v.v0.key);
        storecmd.v.v0.bytes = "KEY";
        storecmd.v.v0.nbytes = strlen(storecmd.v.v0.bytes);
        storecmd.v.v0.operation = LCB_SET;
        err = lcb_store(session, rvs + i, 1, storecmds);
        if (err != LCB_SUCCESS) {
            err_exit("Store operation failed");
        }
    }
    lcb_wait(session);

    for (i = 0; i < iterations; i++) {
        char *errinfo = NULL;
        if (rvs[i].errors != LCB_SUCCESS) {
            errinfo = "Did not get success response";
        } else if (rvs[i].nkey != 3) {
            errinfo = "Did not get expected key length";
        } else if (memcmp(rvs[i].key, "KEY", 3) != 0) {
            errinfo = "Weird key size";
        }
        if (errinfo) {
            err_exit("%s", errinfo);
        }
    }
}

int main(int argc, char **argv)
{
    char str_node_count[16];
    const char *args[] = {"--nodes", "",
                          "--buckets=default::memcache", NULL
                         };

    if (getenv("LCB_VERBOSE_TESTS") == NULL) {
        freopen("/dev/null", "w", stdout);
    }

    total_node_count = 5;
    snprintf(str_node_count, 16, "%d", total_node_count);
    args[1] = str_node_count;

    setup((char **)args, "Administrator", "password", "default");
    test_set1();
    test_set2();
    test_get1();
    test_get2();
    test_version1();
    teardown();

    args[2] = NULL;
    setup((char **)args, "Administrator", "password", "default");
    test_set1();
    test_set2();
    test_get1();
    test_get2();
    test_touch1();
    test_version1();
    teardown();

    assert(test_connect((char **)args, "Administrator", "password", "missing") == LCB_BUCKET_ENOENT);

    args[2] = "--buckets=protected:secret";
    assert(test_connect((char **)args, "protected", "incorrect", "protected") == LCB_AUTH_ERROR);
    setup((char **)args, "protected", "secret", "protected");
    test_set3();
    test_spurious_saslerr();
    teardown();

    (void)argc;
    (void)argv;
    return EXIT_SUCCESS;
}
