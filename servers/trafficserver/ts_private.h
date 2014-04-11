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

#ifndef _IBTS_PRIVATE_H_
#define _IBTS_PRIVATE_H_

/**
 * @file
 * @brief IronBee --- Engine manager private API definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/engine_manager.h>
#include <ts/ts.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeTrafficServerPrivate Apache Traffic Server private definitions.
 * @ingroup IronBeeTrafficServer
 *
 * Provides private definitions for the IronBee Apache Traffic Server plugin.
 *
 * @{
 */

/**
 * Plugin global data
 */
struct ibts_module_data_t {
    bool                     ib_initialized; /**< Is IronBee initialized? */
    TSTextLogObject          logger;         /**< TrafficServer log object */
    ib_manager_t            *manager;        /**< IronBee engine manager object */
    size_t                   max_engines;    /**< Max # of simultaneous engines */
    const char              *config_file;    /**< IronBee configuration file */
    const char              *log_file;       /**< IronBee log file */
    int                      log_level;      /**< IronBee log level */
    bool                     log_disable;    /**< Disable logging? */
    const char              *txlogfile;
    TSTextLogObject          txlogger;
};
typedef struct ibts_module_data_t ibts_module_data_t;

/**
 * Structure of data provided for the controller plugin
 */
struct ibts_shared_data_t {
    ibts_module_data_t  *module_data;   /**< Module data */
};
typedef struct ibts_shared_data_t ibts_shared_data_t;

/**
 * Name of the symbol to use to access module data
 */
#define IBTS_XSTR(s) IBTS_STR(s)
#define IBTS_STR(s) #s
#define IBTS_SHARED_DATA ibts_shared_data
#define IBTS_SHARED_STR  IBTS_STR(ibts_shared_data)

/**
 * @} IronBeeTrafficServerPrivate
 */

#ifdef __cplusplus
}
#endif

#endif /* _IBTS_PRIVATE_H_ */
