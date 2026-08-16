// =============================================================================
// src/ir/lowering/lowering.cc
// =============================================================================

#include "ir/lowering/lowering.h"

#include <unordered_map>

#include "ir/graph/node.h"
#include "ir/types/type.h"

namespace v12 {

LoweringResult LowerGraph(Graph* g, const TargetDescription* target,
                          FunctionInfo* fi) {
    LoweringResult result;
    result.function = std::make_unique<MachineFunction>(target);

    MachineFunction* mf = result.function.get();

    // Create the entry block.
    MachineBlock* entry = mf->NewBlock();
    mf->SetEntryBlock(entry);
    entry->is_entry = true;

    // Map from IR Node → VReg (the VReg that holds the node's result).
    std::unordered_map<Node*, VReg> node_to_vreg;

    // Helper: get or create a VReg for an IR Node's output.
    auto GetVReg = [&](Node* n) -> VReg {
        auto it = node_to_vreg.find(n);
        if (it != node_to_vreg.end()) return it->second;
        VReg v = mf->NewVReg(Rep::kInt32);
        node_to_vreg[n] = v;
        return v;
    };

    // Helper: emit a simple binary op (Add, Sub, Mul, etc.) with 2 VReg inputs
    // and 1 VReg output.
    auto EmitBinop = [&](MachOp op, Node* lhs, Node* rhs, Node* result_node) {
        VReg lhs_v = GetVReg(lhs);
        VReg rhs_v = GetVReg(rhs);
        VReg dst_v = GetVReg(result_node);

        MachineInstruction inst(op);
        inst.AddOperand(MachineOperand::VRegOf(dst_v, Rep::kInt32));  // output
        inst.AddOperand(MachineOperand::VRegOf(lhs_v, Rep::kInt32));  // input 1
        inst.AddOperand(MachineOperand::VRegOf(rhs_v, Rep::kInt32));  // input 2
        entry->AddInstruction(std::move(inst));
    };

    // Helper: emit a comparison (Cmp + Setcc) with 2 VReg inputs and 1 VReg output.
    auto EmitComparison = [&](Cond cond, Node* lhs, Node* rhs, Node* result_node) {
        VReg lhs_v = GetVReg(lhs);
        VReg rhs_v = GetVReg(rhs);
        VReg dst_v = GetVReg(result_node);

        // Emit a Cmp instruction (sets flags).
        MachineInstruction cmp(MachOp::kCmp);
        cmp.AddOperand(MachineOperand::VRegOf(lhs_v, Rep::kInt32));
        cmp.AddOperand(MachineOperand::VRegOf(rhs_v, Rep::kInt32));
        entry->AddInstruction(std::move(cmp));

        // Emit a Setcc instruction (reads flags, writes 0/1 to dst).
        MachineInstruction setcc(MachOp::kSetcc);
        setcc.cond = cond;
        setcc.AddOperand(MachineOperand::VRegOf(dst_v, Rep::kInt32));
        entry->AddInstruction(std::move(setcc));
    };

    // Walk all nodes in creation order and lower them.
    // We skip dead nodes and control-only nodes (Start, End).
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;

        switch (n->op()) {
            case Opcode::kStart:
                // Start node — no instruction needed, just marks the entry.
                break;

            case Opcode::kEnd:
                // End node — the exit block. We'll create it after the loop.
                break;

            case Opcode::kParameter: {
                // Parameter — allocate a VReg (the caller places it).
                // For now, we just allocate a VReg; the calling convention
                // would map this to an argument register.
                (void)GetVReg(n);
                break;
            }

            case Opcode::kInt32Constant:
            case Opcode::kFloat64Constant:
            case Opcode::kConstant: {
                // Constant — emit a Move with an immediate.
                // (We don't have the actual constant value stored in the
                // node yet, so we use 0 as a placeholder.)
                VReg dst_v = GetVReg(n);
                MachineInstruction move(MachOp::kMove);
                move.AddOperand(MachineOperand::VRegOf(dst_v, Rep::kInt32));
                move.AddOperand(MachineOperand::ImmOf(0));  // placeholder
                entry->AddInstruction(std::move(move));
                break;
            }

            // ----- Int32 arithmetic -----
            case Opcode::kInt32Add: {
                // Pure nodes: inputs are [lhs, rhs].
                // Non-pure nodes: inputs are [control, lhs, rhs] or [control, effect, lhs, rhs].
                int count = n->input_count();
                Node* lhs = n->input(count - 2);
                Node* rhs = n->input(count - 1);
                EmitBinop(MachOp::kAdd, lhs, rhs, n);
                break;
            }
            case Opcode::kInt32Sub: {
                int count = n->input_count();
                Node* lhs = n->input(count - 2);
                Node* rhs = n->input(count - 1);
                EmitBinop(MachOp::kSub, lhs, rhs, n);
                break;
            }
            case Opcode::kInt32Mul: {
                int count = n->input_count();
                Node* lhs = n->input(count - 2);
                Node* rhs = n->input(count - 1);
                EmitBinop(MachOp::kMul, lhs, rhs, n);
                break;
            }
            case Opcode::kInt32Div: {
                int count = n->input_count();
                Node* lhs = n->input(count - 2);
                Node* rhs = n->input(count - 1);
                EmitBinop(MachOp::kDiv, lhs, rhs, n);
                break;
            }
            case Opcode::kInt32Mod: {
                int count = n->input_count();
                Node* lhs = n->input(count - 2);
                Node* rhs = n->input(count - 1);
                EmitBinop(MachOp::kMod, lhs, rhs, n);
                break;
            }

            // ----- Bitwise ops -----
            case Opcode::kBitwiseAnd: {
                int count = n->input_count();
                EmitBinop(MachOp::kAnd, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kBitwiseOr: {
                int count = n->input_count();
                EmitBinop(MachOp::kOr, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kBitwiseXor: {
                int count = n->input_count();
                EmitBinop(MachOp::kXor, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kShiftLeft: {
                int count = n->input_count();
                EmitBinop(MachOp::kShl, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kShiftRight: {
                int count = n->input_count();
                EmitBinop(MachOp::kShr, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kShiftRightLogical: {
                int count = n->input_count();
                EmitBinop(MachOp::kRotr, n->input(count - 2), n->input(count - 1), n);
                break;
            }
            case Opcode::kBitwiseNot: {
                // ~x = x ^ -1
                VReg src_v = GetVReg(n->input(n->input_count() - 1));
                VReg dst_v = GetVReg(n);
                MachineInstruction not_(MachOp::kNot);
                not_.AddOperand(MachineOperand::VRegOf(dst_v, Rep::kInt32));
                not_.AddOperand(MachineOperand::VRegOf(src_v, Rep::kInt32));
                entry->AddInstruction(std::move(not_));
                break;
            }

            // ----- Comparisons -----
            case Opcode::kWord32Equal: {
                int count = n->input_count();
                EmitComparison(Cond::kEqual, n->input(count - 2),
                               n->input(count - 1), n);
                break;
            }
            case Opcode::kInt32LessThan: {
                int count = n->input_count();
                EmitComparison(Cond::kLessThan, n->input(count - 2),
                               n->input(count - 1), n);
                break;
            }
            case Opcode::kInt32LessThanOrEqual: {
                int count = n->input_count();
                EmitComparison(Cond::kLessThanOrEqual, n->input(count - 2),
                               n->input(count - 1), n);
                break;
            }

            // ----- Control flow -----
            case Opcode::kReturn: {
                // The return value is the last value input.
                Node* retval = nullptr;
                if (n->input_count() > 0) {
                    retval = n->input(n->input_count() - 1);
                }
                MachineInstruction ret(MachOp::kReturn);
                if (retval != nullptr) {
                    VReg rv = GetVReg(retval);
                    ret.AddOperand(MachineOperand::VRegOf(rv, Rep::kInt32));
                }
                entry->AddInstruction(std::move(ret));
                break;
            }

            default:
                // Unsupported opcode — skip.
                result.skipped_count++;
                continue;
        }

        result.lowered_count++;
    }

    // Create an exit block if we emitted a Return.
    MachineBlock* exit_block = mf->NewBlock();
    exit_block->is_exit = true;
    entry->AddSuccessor(exit_block);
    entry->is_closed = true;

    result.success = (result.skipped_count == 0);
    return result;
}

}  // namespace v12
