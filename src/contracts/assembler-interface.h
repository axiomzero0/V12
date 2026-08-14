// =============================================================================
// src/contracts/assembler-interface.h
// =============================================================================
// The machine-code emitter interface.
//
// This is the CONTRACT that any external assembler library (asmjit, Xbyak,
// DynASM) must implement (through an adapter) to plug into our compiler.
//
// The flow:
//   1. After register allocation, the MachineFunction has all VRegs assigned
//      to physical registers or spill slots.
//   2. The code emitter walks the MachineFunction and translates each
//      MachineInstruction into a call to the MachineEmitter interface.
//   3. The MachineEmitter interface is target-specific but library-agnostic.
//      The concrete adapter (e.g. AsmJitEmitter) implements these methods
//      by calling into the underlying library.
//   4. The output is a CodeObject: a buffer of raw machine code with
//      relocation entries and metadata (safepoints, deopt info, stack maps).
//
// The interface is intentionally low-level: the emitter is a stateful
// "instruction printer" that knows about registers, immediates, and memory
// operands but nothing about IR or JS semantics. This is the only place
// in the codebase that knows about specific ISAs.

#ifndef V12_CONTRACTS_ASSEMBLER_INTERFACE_H_
#define V12_CONTRACTS_ASSEMBLER_INTERFACE_H_

#include <cstdint>
#include <memory>

#include "base/macros.h"
#include "base/small-vector.h"
#include "contracts/machine-ir.h"

namespace v12 {

class CodeObject;
class TargetDescription;

// The abstract emitter. Each target (x64, arm64) implements this.
// Each library adapter (asmjit, Xbyak) implements this via a concrete class.
class MachineEmitter {
public:
    virtual ~MachineEmitter() = default;

    // Finalize and return the emitted code.
    virtual std::unique_ptr<CodeObject> Finalize() = 0;

    // ----- Target queries -----
    virtual const TargetDescription* target() const = 0;
    virtual bool SupportsSimd() const { return false; }
    virtual bool SupportsAvx() const { return false; }

    // ----- Basic block management -----
    virtual void BindLabel(uint32_t block_id) = 0;
    virtual uint32_t CurrentOffset() const = 0;

    // ----- Control flow -----
    virtual void Jump(uint32_t target_block_id) = 0;
    virtual void Branch(Cond cond, uint32_t target_block_id) = 0;
    virtual void Return() = 0;
    virtual void Call(MachineOperand target) = 0;
    virtual void TailCall(MachineOperand target) = 0;
    virtual void Trap() = 0;
    virtual void DebugBreak() = 0;

    // ----- Data movement -----
    virtual void Move(MachineOperand dst, MachineOperand src) = 0;
    virtual void Load(MachineOperand dst, MachineOperand mem) = 0;
    virtual void Store(MachineOperand mem, MachineOperand src) = 0;

    // ----- Arithmetic -----
    virtual void Add(MachineOperand dst, MachineOperand src) = 0;
    virtual void Sub(MachineOperand dst, MachineOperand src) = 0;
    virtual void Mul(MachineOperand dst, MachineOperand src) = 0;
    virtual void Div(MachineOperand dst, MachineOperand src) = 0;
    virtual void Mod(MachineOperand dst, MachineOperand src) = 0;
    virtual void And(MachineOperand dst, MachineOperand src) = 0;
    virtual void Or(MachineOperand dst, MachineOperand src) = 0;
    virtual void Xor(MachineOperand dst, MachineOperand src) = 0;
    virtual void Not(MachineOperand dst) = 0;
    virtual void Neg(MachineOperand dst) = 0;
    virtual void Shl(MachineOperand dst, MachineOperand count) = 0;
    virtual void Shr(MachineOperand dst, MachineOperand count) = 0;
    virtual void Sar(MachineOperand dst, MachineOperand count) = 0;

    // ----- Comparison -----
    virtual void Cmp(MachineOperand a, MachineOperand b) = 0;
    virtual void Test(MachineOperand a, MachineOperand b) = 0;
    virtual void Setcc(Cond cond, MachineOperand dst) = 0;
    virtual void Cmov(Cond cond, MachineOperand dst, MachineOperand src) = 0;

    // ----- Float ops -----
    virtual void FloatAdd(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatSub(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatMul(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatDiv(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatSqrt(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatAbs(MachineOperand dst) = 0;
    virtual void FloatNeg(MachineOperand dst) = 0;

    // ----- Conversions -----
    virtual void IntToFloat(MachineOperand dst, MachineOperand src) = 0;
    virtual void FloatToInt(MachineOperand dst, MachineOperand src) = 0;

    // ----- Stack -----
    virtual void Push(MachineOperand src) = 0;
    virtual void Pop(MachineOperand dst) = 0;
    virtual void AlignStack(uint32_t alignment) = 0;

    // ----- Inline cache / safepoint -----
    virtual void RecordSafepoint(uint32_t safepoint_id) = 0;
    virtual void RecordDeoptPoint(uint32_t deopt_id) = 0;
    virtual void RecordRelocation(uint32_t offset, uint32_t symbol_id) = 0;

    // ----- Raw bytes (escape hatch) -----
    virtual void EmitBytes(const uint8_t* bytes, size_t length) = 0;

    // ----- Diagnostics -----
    virtual void DisassembleAt(uint32_t offset, uint32_t length) = 0;
};

// Factory: creates an emitter for the host architecture using the configured
// adapter (asmjit/Xbyak/etc). Returns nullptr if no emitter is available.
std::unique_ptr<MachineEmitter> CreateHostEmitter(const TargetDescription* target);

}  // namespace v12

#endif  // V12_CONTRACTS_ASSEMBLER_INTERFACE_H_
