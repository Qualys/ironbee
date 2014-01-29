//////////////////////////////////////////////////////////////////////////////
// Licensed to Qualys, Inc. (QUALYS) under one or more
// contributor license agreements.  See the NOTICE file distributed with
// this work for additional information regarding copyright ownership.
// QUALYS licenses this file to You under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief IronBee --- Test ib_tx_{get,set}_module_data()
///
/// @author Nick LeRoy <nleroy@qualys.com>
//////////////////////////////////////////////////////////////////////////////
#include "gtest/gtest.h"

#include "base_fixture.h"
#include <ironbee/engine.h>
#include <ironbee/engine_state.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>

#define MODULE_NAME void_module
#define MODULE_NAME_STR IB_XSTRINGIFY(MODULE_NAME)

IB_MODULE_DECLARE();

IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,           /* Default metadata */
    MODULE_NAME_STR,                     /* Module name */
    IB_MODULE_CONFIG_NULL,               /* Global config data */
    NULL,                                /* Configuration field map */
    NULL,                                /* Config directive map */
    NULL,                                /* Initialize function */
    NULL,                                /* Callback data */
    NULL,                                /* Finish function */
    NULL,                                /* Callback data */
);

/**
 * Transaction module data
 */
typedef struct mod_data_t {
    void        *set_ptr;     /**< Pointer pased to ib_tx_set_module_data() */
    ib_status_t  set_rc;      /**< Status from ib_tx_set_module_data() */
    bool         set_called;  /**< Was ib_tx_set_module_data() called? */
    void        *get_ptr;     /**< Pointer from ib_tx_get_module_data() */
    ib_status_t  get_rc;      /**< Status from ib_tx_get_module_data() */
    bool         get_called;  /**< Was ib_tx_get_module_data() called? */
} mod_data_t;

class TxDataTest : public BaseTransactionFixture
{
public:
    ib_status_t register_start(mod_data_t *mod_data) {
        ib_status_t rc;
        rc = ib_hook_tx_register(ib_engine,
                                 tx_started_event,
                                 tx_started,
                                 mod_data);
        return rc;
    }
    ib_status_t register_finish(mod_data_t *mod_data) {
        ib_status_t rc;
        rc = ib_hook_tx_register(ib_engine,
                                 tx_finished_event,
                                 tx_finished,
                                 mod_data);
        return rc;
    }

private:
    static ib_status_t tx_started(
        ib_engine_t           *ib,
        ib_tx_t               *tx,
        ib_state_event_type_t  event,
        void                  *cbdata
    )
    {
        assert(event == tx_started_event);
        mod_data_t *mod_data = (mod_data_t *)cbdata;

        /* Store the set_ptr into the transaction. */
        mod_data->set_called = true;
        mod_data->set_rc = ib_tx_set_module_data(tx,
                                                   IB_MODULE_STRUCT_PTR,
                                                   mod_data->set_ptr);
        return IB_OK;
    }

    static ib_status_t tx_finished(
        ib_engine_t           *ib,
        ib_tx_t               *tx,
        ib_state_event_type_t  event,
        void                  *cbdata
    )
    {
        assert(event == tx_finished_event);
        mod_data_t *mod_data = (mod_data_t *)cbdata;

        /* Retrieve the get_ptr from the transaction. */
        mod_data->get_called = true;
        mod_data->get_rc = ib_tx_get_module_data(tx,
                                                 IB_MODULE_STRUCT_PTR,
                                                 &(mod_data->get_ptr));
        return IB_OK;
    }
};

TEST_F(TxDataTest, set_get_NULL) {
    ib_status_t rc;
    mod_data_t  mod_data = {
        .set_ptr = NULL,
        .set_rc = IB_OK,
        .set_called = false,
        .get_ptr = &rc,
        .get_rc = IB_OK,
        .get_called = false,
    };

    configureIronBee();

    rc = register_start(&mod_data);
    ASSERT_EQ(IB_OK, rc);
    rc = register_finish(&mod_data);
    ASSERT_EQ(IB_OK, rc);

    performTx();
    ASSERT_TRUE(ib_tx);

    ASSERT_TRUE(mod_data.set_called);
    ASSERT_EQ(IB_OK, mod_data.set_rc);
    ASSERT_TRUE(mod_data.get_called);
    ASSERT_EQ(IB_OK, mod_data.get_rc);
    ASSERT_EQ(mod_data.set_ptr, mod_data.get_ptr);
}

TEST_F(TxDataTest, set_get) {
    ib_status_t rc;
    char        data[20];
    mod_data_t  mod_data = {
        .set_ptr = data,
        .set_rc = IB_OK,
        .set_called = false,
        .get_ptr = NULL,
        .get_rc = IB_OK,
        .get_called = false,
    };

    configureIronBee();

    rc = register_start(&mod_data);
    ASSERT_EQ(IB_OK, rc);
    rc = register_finish(&mod_data);
    ASSERT_EQ(IB_OK, rc);

    performTx();
    ASSERT_TRUE(ib_tx);

    ASSERT_TRUE(mod_data.set_called);
    ASSERT_EQ(IB_OK, mod_data.set_rc);
    ASSERT_TRUE(mod_data.get_called);
    ASSERT_EQ(IB_OK, mod_data.get_rc);
    ASSERT_EQ(mod_data.set_ptr, mod_data.get_ptr);
}

TEST_F(TxDataTest, noset_get) {
    ib_status_t rc;
    char        data[20];
    mod_data_t  mod_data = {
        .set_ptr = data,
        .set_rc = IB_OK,
        .set_called = false,
        .get_ptr = NULL,
        .get_rc = IB_OK,
        .get_called = false,
    };

    configureIronBee();

    rc = register_finish(&mod_data);
    ASSERT_EQ(IB_OK, rc);

    performTx();
    ASSERT_TRUE(ib_tx);

    ASSERT_FALSE(mod_data.set_called);
    ASSERT_EQ(IB_ENOENT, mod_data.get_rc);
    ASSERT_TRUE(mod_data.get_called);
    ASSERT_EQ(NULL, mod_data.get_ptr);
}
