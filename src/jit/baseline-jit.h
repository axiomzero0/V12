// =============================================================================
// src/jit/baseline-jit.h
// =============================================================================
// Baseline JIT compiler (Sparkplug-style).
//
// Walks the bytecode of a FunctionInfo and emits x86-64 machine code —
// one machine instruction (or a short sequence) per bytecode instruction.
// No IR, no register allocation, no inlining. The generated code uses the
// same register file and accumulator as the interpreter, so it can be
// entered from and exit to the interpreter at any bytecode boundary.
//
// Calling convention for JIT code:
//   - RAX = accumulator (acc)
//   - RSI = register file base (regs[0])
//   - RDI = pointer to the current Frame (for context, this, etc.)
//   - R12 = Isolate* (for heap allocation, roots)
//   - R13 = bytecode base pointer (for jump targets)
//   - R14 = pointer to the Interp (for calling runtime functions)
//   - R15 = pointer to the FunctionInfo (for constants, property names)
//
// The JIT code is entered via CodeObject::entry_point() and returns a Value
// in RAX (same as the interpreter).
//
// What this JIT does:
//   - LdaSmi/LdaZero/LdaConst → mov rax, immediate
//   - Ldar/Star → mov between rax and [rsi + reg*8]
//   - Add/Sub/Mul (Smi fast path) → inline overflow-checking arithmetic
//   - Jump/JumpIfTrue/JumpIfFalse → jcc with patched targets
//   - Return → ret
//   - Everything else → call the interpreter's handler (fallback)
//
// The hot path (arithmetic + register moves + jumps) is fully compiled
// to machine code. Cold paths (property access, calls, throws) fall back
// to calling C++ runtime functions.

#ifndef V12_JIT_BASELINE_JIT_H_
#define V12_JIT_BASELINE_JIT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "contracts/code-object.h"
#include "frontend/bytecode/bytecode.h"

namespace v12 {

class Interp;

class BaselineJIT {
public:
    // Compile a FunctionInfo into a CodeObject. Returns nullptr if
    // compilation failed (e.g., unsupported opcode).
    // The CodeObject's machine code is placed in executable memory.
    static std::unique_ptr<CodeObject> Compile(FunctionInfo* fi, Interp* interp);

    // Check if an opcode is supported by the JIT (compiled to machine code).
    // Unsupported opcodes fall back to calling the interpreter.
    static bool IsOpSupported(Op op);

private:
    // The JIT compiler uses a X86Emitter to emit machine code into a
    // growable buffer, then copies it into executable memory.
    struct CompileState;

    static void CompileFunction(CompileState* cs, FunctionInfo* fi);
};

}  // namespace v12

#endif  // V12_JIT_BASELINE_JIT_H_
