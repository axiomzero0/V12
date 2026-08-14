// =============================================================================
// tools/bytecode-dump/bytecode-dump.cc
// =============================================================================
// Dump the bytecode for a JS source file. Used for debugging the bytecode
// generator.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "base/arena.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "vm/isolate/isolate.h"

namespace v12 {

namespace {

const char* OperandKindName(OperandKind k) {
    switch (k) {
        case OperandKind::kNone: return "";
        case OperandKind::kReg: return "r";
        case OperandKind::kRegPair: return "rp";
        case OperandKind::kImm8: return "i8";
        case OperandKind::kImm16: return "i16";
        case OperandKind::kImm32: return "i32";
        case OperandKind::kConst: return "k";
        case OperandKind::kIdx: return "idx";
        case OperandKind::kJump: return "jmp";
        case OperandKind::kArgCount: return "argc";
        case OperandKind::kPropertyIdx: return "pidx";
    }
    return "?";
}

void DumpFunction(FunctionInfo* fi, int indent) {
    std::fprintf(stderr, "%*sFunction: %s  (params=%u, regs=%u, ctx=%u)\n",
                 indent, "", fi->name.c_str(),
                 fi->num_parameters, fi->num_registers, fi->num_context_vars);
    if (!fi->property_names.empty()) {
        std::fprintf(stderr, "%*s  properties:", indent, "");
        for (size_t i = 0; i < fi->property_names.size(); ++i) {
            std::fprintf(stderr, " [%zu]=%s", i, fi->property_names[i].c_str());
        }
        std::fprintf(stderr, "\n");
    }
    if (!fi->constants.empty()) {
        std::fprintf(stderr, "%*s  constants:\n", indent, "");
        for (size_t i = 0; i < fi->constants.size(); ++i) {
            const Constant& c = fi->constants[i];
            std::fprintf(stderr, "%*s    [%zu] ", indent, "", i);
            switch (c.kind) {
                case Constant::Kind::kSmi: std::fprintf(stderr, "Smi(%lld)\n", (long long)c.smi); break;
                case Constant::Kind::kNumber: std::fprintf(stderr, "Number(%g)\n", c.number); break;
                case Constant::Kind::kString:
                    std::fprintf(stderr, "String(%s)\n", fi->property_names[c.index].c_str()); break;
                case Constant::Kind::kBoolean: std::fprintf(stderr, "Boolean(%s)\n", c.boolean?"true":"false"); break;
                case Constant::Kind::kUndefined: std::fprintf(stderr, "Undefined\n"); break;
                case Constant::Kind::kNull: std::fprintf(stderr, "Null\n"); break;
                case Constant::Kind::kFunctionInfo: std::fprintf(stderr, "FunctionInfo[%u]\n", c.index); break;
            }
        }
    }
    std::fprintf(stderr, "%*s  bytecode (size=%zu):\n", indent, "", fi->bytecode.size());
    size_t i = 0;
    while (i < fi->bytecode.size()) {
        Op op = static_cast<Op>(fi->bytecode[i]);
        const OpInfo& oi = GetOpInfo(op);
        std::fprintf(stderr, "%*s    %04zx: %-22s", indent, "", i, oi.name);
        ++i;
        // Decode operands.
        OperandKind ops[3] = { oi.operand1, oi.operand2, oi.operand3 };
        for (int k = 0; k < 3; ++k) {
            switch (ops[k]) {
                case OperandKind::kNone: break;
                case OperandKind::kReg:
                case OperandKind::kImm8:
                case OperandKind::kPropertyIdx: {
                    uint8_t v = fi->bytecode[i++];
                    if (ops[k] == OperandKind::kReg) std::fprintf(stderr, " r%d", v);
                    else if (ops[k] == OperandKind::kPropertyIdx) std::fprintf(stderr, " p[%u]=%s", v, fi->property_names[v].c_str());
                    else std::fprintf(stderr, " #%u", v);
                    break;
                }
                case OperandKind::kImm16:
                case OperandKind::kIdx:
                case OperandKind::kArgCount: {
                    uint16_t v = fi->bytecode[i] | (fi->bytecode[i+1] << 8);
                    i += 2;
                    std::fprintf(stderr, " :%u", v);
                    break;
                }
                case OperandKind::kImm32:
                case OperandKind::kConst:
                case OperandKind::kJump: {
                    uint32_t v = fi->bytecode[i] | (fi->bytecode[i+1] << 8) |
                                  (fi->bytecode[i+2] << 16) | (fi->bytecode[i+3] << 24);
                    i += 4;
                    if (ops[k] == OperandKind::kJump) std::fprintf(stderr, " ->%u", v);
                    else if (ops[k] == OperandKind::kConst) {
                        std::fprintf(stderr, " k[%u]", v);
                    } else std::fprintf(stderr, " %u", v);
                    break;
                }
                case OperandKind::kRegPair: {
                    uint16_t v = fi->bytecode[i] | (fi->bytecode[i+1] << 8);
                    i += 2;
                    std::fprintf(stderr, " rp(%u)", v);
                    break;
                }
            }
        }
        std::fprintf(stderr, "\n");
    }
}

}  // namespace

}  // namespace v12

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
    v12::BytecodeGenerator gen(&iso, &arena);
    auto program = gen.Compile(prog);

    for (auto& fi : program->functions) {
        v12::DumpFunction(fi.get(), 0);
    }
    return 0;
}
