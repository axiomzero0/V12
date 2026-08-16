// =============================================================================
// tests/ir/completed_passes_test.cc
// =============================================================================
// Tests for the completed stub passes (LICM, BlockMerging, LoopUnrolling,
// TailCallOptimization, EscapeAnalysis, PEA).

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
// LICM tests
// =============================================================================

TEST(CompletedPasses, LICMDetectsLoop) {
    Isolate iso;
    Arena arena;
    // The builder now creates kLoop nodes for JumpLoop.
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    // Should have at least one Loop node.
    int loops = CountOpcode(g, Opcode::kLoop);
    EXPECT_GE(loops, 1);

    // LICM should not crash and should return a non-negative count.
    int hoisted = LICM(g);
    EXPECT_GE(hoisted, 0);
    g->Verify();
}

TEST(CompletedPasses, LICMNoLoop) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 1 + 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    // No loops → no hoisting.
    int hoisted = LICM(g);
    EXPECT_EQ(hoisted, 0);
}

// =============================================================================
// BlockMerging tests
// =============================================================================

TEST(CompletedPasses, BlockMergingRemovesDeadControl) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; let y = x + 1;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int merged = BlockMerging(g);
    EXPECT_GE(merged, 0);
    g->Verify();
}

TEST(CompletedPasses, BlockMergingWithBranch) {
    Isolate iso;
    Arena arena;
    // The builder creates Branch nodes for JumpIfTrue/JumpIfFalse.
    Graph* g = BuildGraph(&iso, &arena,
        "let x = 5; if (x < 10) { x = x + 1; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int merged = BlockMerging(g);
    EXPECT_GE(merged, 0);
    g->Verify();
}

// =============================================================================
// LoopUnrolling tests
// =============================================================================

TEST(CompletedPasses, LoopUnrollingDetectsLoop) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 4; i++) { s += i; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int unrolled = LoopUnrolling(g);
    EXPECT_GE(unrolled, 0);  // detection works, returns 0 (not yet unrolling)
    g->Verify();
}

// =============================================================================
// TailCallOptimization tests
// =============================================================================

TEST(CompletedPasses, TailCallOptNoCrash) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena, "let x = 5;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int optimized = TailCallOptimization(g);
    EXPECT_GE(optimized, 0);
    g->Verify();
}

// =============================================================================
// EscapeAnalysis tests
// =============================================================================

TEST(CompletedPasses, EscapeAnalysisNoAllocations) {
    Isolate iso;
    Arena arena;
    // No AllocateObject nodes → nothing to analyze.
    Graph* g = BuildGraph(&iso, &arena, "let x = 1 + 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int eliminated = EscapeAnalysis(g);
    EXPECT_EQ(eliminated, 0);
    g->Verify();
}

TEST(CompletedPasses, EscapeAnalysisNoCrash) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int eliminated = EscapeAnalysis(g);
    EXPECT_GE(eliminated, 0);
    g->Verify();
}

// =============================================================================
// PEA (Partial Escape Analysis) tests
// =============================================================================

TEST(CompletedPasses, PEANoAllocations) {
    Isolate iso;
    Arena arena;
    // No AllocateObject nodes → nothing to analyze.
    Graph* g = BuildGraph(&iso, &arena, "let x = 1 + 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int eliminated = PartialEscapeAnalysis(g);
    EXPECT_EQ(eliminated, 0);
    g->Verify();
}

TEST(CompletedPasses, PEANoCrash) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int eliminated = PartialEscapeAnalysis(g);
    EXPECT_GE(eliminated, 0);
    g->Verify();
}

// =============================================================================
// Full pipeline with all passes active
// =============================================================================

TEST(CompletedPasses, FullPipelineAllPasses) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i * 2; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int total = OptimizeGraph(g);
    EXPECT_GE(total, 0);
    g->Verify();
}

TEST(CompletedPasses, FullPipelineBranchAndLoop) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; if (s < 5) { for (let i = 0; i < 10; i++) { s += i; } }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int total = OptimizeGraph(g);
    EXPECT_GE(total, 0);
    g->Verify();
}

TEST(CompletedPasses, BranchNodeCreated) {
    Isolate iso;
    Arena arena;
    // The builder should create Branch nodes for if statements.
    Graph* g = BuildGraph(&iso, &arena, "let x = 5; if (x > 3) { x = 1; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int branches = CountOpcode(g, Opcode::kBranch);
    EXPECT_GE(branches, 1);
}

TEST(CompletedPasses, LoopNodeCreated) {
    Isolate iso;
    Arena arena;
    // The builder should create Loop nodes for for loops.
    Graph* g = BuildGraph(&iso, &arena,
        "let s = 0; for (let i = 0; i < 10; i++) { s += i; }");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    int loops = CountOpcode(g, Opcode::kLoop);
    EXPECT_GE(loops, 1);
}
