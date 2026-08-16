// =============================================================================
// src/contracts/machine-ir.h
// =============================================================================
// Machine IR: the boundary between the JS-specific optimizer and the
// machine-code libraries (regalloc, assembler).
//
// THIS IS THE CONTRACT. External libraries plug into this. The optimizer
// above lowering/ only sees this representation, never anything from
// asmjit/Xbyak/regalloc libraries.
//
// Design principles:
//   1. The Machine IR is OWNED BY US. We define its types and operations.
//   2. External libraries consume this IR through adapters; they never
//      leak their own types upward.
//   3. The IR is target-aware: it has target-specific register files,
//      calling conventions, and instruction encodings. But it is also
//      abstract enough that a different regalloc can re-interpret it.
//
// Why a separate Machine IR (vs. just using the Sea-of-Nodes IR)?
//   - Sea-of-Nodes is too abstract for register allocation. RA needs:
//       * linear instruction order
//       * explicit virtual registers
//       * explicit operand types (immediate, register, memory)
//       * explicit clobber sets
//   - Maintaining these in the high-level IR would constrain optimization.
//   - So we lower from Sea-of-Nodes to MachineFunction, then run RA on that.

#ifndef V12_CONTRACTS_MACHINE_IR_H_
#define V12_CONTRACTS_MACHINE_IR_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

// Forward declarations
class TargetDescription;
class MachineFunction;
class MachineBlock;
class MachineInstruction;

// -----------------------------------------------------------------------------
// Virtual registers
// -----------------------------------------------------------------------------
// A VirtualRegister is an opaque integer that names a value in the MachineFunction.
// After register allocation, each VirtualRegister maps to either a PhysicalRegister
// or a spill slot.
using VReg = uint32_t;
constexpr VReg kInvalidVReg = 0xFFFFFFFF;

// Representation class - what kind of value lives in a VReg.
enum class Rep : uint8_t {
    kNone,
    kTagged,        // a tagged value (Smi or HeapObject*)
    kTaggedSigned,  // a Smi
    kTaggedPointer, // a HeapObject*
    kInt32,
    kUint32,
    kInt64,
    kFloat32,
    kFloat64,
    kSimd128,
};

const char* RepName(Rep r);

// -----------------------------------------------------------------------------
// Physical registers
// -----------------------------------------------------------------------------
// PhysicalRegister is target-specific. We use a (kind, code) pair to name them
// uniformly across targets. The TargetDescription defines the available codes.
enum class RegKind : uint8_t {
    kGeneral,
    kFloat,
    kVector,
    kMask,        // AVX-512 mask registers
};

struct PhysicalRegister {
    RegKind kind;
    uint16_t code;     // target-specific code (e.g. 0 for RAX on x64)

    bool operator==(PhysicalRegister o) const { return kind == o.kind && code == o.code; }
    bool operator!=(PhysicalRegister o) const { return !(*this == o); }

    static PhysicalRegister None() { return {RegKind::kGeneral, 0xFFFF}; }
    bool IsValid() const { return code != 0xFFFF; }
};

// -----------------------------------------------------------------------------
// Operands
// -----------------------------------------------------------------------------
enum class MachOperandKind : uint8_t {
    kNone,
    kVReg,          // a virtual register
    kPReg,          // a physical register (post-RA, or fixed registers like stack pointer)
    kImmediate,     // an integer immediate
    kFloatImmediate,// a double immediate
    kMemory,        // a memory operand (base + index*scale + disp)
    kLabel,         // a jump target (basic block)
    kSymbol,        // an external symbol (function address, etc.)
};

struct MachineOperand {
    MachOperandKind kind = MachOperandKind::kNone;
    Rep rep = Rep::kNone;

    // Union of all possible payload types.
    union {
        VReg vreg;
        PhysicalRegister preg;
        int64_t imm;
        double fimm;
        uint32_t symbol_id;
        uint32_t block_id;       // for kLabel
    };
    // Memory operand fields (when kind == kMemory).
    VReg base_vreg = kInvalidVReg;
    VReg index_vreg = kInvalidVReg;
    int32_t scale = 0;       // 1, 2, 4, or 8
    int32_t displacement = 0;

    static MachineOperand VRegOf(VReg r, Rep rep = Rep::kTagged) {
        MachineOperand o; o.kind = MachOperandKind::kVReg; o.rep = rep; o.vreg = r; return o;
    }
    static MachineOperand PRegOf(PhysicalRegister p, Rep rep = Rep::kTagged) {
        MachineOperand o; o.kind = MachOperandKind::kPReg; o.rep = rep; o.preg = p; return o;
    }
    static MachineOperand ImmOf(int64_t v, Rep rep = Rep::kInt64) {
        MachineOperand o; o.kind = MachOperandKind::kImmediate; o.rep = rep; o.imm = v; return o;
    }
    static MachineOperand FloatImmOf(double v) {
        MachineOperand o; o.kind = MachOperandKind::kFloatImmediate; o.rep = Rep::kFloat64; o.fimm = v; return o;
    }
    static MachineOperand LabelOf(uint32_t block_id) {
        MachineOperand o; o.kind = MachOperandKind::kLabel; o.block_id = block_id; return o;
    }
    static MachineOperand MemOf(VReg base, int32_t disp, Rep rep = Rep::kTagged) {
        MachineOperand o; o.kind = MachOperandKind::kMemory; o.rep = rep;
        o.base_vreg = base; o.displacement = disp; o.scale = 1;
        return o;
    }
    static MachineOperand SymbolOf(uint32_t id) {
        MachineOperand o; o.kind = MachOperandKind::kSymbol; o.symbol_id = id; return o;
    }

