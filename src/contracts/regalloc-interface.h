// =============================================================================
// src/contracts/regalloc-interface.h
// =============================================================================
// The register allocator interface.
//
// This is the CONTRACT that any external regalloc library must implement
// (through an adapter) to plug into our compiler.
//
// The flow:
//   1. Lowering produces a MachineFunction with VRegs and MachineInstructions.
//   2. Caller invokes RegisterAllocator::Allocate(mf).
//   3. The adapter translates MachineFunction into the external library's
//      representation.
//   4. The external library computes an allocation.
//   5. The adapter translates the allocation back into an AllocationResult
//      (our representation).
//   6. The code emitter consumes AllocationResult to emit machine code with
//      physical registers.
//
// The adapter itself is in src/codegen/regalloc-adapter.h.

#ifndef V12_CONTRACTS_REGALLOC_INTERFACE_H_
#define V12_CONTRACTS_REGALLOC_INTERFACE_H_

#include <memory>

#include "base/macros.h"
#include "contracts/machine-ir.h"

namespace v12 {

class TargetDescription;

// Abstract interface. Concrete implementations live in adapters.
class RegisterAllocator {
public:
    virtual ~RegisterAllocator() = default;

    // Allocate registers for `mf` and return the assignment.
    // Returns nullptr on failure (e.g. irreducible CFG that the RA can't handle).
    virtual std::unique_ptr<AllocationResult> Allocate(
        MachineFunction* mf, const TargetDescription* target) = 0;

    // Human-readable name for diagnostics ("linear-scan", "graph-coloring", etc.).
    virtual const char* Name() const = 0;

    // Optional: provide a hint about expected runtime. Used by the tiering
    // policy to decide whether to invoke a fast or slow RA.
    virtual bool IsFast() const { return false; }
};

}  // namespace v12

#endif  // V12_CONTRACTS_REGALLOC_INTERFACE_H_
