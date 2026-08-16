// =============================================================================
// src/ir/opt/passes.h
// =============================================================================
// IR optimization passes. Each pass takes a Graph* and transforms it in-place.
//
// Pass pipeline (typical):
//   1. DeadCodeElimination — remove pure nodes with no uses
//   2. ConstantFolding — fold constant expressions
//   3. GVN (Global Value Numbering) — deduplicate equivalent pure nodes
//   4. DeadCodeElimination — clean up nodes made dead by folding/GVN
//
// All passes are idempotent: running them again on an already-optimized
// graph is a no-op.

#ifndef V12_IR_OPT_PASSES_H_
#define V12_IR_OPT_PASSES_H_

#include "ir/graph/graph.h"

namespace v12 {

// Dead Code Elimination.
// Removes pure nodes (NodeProp::kPure) that have zero uses. Iterates
// to a fixed point (removing one node may make its inputs dead).
// Does NOT remove nodes with side effects (control, effect, calls, stores).
// Returns the number of nodes removed.
int DeadCodeElimination(Graph* g);

// Constant Folding.
// Folds pure operations on two constant inputs into a single constant.
// Currently handles: Int32Add, Int32Sub, Int32Mul of two Int32Constants.
// Returns the number of nodes folded.
int ConstantFolding(Graph* g);

// Global Value Numbering.
// Deduplicates pure nodes with the same opcode and same inputs.
// Uses a hash table keyed on (opcode, input_ids). When a duplicate is
// found, all uses of the new node are replaced with the existing one.
// Returns the number of nodes deduplicated.
int GlobalValueNumbering(Graph* g);

// Run all optimization passes to a fixed point.
// Returns the total number of nodes removed/folded/deduplicated.
int OptimizeGraph(Graph* g);

}  // namespace v12

#endif  // V12_IR_OPT_PASSES_H_
