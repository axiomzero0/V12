// =============================================================================
// src/ir/lowering/lowering.h
// =============================================================================
// Lowering pass: converts a Sea-of-Nodes IR Graph into a MachineFunction.
//
// This is the bridge between the high-level IR (Nodes with types) and the
// low-level Machine IR (VRegs, MachineInstructions, MachineBlocks).
//
// Currently handles straight-line code (no branches/loops). The IR builder
// marks graphs with control flow as incomplete; the lowering pass only
// processes complete graphs.
//
// Node → MachineInstruction mapping:
//   Int32Constant  → Move(imm) into a fresh VReg
//   Int32Add       → Add(vreg_lhs, vreg_rhs) → vreg_result
//   Int32Sub       → Sub(...)
//   Int32Mul       → Mul(...)
//   Int32Div       → Div(...)
//   BitwiseAnd/Or/Xor → And/Or/Xor(...)
//   ShiftLeft/Right → Shl/Shr(...)
//   Word32Equal    → Cmp + Setcc(kEqual)
//   Int32LessThan  → Cmp + Setcc(kLessThan)
//   Return         → Return(vreg_acc)
//   Start          → entry block (no instruction)

#ifndef V12_IR_LOWERING_LOWERING_H_
#define V12_IR_LOWERING_LOWERING_H_

#include "contracts/machine-ir.h"
#include "contracts/target-description.h"
#include "ir/graph/graph.h"

namespace v12 {

class FunctionInfo;

// Result of lowering a Graph to a MachineFunction.
struct LoweringResult {
    std::unique_ptr<MachineFunction> function;
    bool success = false;  // true if all nodes were lowered
    int lowered_count = 0; // number of IR nodes lowered
    int skipped_count = 0; // number of IR nodes skipped (unsupported)
};

// Lower a Graph to a MachineFunction.
// `target` describes the target architecture (use GetHostTargetDescription()).
// `fi` is the source FunctionInfo (for register count, parameter count, etc.).
LoweringResult LowerGraph(Graph* g, const TargetDescription* target,
                          FunctionInfo* fi);

}  // namespace v12

#endif  // V12_IR_LOWERING_LOWERING_H_
