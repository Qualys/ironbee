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

#ifndef _IB_CTRL_MOD_H_
#define _IB_CTRL_MOD_H_

/**
 * @file
 * @brief IronBee --- IronBee controller module API
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/engine_types.h>
#include <ironbee/release.h>
#include <ironbee/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeControllerModule
 * @ingroup IronBeeController
 *
 * The external command API provides a means for a module to interface with
 * an external process to control IronBee; in particular, the Engine Manager.
 *
 * @note This is not intended for use in production systems!
 *
 * @{
 */

/**
 * Define the controller module handle
 */
typedef struct ib_ctrl_handle_t *ib_ctrl_mod_handle_t;

/**
 * Disable the current engine
 *
 * @param[in] handle Controller handle
 *
 * @returns Status code
 */
ib_status_t ib_ctrl_mod_disable_engine(
    ib_ctrl_mod_handle_t handle
);

/**
 * Clean up engines
 *
 * @param[in] handle Controller handle
 *
 * @returns Status code
 */
ib_status_t ib_ctrl_mod_cleanup_engines(
    ib_ctrl_mod_handle_t handle
);

/**
 * Get engine count
 *
 * @param[in] handle Controller handle
 * @param[out] count Engine count
 *
 * @returns Status code
 */
ib_status_t ib_ctrl_mod_engine_count(
    ib_ctrl_mod_handle_t  handle,
    size_t               *count
);

/**
 * Get engine count
 *
 * @param[in] handle Controller handle
 * @param[out] pengine Pointer to current engine
 *
 * @returns Status code
 */
ib_status_t ib_ctrl_mod_current_engine(
    ib_ctrl_mod_handle_t   handle,
    ib_engine_t          **pengine
);

/**
 * Shutdown the manager / server
 *
 * @param[in] handle Controller handle
 * @param[in] exit_process Exit the server process
 *
 * @returns Status code:
 * - IB_ENOENT if no engine manager
 * - IB_OK if all OK
 */
ib_status_t ib_ctrl_mod_shutdown(
    ib_ctrl_mod_handle_t handle,
    bool                 exit_process
);

/**
 * Cause the server to exit
 *
 * @param[in] handle Controller handle
 *
 * @returns Status code:
 * - IB_OK if all OK
 */
ib_status_t ib_ctrl_mod_exit(
    ib_ctrl_mod_handle_t handle
);

/**
 * Flush logs
 *
 * @param[in] handle Controller handle
 *
 * @returns Status code:
 * - IB_OK if all OK
 */
ib_status_t ib_ctrl_mod_flush_logs(
    ib_ctrl_mod_handle_t handle
);

/**
 * Open a controller module handle
 *
 * @param[out] phandle Controller module handle
 *
 * @returns Status code:
 * - IB_OK All OK
 * - IB_EINVAL Controller module already open
 */
ib_status_t DLL_PUBLIC ib_ctrl_mod_open(
    ib_ctrl_mod_handle_t *phandle
);

/**
 * Close a controller module handle
 *
 * @param[in] handle Controller module handle to close
 *
 * @returns Status code:
 * - IB_OK All OK
 * - IB_EINVAL Invalid handle
 */
ib_status_t DLL_PUBLIC ib_ctrl_mod_close(
    ib_ctrl_mod_handle_t handle
);

/**
 * @} IronBeeControllerModule
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_CTRL_MOD_H_ */
