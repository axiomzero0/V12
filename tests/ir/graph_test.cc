// =============================================================================
// tests/ir/graph_test.cc
// =============================================================================

#include "tests/test-framework.h"

#include "base/arena.h"
#include "ir/graph/graph.h"
#include "ir/graph/node.h"
#include "ir/types/type.h"

using namespace v12;
using namespace v12_test;

TEST(IR, CreateNode) {
    Arena a;
    Graph g(&a);
    Node* start = g.NewNode(Opcode::kStart, NodeProp::kControl, Type::None(),
                            nullptr, nullptr, {});
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(start->op(), Opcode::kStart);
    g.set_start(start);
    EXPECT_EQ(g.node_count(), 1);
}

TEST(IR, NodeInputs) {
    Arena a;
    Graph g(&a);
    Node* start = g.NewNode(Opcode::kStart, NodeProp::kControl, Type::None(),
                            nullptr, nullptr, {});
    Node* c1 = g.NewNode(Opcode::kInt32Constant, NodeProp::kPure, Type::Int32(),
                         nullptr, nullptr, {});
    Node* c2 = g.NewNode(Opcode::kInt32Constant, NodeProp::kPure, Type::Int32(),
                         nullptr, nullptr, {});
    Node::Inputs ins;
    ins.push_back(c1);
    ins.push_back(c2);
    Node* add = g.NewNode(Opcode::kInt32Add, NodeProp::kPure, Type::Int32(),
                          nullptr, nullptr, std::move(ins));

    ASSERT_EQ(add->input_count(), 2);
    EXPECT_EQ(add->input(0), c1);
    EXPECT_EQ(add->input(1), c2);
    EXPECT_EQ(c1->use_count(), 1);
    EXPECT_EQ(c2->use_count(), 1);
}

TEST(IR, ReplaceAllUses) {
    Arena a;
    Graph g(&a);
    Node* c1 = g.NewNode(Opcode::kInt32Constant, NodeProp::kPure, Type::Int32(),
                         nullptr, nullptr, {});
    Node* c2 = g.NewNode(Opcode::kInt32Constant, NodeProp::kPure, Type::Int32(),
                         nullptr, nullptr, {});
    Node::Inputs ins1;
    ins1.push_back(c1);
    Node* add1 = g.NewNode(Opcode::kInt32Add, NodeProp::kPure, Type::Int32(),
                           nullptr, nullptr, std::move(ins1));
    Node::Inputs ins2;
    ins2.push_back(c1);
    Node* add2 = g.NewNode(Opcode::kInt32Add, NodeProp::kPure, Type::Int32(),
                           nullptr, nullptr, std::move(ins2));

    EXPECT_EQ(c1->use_count(), 2);
    c1->ReplaceAllUsesWith(c2);
    EXPECT_EQ(c1->use_count(), 0);
    EXPECT_EQ(c2->use_count(), 2);
    EXPECT_EQ(add1->input(0), c2);
    EXPECT_EQ(add2->input(0), c2);
}

TEST(IR, KillNode) {
    Arena a;
    Graph g(&a);
    Node* c = g.NewNode(Opcode::kInt32Constant, NodeProp::kPure, Type::Int32(),
                        nullptr, nullptr, {});
    Node::Inputs ins;
    ins.push_back(c);
    Node* add = g.NewNode(Opcode::kInt32Add, NodeProp::kPure, Type::Int32(),
                          nullptr, nullptr, std::move(ins));
    EXPECT_EQ(c->use_count(), 1);
    add->Kill();
    EXPECT_TRUE(add->IsDead());
    EXPECT_EQ(c->use_count(), 0);
}

TEST(IR, Verify) {
    Arena a;
    Graph g(&a);
    Node* start = g.NewNode(Opcode::kStart, NodeProp::kControl, Type::None(),
                            nullptr, nullptr, {});
    g.set_start(start);
    g.Verify();   // should not crash
}

TEST(IR, TypeSystem) {
    Type t1 = Type::Int32();
    Type t2 = Type::Float64();
    EXPECT_TRUE(t1.Is(Type::Number()));
    EXPECT_FALSE(t1.Is(t2));
    EXPECT_TRUE(t1.Maybe(Type::Number()));
    EXPECT_EQ(t1.Union(t2).bits, type_bits::kInt32 | type_bits::kFloat64);
    EXPECT_EQ(t1.Intersect(t2).bits, 0u);
}
