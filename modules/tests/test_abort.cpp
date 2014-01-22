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
/// @brief IronBee --- Abort module tests
///
/// @author Nick LeRoy <nleroy@qualys.com>
//////////////////////////////////////////////////////////////////////////////
#include "gtest/gtest.h"

#include "base_fixture.h"
#include <ironbee/operator.h>
#include <ironbee/hash.h>
#include <ironbee/mpool.h>
#include <ironbee/field.h>
#include <ironbee/bytestr.h>

#include <signal.h>

#include <boost/filesystem.hpp>

class AbortTest :
    public BaseTransactionFixture
{
public:
    void SetUp()
    {
        ib_status_t rc;

        BaseTransactionFixture::SetUp();
        rc = ib_operator_create_and_register(
            NULL,
            ib_engine,
            "Error",
            ( IB_OP_CAPABILITY_ALLOW_NULL |
              IB_OP_CAPABILITY_NON_STREAM |
              IB_OP_CAPABILITY_STREAM ),
            NULL, NULL,
            NULL, NULL,
            op_error_execute, NULL);
        if (rc != IB_OK) {
            throw std::runtime_error("Failed to register the Error operator:" +
                                     std::string(ib_status_to_string(rc)));
        }

        /* Register the OK action */
        rc = ib_action_register(ib_engine,
                                "OK",
                                NULL, NULL,
                                NULL, NULL,
                                act_ok_execute, NULL);
        if (rc != IB_OK) {
            throw std::runtime_error("Failed to register the OK action :" +
                                     std::string(ib_status_to_string(rc)));
        }

        /* Register the Error action */
        rc = ib_action_register(ib_engine,
                                "Error",
                                NULL, NULL,
                                NULL, NULL,
                                act_error_execute, NULL);
        if (rc != IB_OK) {
            throw std::runtime_error("Failed to register the Error action:" +
                                     std::string(ib_status_to_string(rc)));
        }
    }

    virtual void sendRequestLine()
    {
        BaseTransactionFixture::sendRequestLine("GET", "/foo/bar", "HTTP/1.1");
    }

private:
    static ib_status_t op_error_execute(
        ib_tx_t          *tx,
        void             *instance_data,
        const ib_field_t *field,
        ib_field_t       *capture,
        ib_num_t         *result,
        void             *cbdata)
    {
        *result = 0;
        return IB_EINVAL;
    }

    static ib_status_t act_ok_execute(
        const ib_rule_exec_t *rule_exec,
        void                 *data,
        void                 *cbdata)
    {
        return IB_OK;
    }

    static ib_status_t act_error_execute(
        const ib_rule_exec_t *rule_exec,
        void                 *data,
        void                 *cbdata)
    {
        return IB_EINVAL;
    }

};

class AbortDeathTest : public AbortTest
{
public:
    AbortDeathTest() : AbortTest()
    {
        struct sigaction act;
        int rc;
        rc = sigaction(SIGABRT, NULL, &act);
        if (rc) {
            throw std::runtime_error("sigaction(SIGABORT, NULL, &act) failed:" + rc);
        }

        act.sa_handler = AbortHandler;
        act.sa_flags |= SA_RESETHAND;
        rc = sigaction(SIGABRT, &act, NULL);
        if (rc) {
            throw std::runtime_error("sigaction(SIGABORT, &act, NULL) failed:" + rc);
        }
    }

private:
    static void AbortHandler(int signum)
    {
        fprintf(stderr, "ABORTED\n");
        raise(SIGABRT);
    }
};

TEST_F(AbortTest, Load) {
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());
    performTx();
    ASSERT_TRUE(ib_tx);
}

TEST_F(AbortDeathTest, NoAbort) {
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:NoAbort phase:REQUEST_HEADER\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());
    performTx();
}

/*
 * Tests of the Abort action.
 */
TEST_F(AbortDeathTest, Abort) {
    std::string name = std::string("Abort");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER Abort:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Abort should always abort */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, FalseAbort) {
    std::string name = std::string("FalseAbort");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER Abort:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Abort should always abort */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, NotAbort) {
    std::string name = std::string("NotAbort");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER !Abort:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Abort negated by "!" */
    performTx();
}

/*
 * Tests of the AbortIf:OpTrue action.
 */
