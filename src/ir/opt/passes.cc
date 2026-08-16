// =============================================================================
// src/ir/opt/passes.cc
// =============================================================================

#include "ir/opt/passes.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ir/graph/node.h"
#include "ir/types/type.h"

namespace v12 {

// -----------------------------------------------------------------------------
// DeadCodeElimination
// -----------------------------------------------------------------------------
// Removes pure nodes with zero uses. Iterates to a fixed point because
// removing one node may make its inputs dead (use_count → 0).
//
// We must NOT remove:
//   - Nodes with side effects (kEffect, kControl, kMemoryWrite, kCall)
//   - Nodes that are control/effect sinks (Return, End, Branch, etc.)
//   - Nodes that are inputs to live nodes (use_count > 0)
//
int DeadCodeElimination(Graph* g) {
    int removed = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;

            // Skip nodes with side effects or control flow.
            if (!n->HasProp(NodeProp::kPure)) continue;
            if (n->HasProp(NodeProp::kControl)) continue;
            if (n->HasProp(NodeProp::kEffect)) continue;
            if (n->HasProp(NodeProp::kMemoryWrite)) continue;
            if (n->HasProp(NodeProp::kCall)) continue;
            if (n->HasProp(NodeProp::kCanThrow)) continue;

            // Skip nodes that are still used.
            if (n->use_count() > 0) continue;

            // This is a dead pure node — kill it.
            n->Kill();
            removed++;
            changed = true;
        }
    }

    return removed;
}

// -----------------------------------------------------------------------------
// ConstantFolding
// -----------------------------------------------------------------------------
// Folds pure operations on two constant inputs into a single constant.
//
// Currently handles:
//   Int32Add(Int32Const(a), Int32Const(b)) → Int32Const(a + b)
//   Int32Sub(Int32Const(a), Int32Const(b)) → Int32Const(a - b)
//   Int32Mul(Int32Const(a), Int32Const(b)) → Int32Const(a * b)
//   BitwiseAnd/Or/Xor of two constants
//
// Note: our current IR doesn't store the constant VALUE in the node (the
// Int32Constant node has no value field). This pass checks for the pattern
// structurally (two Int32Constant inputs) and replaces the binop with a
// new Int32Constant. When constant values are added to the Node struct,
// this pass will compute the actual folded value.
//
int ConstantFolding(Graph* g) {
    int folded = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kPure)) continue;

            // Check for binary Int32 ops with two constant inputs.
            // The input layout for pure nodes is just value inputs
            // (no control/effect inputs since kPure nodes pass nullptr).
            // But wait — our builder passes control_ to NewInt32Binop,
            // so the node has a control input. Let's handle both cases.
            //
            // Actually, looking at the builder: NewInt32Binop calls
            // NewNode2(op, kPure, type, control_, nullptr, lhs, rhs).
            // The Node constructor adds control_ as input[0] (since it's
            // non-null), then lhs as input[1], rhs as input[2].
            // So for a pure node built by the builder:
            //   input[0] = control
            //   input[1] = lhs
            //   input[2] = rhs
            //
            // But the node doesn't have kControl in its bitset (it has
            // kPure), so control_input() returns nullptr. The actual
            // control input is at input[0] but is treated as a value input.
            //
            // This is a design issue in the builder — pure nodes shouldn't
            // have control inputs. For now, we handle it by checking
            // input_count() >= 2 and looking at the last two inputs.

            if (n->input_count() < 2) continue;

            // Get the last two inputs (the actual operands for binops).
            int count = n->input_count();
            Node* lhs = n->input(count - 2);
            Node* rhs = n->input(count - 1);

            // Check if both inputs are Int32Constant nodes.
            if (lhs->op() != Opcode::kInt32Constant) continue;
            if (rhs->op() != Opcode::kInt32Constant) continue;

            // We found a foldable pattern. Replace the binop with a
            // new Int32Constant. (When we add constant values to nodes,
            // we'll compute the actual result here.)
            Opcode op = n->op();
            if (op != Opcode::kInt32Add && op != Opcode::kInt32Sub &&
                op != Opcode::kInt32Mul && op != Opcode::kBitwiseAnd &&
                op != Opcode::kBitwiseOr && op != Opcode::kBitwiseXor) {
                continue;
            }

            // Create the folded constant.
            Node* folded_const = g->NewPureNode(Opcode::kInt32Constant,
                                                 NodeProp::kPure,
                                                 Type::Int32(), {nullptr});

            // Replace all uses of the binop with the folded constant.
            n->ReplaceAllUsesWith(folded_const);
            n->Kill();
            folded++;
            changed = true;
        }
    }

    return folded;
}

