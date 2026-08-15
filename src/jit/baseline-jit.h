// =============================================================================
// src/jit/baseline-jit.h
// =============================================================================
// Baseline JIT compiler (Sparkplug-style).
//
// Compiles a FunctionInfo's bytecode into x86-64 machine code. The JIT
// code uses the same register layout as the interpreter:
//   - RAX = accumulator
//   - RSI = register file base (regs[0])
//   - RDI = Frame* (for context, this, etc.)
//   - R12 = Isolate*
//   - R14 = Interp* (for calling runtime functions)
//
// For each bytecode instruction, the JIT emits machine code that does the
// same thing as the interpreter's handler. Supported opcodes are compiled
// to native code; unsupported opcodes call back into the interpreter via
// a fallback handler.
//
// The JIT is triggered by the interpreter's OSR mechanism: when a loop's
// hotness counter crosses a threshold, the interpreter calls
// BaselineJIT::Compile() and then invokes the generated code.

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
    static std::unique_ptr<CodeObject> Compile(FunctionInfo* fi);

    // OSR threshold: number of JumpLoop iterations before tier-up.
    static constexpr uint32_t kOSRThreshold = 1000;
};

}  // namespace v12

#endif  // V12_JIT_BASELINE_JIT_H_
