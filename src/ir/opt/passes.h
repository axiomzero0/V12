// =============================================================================
// src/ir/opt/passes.h
// =============================================================================
// IR optimization passes. Each pass takes a Graph* and transforms it in-place.
//
// Pass pipeline (typical):
//   1. ConstantPropagation — replace uses of constant nodes with the constant
//   2. ConstantFolding — fold constant expressions (e.g. 1+2 → 3)
//   3. StrengthReduction — replace expensive ops with cheaper ones (x*2 → x<<1)
//   4. InstructionCombining — peephole simplifications (x+0 → x, x*0 → 0)
//   5. Simplification — canonicalize (constants left, normalize)
//   6. BranchElimination — remove dead branches (if(true) → jump)
//   7. CommonSubexpressionElimination — deduplicate with effect awareness
//   8. GlobalValueNumbering — hash-cons equivalent pure nodes
//   9. DeadCodeElimination — remove pure nodes with no uses
//  10. LICM — loop-invariant code motion
//
// All passes are idempotent: running them again on an already-optimized
// graph is a no-op.

#ifndef V12_IR_OPT_PASSES_H_
#define V12_IR_OPT_PASSES_H_

#include "ir/graph/graph.h"

namespace v12 {

// Dead Code Elimination.
// Removes pure nodes (NodeProp::kPure) that have zero uses. Iterates
// to a fixed point. Does NOT remove nodes with side effects.
int DeadCodeElimination(Graph* g);

// Constant Folding.
// Folds pure operations on two constant inputs into a single constant.
// Computes the actual folded value (e.g. Int32Add(1, 2) → Int32Constant(3)).
int ConstantFolding(Graph* g);

// Global Value Numbering.
// Deduplicates pure nodes with the same opcode and same input ids.
int GlobalValueNumbering(Graph* g);

// Constant Propagation.
// For nodes whose only input is a constant, replace the node with a
// constant of the computed value. Handles: Int32Neg(const) → -const,
// BitwiseNot(const) → ~const.
int ConstantPropagation(Graph* g);

// Strength Reduction.
// Replaces expensive operations with cheaper equivalents:
//   x * 2^n → x << n (multiply by power of 2)
//   x / 2^n → x >> n (divide by power of 2, signed)
//   x * 0 → 0
//   x * 1 → x
//   x / 1 → x
//   x % 1 → 0
//   x % 2^n → x & (2^n - 1) (unsigned)
int StrengthReduction(Graph* g);

// Instruction Combining (peephole).
// Simplifies common patterns:
//   x + 0 → x, 0 + x → x
//   x - 0 → x
//   x * 0 → 0
//   x * 1 → x, 1 * x → x
//   x - x → 0
//   x | 0 → x, 0 | x → x
//   x | x → x
//   x & 0 → 0
//   x & -1 → x (all bits set)
//   x & x → x
//   x ^ 0 → x
//   x ^ x → 0
//   x << 0 → x, x >> 0 → x
int InstructionCombining(Graph* g);

// Simplification (canonicalization).
// Normalizes the IR so subsequent passes can pattern-match more easily:
//   - Move constants to the right side of commutative ops (a+1, not 1+a)
//   - Normalize -x to 0-x
//   - Canonicalize comparisons (flip operands to get < instead of >)
int Simplification(Graph* g);

// Branch Elimination.
// Removes branches with constant conditions:
//   Branch(true) → unconditional jump to true target
//   Branch(false) → unconditional jump to false target
// Also removes dead blocks that become unreachable.
int BranchElimination(Graph* g);

// Common Subexpression Elimination.
// Similar to GVN but with effect awareness — only deduplicates nodes
// when no interfering memory operation exists between them. More precise
// than GVN for memory-reading nodes.
int CommonSubexpressionElimination(Graph* g);

// Loop-Invariant Code Motion.
// Hoists pure nodes out of loops when their inputs don't change across
// iterations. Requires loop detection (Loop nodes in the control flow).
int LICM(Graph* g);

// ----- Additional production JIT passes -----

// Algebraic Simplification.
// Applies algebraic identities that GVN/InstCombine miss:
//   x + (-x) → 0
//   x - (-y) → x + y
//   (-x) * (-y) → x * y
//   x * (-1) → -x (0 - x)
//   (-x) * y → -(x * y)
//   x / (-1) → -x
//   x / x → 1 (if x != 0)
//   Double negation: -(-x) → x
//   Double bitwise not: ~~x → x
int AlgebraicSimplification(Graph* g);

// Boolean Simplification.
// Simplifies boolean operations:
//   !!x → x (double negation of truthiness)
//   x === true → x (if x is already boolean)
//   x === false → !x (if x is already boolean)
//   x !== true → !x
//   x !== false → x
//   !(!x) → x (logical double negation)
int BooleanSimplification(Graph* g);

// Comparison Simplification.
// Simplifies comparison chains:
//   !(a < b) → a >= b
//   !(a <= b) → a > b
//   !(a == b) → a != b
//   !(a != b) → a == b
//   a > b → b < a (canonicalize to LessThan)
//   a >= b → b <= a (canonicalize to LessThanOrEqual)
//   a < b == false → a >= b
int ComparisonSimplification(Graph* g);

// Phi Simplification.
// Simplifies Phi nodes:
//   Phi(x) → x (single input)
//   Phi(x, x, x) → x (all inputs identical)
//   Phi(x, y) where one input is the Phi itself → the other input
int PhiSimplification(Graph* g);

// Check Elimination.
// Removes redundant type checks:
//   CheckSmi(CheckSmi(x)) → CheckSmi(x)
//   CheckHeapObject(CheckHeapObject(x)) → CheckHeapObject(x)
//   CheckSmi(Int32Constant) → Int32Constant (constants are known)
//   CheckSmi(Int32Add(a, b)) → CheckSmi(a) + CheckSmi(b) (propagate)
int CheckElimination(Graph* g);

// Redundancy Elimination.
// Eliminates redundant operations discovered by type analysis:
//   If a CheckSmi already verified x is a Smi, remove subsequent CheckSmi(x)
//   If a branch narrowed x's type, remove checks that are now provably true
// This is a simplified version of V8's RedundancyElimination pass.
int RedundancyElimination(Graph* g);

// Value Numbering.
// More sophisticated than GVN — includes algebraic identities:
//   x * 0 → 0 (even if x has side effects, we skip those)
//   x * 1 → x
//   x + 0 → x
//   x - 0 → x
//   x - x → 0
//   x ^ x → 0
//   x & x → x
//   x | x → x
// Hash-conses by (opcode, input values) and applies identities on match.
int ValueNumbering(Graph* g);

// Block Merging.
// Merges blocks connected by a single unconditional edge:
//   If block A ends with Jump to B, and B has only one predecessor (A),
//   merge B into A. Reduces CFG complexity.
int BlockMerging(Graph* g);

// Loop Unrolling.
// Unrolls small loops by N iterations to reduce loop overhead:
//   for (i = 0; i < 4; i++) body → body; body; body; body (if trip count known)
// Currently a stub (requires loop trip-count analysis).
int LoopUnrolling(Graph* g);

// Tail Call Optimization.
// Converts tail calls (call immediately followed by return) into jumps:
//   return f(args) → jump f(args) (no new stack frame)
// Currently a stub (requires call/return pattern matching).
int TailCallOptimization(Graph* g);

// Escape Analysis.
// Detects allocations that don't escape the current function and replaces
// them with scalar values (fields → local variables):
//   let o = {x: 1, y: 2}; return o.x + o.y → return 1 + 2
// Currently a stub (requires allocation tracking).
int EscapeAnalysis(Graph* g);

// Run all optimization passes to a fixed point.
int OptimizeGraph(Graph* g);

}  // namespace v12

#endif  // V12_IR_OPT_PASSES_H_
