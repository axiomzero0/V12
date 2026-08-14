// =============================================================================
// src/contracts/target-description.h
// =============================================================================
// TargetDescription: describes the target architecture's register file,
// calling convention, and other properties needed by the lowering and
// regalloc layers.
//
// This is intentionally a small, data-only struct. Concrete target
// implementations (x64, arm64) construct one and pass it to lowering/RA.

#ifndef V12_CONTRACTS_TARGET_DESCRIPTION_H_
#define V12_CONTRACTS_TARGET_DESCRIPTION_H_

#include <cstdint>

#include "base/macros.h"
#include "base/small-vector.h"
#include "contracts/machine-ir.h"

namespace v12 {

class TargetDescription {
public:
    virtual ~TargetDescription() = default;

    // Architecture name ("x64", "arm64", "riscv64").
    virtual const char* arch_name() const = 0;

    // Pointer size in bytes (4 or 8).
    virtual uint32_t pointer_size() const = 0;

    // Big-endian?
    virtual bool is_big_endian() const = 0;

    // General-purpose registers.
    virtual uint32_t num_general_registers() const = 0;
    virtual PhysicalRegister GeneralRegister(uint32_t index) const = 0;
    virtual const char* GeneralRegisterName(PhysicalRegister r) const = 0;

    // Float / vector registers.
    virtual uint32_t num_float_registers() const = 0;
    virtual PhysicalRegister FloatRegister(uint32_t index) const = 0;
    virtual const char* FloatRegisterName(PhysicalRegister r) const = 0;

    // Caller-saved registers (clobbered by calls).
    virtual const SmallVector<PhysicalRegister, 16>& caller_saved() const = 0;

    // Callee-saved registers (preserved across calls).
    virtual const SmallVector<PhysicalRegister, 16>& callee_saved() const = 0;

    // Argument registers (in order).
    virtual const SmallVector<PhysicalRegister, 8>& argument_registers() const = 0;

    // Float argument registers.
    virtual const SmallVector<PhysicalRegister, 8>& float_argument_registers() const = 0;

    // Return register.
    virtual PhysicalRegister return_register() const = 0;
    virtual PhysicalRegister float_return_register() const = 0;

    // Stack pointer.
    virtual PhysicalRegister stack_pointer() const = 0;
    // Frame pointer.
    virtual PhysicalRegister frame_pointer() const = 0;

    // Callee-saved set membership test.
    virtual bool IsCalleeSaved(PhysicalRegister r) const = 0;
    virtual bool IsCallerSaved(PhysicalRegister r) const = 0;

    // Default alignment for stack slots.
    virtual uint32_t stack_slot_alignment() const = 0;

    // Calling convention name ("SystemV AMD64", "Win64", "AArch64 AAPCS").
    virtual const char* calling_convention_name() const = 0;
};

// Get the host target description.
const TargetDescription* GetHostTargetDescription();

}  // namespace v12

#endif  // V12_CONTRACTS_TARGET_DESCRIPTION_H_
