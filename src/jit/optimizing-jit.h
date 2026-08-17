// =============================================================================
// src/jit/optimizing-jit.h
// =============================================================================
// Tier 2 Optimizing JIT using asmjit's Compiler (with built-in register
// allocation). This generates per-function machine code from the IR graph,
// avoiding the icache pressure problem of the baseline JIT.
//
// Pipeline: bytecode → GraphBuilder → OptimizeGraph → OptimizingJIT → CodeObject
//
// The optimizing JIT uses asmjit::x86::Compiler which provides:
//   - Automatic register allocation (linear scan)
//   - Virtual registers (newReg/newVReg)
//   - Spilling to stack when register pressure is high
//   - Function prologue/epilogue generation
//
// Calling convention (same as baseline JIT):
//   Entry: RDI = acc, RSI = regs, RDX = frame, RCX = iso
//   Exit:  RAX = 0 (normal) or nonzero (deopt)

#ifndef V12_JIT_OPTIMIZING_JIT_H_
#define V12_JIT_OPTIMIZING_JIT_H_

#include <memory>

#include "base/arena.h"
#include "contracts/code-object.h"
#include "frontend/bytecode/bytecode.h"

namespace v12 {

class Graph;
class Isolate;

class OptimizingJIT {
public:
    // Compile a FunctionInfo into optimized machine code.
    // Uses the IR pipeline: build graph → optimize → generate code.
    // Returns nullptr on failure (e.g., unsupported opcodes in the graph).
    static std::unique_ptr<CodeObject> Compile(FunctionInfo* fi,
                                                 Isolate* iso,
                                                 Arena* arena);

    // Tier-up threshold: after IR optimization (1000 iterations),
    // the optimizing JIT triggers at 3000 iterations.
    static constexpr uint32_t kOptThreshold = 3000;
};

}  // namespace v12

#endif  // V12_JIT_OPTIMIZING_JIT_H_
