// =============================================================================
// src/contracts/code-object.h
// =============================================================================
// CodeObject: the final output of the codegen pipeline.
//
// A CodeObject is:
//   - A buffer of executable machine code (in executable memory).
//   - A list of relocations (for embedded pointers that need patching).
//   - A list of safepoints (for GC).
//   - A list of deopt points (for speculative optimizations).
//   - A list of stack maps (for GC root scanning).
//   - Metadata: source function, size, instruction count.
//
// The CodeObject is what the JIT installs into the CodeCache and what the
// interpreter calls into when tiering up.

#ifndef V12_CONTRACTS_CODE_OBJECT_H_
#define V12_CONTRACTS_CODE_OBJECT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

class FunctionInfo;

// Relocation entry: an offset in the code that needs to be patched at link time.
struct Relocation {
    enum class Kind : uint8_t {
        kAbsolute64,    // patch with absolute 64-bit address
        kRelative32,    // patch with relative 32-bit offset
        kExternalSymbol,// patch with external symbol address
    };
    Kind kind;
    uint32_t code_offset;        // byte offset in the code buffer
    uint32_t symbol_id;          // for kExternalSymbol
};

// Safepoint: a PC where the GC may run. At a safepoint, the GC needs to know
// which registers and stack slots contain live pointers.
struct Safepoint {
    uint32_t code_offset;
    uint32_t stack_bitmap_offset;   // index into the stack map table
    SmallVector<uint16_t, 8> live_pointer_registers;
    SmallVector<uint16_t, 8> live_pointer_stack_slots;
    uint32_t deopt_id;              // associated deopt point, if any
};

// Deopt point: a PC where we may bail out to the interpreter.
struct DeoptPoint {
    uint32_t code_offset;
    uint32_t frame_state_id;
    uint32_t bytecode_offset;
};

// Stack map: GC root scanning info.
struct StackMap {
    uint32_t safepoint_id;
    uint32_t frame_size;
    SmallVector<bool, 16> stack_slot_is_pointer;
};

class CodeObject {
public:
    CodeObject() = default;

    // The code buffer. This is NOT executable memory; the caller must copy
    // it into executable memory (or use ExecutableMemory).
    const std::vector<uint8_t>& code() const { return code_; }
    std::vector<uint8_t>& mutable_code() { return code_; }

    void SetCode(std::vector<uint8_t> code) { code_ = std::move(code); }
    void SetEntryPointOffset(uint32_t off) { entry_offset_ = off; }

    // Entry point: the address to call to invoke this code.
    // The CodeObject must be installed in executable memory before this is called.
    void* entry_point() const {
        V12_CHECK(executable_memory_ != nullptr, "CodeObject not installed");
        return static_cast<uint8_t*>(executable_memory_) + entry_offset_;
    }

    void set_executable_memory(void* mem) { executable_memory_ = mem; }
    void* executable_memory() const { return executable_memory_; }

    // Relocations, safepoints, deopt points, stack maps
    std::vector<Relocation>& relocations() { return relocations_; }
    std::vector<Safepoint>& safepoints() { return safepoints_; }
    std::vector<DeoptPoint>& deopt_points() { return deopt_points_; }
    std::vector<StackMap>& stack_maps() { return stack_maps_; }

    const std::vector<Relocation>& relocations() const { return relocations_; }
    const std::vector<Safepoint>& safepoints() const { return safepoints_; }
    const std::vector<DeoptPoint>& deopt_points() const { return deopt_points_; }
    const std::vector<StackMap>& stack_maps() const { return stack_maps_; }

    void AddRelocation(Relocation r) { relocations_.push_back(r); }
    void AddSafepoint(Safepoint s) { safepoints_.push_back(s); }
    void AddDeoptPoint(DeoptPoint d) { deopt_points_.push_back(d); }
    void AddStackMap(StackMap s) { stack_maps_.push_back(s); }

    // Source function (for stack traces).
    void set_source_function(FunctionInfo* f) { source_function_ = f; }
    FunctionInfo* source_function() const { return source_function_; }

    // Tier: which tier produced this code.
    enum class Tier : uint8_t {
        kInterpreter,
        kBaseline,
        kOptimizing,
    };
    Tier tier() const { return tier_; }
    void set_tier(Tier t) { tier_ = t; }

    // Statistics
    uint32_t instruction_count() const { return instruction_count_; }
    void set_instruction_count(uint32_t c) { instruction_count_ = c; }

    size_t code_size() const { return code_.size(); }

private:
    std::vector<uint8_t> code_;
    uint32_t entry_offset_ = 0;
    void* executable_memory_ = nullptr;
    std::vector<Relocation> relocations_;
    std::vector<Safepoint> safepoints_;
    std::vector<DeoptPoint> deopt_points_;
    std::vector<StackMap> stack_maps_;
    FunctionInfo* source_function_ = nullptr;
    Tier tier_ = Tier::kInterpreter;
    uint32_t instruction_count_ = 0;
};

}  // namespace v12

#endif  // V12_CONTRACTS_CODE_OBJECT_H_
