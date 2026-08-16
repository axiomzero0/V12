// =============================================================================
// src/ir/opt/passes.cc
// =============================================================================

#include "ir/opt/passes.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "ir/graph/node.h"
#include "ir/types/type.h"

namespace v12 {

// Helper: check if a node is a constant of a given type.
static bool IsInt32Constant(Node* n) {
    return n != nullptr && n->op() == Opcode::kInt32Constant;
}

static bool IsFloat64Constant(Node* n) {
    return n != nullptr && n->op() == Opcode::kFloat64Constant;
}

static bool IsConstant(Node* n) {
    return IsInt32Constant(n) || IsFloat64Constant(n);
}

// Helper: get the int64 value of a constant node.
static int64_t GetIntValue(Node* n) {
    return n->int_value();
}

static double GetFloatValue(Node* n) {
    return n->float_value();
}

// Helper: create a new Int32Constant with the given value.
static Node* NewInt32Constant(Graph* g, int64_t value) {
    Node* n = g->NewPureNode(Opcode::kInt32Constant, NodeProp::kPure,
                              Type::Int32(), {});
    n->set_int_value(value);
    return n;
}

// Helper: create a new Float64Constant with the given value.
static Node* NewFloat64Constant(Graph* g, double value) {
    Node* n = g->NewPureNode(Opcode::kFloat64Constant, NodeProp::kPure,
                              Type::Float64(), {});
    n->set_float_value(value);
    return n;
}

// Helper: get the two value inputs of a binary op (skipping control input).
// For pure nodes built by our builder, input[0] is control, input[1] is lhs,
// input[2] is rhs. For pure nodes without control, input[0] is lhs, input[1] is rhs.
static Node* GetLHS(Node* n) {
    int count = n->input_count();
    if (count < 2) return nullptr;
    // If the node has kControl, input[0] is control and inputs[1..] are values.
    // Otherwise, all inputs are values.
    // But our builder passes control_ even for pure nodes (design issue).
    // We handle both: if input[0] is a Start/Merge/Loop/Branch node, skip it.
    Node* first = n->input(0);
    if (first->HasProp(NodeProp::kControl) || first->op() == Opcode::kStart) {
        return n->input(count - 2);
    }
    return n->input(count - 2);
}

static Node* GetRHS(Node* n) {
    int count = n->input_count();
    if (count < 2) return nullptr;
    return n->input(count - 1);
}

// -----------------------------------------------------------------------------
// DeadCodeElimination
// -----------------------------------------------------------------------------
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
// Folds binary operations on two constant inputs into a single constant
// with the computed value.
int ConstantFolding(Graph* g) {
    int folded = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kPure)) continue;
            if (n->input_count() < 2) continue;

            Node* lhs = GetLHS(n);
            Node* rhs = GetRHS(n);
            if (lhs == nullptr || rhs == nullptr) continue;

            // Only fold if both inputs are Int32Constants.
            if (!IsInt32Constant(lhs) || !IsInt32Constant(rhs)) continue;

            int64_t a = GetIntValue(lhs);
            int64_t b = GetIntValue(rhs);
            Opcode op = n->op();

            // Check for overflow.
            bool overflow = false;
            int64_t result = 0;

            switch (op) {
                case Opcode::kInt32Add:
                    result = a + b;
                    overflow = (result < a) != (b < 0);  // signed overflow check
                    break;
                case Opcode::kInt32Sub:
                    result = a - b;
                    overflow = (result < a) != (b > 0);
                    break;
                case Opcode::kInt32Mul:
                    result = a * b;
                    overflow = (a != 0 && result / a != b);
                    break;
                case Opcode::kInt32Div:
                    if (b == 0) continue;  // don't fold division by zero
                    result = a / b;
                    break;
                case Opcode::kInt32Mod:
                    if (b == 0) continue;
                    result = a % b;
                    break;
                case Opcode::kBitwiseAnd: result = a & b; break;
                case Opcode::kBitwiseOr:  result = a | b; break;
                case Opcode::kBitwiseXor: result = a ^ b; break;
                case Opcode::kShiftLeft:  result = a << (b & 63); break;
                case Opcode::kShiftRight: result = a >> (b & 63); break;
                case Opcode::kShiftRightLogical:
                    result = static_cast<uint64_t>(a) >> (b & 63);
                    break;
                default:
                    continue;  // not a foldable op
            }

            if (overflow) continue;

            // Create the folded constant.
            Node* folded_const = NewInt32Constant(g, result);

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
namespace {

struct GVNKey {
    Opcode op;
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

