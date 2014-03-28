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

#ifndef _IB_ENGINE_MANAGER_PRIVATE_API_H_
#define _IB_ENGINE_MANAGER_PRIVATE_API_H_

/**
 * @file
 * @brief IronBee --- Engine manager private API definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/engine_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeEngineManagerPrivate
 * @ingroup IronBeeEngineManager
 *
 * This provides functions to support the command socket.
 *
 * @note This is not intended for use in production systems!
 *
 * @{
 */

/**
 * Get the count of IronBee engines.
 *
 * @param[in] manager IronBee engine manager 
 *
 * @returns Count of total IronBee engines
 */
size_t DLL_PUBLIC ib_manager_engine_count(
    const ib_manager_t *manager
);

/**
 * Disable the current engine.
 *
 * @param[in] manager IronBee engine manager 
 *
 * @returns
 * - IB_OK On success.
 */
ib_status_t DLL_PUBLIC ib_manager_disable_engine(
    ib_manager_t *manager
);

/**
 * Get handle of current engine
 *
 * @param[in] manager IronBee engine manager 
 *
 * @returns
 * - Current engine or NULL
 */
ib_engine_t DLL_PUBLIC *ib_manager_current_engine(
    const ib_manager_t *manager
);

/**
 * @} IronBeeEngineManagerPrivate
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_ENGINE_MANAGER_PRIVATE_H_ */
