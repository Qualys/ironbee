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
 * @brief IronBee++ --- API Compatibility
 *
 * This file is a compile time assert that the expected IronBee ABI matches
 * the provided IronBee ABI.
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 */

#ifndef __IBPP__ABI_COMPATIBILITY__
#define __IBPP__ABI_COMPATIBILITY__

#include <ironbee/release.h>

#include <boost/static_assert.hpp>

namespace IronBee {

// Update following line when IronBee ABI changes.
#if IB_ABINUM!=201303180
#error "ABI mismatch between IronBee++ and IronBee"
#endif

} // IronBee

#endif
