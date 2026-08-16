// =============================================================================
// tests/ir/lowering_test.cc
// =============================================================================
// Tests for the lowering pass (Graph → MachineFunction) and x64 target.

#include "base/arena.h"
#include "contracts/machine-ir.h"
#include "contracts/x64-target.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "ir/builder/graph-builder.h"
#include "ir/graph/graph.h"
#include "ir/lowering/lowering.h"
#include "ir/opt/passes.h"
#include "test-framework.h"
#include "vm/isolate/isolate.h"

using namespace v12;

// Helper: compile JS → bytecode → IR → optimized IR.
static Graph* BuildAndOptimizeGraph(Isolate* iso, Arena* arena,
                                     const char* source) {
    Parser parser(arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) return nullptr;
    BytecodeGenerator gen(iso, arena);
    auto program = gen.Compile(prog);
    if (!program) return nullptr;
    FunctionInfo* fi = program->toplevel;
    GraphBuilder builder(arena, fi);
    Graph* g = builder.Build();
    OptimizeGraph(g);
    return g;
}

// =============================================================================
// X64TargetDescription tests
// =============================================================================

TEST(X64Target, BasicProperties) {
    const TargetDescription* target = GetHostTargetDescription();
    EXPECT_EQ(std::string(target->arch_name()), "x64");
    EXPECT_EQ(target->pointer_size(), 8u);
    EXPECT_FALSE(target->is_big_endian());
    EXPECT_EQ(target->num_general_registers(), 16u);
    EXPECT_EQ(target->num_float_registers(), 16u);
}

TEST(X64Target, RegisterNames) {
    const TargetDescription* target = GetHostTargetDescription();
    PhysicalRegister rax{RegKind::kGeneral, 0};
    PhysicalRegister rdi{RegKind::kGeneral, 7};
    PhysicalRegister xmm0{RegKind::kFloat, 0};
    EXPECT_EQ(std::string(target->GeneralRegisterName(rax)), "rax");
    EXPECT_EQ(std::string(target->GeneralRegisterName(rdi)), "rdi");
    EXPECT_EQ(std::string(target->FloatRegisterName(xmm0)), "xmm0");
}

TEST(X64Target, CalleeSaved) {
    const TargetDescription* target = GetHostTargetDescription();
    PhysicalRegister rbx{RegKind::kGeneral, 3};
    PhysicalRegister r12{RegKind::kGeneral, 12};
    PhysicalRegister rax{RegKind::kGeneral, 0};
    EXPECT_TRUE(target->IsCalleeSaved(rbx));
    EXPECT_TRUE(target->IsCalleeSaved(r12));
    EXPECT_FALSE(target->IsCalleeSaved(rax));
    EXPECT_TRUE(target->IsCallerSaved(rax));
    EXPECT_FALSE(target->IsCallerSaved(rbx));
}

TEST(X64Target, ArgumentRegisters) {
    const TargetDescription* target = GetHostTargetDescription();
    const auto& args = target->argument_registers();
    EXPECT_EQ(args.size(), 6u);  // RDI, RSI, RDX, RCX, R8, R9
    // First arg register is RDI (index 7 in x86-64 encoding).
    EXPECT_EQ(args[0].code, 7u);
}

TEST(X64Target, StackAlignment) {
    const TargetDescription* target = GetHostTargetDescription();
    EXPECT_EQ(target->stack_slot_alignment(), 16u);
}

// =============================================================================
// Lowering tests
// =============================================================================

TEST(Lowering, SimpleAdd) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena, "let s = 0; s = s + 1;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_GT(result.lowered_count, 0);
    EXPECT_EQ(result.skipped_count, 0);
    EXPECT_NE(result.function.get(), (void*)nullptr);
    EXPECT_GT(result.function->block_count(), 0u);
}

TEST(Lowering, ArithmeticOps) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena,
        "let a = 1 + 2; let b = a * 3; let c = b - 1; let d = c / 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_GT(result.lowered_count, 0);
}

TEST(Lowering, BitwiseOps) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena,
        "let a = 1 & 2; let b = 1 | 2; let c = 1 ^ 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    EXPECT_TRUE(result.success);
}

TEST(Lowering, Comparisons) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena,
        "let x = 5; let a = x < 10; let b = x == 5;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    EXPECT_TRUE(result.success);
}

TEST(Lowering, VRegAllocation) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena, "let s = 1 + 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    { EXPECT_NE(result.function.get(), (void*)nullptr); }
    // Should have allocated at least some VRegs for constants and results.
    EXPECT_GT(result.function->vreg_count(), 0u);
}

TEST(Lowering, ReversePostOrder) {
    Isolate iso;
    Arena arena;
    Graph* g = BuildAndOptimizeGraph(&iso, &arena, "let s = 1 + 2;");
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);
    { EXPECT_NE(result.function.get(), (void*)nullptr); }

    auto rpo = result.function->ReversePostOrder();
    EXPECT_GT(rpo.size(), 0u);
    // The entry block should be first in RPO.
    EXPECT_EQ(rpo[0]->is_entry, true);
}

TEST(Lowering, FullPipeline) {
    // Full pipeline: source → bytecode → IR → optimize → lower → MachineFunction
    Isolate iso;
    Arena arena;
    const char* src = "let x = 5; let y = x + 3; let z = y * 2;";
    Graph* g = BuildAndOptimizeGraph(&iso, &arena, src);
    { EXPECT_NE(g, (void*)nullptr); if (!g) return; }

    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult result = LowerGraph(g, target, nullptr);

    { EXPECT_NE(result.function.get(), (void*)nullptr); }
    EXPECT_TRUE(result.success);

    // The MachineFunction should have at least 2 blocks (entry + exit).
    // VReg and instruction counts may be small after constant folding.
    EXPECT_GE(result.function->block_count(), 2u);
    EXPECT_GE(result.function->vreg_count(), 1u);

    // Count instructions in the entry block.
    MachineBlock* entry = result.function->EntryBlock();
    { EXPECT_NE(entry, (void*)nullptr); if (!entry) return; }
    EXPECT_GE(entry->instructions.size(), 1u);
}