        // Skip constants — they're deduplicated by value in ConstantFolding.
        if (n->op() == Opcode::kInt32Constant ||
            n->op() == Opcode::kFloat64Constant ||
            n->op() == Opcode::kConstant) {
            continue;
        }

        GVNKey key;
        key.op = n->op();

        for (Node* input : n->inputs()) {
            if (input != nullptr) {
                key.input_ids.push_back(input->id());
            }
        }

        // For commutative ops, sort the input ids so a+b == b+a.
        if (n->HasProp(NodeProp::kCommutative)) {
            std::sort(key.input_ids.begin(), key.input_ids.end());
        }

        auto it = table.find(key);
        if (it != table.end()) {
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
// ConstantPropagation
// -----------------------------------------------------------------------------
// For unary ops on a constant input, compute the result.
//   Int32Neg(const a) → Int32Constant(-a)
//   BitwiseNot(const a) → Int32Constant(~a)
int ConstantPropagation(Graph* g) {
    int propagated = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kPure)) continue;
            if (n->input_count() < 1) continue;

            // Get the last input (the actual operand for unary ops).
            Node* input = n->input(n->input_count() - 1);
            if (!IsInt32Constant(input)) continue;

            int64_t a = GetIntValue(input);
            int64_t result = 0;
            bool foldable = true;

            switch (n->op()) {
                case Opcode::kInt32Neg:
                    result = -a;
                    break;
                case Opcode::kBitwiseNot:
                    result = ~a;
                    break;
                case Opcode::kInt32Abs:
                    result = a < 0 ? -a : a;
                    break;
                default:
                    foldable = false;
                    break;
            }

            if (!foldable) continue;

            Node* folded = NewInt32Constant(g, result);
            n->ReplaceAllUsesWith(folded);
            n->Kill();
            propagated++;
            changed = true;
        }
    }

    return propagated;
}

// -----------------------------------------------------------------------------
// StrengthReduction
// -----------------------------------------------------------------------------
// Replaces expensive operations with cheaper equivalents.
int StrengthReduction(Graph* g) {
    int reduced = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kPure)) continue;
            if (n->input_count() < 2) continue;

            Node* lhs = GetLHS(n);
            Node* rhs = GetRHS(n);
            if (lhs == nullptr || rhs == nullptr) continue;

            Opcode op = n->op();

            // --- Multiplication by power of 2 → shift left ---
            if (op == Opcode::kInt32Mul && IsInt32Constant(rhs)) {
                int64_t multiplier = GetIntValue(rhs);

                // x * 0 → 0
                if (multiplier == 0) {
                    Node* zero = NewInt32Constant(g, 0);
                    n->ReplaceAllUsesWith(zero);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }

                // x * 1 → x
                if (multiplier == 1) {
                    n->ReplaceAllUsesWith(lhs);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }

                // x * 2^n → x << n
                if (multiplier > 0 && (multiplier & (multiplier - 1)) == 0) {
                    // It's a power of 2. Find the exponent.
                    int shift = 0;
                    int64_t m = multiplier;
                    while (m > 1) { m >>= 1; shift++; }

                    Node* shift_amount = NewInt32Constant(g, shift);
                    Node* shl = g->NewNode2(Opcode::kShiftLeft,
                                             NodeProp::kPure, Type::Int32(),
                                             nullptr, nullptr, lhs, shift_amount);
                    n->ReplaceAllUsesWith(shl);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }
            }

            // --- Division optimizations ---
            if (op == Opcode::kInt32Div && IsInt32Constant(rhs)) {
                int64_t divisor = GetIntValue(rhs);

                // x / 1 → x
                if (divisor == 1) {
                    n->ReplaceAllUsesWith(lhs);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }

                // x / 2^n → x >> n (only for positive divisors, signed shift)
                if (divisor > 0 && (divisor & (divisor - 1)) == 0 && divisor != 1) {
                    int shift = 0;
                    int64_t d = divisor;
                    while (d > 1) { d >>= 1; shift++; }

                    Node* shift_amount = NewInt32Constant(g, shift);
                    Node* shr = g->NewNode2(Opcode::kShiftRight,
                                             NodeProp::kPure, Type::Int32(),
                                             nullptr, nullptr, lhs, shift_amount);
                    n->ReplaceAllUsesWith(shr);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }
            }

            // --- Modulo optimizations ---
            if (op == Opcode::kInt32Mod && IsInt32Constant(rhs)) {
                int64_t divisor = GetIntValue(rhs);

                // x % 1 → 0
                if (divisor == 1) {
                    Node* zero = NewInt32Constant(g, 0);
                    n->ReplaceAllUsesWith(zero);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }

                // x % 2^n → x & (2^n - 1) (for positive divisors)
                if (divisor > 0 && (divisor & (divisor - 1)) == 0 && divisor != 1) {
                    int64_t mask = divisor - 1;
                    Node* mask_const = NewInt32Constant(g, mask);
                    Node* and_op = g->NewNode2(Opcode::kBitwiseAnd,
                                                NodeProp::kPure, Type::Int32(),
                                                nullptr, nullptr, lhs, mask_const);
                    n->ReplaceAllUsesWith(and_op);
                    n->Kill();
                    reduced++;
                    changed = true;
                    continue;
                }
            }
        }
    }

    return reduced;
}

