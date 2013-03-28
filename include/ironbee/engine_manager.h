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

#ifndef _IB_ENGINE_MANAGER_H_
#define _IB_ENGINE_MANAGER_H_

/**
 * @file
 * @brief IronBee --- Engine Manager definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/engine.h>
#include <ironbee/engine_types.h>
#include <ironbee/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
* @defgroup IronBeeEngineManager Engine Manager
* @ingroup IronBee
*
* The engine manager provides services to manage multiple IronBee engines.
* Currently, all of these engines run in the same process space.
*
* @{
*/

/** Engine Manager type declarations */
typedef struct ib_engmgr_manager_t ib_engmgr_manager_t;

/**
 * Engine manager logger callback (va_list version).
 *
 * @param cbdata Callback data
 * @param fmt Formatting string
 * @param ap Variable args list
 */
typedef void (*ib_engmgr_logger_va_fn_t)(
    void                      *cbdata,
    const char                *fmt,
    va_list                    ap)
    VPRINTF_ATTRIBUTE(2);

/**
 * Engine manager logger callback (formatted buffer version).
 *
 * @param cbdata Callback data
 * @param buf Formatted buffer
 */
typedef void (*ib_engmgr_logger_buf_fn_t)(
    void                      *cbdata,
    const char                *buf);

/**
 * Create an engine manager.
 *
 * @param[in] server Information on the server instantiating the engine manager
 * @param[out] pmanager Pointer to IronBee engine manager object
 *
 * @returns Status code
 * - IB_OK if all OK
 * - IB_EALLOC for allocation problems
 */
ib_status_t DLL_PUBLIC ib_engmgr_manager_create(
    const ib_server_t         *server,
    ib_engmgr_manager_t      **pmanager);

/**
 * Destroy an engine manager
 *
 * @param[in,out] manager IronBee engine manager
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_engmgr_manager_destroy(
    ib_engmgr_manager_t       *manager);

/**
 * Create a new IronBee engine
 *
 * @param[in,out] manager IronBee engine manager
 * @param[out] pengine Pointer to the new engine
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_engmgr_create_engine(
    ib_engmgr_manager_t        *manager,
    ib_engine_t               **pengine);

/**
 * Get the current IronBee engine
 *
 * @param[in] manager IronBee engine manager
 *
 * @returns Pointer to the current engine
 */
ib_engine_t DLL_PUBLIC *ib_engmgr_current_engine(
    ib_engmgr_manager_t        *manager);

/**
 * Get count of engines
 *
 * @param[in] manager IronBee engine manager
 * @param[out] pcount Count of engines managed by @a manager
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_engmgr_engine_count(
    const ib_engmgr_manager_t *manager,
    size_t                    *pcount);

/**
 * Set logger callback.
 *
 * Specify one of @a logger_va or @a logger_buf, but not both.  This
 * function will assert() if both are NULL or both are non-NULL.
 *
 * @param manager IronBee engine manager.
 * @param va_logger Logger function (va_list version).
 * @param buf_logger Logger function (Formatted buffer version).
 * @param cbdata Data to pass to logger function.
 */
void DLL_PUBLIC ib_engmgr_set_logger_fn(
    ib_engmgr_manager_t        *manager,
    ib_engmgr_logger_va_fn_t    logger_va,
    ib_engmgr_logger_buf_fn_t   logger_buf,
    void                       *cbdata);


/** @} */

#ifdef __cplusplus
}
#endif

#endif /* _IB_ENGINE_MANAGER_H_ */
