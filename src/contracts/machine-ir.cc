// =============================================================================
// src/contracts/machine-ir.cc
// =============================================================================

#include "contracts/machine-ir.h"

#include <algorithm>
#include <functional>
#include <queue>

namespace v12 {

const char* RepName(Rep r) {
    switch (r) {
        case Rep::kNone:          return "None";
        case Rep::kTagged:        return "Tagged";
        case Rep::kTaggedSigned:  return "TaggedSigned";
        case Rep::kTaggedPointer: return "TaggedPointer";
        case Rep::kInt32:         return "Int32";
        case Rep::kUint32:        return "Uint32";
        case Rep::kInt64:         return "Int64";
        case Rep::kFloat32:       return "Float32";
        case Rep::kFloat64:       return "Float64";
        case Rep::kSimd128:       return "Simd128";
    }
    return "<unknown>";
}

const char* MachOpName(MachOp op) {
    switch (op) {
        case MachOp::kNop:        return "nop";
        case MachOp::kLabel:      return "label";
        case MachOp::kCall:       return "call";
        case MachOp::kTailCall:   return "tailcall";
        case MachOp::kReturn:     return "ret";
        case MachOp::kJump:       return "jmp";
        case MachOp::kBranch:     return "br";
        case MachOp::kBranchTable:return "br_table";
        case MachOp::kMove:       return "mov";
        case MachOp::kLoad:       return "load";
        case MachOp::kStore:      return "store";
        case MachOp::kPush:       return "push";
        case MachOp::kPop:        return "pop";
        case MachOp::kStackAlloc: return "stackalloc";
        case MachOp::kAdd:        return "add";
        case MachOp::kSub:        return "sub";
        case MachOp::kMul:        return "mul";
        case MachOp::kDiv:        return "div";
        case MachOp::kMod:        return "mod";
        case MachOp::kAnd:        return "and";
        case MachOp::kOr:         return "or";
        case MachOp::kXor:        return "xor";
        case MachOp::kNot:        return "not";
        case MachOp::kNeg:        return "neg";
        case MachOp::kShl:        return "shl";
        case MachOp::kShr:        return "shr";
        case MachOp::kSar:        return "sar";
        case MachOp::kRotl:       return "rotl";
        case MachOp::kRotr:       return "rotr";
        case MachOp::kCmp:        return "cmp";
        case MachOp::kTest:       return "test";
        case MachOp::kSetcc:      return "setcc";
        case MachOp::kCmov:       return "cmov";
        case MachOp::kLea:        return "lea";
        case MachOp::kSqrt:       return "sqrt";
        case MachOp::kAbs:        return "abs";
        case MachOp::kNegFloat:   return "negf";
        case MachOp::kIntToFloat: return "int_to_float";
        case MachOp::kFloatToInt: return "float_to_int";
        case MachOp::kFloatToFloat:return "float_to_float";
        case MachOp::kIntToInt:   return "int_to_int";
        case MachOp::kTrap:       return "trap";
        case MachOp::kDebugBreak: return "debugbreak";
        case MachOp::kDeopt:      return "deopt";
        case MachOp::kSafepoint:  return "safepoint";
        case MachOp::kInlineCache:return "ic";
        case MachOp::kAsm:        return "asm";
    }
    return "<unknown>";
}

const char* CondName(Cond c) {
    switch (c) {
        case Cond::kAlways:           return "always";
        case Cond::kEqual:            return "eq";
        case Cond::kNotEqual:         return "ne";
        case Cond::kLessThan:         return "lt";
        case Cond::kLessThanOrEqual:  return "le";
        case Cond::kGreaterThan:      return "gt";
        case Cond::kGreaterThanOrEqual:return "ge";
        case Cond::kBelow:            return "b";
        case Cond::kBelowOrEqual:     return "be";
        case Cond::kAbove:            return "a";
        case Cond::kAboveOrEqual:     return "ae";
        case Cond::kOverflow:         return "o";
        case Cond::kNoOverflow:       return "no";
        case Cond::kSigned:           return "s";
        case Cond::kNotSigned:        return "ns";
        case Cond::kZero:             return "z";
        case Cond::kNotZero:          return "nz";
        case Cond::kParityEven:       return "pe";
        case Cond::kParityOdd:        return "po";
    }
    return "<unknown>";
}

MachineBlock* MachineFunction::NewBlock() {
    auto b = std::make_unique<MachineBlock>(static_cast<uint32_t>(blocks_.size()));
    MachineBlock* raw = b.get();
    blocks_.push_back(std::move(b));
    return raw;
}

void MachineFunction::SetEntryBlock(MachineBlock* b) {
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].get() == b) {
            entry_block_index_ = i;
            b->is_entry = true;
            return;
        }
    }
    V12_FAIL("SetEntryBlock: block not in function");
}

std::vector<MachineBlock*> MachineFunction::ReversePostOrder() const {
    // Standard reverse-post-order traversal of the CFG.
    std::vector<MachineBlock*> order;
    std::vector<bool> visited(blocks_.size(), false);
    // DFS from entry.
    std::function<void(MachineBlock*)> dfs = [&](MachineBlock* b) {
        if (visited[b->id]) return;
        visited[b->id] = true;
        for (MachineBlock* s : b->successors) dfs(s);
        order.push_back(b);
    };
    dfs(EntryBlock());
    std::reverse(order.begin(), order.end());
    return order;
}

}  // namespace v12
