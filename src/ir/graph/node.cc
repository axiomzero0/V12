// =============================================================================
// src/ir/graph/node.cc
// =============================================================================

#include "ir/graph/node.h"

#include "ir/graph/graph.h"

namespace v12 {

Node::Node(Graph* graph, Opcode op, uint32_t bitset, Type type,
           Node* control, Node* effect, Inputs value_inputs)
    : graph_(graph), id_(0), op_(op), bitset_(bitset), type_(type) {
    // Build the input list. The convention is:
    //   [control?, effect?, value_inputs...]
    if (control != nullptr) {
        inputs_.push_back(control);
        control->AddUse(this);
    }
    if (effect != nullptr) {
        inputs_.push_back(effect);
        effect->AddUse(this);
    }
    for (Node* in : value_inputs) {
        if (in == nullptr) continue;
        inputs_.push_back(in);
        in->AddUse(this);
    }
}

void Node::SetInput(int i, Node* new_input) {
    V12_DCHECK(i >= 0 && i < input_count(), "input index out of range");
    Node* old = inputs_[i];
    if (old != nullptr) old->RemoveUse(this);
    inputs_[i] = new_input;
    if (new_input != nullptr) new_input->AddUse(this);
}

void Node::AppendInput(Node* new_input) {
    if (new_input == nullptr) return;
    inputs_.push_back(new_input);
    new_input->AddUse(this);
}

void Node::InsertInput(int i, Node* new_input) {
    V12_DCHECK(i >= 0 && i <= input_count(), "input index out of range");
    if (new_input == nullptr) return;
    inputs_.insert(inputs_.begin() + i, new_input);
    new_input->AddUse(this);
}

void Node::RemoveInput(int i) {
    V12_DCHECK(i >= 0 && i < input_count(), "input index out of range");
    Node* old = inputs_[i];
    if (old != nullptr) old->RemoveUse(this);
    inputs_.erase(inputs_.begin() + i);
}

void Node::RemoveUse(Node* n) {
    for (size_t i = 0; i < uses_.size(); ++i) {
        if (uses_[i] == n) {
            uses_[i] = uses_.back();
            uses_.pop_back();
            return;
        }
    }
    V12_DCHECK(false, "RemoveUse: node not in use list");
}

void Node::ReplaceAllUsesWith(Node* other) {
    while (!uses_.empty()) {
        Node* use = uses_.back();
        uses_.pop_back();
        for (int i = 0; i < use->input_count(); ++i) {
            if (use->input(i) == this) {
                use->inputs_[i] = other;
                other->AddUse(use);
            }
        }
    }
}

void Node::Kill() {
    // Detach from all inputs.
    for (int i = 0; i < input_count(); ++i) {
        if (inputs_[i] != nullptr) inputs_[i]->RemoveUse(this);
    }
    inputs_.clear();
    V12_CHECK(uses_.empty(), "Killing a node with remaining uses");
    SetProp(NodeProp::kKilled);
}

void Node::Dump() const {
    std::fprintf(stderr, "N%u: %s\n", id_, op_name());
}

// Opcode names - keep in sync with the Opcode enum.
namespace {
const char* const kOpcodeNames[] = {
    "Start", "End", "Branch", "Switch", "Merge", "Loop", "LoopExit",
    "Return", "Unreachable", "Throw",
    "Parameter", "Constant", "Phi", "EffectPhi", "ControlPhi",
    "Int32Constant", "Int64Constant",
    "Int32Add", "Int32Sub", "Int32Mul", "Int32Div", "Int32Mod",
    "Int32Neg", "Int32Abs",
    "ShiftLeft", "ShiftRight", "ShiftRightLogical",
    "BitwiseAnd", "BitwiseOr", "BitwiseXor", "BitwiseNot",
    "Float64Constant",
    "Float64Add", "Float64Sub", "Float64Mul", "Float64Div", "Float64Mod",
    "Float64Neg", "Float64Abs", "Float64Sqrt",
    "Float64Floor", "Float64Ceil", "Float64Trunc", "Float64Round",
    "ChangeInt32ToFloat64", "ChangeFloat64ToInt32",
    "ChangeTaggedToFloat64", "ChangeTaggedToInt32", "ChangeTaggedToUint32",
    "ChangeInt32ToTagged", "ChangeFloat64ToTagged",
    "TruncateFloat64ToInt32",
    "CheckedTaggedToInt32", "CheckedTaggedToFloat64",
    "Word32Equal", "Word64Equal",
    "Int32LessThan", "Int32LessThanOrEqual",
    "Uint32LessThan", "Uint32LessThanOrEqual",
    "Float64Equal", "Float64LessThan", "Float64LessThanOrEqual",
    "JSAdd", "JSSub", "JSMul", "JSDiv", "JSMod", "JSExp",
    "JSBitwiseAnd", "JSBitwiseOr", "JSBitwiseXor",
    "JSShiftLeft", "JSShiftRight", "JSShiftRightLogical",
    "JSNegate", "JSBitwiseNot", "JSIncrement", "JSDecrement",
    "JSEqual", "JSNotEqual", "JSStrictEqual", "JSStrictNotEqual",
    "JSLessThan", "JSGreaterThan", "JSLessThanOrEqual", "JSGreaterThanOrEqual",
    "LoadField", "StoreField", "LoadElement", "StoreElement",
    "LoadProperty", "StoreProperty", "CheckShape", "CheckMaps",
    "AllocateObject", "AllocateArray", "CheckBounds", "LoadLength",
    "Call", "CallJS", "CallBuiltin", "TailCall",
    "JSCall", "JSConstruct",
    "JSLoadProperty", "JSStoreProperty",
    "JSLoadGlobal", "JSStoreGlobal",
    "JSCreateClosure", "JSCreateObject", "JSCreateArray",
    "JSToBoolean", "JSToNumber", "JSToString", "JSToObject",
    "JSForInPrepare", "JSForInNext", "JSYield", "JSAwait",
    "CheckSmi", "CheckNumber", "CheckString", "CheckHeapObject",
    "CheckJSArray", "CheckFloat64Hole", "CheckNotTaggedHole", "CheckEquals",
    "DeoptimizeIf", "DeoptimizeUnless", "Deoptimize",
    "Load", "Store", "ProtectedLoad", "ProtectedStore",
    "FrameState", "StateValues",
    "ExternalConstant", "NumberConstant", "HeapConstant", "OsrValue",
    "DebugBreak", "Unreachable2", "Dead", "Prologue",
};
static_assert(sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0]) ==
              static_cast<size_t>(Opcode::kCount),
              "kOpcodeNames size must match Opcode::kCount");
}  // namespace

const char* OpcodeName(Opcode op) {
    size_t i = static_cast<size_t>(op);
    V12_DCHECK(i < static_cast<size_t>(Opcode::kCount), "invalid opcode");
    return kOpcodeNames[i];
}

}  // namespace v12
