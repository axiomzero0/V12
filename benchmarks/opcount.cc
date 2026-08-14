// =============================================================================
// benchmarks/opcount.cc
// =============================================================================
// Dynamic opcode frequency profiler. Runs a JS program and counts how many
// times each opcode is dispatched at runtime.
//
// Build: g++ -O2 -std=c++20 -Isrc -I. -DV12_OPCODE_STATS=1 \
//          benchmarks/opcount.cc build/libv12.a -lpthread -o build/bin/opcount
// Usage:  ./build/bin/opcount <file.js>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "base/arena.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

using namespace v12;

// Defined in interpreter.cc when V12_OPCODE_STATS is enabled.
namespace v12 { extern uint64_t g_opcode_dispatch_counts[256]; }
using v12::g_opcode_dispatch_counts;

Value SilentPrint(Interp* interp, Value, Value*, uint32_t) {
    return interp->isolate()->undefined_value();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1]);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    Isolate iso;
    Arena arena;
    Parser parser(&arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) {
        std::fprintf(stderr, "parse error: %s\n", parser.errors()[0].message.c_str());
        return 1;
    }
    HostFunction* p = HostFunction::New(&iso, SilentPrint, 0);
    iso.SetGlobal("print", Value::FromHeap(p));

    BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);

    Interp interp(&iso);
    interp.Run(program.get());

    // Print dynamic opcode dispatch counts sorted by count.
    struct Entry { uint8_t op; uint64_t count; };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (int i = 0; i < 256; ++i) {
        if (g_opcode_dispatch_counts[i] > 0) {
            entries.push_back({static_cast<uint8_t>(i), g_opcode_dispatch_counts[i]});
            total += g_opcode_dispatch_counts[i];
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.count > b.count; });

    std::printf("=== Dynamic opcode dispatch counts ===\n");
    std::printf("Total dispatches: %llu\n\n", static_cast<unsigned long long>(total));
    for (auto& e : entries) {
        double pct = 100.0 * e.count / total;
        std::printf("  %-25s %12llu  (%5.1f%%)\n",
                    OpName(static_cast<Op>(e.op)),
                    static_cast<unsigned long long>(e.count), pct);
    }

    return 0;
}
