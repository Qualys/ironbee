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

/**
 * The engine manager.
 *
 * An engine manager is created via ib_manager_create().
 *
 * Servers which use the engine manager will typically create a single engine
 * manager at startup, and then use the engine manager to create engines when
 * the configuration has changed via ib_manager_engine_create().
 * 
 * The engine manager will then manage the IronBee engines, with the most
 * recent one successfully created being the "current" engine @sa
 * ib_manager_engine_current().  The manager will automatically destroy "old"
 * engines when the engine's last connection is finished.
 */
typedef struct ib_manager_t ib_manager_t;

/**
 * Engine manager logger callback (va_list version).
 *
 * This is the preferred logger.  If this function is provided, the engine
 * manager's IronBee logger will not format the log message, but will
 * instead pass the format (@a fmt) and args (@a ap) directly to the
 * logger function.
 *
 * @param cbdata Callback data
 * @param fmt Formatting string
 * @param ap Variable args list
 */
typedef void (*ib_vlogger_fn_t)(
    void               *cbdata,
    const char         *fmt,
    va_list             ap)
    VPRINTF_ATTRIBUTE(2);

/**
 * Engine manager logger callback (formatted buffer version).
 *
 * This logger is provided for servers that don't provide a va_list logging
 * facility (ATS).  If this function is provided, the engine manager will
 * do all of the formatting, and will then call the logger with a formatted
 * buffer.
 *
 * @param cbdata Callback data
 * @param buf Formatted buffer
 */
typedef void (*ib_logger_fn_t)(
    void               *cbdata,
    const char         *buf);

/**
 * Create an engine manager.
 *
 * @param[in] server Information on the server instantiating the engine manager
 * @param[in] config_file Configuration file path
 * @param[out] pmanager Pointer to IronBee engine manager object
 *
 * @returns Status code
 * - IB_OK if all OK
 * - IB_EALLOC for allocation problems
 */
ib_status_t DLL_PUBLIC ib_manager_create(
    const ib_server_t  *server,
    const char         *config_file,
    ib_manager_t      **pmanager);

/**
 * Destroy an engine manager.
 *
 * @param[in,out] manager IronBee engine manager
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_manager_destroy(
    ib_manager_t       *manager);

/**
 * Create a new IronBee engine
 *
 * When an engine is successfully, the engine manager will automatically
 * register a (private) logger with the engine.  Note that the engine
 * manager will automatically destroy old engines when their final
 * connection is closed.
 *
 * @param[in,out] manager IronBee engine manager
 * @param[out] pengine Pointer to the new engine (or NULL)
 *
 * @returns Status code
 */
ib_status_t DLL_PUBLIC ib_manager_engine_create(
    ib_manager_t        *manager,
    ib_engine_t        **pengine);

/**
 * Get the current IronBee engine
 *
 * @param[in] manager IronBee engine manager
 *
 * @returns Pointer to the current engine
 */
ib_engine_t DLL_PUBLIC *ib_manager_engine_current(
    ib_manager_t        *manager);

/**
 * Set logger callback.
 *
 * Specify one of @a logger_va or @a logger_buf, but not both.  This
 * function will assert() if both are NULL or both are non-NULL.
 * As noted above, the @a vlogger_fn is the preferred logger.
 *
 * @param manager IronBee engine manager.
 * @param vlogger_fn Logger function (va_list version).
 * @param logger_fn Logger function (Formatted buffer version).
 * @param cbdata Data to pass to logger function.
 */
void DLL_PUBLIC ib_manager_set_logger_fn(
    ib_manager_t        *manager,
    ib_vlogger_fn_t      vlogger_fn,
    ib_logger_fn_t       logger_fn,
    void                *cbdata);


/** @} */

#ifdef __cplusplus
}
#endif

#endif /* _IB_ENGINE_MANAGER_H_ */
