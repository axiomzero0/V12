// =============================================================================
// tools/ir-dump/ir-dump.cc
// =============================================================================
// Dump IR for a JS function.

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.js>\n", argv[0]);
        return 1;
    }
    std::fprintf(stderr, "ir-dump not yet implemented (file: %s)\n", argv[1]);
    return 0;
}
