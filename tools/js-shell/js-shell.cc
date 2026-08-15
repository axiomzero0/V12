// =============================================================================
// tools/js-shell/js-shell.cc
// =============================================================================
// Minimal JS shell. Reads a file, parses, compiles to bytecode, runs through
// the interpreter. This is the primary entry point for testing the engine.
//
// Built-in functions exposed to JS:
//   print(...), console.log(...), parseInt, parseFloat, isNaN,
//   Array.isArray, Object.keys, String.fromCharCode, Math.*

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "base/arena.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/lexer/lexer.h"
#include "frontend/lexer/tokens.h"
#include "frontend/parser/parser.h"
#include "interpreter/interpreter.h"
#include "tools/js-shell/builtins.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/objects/primitives.h"
#include "vm/runtime/runtime.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        return 1;
    }

    std::ifstream f(argv[1]);
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    v12::Isolate iso;
    v12::Arena arena;
    v12::Parser parser(&arena, source);
    v12::Program* prog = parser.ParseProgram();

    if (parser.has_error()) {
        for (const auto& err : parser.errors()) {
            std::fprintf(stderr, "error at %u:%u: %s\n",
                         err.line, err.column, err.message.c_str());
        }
        return 1;
    }

    // Register all built-in host functions.
    v12::RegisterBuiltins(&iso);

    // Compile the program to bytecode.
    v12::BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);

    // Run it.
    v12::Interp interp(&iso);
    v12::InterpResult r = interp.Run(program.get());
    if (r.status == v12::InterpStatus::kThrew) {
        v12::Value s = v12::ToString(&iso, r.value);
        if (s.IsString()) {
            auto* js = v12::FlattenString(&iso, s);
            std::fprintf(stderr, "Uncaught: %.*s\n",
                         static_cast<int>(js->length()), js->data());
        }
        return 2;
    }
    return 0;
}
