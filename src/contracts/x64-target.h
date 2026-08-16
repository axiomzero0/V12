// =============================================================================
// src/contracts/x64-target.h
// =============================================================================
// Concrete TargetDescription for x86-64 (System V AMD64 ABI).
//
// Describes the 16 general-purpose registers (RAX-R15) and 16 XMM registers
// (XMM0-XMM15), their classification as caller/callee-saved, and the System V
// calling convention (first 6 int args in RDI/RSI/RDX/RCX/R8/R9, first 8 float
// args in XMM0-XMM7, return in RAX/XMM0).

#ifndef V12_CONTRACTS_X64_TARGET_H_
#define V12_CONTRACTS_X64_TARGET_H_

#include "contracts/target-description.h"

namespace v12 {

class X64TargetDescription : public TargetDescription {
public:
    const char* arch_name() const override { return "x64"; }
    uint32_t pointer_size() const override { return 8; }
    bool is_big_endian() const override { return false; }

    // 16 GPRs: RAX, RCX, RDX, RSI, RDI, RBP, RSP, R8-R15
    uint32_t num_general_registers() const override { return 16; }
    PhysicalRegister GeneralRegister(uint32_t index) const override {
        V12_DCHECK(index < 16, "GPR index out of range");
        return {RegKind::kGeneral, static_cast<uint16_t>(index)};
    }
    const char* GeneralRegisterName(PhysicalRegister r) const override;

    // 16 XMM registers
    uint32_t num_float_registers() const override { return 16; }
    PhysicalRegister FloatRegister(uint32_t index) const override {
        V12_DCHECK(index < 16, "XMM index out of range");
        return {RegKind::kFloat, static_cast<uint16_t>(index)};
    }
    const char* FloatRegisterName(PhysicalRegister r) const override;

    const SmallVector<PhysicalRegister, 16>& caller_saved() const override {
        return caller_saved_;
    }
    const SmallVector<PhysicalRegister, 16>& callee_saved() const override {
        return callee_saved_;
    }

    // System V AMD64: RDI, RSI, RDX, RCX, R8, R9
    const SmallVector<PhysicalRegister, 8>& argument_registers() const override {
        return arg_regs_;
    }
    // System V AMD64: XMM0-XMM7
    const SmallVector<PhysicalRegister, 8>& float_argument_registers() const override {
        return float_arg_regs_;
    }

    PhysicalRegister return_register() const override { return {RegKind::kGeneral, 0}; }  // RAX
    PhysicalRegister float_return_register() const override { return {RegKind::kFloat, 0}; }  // XMM0
    PhysicalRegister stack_pointer() const override { return {RegKind::kGeneral, 4}; }  // RSP
    PhysicalRegister frame_pointer() const override { return {RegKind::kGeneral, 5}; }  // RBP

    bool IsCalleeSaved(PhysicalRegister r) const override;
    bool IsCallerSaved(PhysicalRegister r) const override;

    uint32_t stack_slot_alignment() const override { return 16; }

    const char* calling_convention_name() const override { return "SystemV AMD64"; }

private:
    // Register indices (matching the x86-64 encoding):
    // 0=RAX, 1=RCX, 2=RDX, 3=RBX, 4=RSP, 5=RBP, 6=RSI, 7=RDI,
    // 8-15=R8-R15
    static constexpr uint32_t kRAX = 0, kRCX = 1, kRDX = 2, kRBX = 3;
    static constexpr uint32_t kRSP = 4, kRBP = 5, kRSI = 6, kRDI = 7;

    // Caller-saved (volatile): RAX, RCX, RDX, RSI, RDI, R8-R11
    SmallVector<PhysicalRegister, 16> caller_saved_ = {
        {RegKind::kGeneral, kRAX}, {RegKind::kGeneral, kRCX},
        {RegKind::kGeneral, kRDX}, {RegKind::kGeneral, kRSI},
        {RegKind::kGeneral, kRDI}, {RegKind::kGeneral, 8},
        {RegKind::kGeneral, 9}, {RegKind::kGeneral, 10},
        {RegKind::kGeneral, 11},
    };

    // Callee-saved (non-volatile): RBX, RBP, R12-R15
    SmallVector<PhysicalRegister, 16> callee_saved_ = {
        {RegKind::kGeneral, kRBX}, {RegKind::kGeneral, kRBP},
        {RegKind::kGeneral, 12}, {RegKind::kGeneral, 13},
        {RegKind::kGeneral, 14}, {RegKind::kGeneral, 15},
    };

    // Argument registers: RDI, RSI, RDX, RCX, R8, R9
    SmallVector<PhysicalRegister, 8> arg_regs_ = {
        {RegKind::kGeneral, kRDI}, {RegKind::kGeneral, kRSI},
        {RegKind::kGeneral, kRDX}, {RegKind::kGeneral, kRCX},
        {RegKind::kGeneral, 8}, {RegKind::kGeneral, 9},
    };

    // Float argument registers: XMM0-XMM7
    SmallVector<PhysicalRegister, 8> float_arg_regs_ = {
        {RegKind::kFloat, 0}, {RegKind::kFloat, 1}, {RegKind::kFloat, 2},
        {RegKind::kFloat, 3}, {RegKind::kFloat, 4}, {RegKind::kFloat, 5},
        {RegKind::kFloat, 6}, {RegKind::kFloat, 7},
    };
};

}  // namespace v12

#endif  // V12_CONTRACTS_X64_TARGET_H_