    bool IsVReg() const { return kind == MachOperandKind::kVReg; }
    bool IsPReg() const { return kind == MachOperandKind::kPReg; }
    bool IsImmediate() const { return kind == MachOperandKind::kImmediate; }
    bool IsMemory() const { return kind == MachOperandKind::kMemory; }
    bool IsLabel() const { return kind == MachOperandKind::kLabel; }
    bool IsSymbol() const { return kind == MachOperandKind::kSymbol; }
};

// -----------------------------------------------------------------------------
// Machine opcode
// -----------------------------------------------------------------------------
// We use a single enum that covers all targets. Each target's emitter only
// supports a subset of these. This is similar to LLVM's `TargetOpcode`.
enum class MachOp : uint16_t {
    kNop,
    kLabel,                  // a label / basic block boundary
    kCall,
    kTailCall,
    kReturn,
    kJump,
    kBranch,                 // conditional jump: if (cond) goto target
    kBranchTable,            // switch
    kMove,                   // dst = src
    kLoad,                   // dst = [mem]
    kStore,                  // [mem] = src
    kPush,                   // push src (stack)
    kPop,                    // dst = pop
    kStackAlloc,             // reserve stack space
    kAdd,
    kSub,
    kMul,
    kDiv,
    kMod,
    kAnd,
    kOr,
    kXor,
    kNot,
    kNeg,
    kShl,
    kShr,
    kSar,
    kRotl,
    kRotr,
    kCmp,
    kTest,
    kSetcc,                  // set condition code byte
    kCmov,
    kLea,                    // load effective address
    kSqrt,
    kAbs,
    kNegFloat,
    kIntToFloat,
    kFloatToInt,
    kFloatToFloat,
    kIntToInt,
    kTrap,
    kDebugBreak,
    kDeopt,                  // deoptimize (jump to deopt handler)
    kSafepoint,              // a safepoint poll
    kInlineCache,            // an inline-cache site
    kAsm,                    // raw assembly (escape hatch for target-specific quirks)
};

const char* MachOpName(MachOp op);

// Condition codes (for branches and setcc).
enum class Cond : uint8_t {
    kAlways,
    kEqual,             // ==
    kNotEqual,          // !=
    kLessThan,          // signed <
    kLessThanOrEqual,
    kGreaterThan,
    kGreaterThanOrEqual,
    kBelow,             // unsigned <
    kBelowOrEqual,
    kAbove,
    kAboveOrEqual,
    kOverflow,
    kNoOverflow,
    kSigned,
    kNotSigned,
    kZero,
    kNotZero,
    kParityEven,
    kParityOdd,
};

const char* CondName(Cond c);

// -----------------------------------------------------------------------------
// MachineInstruction
// -----------------------------------------------------------------------------
class MachineInstruction {
public:
    MachOp op = MachOp::kNop;
    Cond cond = Cond::kAlways;
    SmallVector<MachineOperand, 3> operands;
    SmallVector<PhysicalRegister, 2> clobbers;   // physical registers clobbered
    uint32_t block_id = 0;                        // owning block
    uint32_t instruction_id = 0;                  // unique within function
    uint32_t deopt_id = 0;                        // for deopt
    uint32_t safepoint_id = 0;                    // for GC
    bool is_call = false;
    bool can_deoptimize = false;
    bool can_gc = false;
    bool has_safepoint = false;

    MachineInstruction() = default;
    MachineInstruction(MachOp o) : op(o) {}

    void AddOperand(MachineOperand o) { operands.push_back(o); }
    void AddClobber(PhysicalRegister r) { clobbers.push_back(r); }

    // Iterate over all VRegs in operands.
    template <typename F>
    void ForEachVReg(F&& fn) {
        for (auto& o : operands) {
            if (o.IsVReg()) fn(o.vreg, o.rep);
            if (o.IsMemory()) {
                if (o.base_vreg != kInvalidVReg) fn(o.base_vreg, o.rep);
                if (o.index_vreg != kInvalidVReg) fn(o.index_vreg, o.rep);
            }
        }
    }
};

// -----------------------------------------------------------------------------
// MachineBlock
// -----------------------------------------------------------------------------
class MachineBlock {
public:
    uint32_t id;
    SmallVector<MachineBlock*, 2> predecessors;
    SmallVector<MachineBlock*, 2> successors;
    SmallVector<MachineInstruction, 8> instructions;
    bool is_closed = false;       // can't add more instructions
    bool is_entry = false;
    bool is_exit = false;
    uint32_t loop_depth = 0;
    uint32_t loop_header_id = 0;  // 0 = not in a loop

