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
 * @brief Predicate --- DAG Tests
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 **/

#include "../dag.hpp"
#include "../../ironbeepp/tests/fixture.hpp"

#include "gtest/gtest.h"

using namespace IronBee::Predicate;
using namespace std;

class TestDAG : public ::testing::Test, public IBPPTestFixture
{
};

static const ib_field_t c_field = ib_field_t();

class DummyCall : public Call
{
public:
    virtual string name() const
    {
        return "dummy_call";
    }

protected:
    virtual Value calculate(Context)
    {
        return IronBee::ConstField(&c_field);
    }
};

class DummyCall2 : public DummyCall
{
public:
    virtual string name() const
    {
        return "dummy_call2";
    }
};

TEST_F(TestDAG, Node)
{
    node_p n(new DummyCall);

    EXPECT_EQ("(dummy_call)", n->to_s());
    EXPECT_TRUE(n->children().empty());
    EXPECT_TRUE(n->parents().empty());
    EXPECT_FALSE(n->has_value());
    EXPECT_THROW(n->value(), IronBee::einval);

    EXPECT_EQ(&c_field, n->eval(m_transaction).ib());
    EXPECT_TRUE(n->has_value());

    n->reset();
    EXPECT_FALSE(n->has_value());

    node_p n2(new DummyCall);
    n->add_child(n2);
    EXPECT_EQ(1UL, n->children().size());
    EXPECT_EQ(n2, n->children().front());
    EXPECT_EQ(1UL, n2->parents().size());
    EXPECT_EQ(n, n2->parents().front().lock());
}

TEST_F(TestDAG, String)
{
    String n("node");
    EXPECT_EQ("'node'", n.to_s());
    EXPECT_EQ("node", n.value_as_s());
    EXPECT_TRUE(n.is_static());
    EXPECT_EQ(
        "node",
        n.eval(Context()).value_as_byte_string().to_s()
    );
}

TEST_F(TestDAG, StringEscaping)
{
    EXPECT_EQ("'\\''", String("'").to_s());
    EXPECT_EQ("'foo\\'bar'", String("foo'bar").to_s());
    EXPECT_EQ("'foo\\\\bar'", String("foo\\bar").to_s());
    EXPECT_EQ("'foo\\\\'", String("foo\\").to_s());
}

TEST_F(TestDAG, Call)
{
    node_p n(new DummyCall);

    EXPECT_EQ("(dummy_call)", n->to_s());
    EXPECT_EQ(&c_field, n->eval(m_transaction).ib());
    EXPECT_TRUE(n->has_value());

    node_p a1(new DummyCall);
    n->add_child(a1);
    node_p a2(new String("foo"));
    n->add_child(a2);

    EXPECT_EQ("(dummy_call (dummy_call) 'foo')", n->to_s());
}

TEST_F(TestDAG, OutputOperator)
{
    stringstream s;

    s << DummyCall();

    EXPECT_EQ("(dummy_call)", s.str());
}

TEST_F(TestDAG, Null)
{
    Null n;
    EXPECT_EQ("null", n.to_s());
    EXPECT_TRUE(n.is_static());
    EXPECT_FALSE(n.eval(Context()));
}

TEST_F(TestDAG, DeepCall)
{
    node_p n(new DummyCall);
    node_p n2(new DummyCall);
    node_p n3(new DummyCall);
    node_p n4(new DummyCall);
    n->add_child(n2);
    n2->add_child(n3);
    EXPECT_EQ("(dummy_call (dummy_call (dummy_call)))", n->to_s());
    n3->add_child(n4);
    // Note distance between n and n4.
    EXPECT_EQ("(dummy_call (dummy_call (dummy_call (dummy_call))))", n->to_s());
}

TEST_F(TestDAG, ModifyChildren)
{
    node_p p(new DummyCall);
    node_p c1(new DummyCall);
    node_p c2(new DummyCall2);

    EXPECT_THROW(p->remove_child(c1), IronBee::enoent);
    EXPECT_THROW(p->remove_child(node_p()), IronBee::einval);
    EXPECT_THROW(p->add_child(node_p()), IronBee::einval);
    ASSERT_NO_THROW(p->add_child(c1));
    EXPECT_EQ("(dummy_call (dummy_call))", p->to_s());
    ASSERT_NO_THROW(p->add_child(c2));
    EXPECT_EQ("(dummy_call (dummy_call) (dummy_call2))", p->to_s());
    ASSERT_NO_THROW(p->remove_child(c1));
    EXPECT_EQ("(dummy_call (dummy_call2))", p->to_s());
    EXPECT_THROW(p->replace_child(c1, c2), IronBee::enoent);
    EXPECT_THROW(p->replace_child(c2, node_p()), IronBee::einval);
    EXPECT_THROW(p->replace_child(node_p(), c2), IronBee::einval);
    ASSERT_NO_THROW(p->add_child(c1));
    EXPECT_EQ("(dummy_call (dummy_call2) (dummy_call))", p->to_s());
    ASSERT_NO_THROW(p->replace_child(c2, c1));
    EXPECT_EQ("(dummy_call (dummy_call) (dummy_call))", p->to_s());
    EXPECT_EQ(2UL, c1->parents().size());
    EXPECT_EQ(p, c1->parents().front().lock());
    EXPECT_EQ(p, boost::next(c1->parents().begin())->lock());
    EXPECT_TRUE(c2->parents().empty());
}
