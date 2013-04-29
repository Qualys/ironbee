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

#ifndef _IB_ENGINE_MANAGER_PRIVATE_H_
#define _IB_ENGINE_MANAGER_PRIVATE_H_

/**
 * @file
 * @brief IronBee --- Engine Manager private definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */
#include <ironbee/engine_manager.h>

#include <ironbee/engine.h>
#include <ironbee/engine_types.h>
#include <ironbee/list.h>
#include <ironbee/log.h>
#include <ironbee/mpool.h>
#include <ironbee/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
* @defgroup IronBeeEngineManagerPrivate Engine Manager Private Data
* @ingroup IronBeeEngineManager
*
* @{
*/

/**
 * The Engine Manager.
 *
 * This is the basic "meta-engine" to handle multiple IronBee engines.
 */
struct ib_manager_t {
    const ib_server_t *server;          /**< Server object */
    ib_mpool_t        *mpool;           /**< Engine Manager's Memory pool */
    const char        *config_file;     /**< IronBee configuration file */
    ib_engine_t       *current_engine;  /**< Current IronBee engine */
    ib_list_t         *engine_list;     /**< List of active engines */

    /* Logging */
    ib_log_level_t     logger_level;    /**< Log level to use */
    ib_vlogger_fn_t    vlogger_fn;      /**< va_list logger function */
    ib_logger_fn_t     logger_fn;       /**< Buffer logger function */
    void              *logger_data;     /**< Logger callback data */
};

/**
 * The Engine Manager engine wrapper.
 *
 * There is one wrapper per engine instance.
 *
 * @todo Is this required?
 */
struct ib_manager_engine_t {
    ib_manager_t      *manager;         /**< The engine manager */
    ib_engine_t       *engine;          /**< The IronBee engine */
};
typedef struct ib_manager_engine_t ib_manager_engine_t;


/** @} */

#ifdef __cplusplus
}
#endif

#endif /* _IB_ENGINE_MANAGER_PRIVATE_H_ */