// -----------------------------------------------------------------------------
// InstructionCombining
// -----------------------------------------------------------------------------
// Peephole simplifications: x+0→x, x*1→x, x*0→0, x-x→0, x|0→x, etc.
int InstructionCombining(Graph* g) {
    int combined = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kPure)) continue;
            if (n->input_count() < 2) continue;

            Node* lhs = GetLHS(n);
            Node* rhs = GetRHS(n);
            if (lhs == nullptr || rhs == nullptr) continue;

            Opcode op = n->op();
            Node* replacement = nullptr;

            // Check patterns. For commutative ops, check both sides.
            auto isConst = [](Node* n, int64_t v) -> bool {
                return IsInt32Constant(n) && GetIntValue(n) == v;
            };

            switch (op) {
                case Opcode::kInt32Add:
                    // x + 0 → x, 0 + x → x
                    if (isConst(rhs, 0)) replacement = lhs;
                    else if (isConst(lhs, 0)) replacement = rhs;
                    break;

                case Opcode::kInt32Sub:
                    // x - 0 → x
                    if (isConst(rhs, 0)) replacement = lhs;
                    // x - x → 0
                    else if (lhs == rhs) replacement = NewInt32Constant(g, 0);
                    break;

                case Opcode::kInt32Mul:
                    // x * 0 → 0, 0 * x → 0
                    if (isConst(rhs, 0) || isConst(lhs, 0))
                        replacement = NewInt32Constant(g, 0);
                    // x * 1 → x, 1 * x → x
                    else if (isConst(rhs, 1)) replacement = lhs;
                    else if (isConst(lhs, 1)) replacement = rhs;
                    break;

                case Opcode::kInt32Div:
                    // x / 1 → x
                    if (isConst(rhs, 1)) replacement = lhs;
                    break;

                case Opcode::kBitwiseOr:
                    // x | 0 → x, 0 | x → x
                    if (isConst(rhs, 0)) replacement = lhs;
                    else if (isConst(lhs, 0)) replacement = rhs;
                    // x | x → x
                    else if (lhs == rhs) replacement = lhs;
                    break;

                case Opcode::kBitwiseAnd:
                    // x & 0 → 0, 0 & x → 0
                    if (isConst(rhs, 0) || isConst(lhs, 0))
                        replacement = NewInt32Constant(g, 0);
                    // x & -1 → x, -1 & x → x (all bits set)
                    else if (isConst(rhs, -1)) replacement = lhs;
                    else if (isConst(lhs, -1)) replacement = rhs;
                    // x & x → x
                    else if (lhs == rhs) replacement = lhs;
                    break;

                case Opcode::kBitwiseXor:
                    // x ^ 0 → x, 0 ^ x → x
                    if (isConst(rhs, 0)) replacement = lhs;
                    else if (isConst(lhs, 0)) replacement = rhs;
                    // x ^ x → 0
                    else if (lhs == rhs) replacement = NewInt32Constant(g, 0);
                    break;

                case Opcode::kShiftLeft:
                case Opcode::kShiftRight:
                case Opcode::kShiftRightLogical:
                    // x << 0 → x, x >> 0 → x
                    if (isConst(rhs, 0)) replacement = lhs;
                    break;

                default:
                    break;
            }

            if (replacement != nullptr) {
                n->ReplaceAllUsesWith(replacement);
                n->Kill();
                combined++;
                changed = true;
            }
        }
    }

    return combined;
}

