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
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/engine_manager.h>
#include "engine_manager_private.h"

#include <ironbee/log.h>
#include <ironbee/core.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/server.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const size_t default_fmt_size = 128;

/**
 * IronBee Engine Manager logger.
 *
 * Performs IronBee logging for the ATS plugin.
 *
 * @param[in] ib IronBee engine (unused)
 * @param[in] level Debug level
 * @param[in] file File name
 * @param[in] line Line number
 * @param[in] fmt Format string
 * @param[in] ap Var args list to match the format
 * @param[in] cbdata Callback data (engine manager handle)
 */
static void ironbee_logger(
    const ib_engine_t *ib,
    ib_log_level_t     level,
    const char        *file,
    int                line,
    const char        *fmt,
    va_list            ap,
    void              *cbdata)
{
    assert(ib != NULL);
    assert(fmt != NULL);
    assert(cbdata != NULL);

    const ib_manager_t *manager = (const ib_manager_t *)cbdata;
    ib_log_level_t     logger_level = manager->logger_level;
    char               fmt_buf_default[default_fmt_size+1];
    char              *fmt_buf = NULL;
    char              *fmt_free = NULL;
    size_t             fmt_buf_size = 0;
    size_t             fmt_required;
    char              *buf = NULL;
    const size_t       buf_size = 8192;

    /* Do nothing if the log level is sufficiently low */
    if (level < logger_level) {
        goto cleanup;
    }

    /* 64 is more than sufficient. */
    fmt_required = strlen(fmt) + 64;
    if (fmt_required > default_fmt_size) {
        fmt_buf = (char *)malloc(fmt_required);
        if (fmt_buf == NULL) {
            goto cleanup;
        }
        fmt_free = fmt_buf;
    }
    else {
        fmt_buf = fmt_buf_default;
    }
    fmt_buf_size = fmt_required;
    snprintf(fmt_buf, fmt_buf_size, "%-10s- ", ib_log_level_to_string(level));

    /* Add the file name and line number if available and log level >= DEBUG */
    if ( (file != NULL) && (line > 0) && (logger_level >= IB_LOG_DEBUG)) {
        size_t              flen;
        static const size_t line_info_size = 35;
        char                line_info[line_info_size];

        while ( (file != NULL) && (strncmp(file, "../", 3) == 0) ) {
            file += 3;
        }
        flen = strlen(file);
        if (flen > 23) {
            file += (flen - 23);
        }

        snprintf(line_info, line_info_size, "(%23s:%-5d) ", file, line);
        strcat(fmt_buf, line_info);
    }
    strcat(fmt_buf, fmt);

    /* If we're using the va_list logger, use it */
    if (manager->vlogger_fn != NULL) {
        manager->vlogger_fn(manager->logger_data, fmt_buf, ap);
        goto cleanup;
    }

    /* Allocate the c buffer */
    if (buf == NULL) {
        buf = malloc(buf_size);
        if (buf == NULL) {
            manager->logger_fn(manager->logger_data,
                               "Failed to allocate message format buffer");
        }
        goto cleanup;
    }

    /* Otherwise, we need to format into a buffer */
    vsnprintf(buf, buf_size, fmt_buf, ap);
    manager->logger_fn(manager->logger_data, buf);

cleanup:
    if (fmt_free != NULL) {
        free(fmt_free);
    }
    if (buf != NULL) {
        free(buf);
    }
}

/**
 * Generic Logger for the engine manager
 *
 * @param[in] manger IronBee engine manager
 * @param[in] level Log level.
 * @param[in] file Filename.
 * @param[in] line Line number.
 * @param[in] fmt Printf-like format string
 */
static void debug_log_ex(
    const ib_manager_t *manager,
    ib_log_level_t      level,
    const char         *file,
    int                 line,
    const char         *fmt,
    ...)
    PRINTF_ATTRIBUTE(5, 6);
static void debug_log_ex(
    const ib_manager_t *manager,
    ib_log_level_t      level,
    const char         *file,
    int                 line,
    const char         *fmt,
    ...)
{
    assert(manager != NULL);

    va_list ap;
    va_start(ap, fmt);

    ironbee_logger(manager->current_engine,
                   level, file, line, fmt, ap,
                   (void *)manager);

    va_end(ap);
}

#define debug_log(manager, lvl, ...) \
    debug_log_ex((manager), (lvl), __FILE__, __LINE__, __VA_ARGS__)

/**
 * Ironbee callback function to for connection finished events
 *
 * @param[in] engine The IronBee engine
 * @param[in] event The event
 * @param[in] conn The connection object
 * @param[in] cbdata Callback data (engine manager pointer)
 *
 * @return OK, or propagated error
 */
