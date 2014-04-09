
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
/// @brief IronBee --- Engine Test Functions
///
/// @author Brian Rectanus <brectanus@qualys.com>
//////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include "base_fixture.h"

#include <ironbee/engine_manager.h>
#include <ironbee/engine_manager_control_channel.h>

#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

class EngMgrCtrlChanTest : public BaseFixture {

protected:
    ib_manager_t *ib_manager;

public:
    void SetUp() {
        BaseFixture::SetUp();
        ASSERT_EQ(
            IB_OK,
            ib_manager_create(
                &ib_manager,
                &ibt_ibserver,
                10
            )
        );
    }

    virtual void TearDown() {
        ib_manager_destroy(ib_manager);
        BaseFixture::TearDown();
    }
};

TEST_F(EngMgrCtrlChanTest, init) {
    ib_engine_manager_control_channel_t *channel;

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_create(
            &channel,
            MainMM(),
            ib_manager
        )
    );
}

TEST_F(EngMgrCtrlChanTest, socket_path) {
    ib_engine_manager_control_channel_t *channel;

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_create(
            &channel,
            MainMM(),
            ib_manager
        )
    );

    /* Check the default. */
    ASSERT_STREQ(
        "/var/run/ironbee_manager_controller.sock",
        ib_engine_manager_control_channel_socket_path_get(channel)
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_socket_path_set(channel, "path")
    );

    /* Check the custom one. */
    ASSERT_STREQ(
        "path",
        ib_engine_manager_control_channel_socket_path_get(channel)
    );
}

TEST_F(EngMgrCtrlChanTest, start_stop) {
    ib_engine_manager_control_channel_t *channel;

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_create(
            &channel,
            MainMM(),
            ib_manager
        )
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_socket_path_set(channel, "./tmp.sock")
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_start(channel)
    );

    /* Check that the file exists and is not a normal file or dir. */
    ASSERT_TRUE(boost::filesystem::is_other("./tmp.sock"));

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_stop(channel)
    );

    ASSERT_FALSE(boost::filesystem::exists("./tmp.sock"));
}

TEST_F(EngMgrCtrlChanTest, send_echo) {
    ib_engine_manager_control_channel_t *channel;
    const char *response;

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_create(
            &channel,
            MainMM(),
            ib_manager
        )
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_socket_path_set(channel, "./tmp.sock")
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_echo_register(channel)
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_start(channel)
    );

    ASSERT_TRUE(boost::filesystem::is_other("./tmp.sock"));

    boost::function<ib_status_t(void)> f =
        boost::bind(
            ib_engine_manager_control_send,
            "./tmp.sock",
            "echo hi, how are you?",
            MainMM(),
            &response
        );
    boost::packaged_task<ib_status_t> pt(f);
    boost::shared_future<ib_status_t> fut = pt.get_future().share();
    boost::thread thr(boost::move(pt));

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_recv(
            channel
        )
    );

    thr.join();
    ASSERT_EQ(IB_OK, fut.get());

    ASSERT_STREQ(
        "hi, how are you?",
        response
    );

    ASSERT_EQ(
        IB_OK,
        ib_engine_manager_control_channel_stop(channel)
    );

    ASSERT_FALSE(boost::filesystem::exists("./tmp.sock"));
}