    explicit MachineBlock(uint32_t id_) : id(id_) {}

    void AddInstruction(MachineInstruction&& inst) {
        V12_CHECK(!is_closed, "cannot add instruction to closed block");
        inst.block_id = id;
        instructions.push_back(std::move(inst));
    }

    void AddSuccessor(MachineBlock* succ) {
        successors.push_back(succ);
        succ->predecessors.push_back(this);
    }
};

// -----------------------------------------------------------------------------
// MachineFunction
// -----------------------------------------------------------------------------
class MachineFunction {
public:
    explicit MachineFunction(const TargetDescription* target) : target_(target) {}

    MachineBlock* NewBlock();
    MachineBlock* EntryBlock() const { return blocks_[entry_block_index_].get(); }
    void SetEntryBlock(MachineBlock* b);

    MachineBlock* GetBlock(uint32_t id) const { return blocks_[id].get(); }
    size_t block_count() const { return blocks_.size(); }

    VReg NewVReg(Rep rep = Rep::kTagged) {
        VReg r = next_vreg_++;
        rep_classes_.push_back(rep);
        return r;
    }
    Rep GetRep(VReg r) const { return rep_classes_[r]; }
    uint32_t vreg_count() const { return next_vreg_; }

    const TargetDescription* target() const { return target_; }

    // Add a stack slot. Returns the slot index.
    uint32_t NewStackSlot(uint32_t size, uint32_t alignment = 8) {
        StackSlot s;
        s.size = size;
        s.alignment = alignment;
        s.offset = AlignUp(frame_size_, alignment);
        frame_size_ = s.offset + size;
        stack_slots_.push_back(s);
        return static_cast<uint32_t>(stack_slots_.size() - 1);
    }
    uint32_t frame_size() const { return frame_size_; }
    uint32_t stack_slot_count() const { return static_cast<uint32_t>(stack_slots_.size()); }
    uint32_t StackSlotOffset(uint32_t id) const { return stack_slots_[id].offset; }

    // Iterate over all blocks in reverse-post-order (good for linear scan RA).
    std::vector<MachineBlock*> ReversePostOrder() const;

    // Iterate over all blocks.
    const std::vector<std::unique_ptr<MachineBlock>>& blocks() const { return blocks_; }

    // Calling convention used by this function.
    void set_calling_convention(void* cc) { calling_convention_ = cc; }
    void* calling_convention() const { return calling_convention_; }

    // Source function name for diagnostics.
    void set_name(const char* n) { name_ = n; }
    const char* name() const { return name_; }

private:
    struct StackSlot {
        uint32_t size;
        uint32_t alignment;
        uint32_t offset;
    };

    const TargetDescription* target_;
    std::vector<std::unique_ptr<MachineBlock>> blocks_;
    std::vector<Rep> rep_classes_;
    std::vector<StackSlot> stack_slots_;
    uint32_t frame_size_ = 0;
    uint32_t next_vreg_ = 0;
    size_t entry_block_index_ = 0;
    void* calling_convention_ = nullptr;
    const char* name_ = "<anonymous>";
};

// -----------------------------------------------------------------------------
// AllocationResult: output of register allocation.
// -----------------------------------------------------------------------------
// This is the data structure that the code emitter consumes. It maps each
// VReg to either a PhysicalRegister or a spill slot.
struct AllocationResult {
    enum class Location : uint8_t {
        kUnallocated,
        kRegister,
        kStackSlot,
    };

    struct Assignment {
        Location location = Location::kUnallocated;
        PhysicalRegister preg = PhysicalRegister::None();
        int32_t spill_offset = -1;     // negative offset from frame pointer
        Rep rep = Rep::kNone;
    };

    std::vector<Assignment> assignments;   // indexed by VReg
    uint32_t spill_slot_count = 0;
    uint32_t max_register_pressure = 0;

    void Assign(VReg r, PhysicalRegister preg, Rep rep) {
        if (assignments.size() <= r) assignments.resize(r + 1);
        assignments[r].location = Location::kRegister;
        assignments[r].preg = preg;
        assignments[r].rep = rep;
    }

    void Spill(VReg r, int32_t offset, Rep rep) {
        if (assignments.size() <= r) assignments.resize(r + 1);
        assignments[r].location = Location::kStackSlot;
        assignments[r].spill_offset = offset;
        assignments[r].rep = rep;
    }

    bool IsInRegister(VReg r) const {
        return r < assignments.size() &&
               assignments[r].location == Location::kRegister;
    }
    bool IsSpilled(VReg r) const {
        return r < assignments.size() &&
               assignments[r].location == Location::kStackSlot;
    }
    PhysicalRegister GetPhysical(VReg r) const {
        V12_DCHECK(IsInRegister(r), "VReg not in register");
        return assignments[r].preg;
    }
    int32_t GetSpillOffset(VReg r) const {
        V12_DCHECK(IsSpilled(r), "VReg not spilled");
        return assignments[r].spill_offset;
    }
};

}  // namespace v12

#endif  // V12_CONTRACTS_MACHINE_IR_H_