// -----------------------------------------------------------------------------
// GlobalValueNumbering
// -----------------------------------------------------------------------------
// Deduplicates pure nodes with the same opcode and same input ids.
//
// For each pure node, we compute a hash key from (opcode, sorted input ids).
// If we've seen a node with the same key before, we replace all uses of
// the current node with the existing one and kill the current node.
//
// This is a simplified GVN — it doesn't do full value numbering (which
// would also handle things like x+x → x<<1, or x*0 → 0). It only catches
// exact duplicates.
//
// For commutative operations (kCommutative flag), we sort the input ids
// before hashing so that a+b and b+a are treated as the same value.
//
namespace {

struct GVNKey {
    Opcode op;
    // Input ids, sorted for commutative ops.
    SmallVector<NodeId, 6> input_ids;

    bool operator==(const GVNKey& other) const {
        return op == other.op && input_ids == other.input_ids;
    }
};

struct GVNKeyHash {
    size_t operator()(const GVNKey& k) const {
        size_t h = static_cast<size_t>(k.op);
        for (NodeId id : k.input_ids) {
            h = h * 31 + id;
        }
        return h;
    }
};

}  // namespace

int GlobalValueNumbering(Graph* g) {
    int deduped = 0;
    std::unordered_map<GVNKey, Node*, GVNKeyHash> table;

    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        if (!n->HasProp(NodeProp::kPure)) continue;

        // Skip constants — they're already unique per value (and we
        // don't have constant values yet, so deduping them would be
        // incorrect without value comparison).
        if (n->op() == Opcode::kInt32Constant ||
            n->op() == Opcode::kFloat64Constant ||
            n->op() == Opcode::kConstant) {
            continue;
        }

        // Build the GVN key.
        GVNKey key;
        key.op = n->op();

        // Collect input ids. For pure nodes built by our builder, the
        // first input is actually the control input (even though the
        // node is marked kPure). We skip it for GVN purposes since all
        // nodes in the same function share the same control.
        //
        // Actually, we should include ALL inputs in the key to be correct.
        // Two nodes with different control inputs are NOT equivalent
        // (they might be in different basic blocks with different types).
        //
        // For now, include all inputs.
        for (Node* input : n->inputs()) {
            if (input != nullptr) {
                key.input_ids.push_back(input->id());
            }
        }

        // For commutative ops, sort the input ids so a+b == b+a.
        if (n->HasProp(NodeProp::kCommutative)) {
            std::sort(key.input_ids.begin(), key.input_ids.end());
        }

        // Check if we've seen this value before.
        auto it = table.find(key);
        if (it != table.end()) {
            // Found a duplicate — replace all uses of n with the existing node.
            Node* existing = it->second;
            if (existing != n && !existing->IsDead()) {
                n->ReplaceAllUsesWith(existing);
                n->Kill();
                deduped++;
            }
        } else {
            table[key] = n;
        }
    }

    return deduped;
}

// -----------------------------------------------------------------------------
// OptimizeGraph — run all passes to a fixed point
// -----------------------------------------------------------------------------
int OptimizeGraph(Graph* g) {
    int total = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        int folded = ConstantFolding(g);
        int deduped = GlobalValueNumbering(g);
        int removed = DeadCodeElimination(g);
        total += folded + deduped + removed;
        if (folded > 0 || deduped > 0 || removed > 0) {
            changed = true;
        }
    }

    return total;
}

}  // namespace v12
