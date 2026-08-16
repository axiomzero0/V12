// =============================================================================
// tools/ir-dump/ir-dump.cc
// =============================================================================
// Dump the IR for a JS source file.
//
// Usage: ir-dump <file.js>
//        ir-dump -e "let s = 0; s = s + 1;"
//
// Shows the full pipeline: source → bytecode → IR → optimized IR → MachineFunction

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "base/arena.h"
#include "contracts/machine-ir.h"
#include "contracts/x64-target.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "ir/builder/graph-builder.h"
#include "ir/graph/graph.h"
#include "ir/graph/node.h"
#include "ir/lowering/lowering.h"
#include "ir/opt/passes.h"
#include "vm/isolate/isolate.h"

using namespace v12;

static std::string ReadFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void DumpGraph(Graph* g, const char* title) {
    std::printf("=== %s (%d nodes) ===\n", title, g->node_count());
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        std::printf("  N%d: %-24s type=%-8s uses=%d",
                    n->id(), n->op_name(),
                    "any",  // type name not easily printable yet
                    n->use_count());
        if (n->input_count() > 0) {
            std::printf(" inputs=[");
            for (int i = 0; i < n->input_count(); ++i) {
                if (i > 0) std::printf(", ");
                std::printf("N%d", n->input(i)->id());
            }
            std::printf("]");
        }
        std::printf("\n");
    }
    std::printf("\n");
}

static void DumpMachineFunction(MachineFunction* mf) {
    std::printf("=== MachineFunction (%zu blocks, %u vregs) ===\n",
                mf->block_count(), mf->vreg_count());
    auto rpo = mf->ReversePostOrder();
    for (MachineBlock* block : rpo) {
        std::printf("  Block %d%s%s:\n", block->id,
                    block->is_entry ? " (entry)" : "",
                    block->is_exit ? " (exit)" : "");
        for (const auto& inst : block->instructions) {
            std::printf("    %-12s", MachOpName(inst.op));
            for (const auto& operand : inst.operands) {
                if (operand.IsVReg()) {
                    std::printf(" v%d", operand.vreg);
                } else if (operand.IsImmediate()) {
                    std::printf(" #%lld", (long long)operand.imm);
                } else if (operand.IsPReg()) {
                    std::printf(" p%d", operand.preg.code);
                }
            }
            std::printf("\n");
        }
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    std::string source;
    if (argc >= 3 && std::string(argv[1]) == "-e") {
        source = argv[2];
    } else if (argc >= 2) {
        source = ReadFile(argv[1]);
        if (source.empty()) {
            std::fprintf(stderr, "error: cannot read %s\n", argv[1]);
            return 1;
        }
    } else {
        std::fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        std::fprintf(stderr, "       %s -e \"<source>\"\n", argv[0]);
        return 1;
    }

    Isolate iso;
    Arena arena;

    // Parse
    Parser parser(&arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) {
        std::fprintf(stderr, "parse error: %s\n",
                     parser.errors()[0].message.c_str());
        return 1;
    }

    // Compile to bytecode
    BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);
    FunctionInfo* fi = program->toplevel;

    std::printf("=== Bytecode (%zu bytes, %d registers, %d params) ===\n",
                fi->bytecode.size(), fi->num_registers, fi->num_parameters);
    std::printf("\n");

    // Build IR
    GraphBuilder builder(&arena, fi);
    Graph* g = builder.Build();

    DumpGraph(g, "IR (before optimization)");

    // Optimize
    int total = OptimizeGraph(g);
    std::printf("=== Optimization: %d nodes removed/folded/deduped ===\n\n", total);

    DumpGraph(g, "IR (after optimization)");

    // Verify
    g->Verify();
    std::printf("=== IR verification: OK ===\n\n");

    // Lower to MachineFunction
    const TargetDescription* target = GetHostTargetDescription();
    LoweringResult lr = LowerGraph(g, target, fi);

    if (lr.success) {
        DumpMachineFunction(lr.function.get());
    } else {
        std::printf("=== Lowering: %d nodes skipped (incomplete) ===\n",
                    lr.skipped_count);
    }

    std::printf("=== Builder: complete=%d, skipped=%d ===\n",
                builder.IsComplete(), builder.SkippedCount());
    std::printf("=== Lowering: lowered=%d, skipped=%d ===\n",
                lr.lowered_count, lr.skipped_count);

    return 0;
}
