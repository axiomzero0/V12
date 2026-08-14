// =============================================================================
// tools/js-shell/js-shell.cc
// =============================================================================
// Minimal JS shell. Reads a file, parses, runs through the interpreter.
// This is the primary entry point for testing the engine.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "base/arena.h"
#include "frontend/lexer/lexer.h"
#include "frontend/lexer/tokens.h"
#include "frontend/parser/parser.h"
#include "vm/isolate/isolate.h"

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

    std::printf("parsed %zu top-level statements\n", prog->body.size());
    // TODO: bytecode generation, then interpret.
    return 0;
}
