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
 * @brief IronBee --- Controller
 *
 * Provide an API for an external command handler module to control IronBee
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <ironbee/context.h>
#include <ironbee/core.h>
#include <ironbee/ctrl_mod.h>
#include <ironbee/ctrl_srv.h>
#include <ironbee/engine_manager.h>
#include "engine_manager_private.h"
#include <ironbee/engine_state.h>
#include <ironbee/log.h>
#include <ironbee/mm_mpool.h>
#include <ironbee/module.h>

#include <assert.h>
#include <errno.h>
#include <memory.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>

/**
 * @defgroup IronBeeController
 * @ingroup IronBee
 *
 * The external command API provides a means for a module to interface with
 * an external process to control IronBee; in particular, the Engine Manager.
 *
 * @note This is not intended for use in production systems!
 *
 * @{
 */

/**
 * Module static data type declaration
 */
typedef struct ctrl_t ctrl_t;

/**
 * Controller module/server object (used for handles)
 */
struct ib_ctrl_handle_t {
    ctrl_t *ctrl;
};
typedef struct ib_ctrl_handle_t ib_ctrl_handle_t;

/**
 * The controller object type
 */
struct ctrl_t {
    ib_manager_t              *manager;   /**< The engine manager */
    const ib_ctrl_srv_hooks_t *hooks;     /**< Server hooks */
    ib_ctrl_handle_t           server;    /**< Controller server object */
    ib_ctrl_handle_t          *pserver;   /**< Pointer to controller server object */
    ib_ctrl_handle_t           module;    /**< Controller module object */
    ib_ctrl_handle_t          *pmodule;   /**< Pointer to controller module object */
};

/**
 * Static controller object
 */
static ctrl_t controller = {
    .manager = NULL,
    .hooks = NULL,
    .server = {
        .ctrl = NULL,
    },
    .pserver = NULL,
    .module = {
        .ctrl = NULL,
    },
    .pmodule = NULL,
};

ib_status_t ib_ctrl_mod_disable_engine(
    ib_ctrl_mod_handle_t handle
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);
    ib_status_t rc;

    rc = ib_manager_disable_engine(handle->ctrl->manager);
    return rc;
}

ib_status_t ib_ctrl_mod_cleanup_engines(
    ib_ctrl_mod_handle_t handle
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);
    ib_status_t rc;

    rc = ib_manager_engine_cleanup(handle->ctrl->manager);
    return rc;
}

ib_status_t ib_ctrl_mod_engine_count(
    ib_ctrl_mod_handle_t  handle,
    size_t               *count
)
{
    assert(handle != NULL);
    assert(count != NULL);

    *count = ib_manager_engine_count(handle->ctrl->manager);
    return IB_OK;
}

ib_status_t ib_ctrl_mod_current_engine(
    ib_ctrl_mod_handle_t   handle,
    ib_engine_t          **pengine
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);
    assert(pengine != NULL);

    *pengine = ib_manager_current_engine(handle->ctrl->manager);
    return IB_OK;
}

ib_status_t ib_ctrl_mod_shutdown(
    ib_ctrl_mod_handle_t handle,
    bool                 exit_process
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);

    if (handle->ctrl->manager == NULL) {
        return IB_ENOENT;
    }

    /* Destroy the engine manager */
    ib_manager_destroy(handle->ctrl->manager);

    /* Kill the server */
    if (exit_process) {
        ib_ctrl_mod_exit(handle);
    }

    /* kill should never return, but this will keep the compiler happy */
    return IB_OK;
}

ib_status_t ib_ctrl_mod_exit(
    ib_ctrl_mod_handle_t handle
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);

    /* Log a message, flush the logs */
    ib_ctrl_mod_flush_logs(handle);

    /* If the server provided a shutdown hook, use it */
    if ( (handle->ctrl->hooks != NULL) &&  (handle->ctrl->hooks->kill_fn != NULL) ) {
        handle->ctrl->hooks->kill_fn(handle->ctrl->hooks->kill_data);
    }

    /* In case no shutdown function, or it failed to shutdown, exit now */
    exit(0);
}

ib_status_t ib_ctrl_mod_flush_logs(
    ib_ctrl_mod_handle_t handle
)
{
    assert(handle != NULL);
    assert(handle->ctrl != NULL);

    if ( (handle->ctrl->hooks != NULL) && (handle->ctrl->hooks->flush_fn != NULL) ) {
        handle->ctrl->hooks->flush_fn(handle->ctrl->hooks->flush_data);
    }
    return IB_OK;
}

ib_status_t ib_ctrl_mod_open(
    ib_ctrl_mod_handle_t *phandle
)
{
    assert(phandle != NULL);

    /* Verify that it's not already open */
    if (controller.pmodule != NULL) {
        return IB_EINVAL;
    }

    /* All OK, fill in everything */
    controller.module.ctrl = &controller;
    controller.pmodule = &controller.module;

    *phandle = controller.pmodule;
    return IB_OK;
}

ib_status_t ib_ctrl_mod_close(
    ib_ctrl_mod_handle_t handle
)
{
    assert(handle != NULL);

    if (handle != controller.pmodule) {
        return IB_EINVAL;
    }
    controller.module.ctrl = NULL;
    controller.pmodule = NULL;

    return IB_OK;
}

ib_status_t ib_ctrl_srv_open(
    ib_manager_t              *manager,
    const ib_ctrl_srv_hooks_t *hooks,
    ib_ctrl_srv_handle_t      *phandle
)
{
    assert(phandle != NULL);
    assert(manager != NULL);

    if (controller.pserver != NULL) {
        return IB_EINVAL;
    }
    if (controller.pmodule == NULL) {
        return IB_ENOENT;
    }
    controller.server.ctrl = &controller;
    controller.pserver = &controller.server;
    controller.manager = manager;
    controller.hooks = hooks;

    *phandle = controller.pserver;
    return IB_OK;
}

ib_status_t ib_ctrl_srv_close(
    ib_ctrl_srv_handle_t handle
)
{
    assert(handle != NULL);

    if (handle != controller.pserver) {
        return IB_EINVAL;
    }
    controller.server.ctrl = NULL;
    controller.pserver = NULL;

    return IB_OK;
}

/**
 * @} IronBeeController
 */
