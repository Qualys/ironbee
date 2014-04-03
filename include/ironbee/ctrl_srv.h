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

#ifndef _IB_CTRL_SRV_H_
#define _IB_CTRL_SRV_H_

/**
 * @file
 * @brief IronBee --- IronBee controller server API
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/engine_manager.h>
#include <ironbee/engine_types.h>
#include <ironbee/release.h>
#include <ironbee/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * Define the controller server handle
 */
typedef struct ib_ctrl_handle_t *ib_ctrl_srv_handle_t;

/**
 * Flush log(s) data
 *
 * @param[in] cbdata Callback data.
 */
typedef void (*ib_ctrl_srv_flush_logs_fn_t)(
    void *cbdata
);

/**
 * Destroy the engine manager
 *
 * @param[in] cbdata Callback data.
 */
typedef void (*ib_ctrl_srv_destroy_manager_fn_t)(
    void *cbdata
);

/**
 * Cause server to exit
 *
 * @param[in] cbdata Callback data.
 */
typedef void (*ib_ctrl_srv_kill_fn_t)(
    void *cbdata
);

/**
 * Controller server hooks
 */
struct ib_ctrl_srv_hooks_t {
    ib_ctrl_srv_flush_logs_fn_t       flush_fn;      /**< Log flush function */
    void                             *flush_data;    /**< Callback data */
    ib_ctrl_srv_destroy_manager_fn_t  destroy_fn;    /**< Manager destroy fn */
    void                             *destroy_data;  /**< Callback data */
    ib_ctrl_srv_kill_fn_t             kill_fn;       /**< Kill server fn */
    void                             *kill_data;     /**< Callback data */
};
typedef struct ib_ctrl_srv_hooks_t ib_ctrl_srv_hooks_t;

/**
 * Command socket settings
 */
struct ib_ctrl_settings_t {
    ib_logger_level_t  log_level;           /**< Log level */
    FILE              *fallback_log_stream; /**< Fallback logging stream */
};
typedef struct ib_ctrl_settings_t ib_ctrl_settings_t;

/**
 * Set the engine manager for the controller
 *
 * @param[in] handle Controller server handle
 * @param[in] manager Engine manager
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_ctrl_srv_set_manager(
    ib_ctrl_srv_handle_t  handle,
    ib_manager_t         *manager
);

/**
 * Set server hooks for the controller
 *
 * @param[in] handle Controller server handle
 * @param[in] hooks Server hooks
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_ctrl_srv_set_hooks(
    ib_ctrl_srv_handle_t        handle,
    const ib_ctrl_srv_hooks_t  *hooks
);

/**
 * Open a controller server handle
 *
 * @param[in] manager Engine manager
 * @param[in] hooks Controller hooks (or NULL)
 * @param[out] phandle Controller server handle
 *
 * @returns Status code:
 * - IB_OK All OK
 * - IB_EINVAL Controller server already open
 * - IB_ENOENT No controller module
 */
ib_status_t DLL_PUBLIC ib_ctrl_srv_open(
    ib_manager_t              *manager,
    const ib_ctrl_srv_hooks_t *hooks,
    ib_ctrl_srv_handle_t      *phandle
);

/**
 * Close a controller server handle
 *
 * @param[in] handle Controller server handle to close
 *
 * @returns Status code:
 * - IB_OK All OK
 * - IB_EINVAL Invalid handle
 */
ib_status_t DLL_PUBLIC ib_ctrl_srv_close(
    ib_ctrl_srv_handle_t handle
);

/**
 * @} IronBeeController
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_CTRL_SRV_H_ */
