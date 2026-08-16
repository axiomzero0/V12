// =============================================================================
// src/ir/builder/graph-builder.cc
// =============================================================================

#include "ir/builder/graph-builder.h"

#include "frontend/bytecode/bytecode.h"
#include "ir/graph/node.h"
#include "ir/types/type.h"
#include "vm/isolate/isolate.h"

namespace v12 {

GraphBuilder::GraphBuilder(Arena* arena, FunctionInfo* fi)
    : arena_(arena), fi_(fi) {
    // Pre-size the register value map to the function's register count.
    reg_values_.resize(fi_->num_registers > 0 ? fi_->num_registers : 1, nullptr);
}

Graph* GraphBuilder::Build() {
    graph_ = arena_->New<Graph>(arena_);

    // Create the Start node — the root of control flow and the initial
    // effect token. All parameters and the initial effect flow from Start.
    control_ = graph_->NewNode(Opcode::kStart, NodeProp::kControl,
                               Type::None(), nullptr, nullptr, {});
    graph_->set_start(control_);
    effect_ = control_;  // Start is also the initial effect.

    // Create Parameter nodes for each function parameter.
    for (uint16_t i = 0; i < fi_->num_parameters; ++i) {
        Node* param = graph_->NewNode1(Opcode::kParameter, NodeProp::kPure,
                                        Type::Any(), control_, nullptr, control_);
        SetReg(static_cast<uint8_t>(i), param);
    }

    // Walk the bytecode and build IR nodes.
    auto& bc = fi_->bytecode;
    size_t i = 0;
    while (i < bc.size()) {
        int next = ProcessInstruction(i);
        if (next < 0) {
            // Unsupported opcode — skip it and mark graph as incomplete.
            Op op = static_cast<Op>(bc[i]);
            const OpInfo& oi = GetOpInfo(op);
            SkipInstruction(op);
            i += oi.length;
            continue;
        }
        i = static_cast<size_t>(next);
    }

    // If we reached the end without a Return, create an implicit
    // ReturnUndefined to terminate the graph.
    if (control_ != nullptr && complete_) {
        Node* end = graph_->NewNode1(Opcode::kEnd, NodeProp::kControl,
                                      Type::None(), nullptr, nullptr, control_);
        graph_->set_end(end);
    }

    return graph_;
}

Node* GraphBuilder::NewSmiConstant(int64_t value) {
    // Create a pure Int32Constant node with no inputs.
    // Note: the actual constant value is not stored in the node yet (the
    // Node struct doesn't have a value field). The type carries the info
    // that this is a Smi. When constant values are added, this will be
    // updated to store the actual value.
    return graph_->NewPureNode(Opcode::kInt32Constant, NodeProp::kPure,
                               Type::Smi(), {});
}

Node* GraphBuilder::NewInt32Binop(Opcode op, Node* lhs, Node* rhs) {
    return graph_->NewNode2(op, NodeProp::kPure, Type::Int32(),
                            control_, nullptr, lhs, rhs);
}

Node* GraphBuilder::NewComparison(Opcode op, Node* lhs, Node* rhs) {
    return graph_->NewNode2(op, NodeProp::kPure, Type::Boolean(),
                            control_, nullptr, lhs, rhs);
}

void GraphBuilder::SkipInstruction(Op op) {
    complete_ = false;
    skipped_++;
}

int GraphBuilder::ProcessInstruction(size_t i) {
    auto& bc = fi_->bytecode;
    Op op = static_cast<Op>(bc[i]);
    const OpInfo& oi = GetOpInfo(op);

    switch (op) {
        // ----- Loading constants -----
        case Op::LdaZero:
            acc_ = NewSmiConstant(0);
            break;
        case Op::LdaSmi: {
            int8_t v = static_cast<int8_t>(bc[i + 1]);
            acc_ = NewSmiConstant(v);
            break;
        }
        case Op::LdaSmi16: {
            int16_t v = static_cast<int16_t>(
                static_cast<uint16_t>(bc[i+1]) |
                (static_cast<uint16_t>(bc[i+2]) << 8));
            acc_ = NewSmiConstant(v);
            break;
        }
        case Op::LdaConst: {
            // For now, treat as an opaque constant (type depends on the
            // constant kind, but we conservatively use Type::Any()).
            acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                        Type::Any(), {});
            break;
        }
        case Op::LdaUndefined:
            acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                        Type::Undefined(), {});
            break;
        case Op::LdaNull:
            acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                        Type::Null(), {});
            break;
        case Op::LdaTrue:
            acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                        Type::Boolean(), {});
            break;
        case Op::LdaFalse:
            acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                        Type::Boolean(), {});
            break;

        // ----- Register moves -----
        case Op::Ldar: {
            uint8_t r = bc[i + 1];
            acc_ = GetReg(r);
            if (acc_ == nullptr) {
                acc_ = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                            Type::Undefined(), {});
            }
            break;
        }
        case Op::Star: {
            uint8_t r = bc[i + 1];
            SetReg(r, acc_);
            break;
        }
        case Op::Mov: {
            uint8_t dst = bc[i + 1];
            uint8_t src = bc[i + 2];
            Node* val = GetReg(src);
            if (val == nullptr) {
                val = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                           Type::Undefined(), {});
            }
            SetReg(dst, val);
            break;
        }

        // ----- Binary arithmetic -----
        case Op::Add: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kInt32Add, acc_, rhs);
            break;
        }
        case Op::Sub: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kInt32Sub, acc_, rhs);
            break;
        }
        case Op::Mul: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kInt32Mul, acc_, rhs);
            break;
        }
        case Op::Div: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kInt32Div, acc_, rhs);
            break;
        }
        case Op::Mod: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kInt32Mod, acc_, rhs);
            break;
        }

        // ----- Binary with constant -----
        case Op::AddConst: {
            // For now, just create a constant node and use Int32Add.
            Node* rhs = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                             Type::Any(), {});
            acc_ = NewInt32Binop(Opcode::kInt32Add, acc_, rhs);
            break;
        }
        case Op::SubConst: {
            Node* rhs = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                             Type::Any(), {});
            acc_ = NewInt32Binop(Opcode::kInt32Sub, acc_, rhs);
            break;
        }
        case Op::MulConst: {
            Node* rhs = graph_->NewPureNode(Opcode::kConstant, NodeProp::kPure,
                                             Type::Any(), {});
            acc_ = NewInt32Binop(Opcode::kInt32Mul, acc_, rhs);
            break;
        }
        case Op::AddSmiConst: {
            uint8_t imm = bc[i + 1];
            Node* rhs = NewSmiConstant(imm);
            acc_ = NewInt32Binop(Opcode::kInt32Add, acc_, rhs);
            break;
        }
        case Op::SubSmiConst: {
            uint8_t imm = bc[i + 1];
            Node* rhs = NewSmiConstant(imm);
            acc_ = NewInt32Binop(Opcode::kInt32Sub, acc_, rhs);
            break;
        }

        // ----- Bitwise ops -----
        case Op::BitAnd: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kBitwiseAnd, acc_, rhs);
            break;
        }
        case Op::BitOr: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kBitwiseOr, acc_, rhs);
            break;
        }
        case Op::BitXor: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kBitwiseXor, acc_, rhs);
            break;
        }
        case Op::Shl: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kShiftLeft, acc_, rhs);
            break;
        }
        case Op::Shr: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kShiftRight, acc_, rhs);
            break;
        }
        case Op::Ushr: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewInt32Binop(Opcode::kShiftRightLogical, acc_, rhs);
            break;
        }

        // ----- Comparisons -----
        case Op::TestLessThan: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewComparison(Opcode::kInt32LessThan, acc_, rhs);
            break;
        }
        case Op::TestGreaterThan: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            // a > b is equivalent to b < a
            acc_ = NewComparison(Opcode::kInt32LessThan, rhs, acc_);
            break;
        }
        case Op::TestLessThanOrEqual: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewComparison(Opcode::kInt32LessThanOrEqual, acc_, rhs);
            break;
        }
        case Op::TestGreaterThanOrEqual: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            // a >= b is equivalent to b <= a
            acc_ = NewComparison(Opcode::kInt32LessThanOrEqual, rhs, acc_);
            break;
        }
        case Op::TestEqual: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewComparison(Opcode::kWord32Equal, acc_, rhs);
            break;
        }
        case Op::TestNotEqual: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            // NotEqual = !Equal — we represent this as Equal + BitwiseNot
            Node* eq = NewComparison(Opcode::kWord32Equal, acc_, rhs);
            acc_ = graph_->NewNode1(Opcode::kBitwiseNot, NodeProp::kPure,
                                     Type::Boolean(), control_, nullptr, eq);
            break;
        }
        case Op::TestEqStrict: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            acc_ = NewComparison(Opcode::kWord32Equal, acc_, rhs);
            break;
        }
        case Op::TestNotEqStrict: {
            uint8_t r = bc[i + 1];
            Node* rhs = GetReg(r);
            if (rhs == nullptr) break;
            Node* eq = NewComparison(Opcode::kWord32Equal, acc_, rhs);
            acc_ = graph_->NewNode1(Opcode::kBitwiseNot, NodeProp::kPure,
                                     Type::Boolean(), control_, nullptr, eq);
            break;
        }

        // ----- Control flow -----
        case Op::Jump: {
            // Forward jump — for now, we don't build a full CFG.
            // Just mark the graph as incomplete (control flow not modeled).
            uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                              (static_cast<uint32_t>(bc[i+2]) << 8) |
                              (static_cast<uint32_t>(bc[i+3]) << 16) |
                              (static_cast<uint32_t>(bc[i+4]) << 24);
            // TODO: build Merge node at target.
            break;
        }
        case Op::JumpLoop: {
            // Backward jump (loop) — mark incomplete for now.
            // TODO: build Loop node at target.
            break;
        }
        case Op::JumpIfTrue: {
            // Branch on acc being truthy.
            // TODO: build Branch node.
            break;
        }
        case Op::JumpIfFalse: {
            // Branch on acc being falsy.
            // TODO: build Branch node.
            break;
        }

        // ----- Returns -----
        case Op::Return: {
            Node* ret = graph_->NewNode1(Opcode::kReturn,
                                          NodeProp::kControl,
                                          Type::None(),
                                          control_, effect_, acc_);
            // After Return, control is dead (unreachable).
            control_ = nullptr;
            effect_ = nullptr;
            break;
        }
        case Op::ReturnUndefined: {
            Node* undef = graph_->NewPureNode(Opcode::kConstant,
                                               NodeProp::kPure,
                                               Type::Undefined(), {});
            Node* ret = graph_->NewNode1(Opcode::kReturn,
                                          NodeProp::kControl,
                                          Type::None(),
                                          control_, effect_, undef);
            control_ = nullptr;
            effect_ = nullptr;
            break;
        }

        // ----- Increment / Decrement -----
        case Op::Inc: {
            Node* one = NewSmiConstant(1);
            acc_ = NewInt32Binop(Opcode::kInt32Add, acc_, one);
            break;
        }
        case Op::Dec: {
            Node* one = NewSmiConstant(1);
            acc_ = NewInt32Binop(Opcode::kInt32Sub, acc_, one);
            break;
        }
        case Op::IncReg: {
            uint8_t r = bc[i + 1];
            Node* val = GetReg(r);
            if (val == nullptr) break;
            Node* one = NewSmiConstant(1);
            Node* result = NewInt32Binop(Opcode::kInt32Add, val, one);
            SetReg(r, result);
            acc_ = result;
            break;
        }
        case Op::DecReg: {
            uint8_t r = bc[i + 1];
            Node* val = GetReg(r);
            if (val == nullptr) break;
            Node* one = NewSmiConstant(1);
            Node* result = NewInt32Binop(Opcode::kInt32Sub, val, one);
            SetReg(r, result);
            acc_ = result;
            break;
        }

        case Op::Nop:
            break;

        default:
            // Unsupported opcode.
            SkipInstruction(op);
            break;
    }

    return static_cast<int>(i + oi.length);
}

}  // namespace v12
