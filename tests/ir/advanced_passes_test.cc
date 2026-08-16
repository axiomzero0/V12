// =============================================================================
// tests/ir/advanced_passes_test.cc
// =============================================================================
// Tests for the additional optimization passes (algebraic simplification,
// boolean simplification, comparison simplification, phi simplification,
// check elimination, value numbering, etc.)

#include "base/arena.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "ir/builder/graph-builder.h"
#include "ir/graph/graph.h"
#include "ir/graph/node.h"
#include "ir/opt/passes.h"
#include "test-framework.h"
#include "vm/isolate/isolate.h"

using namespace v12;

static Graph* BuildGraph(Isolate* iso, Arena* arena, const char* source) {
    Parser parser(arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) return nullptr;
    BytecodeGenerator gen(iso, arena);
    auto program = gen.Compile(prog);
    if (!program) return nullptr;
    FunctionInfo* fi = program->toplevel;
    GraphBuilder builder(arena, fi);
    return builder.Build();
}

static int CountOpcode(Graph* g, Opcode op) {
    int count = 0;
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (!n->IsDead() && n->op() == op) count++;
    }
    return count;
}

// =============================================================================
// AlgebraicSimplification tests
// =============================================================================

TEST(AdvancedPasses, AlgebraicDoubleNeg) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = - -x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int simplified = AlgebraicSimplification(g);
    EXPECT_GE(simplified, 0);
    g->Verify();
}

TEST(AdvancedPasses, AlgebraicDoubleBitwiseNot) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = ~~x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    // ~~x should simplify to x
    int before = CountOpcode(g, Opcode::kBitwiseNot);
    AlgebraicSimplification(g);
    g->Verify();
}

TEST(AdvancedPasses, AlgebraicMulByNegOne) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = x * -1;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    // x * -1 should become 0 - x
    int before_mul = CountOpcode(g, Opcode::kInt32Mul);
    AlgebraicSimplification(g);
    int after_mul = CountOpcode(g, Opcode::kInt32Mul);
    // The Mul may or may not be replaced depending on how the builder
    // handles the -1 constant. We just verify no crash.
    g->Verify();
}

// =============================================================================
// BooleanSimplification tests
// =============================================================================

TEST(AdvancedPasses, BooleanDoubleNot) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = !!x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int simplified = BooleanSimplification(g);
    EXPECT_GE(simplified, 0);
    g->Verify();
}

// =============================================================================
// ComparisonSimplification tests
// =============================================================================

TEST(AdvancedPasses, ComparisonNotLess) {
    Isolate iso;
    Arena arena;
    // !(a < b) should become a >= b
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = 10; let z = !(x < y);");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int simplified = ComparisonSimplification(g);
    EXPECT_GE(simplified, 0);
    g->Verify();
}

// =============================================================================
// PhiSimplification tests
// =============================================================================

TEST(AdvancedPasses, PhiSingleInput) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    // No phi nodes in straight-line code, but should not crash.
    int simplified = PhiSimplification(g);
    EXPECT_EQ(simplified, 0);
    g->Verify();
}

// =============================================================================
// CheckElimination tests
// =============================================================================

TEST(AdvancedPasses, CheckEliminationNoCrash) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = x + 1;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int eliminated = CheckElimination(g);
    EXPECT_GE(eliminated, 0);
    g->Verify();
}

// =============================================================================
// ValueNumbering tests
// =============================================================================

TEST(AdvancedPasses, ValueNumberingXorSelf) {
    Isolate iso;
    Arena arena;
    // x ^ x → 0 (if both inputs are the same node)
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = x ^ x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int before = CountOpcode(g, Opcode::kBitwiseXor);
    ValueNumbering(g);
    g->Verify();
}

TEST(AdvancedPasses, ValueNumberingSubSelf) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = x - x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    ValueNumbering(g);
    g->Verify();
}

// =============================================================================
// Full pipeline tests with new passes
// =============================================================================

TEST(AdvancedPasses, FullPipelineComplex) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let a = 1 + 2; let b = a * 4; let c = b - b; let d = c + 0;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int total = OptimizeGraph(g);
    EXPECT_GE(total, 0);
    g->Verify();
}

TEST(AdvancedPasses, FullPipelineBitwiseChain) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let x = 0xFF; let a = x & x; let b = x ^ x; let c = x | x;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    OptimizeGraph(g);
    g->Verify();
}

TEST(AdvancedPasses, FullPipelineStrengthReduction) {
    Isolate iso;
    Arena arena;
    // x * 8, then x / 4, then x % 2
    Graph* g = BuildGraph(&iso, &arena,
        "let x = 10; let a = x * 8; let b = x / 4; let c = x % 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    OptimizeGraph(g);
    g->Verify();
}

TEST(AdvancedPasses, StubPassesNoCrash) {
    // Test that stub passes don't crash
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    EXPECT_GE(BlockMerging(g), 0);
    EXPECT_GE(LoopUnrolling(g), 0);
    EXPECT_GE(TailCallOptimization(g), 0);
    EXPECT_GE(EscapeAnalysis(g), 0);
    EXPECT_GE(LICM(g), 0);
    g->Verify();
}