static ib_status_t connection_finished_handler(
    ib_engine_t           *engine,
    ib_state_event_type_t  event,
    ib_conn_t             *conn,
    void                  *cbdata)
{
    assert(engine != NULL);
    assert(event = conn_finished_event);
    assert(conn != NULL);
    assert(cbdata != NULL);

    ib_manager_t   *manager = (ib_manager_t *)cbdata;
    uint64_t        connections;
    ib_list_node_t *node;
    bool            found;
    const char     *uuid;

    /* Check the number of connections for the engine */
    uuid = ib_engine_uuid(engine);
    if (uuid == NULL) {
        uuid = "UNKNOWN";
    }
    connections = ib_conn_count(engine);
    debug_log(manager, IB_LOG_DEBUG,
              "engine manager: engine %s (%p): conn closed, count = %"PRIu64,
              uuid, (void *)engine, connections);
    if (connections != 0) {
        return IB_OK;
    }

    /* If it's the current engine, don't destroy it! */
    if (engine == manager->current_engine) {
        debug_log(manager, IB_LOG_DEBUG,
                  "engine manager: Not destroying current engine");
        return IB_OK;
    }

    /* Verify that the engine is in our list */
    found = false;
    IB_LIST_LOOP(manager->engine_list, node) {
        if (node->data == engine) {
            found = true;
            break;
        }
    }

    /* Not in the list??? */
    if (! found) {
        debug_log(manager, IB_LOG_ERROR,
                  "engine manager: engine %s (%p) not in our list!!!",
                  uuid, engine);
        return IB_EUNKNOWN;
    }

    debug_log(manager, IB_LOG_DEBUG,
              "engine manager: Destroying engine %s (%p)", uuid, engine);

    /* After the destroy, the engine's memory pool is also destroyed,
     * so don't use the UUID for logging */

    /* Destroy the engine, remove it from the list */
    IB_LIST_NODE_REMOVE(manager->engine_list, node);
    ib_engine_destroy(engine);

    debug_log(manager, IB_LOG_INFO,
              "engine manager: Destroyed engine %p", engine);

    return IB_OK;
}

ib_status_t ib_manager_create(
    const ib_server_t  *server,
    const char         *config_file,
    ib_manager_t      **pmanager)
{
    assert(server != NULL);
    assert(config_file != NULL);
    assert(pmanager != NULL);

    ib_status_t   rc;
    ib_mpool_t   *mpool;
    ib_manager_t *manager;
    ib_list_t    *engine_list;
    const char   *config_file_copy;

    /* Create our memory pool */
    rc = ib_mpool_create(&mpool, "Engine Manager", NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* Create the manager object */
    manager = ib_mpool_calloc(mpool, sizeof(*manager), 1);
    if (manager == NULL) {
        return IB_EALLOC;
    }

    /* Create the engine list */
    rc = ib_list_create(&engine_list, mpool);
    if (rc != IB_OK) {
        return rc;
    }

    /* Copy the configuration file */
    config_file_copy = ib_mpool_strdup(mpool, config_file);
    if (config_file_copy == NULL) {
        return IB_EALLOC;
    }

    /* Populate the manager object */
    manager->server = server;
    manager->mpool = mpool;
    manager->config_file = config_file_copy;
    manager->logger_level = IB_LOG_WARNING;
    manager->engine_list = engine_list;

    *pmanager = manager;
    return IB_OK;
}

ib_status_t ib_manager_engine_create(
    ib_manager_t       *manager,
    ib_engine_t       **pengine)
{
    assert(manager != NULL);

    ib_status_t          rc;
    ib_status_t          rc2;
    ib_cfgparser_t      *parser = NULL;
    ib_context_t        *ctx;
    ib_engine_t         *engine = NULL;
    ib_manager_engine_t *wrapper;

    /* Create the engine */
    rc = ib_engine_create(&engine, manager->server);
    if (rc != IB_OK) {
        goto cleanup;
    }

    /* Allocate an engine wrapper */
    wrapper = ib_mpool_calloc(ib_engine_pool_main_get(engine),
                              sizeof(*wrapper), 1);
    if (wrapper == NULL) {
        goto cleanup;
    }

    /* Set the logger */
    ib_log_set_logger_fn(engine, ironbee_logger, manager);

    /* This creates the main context */
    rc = ib_cfgparser_create(&parser, engine);
    if (rc != IB_OK) {
        goto cleanup;
    }

    /* Tell the engine about the new parser */
    rc = ib_engine_config_started(engine, parser);
    if (rc != IB_OK) {
        goto cleanup;
    }

    /* Get the main context, set some defaults */
    ctx = ib_context_main(engine);
    ib_context_set_num(ctx, "logger.log_level", (ib_num_t)IB_LOG_WARNING);

    /* Parse the configuration */
    rc = ib_cfgparser_parse(parser, manager->config_file);

    /* Report the status to the engine */
    rc2 = ib_engine_config_finished(engine, rc);
    if ( (rc2 != IB_OK) && (rc == IB_OK) ) {
        rc = rc2;
    }

    /* Adjust our log level */
    if (rc != IB_OK) {
        goto cleanup;
    }
    manager->logger_level = ib_log_get_level(engine);

    /* Store it in the list */
    rc = ib_list_push(manager->engine_list, engine);
    if (rc != IB_OK) {
        goto cleanup;
    }

    /* Register a connection closed handler */
    rc = ib_hook_conn_register(engine, conn_finished_event,
                               connection_finished_handler, manager);
    if (rc != IB_OK) {
        goto cleanup;
    }

    /* Tidy up */
    manager->current_engine = engine;

    /* Fill in the wrapper */
    wrapper->manager   = manager;
    wrapper->engine    = engine;

cleanup:
    if (parser != NULL) {
        ib_cfgparser_destroy(parser);
    }
    if ( (rc != IB_OK) && (engine != NULL) ) {
        ib_engine_destroy(engine);
        engine = NULL;
    }

    /* Hand pointer to the engine off to the server, if required */
    if (pengine != NULL) {
        *pengine = engine;
    }
    return rc;
}

void ib_manager_set_logger_fn(
    ib_manager_t        *manager,
    ib_vlogger_fn_t      vlogger_fn,
    ib_logger_fn_t       logger_fn,
    void                *cbdata)
{
    assert(manager != NULL);
    assert( (vlogger_fn != NULL) || (logger_fn != NULL) );
    assert( (vlogger_fn == NULL) || (logger_fn == NULL) );

    manager->vlogger_fn  = vlogger_fn;
    manager->logger_fn   = logger_fn;
    manager->logger_data = cbdata;
}