// -----------------------------------------------------------------------------
// Simplification (canonicalization)
// -----------------------------------------------------------------------------
// Normalizes the IR so subsequent passes can pattern-match more easily.
// Currently: move constants to the right side of commutative ops.
int Simplification(Graph* g) {
    int simplified = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
            if (n->IsDead()) continue;
            if (!n->HasProp(NodeProp::kCommutative)) continue;
            if (n->input_count() < 2) continue;

            Node* lhs = GetLHS(n);
            Node* rhs = GetRHS(n);
            if (lhs == nullptr || rhs == nullptr) continue;

            // If lhs is a constant and rhs is not, swap them.
            // This canonicalizes to "value + constant" form.
            if (IsConstant(lhs) && !IsConstant(rhs)) {
                // Find the indices of lhs and rhs in the inputs.
                int count = n->input_count();
                int lhs_idx = count - 2;
                int rhs_idx = count - 1;
                Node* tmp = n->input(lhs_idx);
                n->SetInput(lhs_idx, n->input(rhs_idx));
                n->SetInput(rhs_idx, tmp);
                simplified++;
                changed = true;
            }
        }
    }

    return simplified;
}

// -----------------------------------------------------------------------------
// BranchElimination
// -----------------------------------------------------------------------------
// Removes branches with constant conditions. Currently a stub since the
// IR builder doesn't fully model Branch nodes yet.
int BranchElimination(Graph* g) {
    int eliminated = 0;

    // Look for Branch nodes with a constant condition input.
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        if (n->op() != Opcode::kBranch) continue;

        // The condition is the last value input.
        if (n->input_count() < 1) continue;
        Node* cond = n->input(n->input_count() - 1);

        if (IsInt32Constant(cond)) {
            int64_t val = GetIntValue(cond);
            // TODO: when we have proper CFG, replace the branch with an
            // unconditional jump to the appropriate target.
            // For now, just count it.
            (void)val;
            eliminated++;
        }
    }

    return eliminated;
}

// -----------------------------------------------------------------------------
// CommonSubexpressionElimination
// -----------------------------------------------------------------------------
// Similar to GVN but with effect awareness. For pure nodes, this is
// equivalent to GVN. For memory-reading nodes, we'd need to check for
// interfering stores. Currently delegates to GVN for pure nodes.
int CommonSubexpressionElimination(Graph* g) {
    // For pure nodes, CSE == GVN.
    // For memory nodes, we'd need effect-chain analysis.
    // For now, just run GVN.
    return GlobalValueNumbering(g);
}

// -----------------------------------------------------------------------------
// LICM (Loop-Invariant Code Motion)
// -----------------------------------------------------------------------------
// Hoists pure nodes out of loops. Currently a stub since the IR builder
// doesn't fully model Loop nodes yet.
int LICM(Graph* g) {
    int hoisted = 0;

    // Look for Loop nodes and try to hoist pure nodes out.
    // TODO: when we have proper loop detection, implement:
    // 1. Find all Loop nodes.
    // 2. For each pure node inside the loop:
    //    a. Check if all inputs are loop-invariant (defined outside the loop).
    //    b. If so, move the node's control input to the loop preheader.
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        if (n->op() == Opcode::kLoop) {
            // Found a loop — would need to analyze it.
            // For now, just count it.
            hoisted++;
        }
    }

    return 0;  // not yet implemented
}

// -----------------------------------------------------------------------------
// OptimizeGraph — run all passes to a fixed point
// -----------------------------------------------------------------------------
int OptimizeGraph(Graph* g) {
    int total = 0;
    bool changed = true;
    int iterations = 0;
    const int kMaxIterations = 10;

    while (changed && iterations < kMaxIterations) {
        changed = false;
        iterations++;

        int n = 0;
        n += ConstantPropagation(g);
        n += ConstantFolding(g);
        n += StrengthReduction(g);
        n += InstructionCombining(g);
        n += Simplification(g);
        n += BranchElimination(g);
        n += CommonSubexpressionElimination(g);
        n += GlobalValueNumbering(g);
        n += DeadCodeElimination(g);
        // LICM not yet effective (no loop modeling)
        // n += LICM(g);

        total += n;
        if (n > 0) changed = true;
    }

    return total;
}

}  // namespace v12
