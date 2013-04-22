/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 * @author Christopher Alfeld <calfeld@qualys.com>
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/engine.h>
#include "engine_private.h"

#include "core_private.h"
#include "state_notify_private.h"

#include <ironbee/array.h>
#include <ironbee/cfgmap.h>
#include <ironbee/context_selection.h>
#include <ironbee/core.h>
#include <ironbee/hash.h>
#include <ironbee/ip.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/provider.h>
#include <ironbee/server.h>
#include <ironbee/state_notify.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* -- Constants -- */

/** Constant max path used in calls to getcwd() */
static const size_t maxpath = 512;

/** Constant String Values */
const ib_default_string_t ib_default_string = {
    "",          /* empty */
    "unknown",   /* unknown */
    "core",      /* core */
    "/",         /* root_path */
    "/",         /* uri_root_path */
};

static const char *default_auditlog_index = "ironbee-index.log";

/* -- Internal Structures -- */
typedef struct {
    ib_state_event_type_t  event;      /**< Event type */
    const char            *event_name; /**< Event name */
    ib_state_hook_type_t   hook_type;  /**< Hook's type */
} ib_event_type_data_t;

/**
 * List of callback data types for event id to type lookups.
 */
static ib_event_type_data_t ib_event_table[IB_STATE_EVENT_NUM];

/**
 * Initialize the event table entry for @a event
 *
 * @note This is done as a macro take advantage of IB_STRINGIFY(),
 * and invokes init_event_table_entry().
 *
 * @param[in] event Event (ib_state_event_type_t) to initialize
 * @param[in] hook_type The associated hook type
 */
#define INIT_EVENT_TABLE_ENT(event,hook_type)              \
    init_event_table_entry(event, IB_STRINGIFY(event), hook_type)

/* -- Internal Routines -- */
/**
 * Initialize the event table entry for @a event.
 *
 * @param[in] event Event to initialize
 * @param[in] event_name Name of @a event
 * @param[in] hook_type The associated hook type
 */
static void init_event_table_entry(
    ib_state_event_type_t  event,
    const char            *event_name,
    ib_state_hook_type_t   hook_type)
{
    assert(event < IB_STATE_EVENT_NUM);
    assert(event_name != NULL);

    ib_event_type_data_t *ent = &ib_event_table[event];
    ent->event = event;
    ent->event_name = event_name;
    ent->hook_type = hook_type;
}

ib_status_t ib_hook_check(
    ib_engine_t* ib,
    ib_state_event_type_t event,
    ib_state_hook_type_t hook_type
) {
    static const size_t num_events =
        sizeof(ib_event_table) / sizeof(ib_event_type_data_t);
    ib_state_hook_type_t expected_hook_type;

    if (event > num_events) {
        ib_log_error( ib,
            "Event/hook mismatch: Unknown event type: %d", event
        );
        return IB_EINVAL;
    }

    expected_hook_type = ib_event_table[event].hook_type;
    if (expected_hook_type != hook_type) {
        ib_log_debug(ib,
                     "Event/hook mismatch: "
                     "Event type %s expected %d but received %d",
                     ib_state_event_name(event),
                     expected_hook_type, hook_type);
        return IB_EINVAL;
    }

    return IB_OK;
}

static ib_status_t ib_hook_register(
    ib_engine_t* ib,
    ib_state_event_type_t event,
    ib_hook_t* hook
) {
    ib_hook_t *last = ib->hook[event];

    /* Insert the hook at the end of the list */
    if (last == NULL) {
        ib_log_debug3(ib, "Registering %s hook: %p",
                      ib_state_event_name(event),
                      hook->callback.as_void);

        ib->hook[event] = hook;

        return IB_OK;
    }
    while (last->next != NULL) {
        last = last->next;
    }

    last->next = hook;

    ib_log_debug3(ib, "Registering %s hook after %p: %p",
                  ib_state_event_name(event), last->callback.as_void,
                  hook->callback.as_void);

    return IB_OK;
}

static ib_status_t ib_hook_unregister(
    ib_engine_t* ib,
    ib_state_event_type_t event,
    ib_void_fn_t cb
) {
    ib_hook_t *prev = NULL;
    ib_hook_t *hook = ib->hook[event];

    /* Remove the first matching hook */
    while (hook != NULL) {
        if (hook->callback.as_void == cb) {
            if (prev == NULL) {
                ib->hook[event] = hook->next;
            }
            else {
                prev->next = hook->next;
            }
            return IB_OK;
        }
        prev = hook;
        hook = hook->next;
    }

    return IB_ENOENT;
}

/**
 * Initialize the IronBee event table.
 *
 * @note Asserts if error detected.
 *
 * @returns Status code:
 *    - IB_OK All OK
 */