TEST_F(AbortDeathTest, AbortIf_OpTrue_OpTrue) {
    std::string name = std::string("AbortIf_OpTrue_OpTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpTrue:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, AbortIf matched */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, AbortIf_OpTrue_OpFalse) {
    std::string name = std::string("AbortIf_OpTrue_OpFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpTrue:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, AbortIf not matched */
    performTx();
}
TEST_F(AbortDeathTest, AbortIf_OpTrue_OpNotTrue) {
    std::string name = std::string("AbortIf_OpTrue_OpNotTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x !@True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpTrue:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, negated, AbortIf shouldn't match */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_OpTrue_OpNotFalse) {
    std::string name = std::string("AbortIf_OpTrue_OpNotFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x !@False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpTrue:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, inverted, AbortIf match */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

/*
 * Tests of the AbortIf:OpFalse action.
 */
TEST_F(AbortDeathTest, AbortIf_OpFalse_OpFalse) {
    std::string name = std::string("AbortIf_OpFalse_OpFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFalse:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, AbortIf matched */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, AbortIf_OpFalse_OpTrue) {
    std::string name = std::string("AbortIf_OpFalse_OpTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFalse:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, AbortIf not matched */
    performTx();
}
TEST_F(AbortDeathTest, AbortIf_OpFalse_OpNotFalse) {
    std::string name = std::string("AbortIf_OpFalse_OpFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x !@False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFalse:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, inverted, AbortIf not matched */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_OpFalse_OpNotTrue) {
    std::string name = std::string("AbortIf_OpFalse_OpNotTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x !@True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFalse:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, inverted, AbortIf matched */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}


/*
 * Tests of the AbortIf:OpOk action.
 */
TEST_F(AbortDeathTest, AbortIf_OpOkTrue) {
    std::string name = std::string("AbortIf_OpOkTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator result irrelevant, AbortIf fired */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, AbortIf_OpOkFalse) {
    std::string name = std::string("AbortIf_OpOkFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator result irrelevant, AbortIf fired */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, AbortIf_OpOkError) {
    std::string name = std::string("AbortIf_OpOkError");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned error, AbortIf didn't fire */
    performTx();
}

/*
 * Tests of the AbortIf:OpFail action.
 */
TEST_F(AbortDeathTest, AbortIf_OpFailTrue) {
    std::string name = std::string("AbortIf_OpFailTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator result irrelevant, AbortIf didn't fire */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_OpFailFalse) {
    std::string name = std::string("AbortIf_OpFailFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator result irrelevant, AbortIf didn't fire */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_OpFailError) {
    std::string name = std::string("AbortIf_OpFailError");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER AbortIf:OpFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned error, AbortIf fired */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

/*
 * Tests of the AbortIf:ActOk action.
 */
TEST_F(AbortDeathTest, AbortIf_ActOkTrue) {
    std::string name = std::string("AbortIf_ActOkTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, no actions failed */
    EXPECT_EXIT(performTx(), ::testing::KilledBySignal(SIGABRT), ".*ABORTED.*");
}

TEST_F(AbortDeathTest, AbortIf_ActOkFalse) {
    std::string name = std::string("AbortIf_ActOkFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, no actions executed */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActOkError) {
    std::string name = std::string("AbortIf_ActOkError");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER Error AbortIf:ActOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, Error action failed */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActOkOpError) {
    std::string name = std::string("AbortIf_ActOpError");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActOk:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator failed, no actions executed */
    performTx();
}

/*
 * Tests of the AbortIf:ActFail action.
 */
TEST_F(AbortDeathTest, AbortIf_ActFailTrue) {
    std::string name = std::string("AbortIf_ActFailTrue");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @True 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned True, no actions failed */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActFailFalse) {
    std::string name = std::string("AbortIf_ActFailFalse");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @False 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator returned False, no actions failed */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActFailError) {
    std::string name = std::string("AbortIf_ActFailError");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER Error AbortIf:ActFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator failed, no actions executed. */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActOkOpError1) {
    std::string name = std::string("AbortIf_ActOpError1");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER OK AbortIf:ActFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator failed, not actions executed */
    performTx();
}

TEST_F(AbortDeathTest, AbortIf_ActOkOpError2) {
    std::string name = std::string("AbortIf_ActOpError2");
    std::string config =
        std::string(
            "LogLevel INFO\n"
            "RuleEngineLogData all\n"
            "LoadModule \"ibmod_rules.so\"\n"
            "LoadModule \"ibmod_testops.so\"\n"
            "LoadModule \"ibmod_abort.so\"\n"
            "SensorId B9C1B52B-C24A-4309-B9F9-0EF4CD577A3E\n"
            "SensorName UnitTesting\n"
            "SensorHostname unit-testing.sensor.tld\n"
            "<Site test-site>\n"
            "   SiteId AAAABBBB-1111-2222-3333-000000000000\n"
            "   Hostname *\n"
            "   Rule x @Error 1 ID:"+name+" phase:REQUEST_HEADER Error AbortIf:ActFail:"+name+"\n"
            "</Site>\n"
        );

    configureIronBeeByString(config.c_str());

    /* Operator failed: No actions executed */
    performTx();
}
