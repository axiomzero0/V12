// V12 benchmark runner with per-section timing
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "base/arena.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/objects/primitives.h"
#include "vm/runtime/runtime.h"

using namespace v12;

static int g_test_num = 0;
static auto g_t0 = std::chrono::high_resolution_clock::now();

Value HostPrint(Interp* interp, Value, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    for (uint32_t i = 0; i < argc; ++i) {
        if (i > 0) std::fputc(' ', stdout);
        Value s = ToString(iso, args[i]);
        if (s.IsString()) {
            auto* js = FlattenString(iso, s);
            std::fwrite(js->data(), 1, js->length(), stdout);
        }
    }
    std::fputc('\n', stdout);
    
    // Print timing for the section that just completed
    g_test_num++;
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - g_t0).count();
    fprintf(stderr, "  Test %2d: %8.0f ms\n", g_test_num, ms);
    g_t0 = t1;
    
    return iso->undefined_value();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1]);
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    fprintf(stderr, "=== V12 Interpreter ===\n");
    Isolate iso;
    Arena arena;
    Parser parser(&arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) {
        for (auto& e : parser.errors())
            fprintf(stderr, "error: %s\n", e.message.c_str());
        return 1;
    }
    HostFunction* p = HostFunction::New(&iso, HostPrint, 0);
    iso.SetGlobal("print", Value::FromHeap(p));
    BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);
    Interp interp(&iso);
    
    g_t0 = std::chrono::high_resolution_clock::now();
    interp.Run(program.get());
    
    auto t_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - g_t0).count();
    fprintf(stderr, "  Total:  %8.0f ms\n", total_ms);
    return 0;
}
