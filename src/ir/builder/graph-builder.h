// =============================================================================
// src/ir/builder/graph-builder.h
// =============================================================================
// GraphBuilder: converts a FunctionInfo's bytecode into a Sea-of-Nodes IR
// Graph. This is the first step in the optimizing pipeline:
//
//   bytecode → GraphBuilder → Graph → [optimization passes] → lowering → code
//
// The builder walks the bytecode linearly, maintaining:
//   - A per-register " SSA value" stack (the current IR Node for each reg)
//   - The current control node (Start, Merge, Loop, etc.)
//   - The current effect node (for memory operations)
//   - Pending branch/merge targets for forward jumps
//
// Supported opcodes (the builder deopts gracefully on unsupported opcodes
// by leaving the graph incomplete — the caller can check IsComplete()):
//   - LdaSmi, LdaSmi16, LdaZero, LdaConst
//   - Ldar, Star, Mov
//   - Add, Sub, Mul, Div (with Smi fast path → Int32 ops)
//   - AddConst, SubConst, AddSmiConst, SubSmiConst
//   - BitAnd, BitOr, BitXor, Shl, Shr, Ushr
//   - TestLessThan, TestGreaterThan, TestLessThanOrEqual, TestGreaterThanOrEqual
//   - TestEqual, TestNotEqual, TestEqStrict, TestNotEqStrict
//   - Jump, JumpIfTrue, JumpIfFalse, JumpLoop
//   - Return, ReturnUndefined
//
// Unsupported opcodes (Call, CallProperty, LoadProperty, etc.) cause the
// builder to mark the graph as incomplete. The caller should then skip
// optimization and fall back to the interpreter or baseline JIT.

#ifndef V12_IR_BUILDER_GRAPH_BUILDER_H_
#define V12_IR_BUILDER_GRAPH_BUILDER_H_

#include <cstdint>
#include <vector>

#include "base/arena.h"
#include "frontend/bytecode/bytecode.h"
#include "ir/graph/graph.h"

namespace v12 {

class FunctionInfo;

class GraphBuilder {
public:
    GraphBuilder(Arena* arena, FunctionInfo* fi);

    // Build the IR graph from the FunctionInfo's bytecode.
    // Returns the Graph (owned by the Arena). Check IsComplete() to see
    // if all opcodes were successfully converted.
    Graph* Build();

    // Returns true if all opcodes in the bytecode were successfully
    // converted to IR nodes. False if any opcode was unsupported.
    bool IsComplete() const { return complete_; }

    // Number of opcodes that were skipped (unsupported).
    int SkippedCount() const { return skipped_; }

private:
    Arena* arena_;
    FunctionInfo* fi_;
    Graph* graph_ = nullptr;

    // Current SSA value for each register slot. Indexed by register number.
    // nullptr means the register hasn't been written yet (undefined).
    std::vector<Node*> reg_values_;

    // Current control node (Start, Merge, Loop, or a Branch target).
    Node* control_ = nullptr;

    // Current effect node (for memory ordering). Start node initially.
    Node* effect_ = nullptr;

    // The accumulator's current IR value.
    Node* acc_ = nullptr;

    // Whether all opcodes were converted.
    bool complete_ = true;
    int skipped_ = 0;

    // ----- Helpers -----

    // Get/set the current value of a register.
    Node* GetReg(uint8_t reg) {
        if (reg < reg_values_.size()) return reg_values_[reg];
        return nullptr;
    }
    void SetReg(uint8_t reg, Node* val) {
        if (reg >= reg_values_.size()) reg_values_.resize(reg + 1, nullptr);
        reg_values_[reg] = val;
    }

    // Create a constant node for a Smi value.
    Node* NewSmiConstant(int64_t value);

    // Create an Int32 binary operation node.
    Node* NewInt32Binop(Opcode op, Node* lhs, Node* rhs);

    // Create a comparison node (returns Boolean type).
    Node* NewComparison(Opcode op, Node* lhs, Node* rhs);

    // Process a single bytecode instruction at offset `i`.
    // Returns the offset of the next instruction (i + instruction_length).
    // Returns -1 if the instruction is unsupported.
    int ProcessInstruction(size_t i);

    // Skip an unsupported instruction (NOP it out in the IR).
    void SkipInstruction(Op op);
};

}  // namespace v12

#endif  // V12_IR_BUILDER_GRAPH_BUILDER_H_
