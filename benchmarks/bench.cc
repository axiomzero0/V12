// =============================================================================
// benchmarks/bench.cc
// =============================================================================
// Microbenchmark for the interpreter. Runs a series of compute-heavy JS
// programs and reports the time taken per iteration.
//
// Build: g++ -O2 -std=c++20 -Isrc -I. benchmarks/bench.cc \
//          build/libv12.a -lpthread -o build/bin/bench
// (Use -O2 or -O3 for realistic numbers.)

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "base/arena.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

using namespace v12;
using clock_type = std::chrono::high_resolution_clock;

// A no-op print that swallows output (so we don't measure I/O).
Value SilentPrint(Interp* interp, Value, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    (void)args; (void)argc;
    return iso->undefined_value();
}

static double Bench(const std::string& name, const std::string& source,
                     int iterations) {
    Isolate iso;
    Arena arena;
    Parser parser(&arena, source);
    Program* prog = parser.ParseProgram();
    if (parser.has_error()) {
        std::fprintf(stderr, "parse error in %s: %s\n", name.c_str(),
                     parser.errors()[0].message.c_str());
        return 0;
    }
    HostFunction* p = HostFunction::New(&iso, SilentPrint, 0);
    iso.SetGlobal("print", Value::FromHeap(p));

    BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);
    Interp interp(&iso);

    // Warmup.
    for (int i = 0; i < 3; ++i) interp.Run(program.get());

    auto t0 = clock_type::now();
    for (int i = 0; i < iterations; ++i) {
        interp.Run(program.get());
    }
    auto t1 = clock_type::now();
    double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                / static_cast<double>(iterations);
    std::printf("  %-32s %10.2f us/iter\n", name.c_str(), us);
    return us;
}

int main() {
    std::printf("=== V12 Interpreter Benchmark ===\n");
    int iters = 50;

    Bench("loop_add_100k",
          "let s = 0; for (let i = 0; i < 100000; i++) { s += i; } print(s);",
          iters);
    Bench("loop_mul_20",
          "let s = 1; for (let i = 1; i <= 20; i++) { s = s * i; } print(s);",
          iters * 50);
    Bench("prop_access_100k",
          "let o = {x:0}; for (let i = 0; i < 100000; i++) { o.x = i; } print(o.x);",
          iters);
    Bench("array_index_100k",
          "let a = [0,0,0,0,0]; for (let i = 0; i < 100000; i++) { a[i%5] = i; } print(a[0]);",
          iters);
    Bench("func_call_100k",
          "function f(x) { return x + 1; } for (let i = 0; i < 100000; i++) { f(i); } print(0);",
          iters);
    Bench("fib_recursive_25",
          "function fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); } print(fib(25));",
          5);
    Bench("str_concat_1k",
          "let s = \"\"; for (let i = 0; i < 1000; i++) { s = s + \"x\"; } print(s.length);",
          iters / 5);
    Bench("obj_alloc_10k",
          "for (let i = 0; i < 10000; i++) { let o = {a:i, b:i*2, c:i*3}; } print(0);",
          iters / 5);
    Bench("global_access_100k",
          "let g = 0; for (let i = 0; i < 100000; i++) { g = i; } print(g);",
          iters);
    Bench("comparisons_100k",
          "let s = 0; for (let i = 0; i < 100000; i++) { if (i < 50000) s++; else s--; } print(s);",
          iters);

    return 0;
}
