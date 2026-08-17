// =============================================================================
// src/ir/opt/bytecode-optimizer.h
// =============================================================================
// IR-based bytecode optimizer. Takes a FunctionInfo, builds a Sea-of-Nodes
// IR graph, runs all optimization passes, and produces optimized bytecode.
//
// The optimized bytecode is stored in FunctionInfo::optimized_bytecode and
// swapped in at runtime. The interpreter and baseline JIT then execute the
// optimized bytecode — no code emitter or register allocator needed.
//
// Optimizations applied:
//   - Constant folding (1 + 2 → 3)
//   - Strength reduction (x * 8 → x << 3)
//   - Dead code elimination
//   - GVN (deduplicate x+1; x+1 → one x+1)
//   - Instruction combining (x + 0 → x)
//   - Algebraic simplification (~~x → x)
//   - And 15+ more passes
//
// This is a "tier 1.5" optimizer — it runs between the interpreter and
// the baseline JIT, producing better bytecode for the JIT to compile.

#ifndef V12_IR_OPT_BYTECODE_OPTIMIZER_H_
#define V12_IR_OPT_BYTECODE_OPTIMIZER_H_

#include "frontend/bytecode/bytecode.h"
#include "ir/graph/graph.h"
#include "ir/opt/passes.h"

namespace v12 {

class Isolate;
class Arena;

// Optimize a FunctionInfo's bytecode using the IR pipeline.
// Returns the number of optimizations applied (nodes removed/folded/etc.).
// If the function can't be optimized (unsupported opcodes), returns 0.
//
// After optimization, fi->optimized_bytecode contains the optimized
// bytecode. The interpreter checks for optimized_bytecode and uses it
// if present.
int OptimizeBytecode(Isolate* iso, Arena* arena, FunctionInfo* fi);

// Check if a FunctionInfo has been optimized.
inline bool IsOptimized(FunctionInfo* fi) {
    return !fi->optimized_bytecode.empty();
}

// Tier-up threshold for IR optimization (higher than baseline JIT).
// The baseline JIT triggers at 500 iterations; IR optimization triggers
// at 1000 iterations (after the baseline JIT has profiled the function).
static constexpr uint32_t kIROptThreshold = 1000;

}  // namespace v12

#endif  // V12_IR_OPT_BYTECODE_OPTIMIZER_H_
