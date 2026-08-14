// =============================================================================
// tools/bytecode-dump/bytecode-dump.cc
// =============================================================================
// Dump bytecode for a JS file.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "base/arena.h"
#include "frontend/parser/parser.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        return 1;
    }
    std::fprintf(stderr, "bytecode-dump not yet implemented (file: %s)\n", argv[1]);
    return 0;
}
