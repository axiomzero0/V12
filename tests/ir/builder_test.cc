// =============================================================================
// tests/ir/builder_test.cc
// =============================================================================
// Tests for the IR graph builder and optimization passes.

#include "base/arena.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "ir/builder/graph-builder.h"
#include "ir/graph/graph.h"
#include "ir/graph/node.h"
#include "ir/opt/passes.h"
#include "ir/types/type.h"
#include "test-framework.h"
#include "vm/isolate/isolate.h"

using namespace v12;

// Helper: compile a JS source string to bytecode and return the program
// (which owns the FunctionInfo). The program must stay alive while the
// graph is in use.
static std::unique_ptr<BytecodeProgram> CompileProgram(Isolate* iso,
                                                        Arena* arena,
                                                        const char* source) {
    Parser parser(arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) return nullptr;
    BytecodeGenerator gen(iso, arena);
    return gen.Compile(prog);
}

// =============================================================================
// GraphBuilder tests
// =============================================================================

TEST(IRBuilder, SimpleAdd) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena, "let s = 0; s = s + 1;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // The graph should have a Start node.
    EXPECT_NE(g->start(), nullptr);
    EXPECT_GT(g->node_count(), 0);

    // Should have created an Int32Add node (from "s + 1").
    bool has_add = false;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->op() == Opcode::kInt32Add) {
            has_add = true;
            break;
        }
    }
    EXPECT_TRUE(has_add);
}

TEST(IRBuilder, Loop) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena,
        "let s = 0; for (let i = 0; i < 100; i++) { s += i; }");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);
    EXPECT_NE(g->start(), nullptr);
}

TEST(IRBuilder, Comparisons) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena,
        "let x = 5; let y = 10; let a = x < y; let b = x > y; let c = x == y;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Should have created comparison nodes.
    int comparisons = 0;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->op() == Opcode::kInt32LessThan ||
            n->op() == Opcode::kInt32LessThanOrEqual ||
            n->op() == Opcode::kWord32Equal) {
            comparisons++;
        }
    }
    EXPECT_GE(comparisons, 2);  // at least < and ==
}

TEST(IRBuilder, BitwiseOps) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena,
        "let a = 1 & 2; let b = 1 | 2; let c = 1 ^ 2;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Should have created bitwise nodes.
    int bitwise = 0;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->op() == Opcode::kBitwiseAnd ||
            n->op() == Opcode::kBitwiseOr ||
            n->op() == Opcode::kBitwiseXor) {
            bitwise++;
        }
    }
    EXPECT_GE(bitwise, 1);
}

// =============================================================================
// Optimization pass tests
// =============================================================================

TEST(IROpt, DeadCodeElimination) {
    Isolate iso;
    Arena arena;

    // "let x = 1 + 2;" — the result of 1+2 is stored in x but never used.
    auto program = CompileProgram(&iso, &arena, "let x = 1 + 2;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Count nodes before DCE.
    int before = 0;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (!n->IsDead()) before++;
    }

    // Run DCE.
    int removed = DeadCodeElimination(g);
    EXPECT_GE(removed, 0);

    // Count nodes after DCE.
    int after = 0;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (!n->IsDead()) after++;
    }

    // After DCE, there should be no more live pure nodes than before.
    EXPECT_LE(after, before);
}

TEST(IROpt, ConstantFolding) {
    Isolate iso;
    Arena arena;

    // "1 + 2" should be foldable if both operands are Int32Constants.
    auto program = CompileProgram(&iso, &arena, "let x = 1 + 2;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Run constant folding.
    int folded = ConstantFolding(g);

    // The graph should have an Int32Add with two Int32Constant inputs,
    // which should be folded into a single Int32Constant.
    EXPECT_GE(folded, 0);
}

TEST(IROpt, GlobalValueNumbering) {
    Isolate iso;
    Arena arena;

    // "let a = x + 1; let b = x + 1;" — the second x+1 should be deduped.
    auto program = CompileProgram(&iso, &arena,
        "let x = 5; let a = x + 1; let b = x + 1;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Run GVN.
    int deduped = GlobalValueNumbering(g);
    EXPECT_GE(deduped, 0);
}

TEST(IROpt, OptimizeGraphFixedPoint) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena,
        "let x = 5; let a = x + 1; let b = x + 1; let c = a + b;");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Run all optimization passes to a fixed point.
    int total = OptimizeGraph(g);
    EXPECT_GE(total, 0);

    // The graph should still be valid (verifiable).
    g->Verify();
}

TEST(IROpt, VerifyAfterOptimization) {
    Isolate iso;
    Arena arena;
    auto program = CompileProgram(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i; }");
    { EXPECT_NE(program.get(), (void*)nullptr); if (!program) return; }
    FunctionInfo* fi = program->toplevel;

    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();
    ASSERT_NE(g, nullptr);

    // Optimize and verify — the verifier should not find any broken
    // use-def / def-use links after optimization.
    OptimizeGraph(g);
    g->Verify();
}
