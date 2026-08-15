// =============================================================================
// src/jit/baseline-jit.h
// =============================================================================
// Baseline JIT compiler (Sparkplug-style) using asmjit.
//
// Compiles a FunctionInfo's bytecode into x86-64 machine code. The JIT
// code uses the same register layout as the interpreter:
//   - RAX = accumulator
//   - RSI = register file base (regs[0])
//   - RDI = Frame* (for context, this, etc.)
//   - R12 = Isolate*
//
// Supported opcodes are compiled to native code with inline Smi fast
// paths. Unsupported opcodes call back into C++ runtime handlers.
//
// asmjit handles all instruction encoding, register allocation, and
// label management — no hand-rolled x86 encoding.

#ifndef V12_JIT_BASELINE_JIT_H_
#define V12_JIT_BASELINE_JIT_H_

#include <cstdint>
#include <memory>

#include "base/macros.h"
#include "contracts/code-object.h"
#include "frontend/bytecode/bytecode.h"

namespace v12 {

class Interp;

class BaselineJIT {
public:
    // Compile a FunctionInfo into a CodeObject. Returns nullptr on failure.
    // The `osr_entry_offset` is the bytecode offset of the loop start —
    // the JIT entry point jumps directly there instead of offset 0.
    static std::unique_ptr<CodeObject> Compile(FunctionInfo* fi,
                                                uint32_t osr_entry_offset = 0);

    // OSR threshold: number of JumpLoop iterations before tier-up.
    static constexpr uint32_t kOSRThreshold = 500;
};

}  // namespace v12

#endif  // V12_JIT_BASELINE_JIT_H_
