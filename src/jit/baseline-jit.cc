// =============================================================================
// src/jit/baseline-jit.cc
// =============================================================================
// Baseline JIT compiler implementation.
//
// Strategy: for each bytecode instruction, emit a call to a C++ handler
// function that does the same work as the interpreter's dispatch handler.
// This gives us the "one machine instruction per bytecode" baseline
// without needing to hand-code every opcode in x86-64.
//
// The C++ handlers are stored in a table indexed by opcode. Each handler
// takes (Interp*, Frame*) and returns void (it modifies the frame in place).
// The JIT code sets up the call, invokes the handler, and continues to the
// next bytecode.
//
// This is intentionally simple — it's the "Sparkplug" tier. The win comes
// from eliminating the dispatch overhead (computed-goto table lookup +
// branch prediction) and the C++ function call overhead of ExecuteTop's
// deep switch. Each bytecode becomes a direct call to its handler, which
// the CPU can predict independently.

#include "jit/baseline-jit.h"

#include <cstring>
#include <sys/mman.h>

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "interpreter/interpreter.h"
#include "jit/x86-emitter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

namespace v12 {

// x86-64 register assignments.
static constexpr X86Reg RAX = X86Reg::RAX;
static constexpr X86Reg RCX = X86Reg::RCX;
static constexpr X86Reg RDX = X86Reg::RDX;
static constexpr X86Reg RSI = X86Reg::RSI;
static constexpr X86Reg RDI = X86Reg::RDI;
static constexpr X86Reg R12 = X86Reg::R12;
static constexpr X86Reg R14 = X86Reg::R14;

// Smi tag constants (must match TaggedValue).
static constexpr uintptr_t kSmiTag = 1;
static constexpr uintptr_t kSmiShift = 1;

struct CompileState {
    X86Emitter e;
    FunctionInfo* fi;
    // Map from bytecode offset to machine code offset.
    std::vector<uint32_t> bc_to_mc;
    // Pending jumps: (mc_patch_offset, bc_target).
    struct PendingJump {
        size_t mc_patch_offset;
        uint32_t bc_target;
        bool conditional;  // false = jmp, true = jcc
        uint8_t cond;      // condition code for jcc
    };
    std::vector<PendingJump> pending_jumps;
};

std::unique_ptr<CodeObject> BaselineJIT::Compile(FunctionInfo* fi) {
    auto cs = std::make_unique<CompileState>();
    cs->fi = fi;
    X86Emitter& e = cs->e;
    auto& bc = fi->bytecode;

    if (bc.empty()) return nullptr;

    // Prologue: save callee-saved registers.
    e.push(X86Reg::RBX);
    e.push(R12);
    e.push(R14);

    // RAX = acc (already set by caller)
    // RSI = regs (already set by caller)
    // RDI = Frame* (already set by caller)

    cs->bc_to_mc.resize(bc.size() + 1, 0xFFFFFFFF);

    size_t i = 0;
    while (i < bc.size()) {
        Op op = static_cast<Op>(bc[i]);
        const OpInfo& oi = GetOpInfo(op);
        cs->bc_to_mc[i] = static_cast<uint32_t>(e.size());

        switch (op) {
            // ----- Nop -----
            case Op::Nop:
                e.nop();
                break;

            // ----- Loading constants (inline, no call) -----
            case Op::LdaZero:
                // acc = Smi(0) = 1 (tagged)
                e.mov_imm64(RAX, kSmiTag);
                break;

            case Op::LdaSmi: {
                uint8_t v = bc[i + 1];
                e.mov_imm64(RAX, (static_cast<uint64_t>(v) << kSmiShift) | kSmiTag);
                break;
            }

            case Op::LdaSmi16: {
                uint16_t v = static_cast<uint16_t>(bc[i+1]) |
                             (static_cast<uint16_t>(bc[i+2]) << 8);
                int16_t sv = static_cast<int16_t>(v);
                e.mov_imm64(RAX, (static_cast<uint64_t>(static_cast<intptr_t>(sv))
                                  << kSmiShift) | kSmiTag);
                break;
            }

            // ----- Register moves (inline, no call) -----
            case Op::Ldar: {
                uint8_t r = bc[i + 1];
                // acc = regs[r]
                e.mov_mem(RAX, RSI, r * 8);
                break;
            }
            case Op::Star: {
                uint8_t r = bc[i + 1];
                // regs[r] = acc
                e.mov_mem_store(RSI, r * 8, RAX);
                break;
            }
            case Op::Mov: {
                uint8_t dst = bc[i + 1];
                uint8_t src = bc[i + 2];
                e.mov_mem(RCX, RSI, src * 8);
                e.mov_mem_store(RSI, dst * 8, RCX);
                break;
            }

            // ----- IncReg / DecReg (inline Smi fast path) -----
            case Op::IncReg: {
                uint8_t r = bc[i + 1];
                // regs[r] += 2 (Smi increment = add 2 to tagged value)
                e.mov_mem(RAX, RSI, r * 8);
                // add rax, 2
                e.mov_imm64(RCX, 2);
                e.add_reg(RAX, RCX);
                e.mov_mem_store(RSI, r * 8, RAX);
                break;
            }
            case Op::DecReg: {
                uint8_t r = bc[i + 1];
                e.mov_mem(RAX, RSI, r * 8);
                e.mov_imm64(RCX, 2);
                e.sub_reg(RAX, RCX);
                e.mov_mem_store(RSI, r * 8, RAX);
                break;
            }

            // ----- Jumps (inline, patched) -----
            case Op::Jump:
            case Op::JumpLoop: {
                uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                                  (static_cast<uint32_t>(bc[i+2]) << 8) |
                                  (static_cast<uint32_t>(bc[i+3]) << 16) |
                                  (static_cast<uint32_t>(bc[i+4]) << 24);
                size_t patch = e.jmp();
                cs->pending_jumps.push_back({patch, target, false, 0});
                break;
            }

            // ----- Returns (inline) -----
            case Op::Return:
                // RAX already has the return value.
                e.pop(R14);
                e.pop(R12);
                e.pop(X86Reg::RBX);
                e.ret();
                break;

            // ----- Everything else: fall back to interpreter -----
            // For unsupported opcodes, we emit a "deopt" — the JIT code
            // returns a special value that tells the interpreter to resume
            // from the current bytecode offset. This is simpler than
            // calling C++ handlers inline.
            default:
                // For now, emit a ret that signals "deopt to interpreter".
                // The interpreter will resume from this bytecode offset.
                // We store the bytecode offset in RAX so the interpreter
                // knows where to resume.
                e.mov_imm64(RAX, static_cast<uint64_t>(i));  // bc offset
                e.pop(R14);
                e.pop(R12);
                e.pop(X86Reg::RBX);
                e.ret();
                break;
        }

        i += oi.length;
    }

    // If we reach the end without a Return, emit one.
    // (ReturnUndefined fallback)
    e.mov_imm64(RAX, 0xFFFFFFFF);  // sentinel: "fell off end"
    e.pop(R14);
    e.pop(R12);
    e.pop(X86Reg::RBX);
    e.ret();

    // Patch all pending jumps.
    for (auto& pj : cs->pending_jumps) {
        if (pj.bc_target < cs->bc_to_mc.size()) {
            uint32_t mc_target = cs->bc_to_mc[pj.bc_target];
            if (mc_target != 0xFFFFFFFF) {
                e.patch_rel32(pj.mc_patch_offset, mc_target);
            }
        }
    }

    // Copy into executable memory.
    size_t code_size = e.size();
    if (code_size == 0) return nullptr;

    size_t page_size = 4096;
    size_t alloc_size = (code_size + page_size - 1) & ~(page_size - 1);
    void* exec_mem = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (exec_mem == MAP_FAILED) return nullptr;
    std::memcpy(exec_mem, e.code().data(), code_size);
    if (mprotect(exec_mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(exec_mem, alloc_size);
        return nullptr;
    }

    auto co = std::make_unique<CodeObject>();
    co->SetCode(std::vector<uint8_t>(e.code().begin(), e.code().end()));
    co->SetEntryPointOffset(0);
    co->set_executable_memory(exec_mem);
    co->set_source_function(fi);
    co->set_tier(CodeObject::Tier::kBaseline);
    co->set_instruction_count(static_cast<uint32_t>(code_size));
    return co;
}

}  // namespace v12