static ib_status_t ib_event_table_init(void)
{
    ib_state_event_type_t event;
    static bool           table_initialized = false;

    if (table_initialized) {
        goto validate;
    };

    memset(ib_event_table, 0, sizeof(ib_event_table));

    /* Engine States */
    INIT_EVENT_TABLE_ENT(conn_started_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(conn_finished_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(tx_started_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(tx_process_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(tx_finished_event, IB_STATE_HOOK_TX);

    /* Handler States */
    INIT_EVENT_TABLE_ENT(handle_context_conn_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(handle_connect_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(handle_context_tx_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_request_header_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_request_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_response_header_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_response_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_disconnect_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(handle_postprocess_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(handle_logging_event, IB_STATE_HOOK_TX);

    /* Server States */
    INIT_EVENT_TABLE_ENT(conn_opened_event, IB_STATE_HOOK_CONN);
    INIT_EVENT_TABLE_ENT(conn_closed_event, IB_STATE_HOOK_CONN);

    /* Parser States */
    INIT_EVENT_TABLE_ENT(request_started_event, IB_STATE_HOOK_REQLINE);
    INIT_EVENT_TABLE_ENT(request_header_data_event, IB_STATE_HOOK_HEADER);
    INIT_EVENT_TABLE_ENT(request_header_finished_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(request_body_data_event, IB_STATE_HOOK_TXDATA);
    INIT_EVENT_TABLE_ENT(request_finished_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(response_started_event, IB_STATE_HOOK_RESPLINE);
    INIT_EVENT_TABLE_ENT(response_header_data_event, IB_STATE_HOOK_HEADER);
    INIT_EVENT_TABLE_ENT(response_header_finished_event, IB_STATE_HOOK_TX);
    INIT_EVENT_TABLE_ENT(response_body_data_event, IB_STATE_HOOK_TXDATA);
    INIT_EVENT_TABLE_ENT(response_finished_event, IB_STATE_HOOK_TX);

    /* Logevent updated */
    INIT_EVENT_TABLE_ENT(handle_logevent_event, IB_STATE_HOOK_TX);

    /* Sanity check the table, make sure all events are initialized */
validate:
    for(event = conn_started_event;  event < IB_STATE_EVENT_NUM;  ++event) {
        assert(ib_event_table[event].event == event);
        assert(ib_event_table[event].hook_type != IB_STATE_HOOK_INVALID);
        assert(ib_event_table[event].event_name != NULL);
    }

    table_initialized = true;
    return IB_OK;
}

/* -- Main Engine Routines -- */
ib_status_t ib_initialize(void)
{
    ib_status_t rc;

    /* Initialize the utility library */
    rc = ib_util_initialize();
    if (rc != IB_OK) {
        return rc;
    }

    /* Initialize the event table */
    rc = ib_event_table_init();
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;

}

ib_status_t ib_shutdown(void)
{
    /* Shut down the utility library */
    ib_util_shutdown();

    return IB_OK;
}

ib_status_t ib_engine_create(ib_engine_t **pib,
                             const ib_server_t *server)
{
    ib_mpool_t *pool;
    ib_status_t rc;

    /* Create primary memory pool */
    rc = ib_mpool_create(&pool, "engine", NULL);
    if (rc != IB_OK) {
        rc = IB_EALLOC;
        goto failed;
    }

    /* Create the main structure in the primary memory pool */
    *pib = (ib_engine_t *)ib_mpool_calloc(pool, 1, sizeof(**pib));
    if (*pib == NULL) {
        rc = IB_EALLOC;
        goto failed;
    }
    (*pib)->mp = pool;

    /* Create temporary memory pool */
    rc = ib_mpool_create(&((*pib)->temp_mp),
                         "temp",
                         (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create the config memory pool */
    rc = ib_mpool_create(&((*pib)->config_mp),
                         "config",
                         (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a list to hold config contexts */
    rc = ib_list_create(&((*pib)->contexts), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create an engine config context and use it as the
     * main context until the engine can be configured.
     */
    rc = ib_context_create(*pib, NULL, IB_CTYPE_ENGINE,
                           "engine", "engine", &((*pib)->ectx));
    if (rc != IB_OK) {
        goto failed;
    }

    /* Set the context's CWD */
    rc = ib_context_set_cwd((*pib)->ectx, NULL);
    if (rc != IB_OK) {
        goto failed;
    }

    (*pib)->ctx = (*pib)->ectx;
    (*pib)->cfg_state = CFG_NOT_STARTED;

    /* Check server for ABI compatibility with this engine */
    if (server == NULL) {
        ib_log_error(*pib,  "Error in ib_create: server info required");
        rc = IB_EINVAL;
        goto failed;
    }
    if (server->vernum > IB_VERNUM) {
        ib_log_alert(*pib,
                     "Server %s (built against engine version %s) is not "
                     "compatible with this engine (version %s): "
                     "ABI %d > %d",
                     server->filename, server->version, IB_VERSION,
                     server->abinum, IB_ABINUM);
        rc = IB_EINCOMPAT;
        goto failed;
    }
    (*pib)->server = server;

    /* Sensor info. */
    (*pib)->sensor_name = IB_DSTR_UNKNOWN;
    (*pib)->sensor_version = IB_PRODUCT_VERSION_NAME;
    (*pib)->sensor_hostname = IB_DSTR_UNKNOWN;

    /* Create an array to hold loaded modules */
    /// @todo Need good defaults here
    rc = ib_array_create(&((*pib)->modules), (*pib)->mp, 16, 8);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create an array to hold filters */
    /// @todo Need good defaults here
    rc = ib_array_create(&((*pib)->filters), (*pib)->mp, 16, 8);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold configuration directive mappings by name */
    rc = ib_hash_create_nocase(&((*pib)->dirmap), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold provider apis by name */
    rc = ib_hash_create_nocase(&((*pib)->apis), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold providers by name */
    rc = ib_hash_create_nocase(&((*pib)->providers), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold transformations by name */
    rc = ib_hash_create_nocase(&((*pib)->tfns), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold operators by name */
    rc = ib_hash_create_nocase(&((*pib)->operators), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold actions by name */
    rc = ib_hash_create_nocase(&((*pib)->actions), (*pib)->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Initialize the core static module. */
    /// @todo Probably want to do this in a less hard-coded manner.
    rc = ib_module_init(ib_core_module_sym(), *pib);
    if (rc != IB_OK) {
        ib_log_alert(*pib,  "Error in ib_module_init");
        goto failed;
    }

    return rc;

failed:
    /* Make sure everything is cleaned up on failure */
    if (*pib != NULL) {
        ib_engine_pool_destroy(*pib, (*pib)->mp);
    }
    *pib = NULL;

    return rc;
}

ib_status_t ib_engine_init(ib_engine_t *ib)
{
    ib_status_t rc;

    rc = ib_context_open(ib->ectx);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_context_close(ib->ectx);
    return rc;
}

/* Create a main context to operate in. */
ib_status_t ib_engine_context_create_main(ib_engine_t *ib)
{
    ib_context_t *ctx;
    ib_status_t rc;

    rc = ib_context_create(ib, ib->ectx, IB_CTYPE_MAIN, "main", "main", &ctx);
    if (rc != IB_OK) {
        return rc;
    }

    /* Set the context's CWD */
    rc = ib_context_set_cwd(ctx, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    ib->ctx = ctx;

    return IB_OK;
}

/**
 * Configuration event handler
 *
 * @param[in] parser Configuration parser
 * @param[in] event Configuration event
 * @param[in] status Configuration status
 * @param[in] cbdata Callback data (ib_engine_t *)
 *
 * @returns Status code
 *  - Status code from ib_engine_config_{started,finished}()
 */
static ib_status_t ib_engine_config_handler(
    const ib_cfgpaser_t *parser,
    ib_config_event_t    event,
    ib_status_t          status,
    void                *cbdata)
{
    assert(parser != NULL);
    assert(cbdata != NULL);

    ib_engine_t *ib = (ib_engine_t *)cbdata;
    ib_status_t  rc;

    switch(event) {
    case IB_CONFIG_EVENT_STARTED:
        rc = ib_engine_config_started(ib, cp);
        break;
    case IB_CONFIG_EVENT_FINISHED:
        rc = ib_engine_config_finished(ib, cp);
        break;
    default:
        rc = IB_OK;
    }

    return rc;
}

ib_status_t ib_engine_cfgparser_set(ib_engine_t *ib,
                                    ib_cfgparser_t *cp)
{
    assert(ib != NULL);
    assert(cp != NULL);

    ib_status_t rc;

    /* Register the config started event handler */
    rc = ib_cfgparser_hook_register(cp, IB_CONFIG_EVENT_STARTED,
                                    ib_engine_config_handler, ib);
    if (rc != IB_OK) {
        return rc;
    }

    /* Register the config finished event handler */
    rc = ib_cfgparser_hook_register(cp, IB_CONFIG_EVENT_FINISHED,
                                    ib_engine_config_handler, ib);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_engine_config_started(ib_engine_t *ib,
                                     ib_cfgparser_t *cp)
{
    assert(ib != NULL);
    assert(cp != NULL);
    assert(ib->cfg_state == CFG_NOT_STARTED);
    ib_status_t rc;

    /* Store the configuration parser in the engine */
    ib->cfgparser = cp;
    ib->cfg_state = CFG_STARTED;

    /* Create and configure the main configuration context. */
    rc = ib_engine_context_create_main(ib);
    if (rc != IB_OK) {
        return rc;
    }

    /* Open the main context */
    rc = ib_context_open(ib->ctx);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_engine_config_finished(ib_engine_t *ib,
                                      ib_status_t status)
{
    assert(ib != NULL);
    assert(ib->cfg_state == CFG_STARTED);
    ib_status_t rc;

    /* Initialize (and close) the main configuration context.
     * Note: The context can be NULL for unit tests. */
    if (ib->ctx != NULL) {
        rc = ib_context_close(ib->ctx);
        if (rc != IB_OK) {
            return rc;
        }
    }

    /* Clear config parser pointer */
    ib->cfgparser = NULL;
    ib->cfg_state = CFG_FINISHED;

    /* Destroy the temporary memory pool. */
    ib_engine_pool_temp_destroy(ib);

    return IB_OK;
}

ib_status_t ib_engine_module_get(const ib_engine_t *ib,
                                 const char * name,
                                 ib_module_t **pm)
{
    size_t n;
    size_t i;
    ib_module_t *m;

    /* Return the first module matching the name. */
    IB_ARRAY_LOOP(ib->modules, n, i, m) {
        if (m == NULL) {
            continue;
        }
        if (strcmp(name, m->name) == 0) {
            *pm = m;
            return IB_OK;
        }
    }

    *pm = NULL;

    return IB_ENOENT;
}

ib_status_t ib_engine_cfgparser_get(const ib_engine_t *ib,
                                    const ib_cfgparser_t **pparser)
{
    assert(ib != NULL);
    assert(pparser != NULL);

    *pparser = ib->cfgparser;
    return IB_OK;
}

ib_mpool_t *ib_engine_pool_main_get(const ib_engine_t *ib)
{
    return ib->mp;
}

ib_mpool_t *ib_engine_pool_config_get(const ib_engine_t *ib)
{
    return ib->mp;
}

ib_mpool_t *ib_engine_pool_temp_get(const ib_engine_t *ib)
{
    return ib->temp_mp;
}

void ib_engine_pool_temp_destroy(ib_engine_t *ib)
{
    ib_engine_pool_destroy(ib, ib->temp_mp);
    ib->temp_mp = NULL;
    return;
}

void ib_engine_pool_destroy(ib_engine_t *ib, ib_mpool_t *mp)
{
    assert(ib != NULL);


    if (mp == NULL) {
        return;
    }

#ifdef IB_DEBUG_MEMORY
    {
        ib_status_t rc;
        char *message = NULL;
        char *path = ib_mpool_path(mp);

        if (path == NULL) {
            /* This will probably also fail... */
            ib_log_emergency(ib, "Allocation error.");
            goto finish;
        }

        rc = ib_mpool_validate(mp, &message);
        if (rc != IB_OK) {
            ib_log_error(ib, "Memory pool %s failed to validate: %s",
                path, (message ? message : "no message")
            );
        }

        if (message != NULL) {
            free(message);
            message = NULL;
        }

        message = ib_mpool_analyze(mp);
        if (message == NULL) {
            ib_log_emergency(ib, "Allocation error.");
            goto finish;
        }

        /* We use printf to coincide with the final memory debug which
         * can't be logged because it occurs too late in engine destruction.
         */
        printf("Memory Pool Analysis of %s:\n%s", path, message);

    finish:
        if (path != NULL) {
            free(path);
        }
        if (message != NULL) {
            free(message);
        }
    }
#endif

    ib_mpool_release(mp);

    return;
}


void ib_engine_destroy(ib_engine_t *ib)
{
    size_t ne;
    size_t idx;
    ib_list_node_t *node;
    ib_module_t *cm = ib_core_module(ib);
    ib_module_t *m;

    if (ib == NULL) {
        return;
    }

    /// @todo Destroy filters

    ib_log_debug3(ib, "Destroying configuration contexts...");
    IB_LIST_LOOP_REVERSE(ib->contexts, node) {
        ib_context_t *ctx = (ib_context_t *)node->data;
        if ( (ctx != ib->ctx) && (ctx != ib->ectx) ) {
            ib_context_destroy(ctx);
        }
    }

    ib_log_debug3(ib, "Destroying engine context...");
    if (ib->ctx != ib->ectx) {
        ib_context_destroy(ib->ctx);
        ib->ctx = NULL;
    }
    ib_context_destroy(ib->ectx);
    ib->ectx = NULL;

    /* Important: Logging does not work after this point! */
    IB_ARRAY_LOOP_REVERSE(ib->modules, ne, idx, m) {
        if ( (m != NULL) && (m != cm) ) {
            ib_module_unload(m);
        }
    }

    /* Unload core module. */
    ib_module_unload(cm);
    /* No logging from here on out. */

#ifdef IB_DEBUG_MEMORY
    /* We can't use ib_engine_pool_destroy here as too little of the
     * the engine is left.
     *
     * But always output memory usage stats.
     *
     * Also can't use logging anymore.
     */
    {
        char *report = ib_mpool_analyze(ib->mp);
        if (report != NULL) {
            printf("Engine Memory Use:\n%s\n", report);
            free(report);
        }
    }
#endif
    ib_mpool_destroy(ib->mp);

    return;
}

ib_status_t ib_conn_create(ib_engine_t *ib,
                           ib_conn_t **pconn, void *server_ctx)
{
    ib_mpool_t *pool;
    ib_status_t rc;
    char namebuf[64];

    /* Create a sub-pool for each connection and allocate from it */
    /// @todo Need to tune the pool size
    rc = ib_mpool_create(&pool, "conn", ib->mp);
    if (rc != IB_OK) {
        ib_log_alert(ib,
            "Failed to create connection memory pool: %s",
            ib_status_to_string(rc)
        );
        rc = IB_EALLOC;
        goto failed;
    }
    *pconn = (ib_conn_t *)ib_mpool_calloc(pool, 1, sizeof(**pconn));
    if (*pconn == NULL) {
        ib_log_alert(ib, "Failed to allocate memory for connection");
        rc = IB_EALLOC;
        goto failed;
    }

    /* Mark time. */
    ib_clock_gettimeofday(&(*pconn)->tv_created);
    (*pconn)->t.started = ib_clock_get_time();

    /* Name the connection pool */
    snprintf(namebuf, sizeof(namebuf), "conn[%p]", (void *)(*pconn));
    ib_mpool_setname(pool, namebuf);

    (*pconn)->ib = ib;
    (*pconn)->mp = pool;
    (*pconn)->ctx = ib->ctx;
    (*pconn)->server_ctx = server_ctx;

    /* Data */
    rc = ib_data_create((*pconn)->mp, &(*pconn)->data);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to create conn data: %s",
                     ib_status_to_string(rc));
        return rc;
    }

    /* Create the per-module data data store. */
    rc = ib_array_create(&((*pconn)->module_data), pool, 16, 8);
    if (rc != IB_OK) {
        rc = IB_EALLOC;
        goto failed;
    }

    return IB_OK;

failed:
    /* Make sure everything is cleaned up on failure */
    if (*pconn != NULL) {
        ib_engine_pool_destroy(ib, (*pconn)->mp);
    }
    *pconn = NULL;

    return rc;
}

ib_status_t ib_conn_get_module_data(
    const ib_conn_t    *conn,
    const ib_module_t  *m,
    void              **data
)
{
  assert(conn != NULL);
  assert(m != NULL);
  assert(data != NULL);

  ib_status_t rc = ib_array_get(conn->module_data, m->idx, data);
  return rc;
}

ib_status_t DLL_PUBLIC ib_conn_set_module_data(
    ib_conn_t         *conn,
    const ib_module_t *m,
    void              *data
)
{
  assert(conn != NULL);
  assert(m != NULL);

  ib_status_t rc = ib_array_setn(conn->module_data, m->idx, data);
  return rc;
}

void ib_conn_parser_context_set(ib_conn_t *conn,
                                void *parser_ctx)
{
    assert(conn != NULL);

    conn->parser_ctx = parser_ctx;

    return;
}

void *ib_conn_parser_context_get(ib_conn_t *conn)
{
    assert(conn != NULL);

    return conn->parser_ctx;
}

void ib_conn_destroy(ib_conn_t *conn)
{
    /// @todo Probably need to update state???
    if ( conn != NULL && conn->mp != NULL ) {
        ib_engine_pool_destroy(conn->ib, conn->mp);
        /* Don't do this: conn->mp = NULL; conn is now freed memory! */
    }
}

ib_status_t ib_tx_generate_id(ib_tx_t *tx,
                              ib_mpool_t *mp)
{
    ib_uuid_t uuid;
    ib_status_t rc;
    char *str;

    rc = ib_uuid_create_v4(&uuid);
    if (rc != IB_OK) {
        return rc;
    }

    /* Convert to a hex-string representation */
    str = (char *)ib_mpool_alloc(mp, IB_UUID_HEX_SIZE);
    if (str == NULL) {
        return IB_EALLOC;
    }
    tx->id = str;

    rc = ib_uuid_bin_to_ascii(str, &uuid);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}

ib_status_t ib_tx_create(ib_tx_t **ptx,
                         ib_conn_t *conn,
                         void *sctx)
{
    ib_mpool_t *pool;
    ib_status_t rc;
    char namebuf[64];
    ib_tx_t *tx = NULL;
    ib_core_cfg_t *corecfg;

    ib_engine_t *ib = conn->ib;

    rc = ib_core_context_config(ib->ctx, &corecfg);
    if (rc != IB_OK) {
        ib_log_alert(ib, "Failed to retrieve core module configuration.");
    }

    assert(corecfg != NULL);

    /* Create a sub-pool from the connection memory pool for each
     * transaction and allocate from it
     */
    rc = ib_mpool_create(&pool, "tx", conn->mp);
    if (rc != IB_OK) {
        ib_log_alert(ib,
            "Failed to create transaction memory pool: %s",
            ib_status_to_string(rc)
        );
        rc = IB_EALLOC;
        goto failed;
    }
    tx = (ib_tx_t *)ib_mpool_calloc(pool, 1, sizeof(*tx));
    if (tx == NULL) {
        ib_log_alert(ib, "Failed to allocate memory for transaction");
        rc = IB_EALLOC;
        goto failed;
    }

    /* Name the transaction pool */
    snprintf(namebuf, sizeof(namebuf), "tx[%p]", (void *)tx);
    ib_mpool_setname(pool, namebuf);

    /* Mark time. */
    ib_clock_gettimeofday(&tx->tv_created);
    tx->t.started = ib_clock_get_time();

    tx->ib = ib;
    tx->mp = pool;
    tx->ctx = ib->ctx;
    tx->sctx = sctx;
    tx->conn = conn;
    tx->er_ipstr = conn->remote_ipstr;
    tx->hostname = IB_DSTR_EMPTY;
    tx->path = IB_DSTR_URI_ROOT_PATH;
    tx->auditlog_parts = corecfg->auditlog_parts;
    tx->block_status = corecfg->block_status;

    ++conn->tx_count;
    ib_tx_generate_id(tx, tx->mp);

    /* Create data */
    rc = ib_data_create(tx->mp, &tx->data);
    if (rc != IB_OK) {
        ib_log_alert_tx(tx,
                        "Failed to create tx data: %s",
                        ib_status_to_string(rc));
        return rc;
    }

    /* Create logevents */
    rc = ib_list_create(&tx->logevents, tx->mp);
    if (rc != IB_OK) {
        return rc;
    }

    /* Create the per-module data data store. */
    rc = ib_array_create(&(tx->module_data), tx->mp, 16, 8);
    if (rc != IB_OK) {
        rc = IB_EALLOC;
        goto failed;
    }

    /* Create a filter controller. */
    rc = ib_fctl_tx_create(&(tx->fctl), tx, tx->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create the body buffers. */
    rc = ib_stream_create(&tx->request_body, tx->mp);
    if (rc != IB_OK) {
        goto failed;
    }
    rc = ib_stream_create(&tx->response_body, tx->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /**
     * After this, we have generally succeeded and are now outputting
     * the transaction to the conn object and the ptx pointer.
     */

    /* Add transaction to the connection list */
    if (conn->tx_first == NULL) {
        conn->tx_first = tx;
        conn->tx = tx;
        conn->tx_last = tx;
        ib_log_debug3_tx(tx, "First transaction: %p", tx);
    }
    else {
        conn->tx = tx;
        conn->tx_last->next = tx;
        conn->tx_last = tx;

        /* If there are more than one transactions, then this is a pipeline
         * request and needs to be marked as such.
         */
        if (conn->tx_first->next == tx) {
            ib_tx_flags_set(conn->tx_first, IB_TX_FPIPELINED);
        }
        ib_tx_flags_set(tx, IB_TX_FPIPELINED);

        ib_log_debug3_tx(tx, "Found a pipelined transaction: %p", tx);
    }

    /* Only when we are successful, commit changes to output variable. */
    *ptx = tx;
    ib_log_debug3_tx(tx, "TX CREATE p=%p id=%s", tx, tx->id);

    return IB_OK;

failed:
    /* Make sure everything is cleaned up on failure */
    if (tx != NULL) {
        ib_engine_pool_destroy(ib, tx->mp);
    }
    tx = NULL;

    return rc;
}

ib_status_t ib_tx_get_module_data(
    const ib_tx_t *tx,
    const ib_module_t *m,
    void *pdata
)
{
  assert(tx != NULL);
  assert(m != NULL);
  assert(pdata != NULL);

  ib_status_t rc = ib_array_get(tx->module_data, m->idx, pdata);
  return rc;
}

ib_status_t DLL_PUBLIC ib_tx_set_module_data(
    ib_tx_t *tx,
    const ib_module_t *m,
    void *data
)
{
  assert(tx != NULL);
  assert(m != NULL);

  ib_status_t rc = ib_array_setn(tx->module_data, m->idx, data);
  return rc;
}

void ib_tx_destroy(ib_tx_t *tx)
{
    /// @todo It should always be the first one in the list,
    ///       so this should not be needed and should cause an error
    ///       or maybe for us to throw a flag???
    assert(tx != NULL);
    assert(tx->conn != NULL);
    assert(tx->conn->tx_first == tx);
    ib_tx_t *curr;

    ib_log_debug3_tx(tx, "TX DESTROY p=%p id=%s", tx, tx->id);

    /* Make sure that the post processing state was notified. */
    // TODO: Remove the need for this
    if (! ib_tx_flags_isset(tx, IB_TX_FPOSTPROCESS)) {
        ib_log_warning_tx(tx,
                          "Post processing not run on transaction!");
    }

    /* Make sure that the post processing state was notified. */
    // TODO: Remove the need for this
    if (! ib_tx_flags_isset(tx, IB_TX_FLOGGING)) {
        ib_log_warning_tx(tx,
                          "Logging not run on transaction!");
    }

    /* Keep track of the first/current tx. */
    tx->conn->tx_first = tx->next;
    tx->conn->tx = tx->next;

    for (curr = tx->conn->tx_first; curr != NULL; curr = curr->next) {
        if (curr == tx) {
            curr->next = curr->next ? curr->next->next : NULL;
            break;
        }
    }

    /* Keep track of the last tx. */
    if (tx->conn->tx_last == tx) {
        tx->conn->tx_last = NULL;
    }

    /// @todo Probably need to update state???
    ib_engine_pool_destroy(tx->ib, tx->mp);
}

/* -- State Routines -- */

const char *ib_state_event_name(ib_state_event_type_t event)
{
    return ib_event_table[event].event_name;
}
/* -- Hook Routines -- */

ib_state_hook_type_t ib_state_hook_type(ib_state_event_type_t event)
{
    static const size_t num_events =
        sizeof(ib_event_table) / sizeof(ib_event_type_data_t);

    if (event > num_events) {
        return IB_STATE_HOOK_INVALID;
    }

    return ib_event_table[event].hook_type;
}

ib_status_t DLL_PUBLIC ib_hook_null_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_null_hook_fn_t cb,
    void *cdata
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_NULL);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.null = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_null_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_null_hook_fn_t cb
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_NULL);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_conn_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_conn_hook_fn_t cb,
    void *cdata
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_CONN);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.conn = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_conn_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_conn_hook_fn_t cb
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_CONN);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_tx_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_tx_hook_fn_t cb,
    void *cdata
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_TX);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.tx = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_tx_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_tx_hook_fn_t cb
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_TX);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_txdata_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_txdata_hook_fn_t cb,
    void *cdata
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_TXDATA);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.txdata = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_txdata_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_txdata_hook_fn_t cb
) {
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_TXDATA);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_header_data_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_header_data_fn_t cb,
    void *cdata)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_HEADER);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.headerdata = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_header_data_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_header_data_fn_t cb)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_HEADER);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_req_line_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_request_line_fn_t cb,
    void *cdata)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_REQLINE);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.requestline = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_req_line_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_request_line_fn_t cb)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_REQLINE);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_resp_line_register(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_response_line_fn_t cb,
    void *cdata)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_RESPLINE);
    if (rc != IB_OK) {
        return rc;
    }

    ib_hook_t *hook = (ib_hook_t *)ib_mpool_alloc(ib->mp, sizeof(*hook));
    if (hook == NULL) {
        ib_log_emergency(ib, "Error in ib_mpool_calloc");
        return IB_EALLOC;
    }

    hook->callback.responseline = cb;
    hook->cdata = cdata;
    hook->next = NULL;

    rc = ib_hook_register(ib, event, hook);

    return rc;
}

ib_status_t DLL_PUBLIC ib_hook_parsed_resp_line_unregister(
    ib_engine_t *ib,
    ib_state_event_type_t event,
    ib_state_response_line_fn_t cb)
{
    ib_status_t rc;

    rc = ib_hook_check(ib, event, IB_STATE_HOOK_RESPLINE);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_hook_unregister(ib, event, (ib_void_fn_t)cb);

    return rc;
}


/* -- Connection Handling -- */

/* -- Transaction Handling -- */

/* -- Configuration Contexts -- */

const ib_list_t *ib_context_get_all(const ib_engine_t *ib)
{
    assert(ib != NULL);

    return ib->contexts;
}

ib_status_t ib_context_create(ib_engine_t *ib,
                              ib_context_t *parent,
                              ib_ctype_t ctype,
                              const char *ctx_type,
                              const char *ctx_name,
                              ib_context_t **pctx)
{
    ib_mpool_t *ppool;
    ib_mpool_t *pool;
    ib_status_t rc;
    ib_context_t *ctx = NULL;
    char *full;
    size_t full_len;

    /* Create memory subpool */
    ppool = (parent == NULL) ? ib->mp : parent->mp;
    rc = ib_mpool_create(&pool, "context", ppool);
    if (rc != IB_OK) {
        rc = IB_EALLOC;
        goto failed;
    }

    /* Create the main structure */
    ctx = (ib_context_t *)ib_mpool_calloc(pool, 1, sizeof(*ctx));
    if (ctx == NULL) {
        rc = IB_EALLOC;
        goto failed;
    }

    ctx->ib = ib;
    ctx->mp = pool;
    ctx->parent = parent;
    ctx->ctype = ctype;
    ctx->ctx_type = ctx_type;
    ctx->ctx_name = ctx_name;
    ctx->state = CTX_CREATED;

    /* Generate the full name of the context */
    full_len = 2;
    if (parent != NULL) {
        full_len += strlen(parent->ctx_name) + 1;
    }
    if (ctx_type != NULL) {
        full_len += strlen(ctx_type);
    }
    if (ctx_name != NULL) {
        full_len += strlen(ctx_name);
    }
    full = (char *)ib_mpool_alloc(pool, full_len);
    if (full == NULL) {
        rc = IB_EALLOC;
        goto failed;
    }
    *full = '\0';
    if (parent != NULL) {
        strcat(full, parent->ctx_name);
        strcat(full, ":");
    }
    if (ctx_type != NULL) {
        strcat(full, ctx_type);
    }
    strcat(full, ":");
    if (ctx_name != NULL) {
        strcat(full, ctx_name);
    }
    ctx->ctx_full = full;

    /* Create a cfgmap to hold the configuration */
    rc = ib_cfgmap_create(&(ctx->cfg), ctx->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create an array to hold the module config data */
    rc = ib_array_create(&(ctx->cfgdata), ctx->mp, 16, 8);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a list to hold the enabled filters */
    rc = ib_list_create(&(ctx->filters), ctx->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a list to hold child contexts */
    rc = ib_list_create(&(ctx->children), ctx->mp);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Create a hash to hold the module-specific data */
    rc = ib_array_create(&(ctx->cfgdata), ctx->mp, 16, 8);
    if (rc != IB_OK) {
        goto failed;
    }

    /* Add myself to my parent's child list */
    if (parent != NULL) {
        rc = ib_list_push(parent->children, ctx);
        if (rc != IB_OK) {
            goto failed;
        }
    }

    /* Add to the engine's list of all contexts */
    rc = ib_list_push(ib->contexts, ctx);
    if (rc != IB_OK) {
        goto failed;
    }

    if (parent != NULL) {
        rc = ib_context_set_auditlog_index(
            ctx,
            parent->auditlog->index_enabled,
            parent->auditlog->index_default ? NULL : parent->auditlog->index);
    }
    else {
        rc = ib_context_set_auditlog_index(ctx, true, NULL);
    }
    if (rc != IB_OK) {
        goto failed;
    }

    /* Register the modules */
    /// @todo Later on this needs to be triggered by ActivateModule or similar
    if (ib->modules) {
        ib_module_t *m;
        size_t n;
        size_t i;
        IB_ARRAY_LOOP(ib->modules, n, i, m) {
            if (m == NULL) {
                ib_log_debug(ib, "Not registering NULL module idx=%zd", i);
                continue;
            }
            ib_log_debug3(ib, "Registering module=\"%s\" idx=%zd",
                          m->name, m->idx);
            rc = ib_module_register_context(m, ctx);
            if (rc != IB_OK) {
                goto failed;
            }
        }
    }
    else {
        /* Register the core module by default. */
        rc = ib_module_register_context(ib_core_module_sym(), ctx);
        if (rc != IB_OK) {
            goto failed;
        }
    }

    /* Commit the new ctx to pctx. */
    *pctx = ctx;

    return IB_OK;


failed:
    /* Make sure everything is cleaned up on failure */
    if (ctx != NULL) {
        ib_engine_pool_destroy(ib, ctx->mp);
    }

    return rc;
}

ib_status_t ib_context_open(ib_context_t *ctx)
{
    ib_engine_t *ib = ctx->ib;
    ib_context_data_t *cfgdata;
    ib_status_t rc;
    size_t ncfgdata;
    size_t i;

    if (ctx->state != CTX_CREATED) {
        return IB_EINVAL;
    }
    ib_log_debug3(ib, "Opening context ctx=%p '%s'", ctx, ctx->ctx_full);

    if (ctx->ctype != IB_CTYPE_ENGINE) {
        rc = ib_cfgparser_context_push(ib->cfgparser, ctx);
        if (rc != IB_OK) {
            return rc;
        }
        rc = ib_context_set_cwd(ctx, ib->cfgparser->cur_cwd);
        if (rc != IB_OK) {
            return rc;
        }
    }

    IB_ARRAY_LOOP(ctx->cfgdata, ncfgdata, i, cfgdata) {
        if (cfgdata == NULL) {
            continue;
        }
        ib_module_t *m = cfgdata->module;

        if (m->fn_ctx_open != NULL) {
            rc = m->fn_ctx_open(ib, m, ctx, m->cbdata_ctx_open);
            if (rc != IB_OK) {
                /// @todo Log the error???  Fail???
                ib_log_error(ib, "Failed to call context open: %s",
                             ib_status_to_string(rc));
                return rc;
            }
        }
    }

    ctx->state = CTX_OPEN;
    return IB_OK;
}

ib_status_t ib_context_close(ib_context_t *ctx)
{
        ib_engine_t *ib = ctx->ib;
    ib_context_data_t *cfgdata;
    ib_status_t rc;
    size_t ncfgdata;
    size_t i;

    if (ctx->state != CTX_OPEN) {
        return IB_EINVAL;
    }
    ib_log_debug3(ib, "Closing context ctx=%p '%s'", ctx, ctx->ctx_full);

    IB_ARRAY_LOOP(ctx->cfgdata, ncfgdata, i, cfgdata) {
        if (cfgdata == NULL) {
            continue;
        }
        ib_module_t *m = cfgdata->module;

        if (m->fn_ctx_close != NULL) {
            rc = m->fn_ctx_close(ib, m, ctx, m->cbdata_ctx_close);
            if (rc != IB_OK) {
                /// @todo Log the error???  Fail???
                ib_log_error(ib,
                             "Failed to call context close: %s",
                             ib_status_to_string(rc)
                );
                return rc;
            }
        }
    }

    if (ctx->ctype != IB_CTYPE_ENGINE) {
        rc = ib_cfgparser_context_pop(ib->cfgparser, NULL, NULL);
        if (rc != IB_OK) {
            return rc;
        }
    }

    ctx->state = CTX_CLOSED;
    return IB_OK;
}

ib_status_t ib_context_set_cwd(ib_context_t *ctx, const char *dir)
{
    assert(ctx != NULL);

    /* For special cases (i.e. tests), allow handle NULL directory */
    if (dir == NULL) {
        char *buf = (char *)ib_mpool_alloc(ctx->mp, maxpath);
        if (buf == NULL) {
            return IB_EALLOC;
        }
        ctx->ctx_cwd = getcwd(buf, maxpath);
        if (ctx->ctx_cwd == NULL) {
            return IB_EALLOC;
        }
        return IB_OK;
    }

    /* Copy it */
    ctx->ctx_cwd = ib_mpool_strdup(ctx->mp, dir);
    if (ctx->ctx_cwd == NULL) {
        return IB_EALLOC;
    }
    return IB_OK;
}

const char *ib_context_config_cwd(const ib_context_t *ctx)
{
    assert(ctx != NULL);

    if (ctx->ib->cfgparser == NULL) {
         return ctx->ctx_cwd;
    }
    else {
         return ctx->ib->cfgparser->cur_cwd;
    }
}

ib_status_t ib_context_set_auditlog_index(ib_context_t *ctx,
                                          bool enable,
                                          const char *idx)
{
    ib_status_t rc;

    assert(ctx != NULL);
    assert(ctx->ib != NULL);
    assert(ctx->mp != NULL);

    /* Check if a new audit log structure must be allocated:
     *   1. if auditlog == NULL or
     *   2. if the allocated audit log belongs to another context we may
     *      not change its auditlog->index value (or auditlog->index_fp).
     *      We must make a new auditlog that the passed in ib_context_t
     *      ctx owns.  */
    if (ctx->auditlog == NULL || ctx->auditlog->owner != ctx) {

        ctx->auditlog = (ib_auditlog_cfg_t *)
            ib_mpool_calloc(ctx->mp, 1, sizeof(*ctx->auditlog));

        if (ctx->auditlog == NULL) {
            return IB_EALLOC;
        }

        /* Set owner. */
        ctx->auditlog->owner = ctx;

        /* Set index_fp_lock. */
        if (enable == true) {
            rc = ib_lock_init(&ctx->auditlog->index_fp_lock);
            if (rc != IB_OK) {
                ib_log_debug2(ctx->ib,
                              "Failed to initialize lock "
                              "for audit index %s: %s",
                              idx, ib_status_to_string(rc));
                return rc;
            }

            /* Set index. */
            if (idx == NULL) {
                ctx->auditlog->index_default = true;
                idx = default_auditlog_index;
            }
            else {
                ctx->auditlog->index_default = false;
            }
            ctx->auditlog->index = ib_mpool_strdup(ctx->mp, idx);
            if (ctx->auditlog->index == NULL) {
                return IB_EALLOC;
            }
            ctx->auditlog->index_enabled = true;
        }
        else {
            ctx->auditlog->index = NULL;
            ctx->auditlog->index_fp = NULL;
        }
    }
    /* Else the auditlog struct is initialized and owned by this ctx. */
    else {
        bool unlock = false;
        if (ctx->auditlog->index_enabled == true) {
            const char *cidx = ctx->auditlog->index; /* Current index */

            rc = ib_lock_lock(&ctx->auditlog->index_fp_lock);
            if (rc != IB_OK) {
                ib_log_debug2(ctx->ib, "Failed lock to audit index %s",
                              ctx->auditlog->index);
                return rc;
            }
            unlock = true;

            /* Check that we aren't re-setting a value in the same context. */
            if ( (enable && ctx->auditlog->index_enabled) &&
                 ( ((idx == NULL) && ctx->auditlog->index_default) ||
                   ((idx != NULL) && (cidx != NULL) &&
                    (strcmp(idx, cidx) == 0)) ) )
            {
                if (unlock) {
                    ib_lock_unlock(&ctx->auditlog->index_fp_lock);
                }
                ib_log_debug2(ctx->ib,
                              "Re-setting log same value. No action: %s",
                              idx);

                return IB_OK;
            }
        }

        /* Replace the old index value with the new index value. */
        if (enable == false) {
            ctx->auditlog->index_enabled = false;
            ctx->auditlog->index_default = false;
            ctx->auditlog->index = NULL;
        }
        else {
            if (idx == NULL) {
                idx = default_auditlog_index;
                ctx->auditlog->index_default = true;
            }
            else {
                ctx->auditlog->index_default = false;
            }
            ctx->auditlog->index = ib_mpool_strdup(ctx->mp, idx);
            if (ctx->auditlog->index == NULL) {
                if (unlock) {
                    ib_lock_unlock(&ctx->auditlog->index_fp_lock);
                }
                return IB_EALLOC;
            }
            ctx->auditlog->index_enabled = true;
        }

        /* Close the audit log file if it is open. */
        if (ctx->auditlog->index_fp != NULL) {
            fclose(ctx->auditlog->index_fp);
            ctx->auditlog->index_fp = NULL;
        }

        if (unlock) {
            ib_lock_unlock(&ctx->auditlog->index_fp_lock);
        }
    }

    return IB_OK;
}

ib_status_t ib_context_site_set(ib_context_t *ctx,
                                const ib_site_t *site)
{
    assert(ctx != NULL);

    if (ctx->state == CTX_CLOSED) {
        return IB_EINVAL;
    }
    if ( (ctx->ctype != IB_CTYPE_SITE) && (ctx->ctype != IB_CTYPE_LOCATION) ) {
        return IB_EINVAL;
    }

    ctx->site = site;
    return IB_OK;
}

ib_status_t ib_context_site_get(const ib_context_t *ctx,
                                const ib_site_t **psite)
{
    assert(ctx != NULL);
    assert(psite != NULL);

    if ( (ctx->ctype != IB_CTYPE_SITE) && (ctx->ctype != IB_CTYPE_LOCATION) ) {
        return IB_EINVAL;
    }

    *psite = ctx->site;
    return IB_OK;
}

ib_status_t ib_context_location_set(ib_context_t *ctx,
                                    const ib_site_location_t *location)
{
    assert(ctx != NULL);

    if (ctx->state == CTX_CLOSED) {
        return IB_EINVAL;
    }
    if (ctx->ctype != IB_CTYPE_LOCATION) {
        return IB_EINVAL;
    }

    ctx->location = location;
    return IB_OK;
}

ib_status_t ib_context_location_get(const ib_context_t *ctx,
                                    const ib_site_location_t **plocation)
{
    assert(ctx != NULL);
    assert(plocation != NULL);

    if (ctx->ctype != IB_CTYPE_LOCATION) {
        return IB_EINVAL;
    }

    *plocation = ctx->location;
    return IB_OK;
}

ib_context_t *ib_context_parent_get(const ib_context_t *ctx)
{
    return ctx->parent;
}

void ib_context_parent_set(ib_context_t *ctx,
                           ib_context_t *parent)
{
    ctx->parent = parent;
    return;
}

ib_ctype_t ib_context_type(const ib_context_t *ctx)
{
    assert(ctx != NULL);

    return ctx->ctype;
}

bool ib_context_type_check(const ib_context_t *ctx, ib_ctype_t ctype)
{
    assert(ctx != NULL);

    return ctx->ctype == ctype;
}

const char *ib_context_type_get(const ib_context_t *ctx)
{
    assert(ctx != NULL);

    if (ctx->ctx_type == NULL) {
        return "";
    }
    else {
        return ctx->ctx_type;
    }
}

const char *ib_context_name_get(const ib_context_t *ctx)
{
    assert(ctx != NULL);

    if (ctx->ctx_name == NULL) {
        return "";
    }
    else {
        return ctx->ctx_name;
    }
}

const char *ib_context_full_get(const ib_context_t *ctx)
{
    assert(ctx != NULL);

    return ctx->ctx_full;
}

void ib_context_destroy(ib_context_t *ctx)
{
    ib_engine_t *ib;
    ib_context_data_t *cfgdata;
    ib_status_t rc;
    size_t ncfgdata, i;

    if (ctx == NULL) {
        return;
    }

    ib = ctx->ib;

    ib_log_debug3(ib, "Destroying context ctx=%p '%s'", ctx, ctx->ctx_full);

    /* Run through the context modules to call any ctx_fini functions. */
    /// @todo Not sure this is needed anymore
    IB_ARRAY_LOOP(ctx->cfgdata, ncfgdata, i, cfgdata) {
        if (cfgdata == NULL) {
            continue;
        }
        ib_module_t *m = cfgdata->module;

        if (m->fn_ctx_destroy != NULL) {
            ib_log_debug3(ib,
                          "Finishing context ctx=%p '%s' for module=%s (%p)",
                          ctx, ctx->ctx_full, m->name, m);
            rc = m->fn_ctx_destroy(ib, m, ctx, m->cbdata_ctx_destroy);
            if (rc != IB_OK) {
                /// @todo Log the error???  Fail???
                ib_log_error(ib,
                    "Failed to call context fini: %s",
                    ib_status_to_string(rc)
                );
            }
        }
    }

    ib_engine_pool_destroy(ib, ctx->mp);

    return;
}

ib_context_t *ib_context_engine(const ib_engine_t *ib)
{
    return ib->ectx;
}

ib_context_t *ib_context_main(const ib_engine_t *ib)
{
    return ib->ctx;
}

ib_engine_t *ib_context_get_engine(const ib_context_t *ctx)
{
    return ctx->ib;
}

ib_mpool_t *ib_context_get_mpool(const ib_context_t *ctx)
{
    return ctx->mp;
}

ib_status_t ib_context_init_cfg(ib_context_t *ctx,
                                void *base,
                                const ib_cfgmap_init_t *init)
{
    ib_status_t rc;

    ib_log_debug3(ctx->ib, "Initializing context %s base=%p",
                  ib_context_full_get(ctx), base);

    if (init == NULL) {
        return IB_OK;
    }

    rc = ib_cfgmap_init(ctx->cfg, base, init);

    return rc;
}

ib_status_t ib_context_module_config(ib_context_t *ctx,
                                     ib_module_t *m,
                                     void *pcfg)
{
    ib_context_data_t *cfgdata;
    ib_status_t rc;

    rc = ib_array_get(ctx->cfgdata, m->idx, (void *)&cfgdata);
    if (rc != IB_OK) {
        *(void **)pcfg = NULL;
        return rc;
    }

    if (cfgdata == NULL) {
        *(void **)pcfg = NULL;
        return IB_EINVAL;
    }

    *(void **)pcfg = cfgdata->data;

    return IB_OK;
}

ib_status_t ib_context_set(ib_context_t *ctx,
                           const char *name,
                           void *val)
{
    ib_status_t rc = ib_cfgmap_set(ctx->cfg, name, val);
    return rc;
}

ib_status_t ib_context_set_num(ib_context_t *ctx,
                               const char *name,
                               ib_num_t val)
{
    ib_status_t rc = ib_cfgmap_set(ctx->cfg, name, ib_ftype_num_in(&val));
    return rc;
}

ib_status_t ib_context_set_string(ib_context_t *ctx,
                                  const char *name,
                                  const char *val)
{
    ib_status_t rc = ib_cfgmap_set(ctx->cfg, name, ib_ftype_nulstr_in(val));
    return rc;
}


ib_status_t ib_context_get(ib_context_t *ctx,
                           const char *name,
                           void *pval, ib_ftype_t *ptype)
{
    ib_status_t rc = ib_cfgmap_get(ctx->cfg, name, pval, ptype);
    return rc;
}

ib_status_t ib_auditlog_write(ib_provider_inst_t *pi)
{
    IB_PROVIDER_API_TYPE(audit) *api;
    ib_status_t rc;

    if (pi == NULL) {
        return IB_EINVAL;
    }

    api = (IB_PROVIDER_API_TYPE(audit) *)pi->pr->api;

    rc = api->write_log(pi);
    return rc;
}
