// =============================================================================
// src/jit/baseline-jit.cc
// =============================================================================
// Baseline JIT compiler using asmjit for x86-64 code emission.
//
// The JIT walks the bytecode and emits machine code for each instruction.
// asmjit handles all instruction encoding, label patching, and register
// management — we just describe what we want and asmjit produces correct
// machine code.
//
// Calling convention for JIT code (System V AMD64):
//   Entry: RAX = acc, RSI = regs[0], RDI = Frame*, R12 = Isolate*
//   Exit:  RAX = result Value (or deopt sentinel)
//
// Register usage within JIT:
//   RAX = accumulator (same as interpreter)
//   RSI = register file base (regs[0])
//   RDI = Frame* (for context, this, etc.)
//   R12 = Isolate* (for heap allocation, roots)
//   RCX, RDX = scratch registers

#include "jit/baseline-jit.h"

#include <asmjit/x86.h>
#include <cstring>
#include <sys/mman.h>

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

namespace v12 {

using namespace asmjit;

// The JIT entry point is called by the interpreter with 4 arguments
// (System V AMD64 ABI):
//   arg1 (RDI) = acc   (uintptr_t, raw tagged bits)
//   arg2 (RSI) = regs  (Value*)
//   arg3 (RDX) = frame (void*)
//   arg4 (RCX) = iso   (Isolate*)
//
// The JIT rearranges these into its internal register convention in the
// prologue. There is no separate deopt_flag argument — deopt is signaled
// by the JIT's return value (nonzero = deopt at offset ret-1).

// Smi tag constants (must match TaggedValue).
static constexpr uint64_t kSmiTag = 1;
static constexpr uint64_t kSmiShift = 1;

// Check if an opcode is fully compiled (vs deopt to interpreter).
static bool IsOpcodeSupported(Op op) {
    switch (op) {
        case Op::Nop:
        case Op::LdaZero:
        case Op::LdaSmi:
        case Op::LdaSmi16:
        case Op::LdaConst:
        case Op::Ldar:
        case Op::Star:
        case Op::Mov:
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::AddSmiConst:
        case Op::SubSmiConst:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::IncReg:
        case Op::DecReg:
        case Op::Inc:
        case Op::Dec:
        case Op::Jump:
        case Op::JumpLoop:
        case Op::JumpIfTrue:
        case Op::JumpIfFalse:
        case Op::TestLessThan:
        case Op::TestGreaterThan:
        case Op::TestLessThanOrEqual:
        case Op::TestGreaterThanOrEqual:
        case Op::TestEqStrict:
        case Op::Return:
        case Op::ReturnUndefined:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<CodeObject> BaselineJIT::Compile(FunctionInfo* fi,
                                                uint32_t osr_entry_offset) {
    auto& bc = fi->bytecode;
    if (bc.empty()) return nullptr;

    // Fetch the root Value raw bits at compile time so we can embed them
    // as immediates in the JIT code. This avoids needing the Isolate
    // pointer at runtime for comparison results and truthiness checks.
    Isolate* iso_compile = Isolate::Current();
    const uint64_t true_bits      = iso_compile->true_value().raw().raw_bits();
    const uint64_t false_bits     = iso_compile->false_value().raw().raw_bits();
    const uint64_t undefined_bits = iso_compile->undefined_value().raw().raw_bits();
    const uint64_t null_bits      = iso_compile->null_value().raw().raw_bits();

    // We compile the entire function. Supported opcodes get native code;
    // unsupported opcodes emit a deopt stub (return to interpreter at that
    // bytecode offset). The interpreter resumes and will re-enter the JIT
    // on the next JumpLoop.
    // The osr_entry_offset is the bytecode offset of the loop start.
    // After the prologue, we jump directly to that offset's label,
    // skipping the function setup (which the interpreter already did).

    Environment env(Arch::kX64);
    CodeHolder code;
    code.init(env, 0);
    x86::Assembler a(&code);

    using namespace x86::regs;

    // Register assignments (after prologue setup):
    //   rax = acc
    //   rsi = regs base
    //   rdi = Frame*
    //   r12 = Isolate*
    //   rcx, rdx = scratch
    //
    // The JIT entry point is called by the interpreter with:
    //   arg1 (RDI) = acc   (uintptr_t, raw tagged bits)
    //   arg2 (RSI) = regs  (Value*)
    //   arg3 (RDX) = frame (void*)
    //   arg4 (RCX) = iso   (Isolate*)
    const x86::Gp acc = rax;
    const x86::Gp regs = rsi;
    const x86::Gp frame = rdi;
    const x86::Gp iso = r12;
    const x86::Gp scratch1 = rcx;
    const x86::Gp scratch2 = rdx;

    // Prologue: save callee-saved registers that we actually clobber.
    a.push(r12);
    a.push(r14);

    // Rearrange args from ABI to internal convention:
    // arg1 (RDI) = acc → rax
    // arg2 (RSI) = regs → stays rsi
    // arg3 (RDX) = frame → rdi
    // arg4 (RCX) = iso → r12
    a.mov(acc, rdi);         // acc = arg1
    // regs already in rsi (arg2)
    a.mov(frame, rdx);       // frame = arg3
    a.mov(iso, rcx);         // iso = arg4

    // Labels for each bytecode offset (for jump patching).
    std::vector<Label> labels(bc.size() + 1);
    for (size_t j = 0; j <= bc.size(); ++j) {
        labels[j] = a.new_label();
    }

    // OSR: jump directly to the loop entry point (skip function setup).
    if (osr_entry_offset > 0 && osr_entry_offset < bc.size()) {
        a.jmp(labels[osr_entry_offset]);
    }

    size_t i = 0;
    while (i < bc.size()) {
        Op op = static_cast<Op>(bc[i]);
        const OpInfo& oi = GetOpInfo(op);

        a.bind(labels[i]);

        switch (op) {
            case Op::Nop:
                break;

            // ----- Loading constants -----
            case Op::LdaZero:
                // acc = Smi(0) = 1 (tagged)
                a.mov(acc, kSmiTag);
                break;

            case Op::LdaSmi: {
                uint8_t v = bc[i + 1];
                a.mov(acc, (static_cast<uint64_t>(v) << kSmiShift) | kSmiTag);
                break;
            }

            case Op::LdaSmi16: {
                uint16_t v = static_cast<uint16_t>(bc[i+1]) |
                             (static_cast<uint16_t>(bc[i+2]) << 8);
                int16_t sv = static_cast<int16_t>(v);
                a.mov(acc, (static_cast<uint64_t>(static_cast<intptr_t>(sv))
                            << kSmiShift) | kSmiTag);
                break;
            }

            case Op::LdaConst: {
                // LdaConst: op  const_idx:32  idx:16
                // If the constants have been pre-resolved (by PreResolveConstants),
                // we can load the value directly from the resolved_constants
                // array — no deopt needed for any constant kind.
                uint32_t cidx = static_cast<uint32_t>(bc[i+1]) |
                                (static_cast<uint32_t>(bc[i+2]) << 8) |
                                (static_cast<uint32_t>(bc[i+3]) << 16) |
                                (static_cast<uint32_t>(bc[i+4]) << 24);
                if (fi->resolved_constants != nullptr &&
                    cidx < fi->constants.size() &&
                    fi->resolved_constants[cidx] != 0) {
                    // Load the pre-resolved value (raw tagged bits).
                    a.mov(acc, static_cast<uint64_t>(fi->resolved_constants[cidx]));
                    break;
                }
                // Fallback: for Smi constants, embed directly.
                if (cidx < fi->constants.size()) {
                    const Constant& c = fi->constants[cidx];
                    if (c.kind == Constant::Kind::kSmi) {
                        a.mov(acc, (static_cast<uint64_t>(c.smi) << kSmiShift) | kSmiTag);
                        break;
                    }
                }
                // Non-Smi constant without pre-resolution → deopt.
                a.jmp(labels[bc.size()]);
                break;
            }

            // ----- Register moves -----
            case Op::Ldar: {
                uint8_t r = bc[i + 1];
                a.mov(acc, x86::ptr(regs, r * 8));
                break;
            }
            case Op::Star: {
                uint8_t r = bc[i + 1];
                a.mov(x86::ptr(regs, r * 8), acc);
                break;
            }
            case Op::Mov: {
                uint8_t dst = bc[i + 1];
                uint8_t src = bc[i + 2];
                a.mov(scratch1, x86::ptr(regs, src * 8));
                a.mov(x86::ptr(regs, dst * 8), scratch1);
                break;
            }

            // ----- Arithmetic (Smi fast path with type feedback) -----
            // For Add/Sub/Mul: if type feedback says both operands are Smis
            // (type_feedback == 1), skip the Smi tag checks entirely.
            // Otherwise, emit the checks and deopt on non-Smi.
            case Op::Add: {
                uint8_t r = bc[i + 1];
                uint16_t ic_slot = static_cast<uint16_t>(bc[i+2]) |
                                   (static_cast<uint16_t>(bc[i+3]) << 8);
                // Check type feedback: 1 = Smi, 0 = unknown.
                bool known_smi = false;
                if (ic_slot < fi->ic_entries.size()) {
                    known_smi = fi->ic_entries[ic_slot].type_feedback == 1;
                }

                Label no_overflow = a.new_label();
                a.mov(scratch1, x86::ptr(regs, r * 8));

                if (!known_smi) {
                    // Emit Smi tag checks (deopt on non-Smi).
                    a.test(acc, 1);
                    a.jz(labels[bc.size()]);
                    a.test(scratch1, 1);
                    a.jz(labels[bc.size()]);
                }
                // Smi add: a + b - 1 = correct tagged result.
                a.add(acc, scratch1);
                a.jno(no_overflow);
                // Overflow → deopt.
                a.mov(x86::ptr(regs, 0), acc);
                a.mov(acc, static_cast<uint64_t>(0xDEAD));
                a.pop(r14); a.pop(r12);
                a.ret();
                a.bind(no_overflow);
                a.sub(acc, 1);
                break;
            }
            case Op::Sub: {
                uint8_t r = bc[i + 1];
                uint16_t ic_slot = static_cast<uint16_t>(bc[i+2]) |
                                   (static_cast<uint16_t>(bc[i+3]) << 8);
                bool known_smi = false;
                if (ic_slot < fi->ic_entries.size()) {
                    known_smi = fi->ic_entries[ic_slot].type_feedback == 1;
                }
                a.mov(scratch1, x86::ptr(regs, r * 8));
                if (!known_smi) {
                    a.test(acc, 1);
                    a.jz(labels[bc.size()]);
                    a.test(scratch1, 1);
                    a.jz(labels[bc.size()]);
                }
                a.sub(acc, scratch1);
                a.jo(labels[bc.size()]);
                a.add(acc, 1);
                break;
            }
            case Op::Mul: {
                uint8_t r = bc[i + 1];
                uint16_t ic_slot = static_cast<uint16_t>(bc[i+2]) |
                                   (static_cast<uint16_t>(bc[i+3]) << 8);
                bool known_smi = false;
                if (ic_slot < fi->ic_entries.size()) {
                    known_smi = fi->ic_entries[ic_slot].type_feedback == 1;
                }
                a.mov(scratch1, x86::ptr(regs, r * 8));
                if (!known_smi) {
                    a.test(acc, 1);
                    a.jz(labels[bc.size()]);
                    a.test(scratch1, 1);
                    a.jz(labels[bc.size()]);
                }
                a.shr(acc, 1);
                a.shr(scratch1, 1);
                a.imul(acc, scratch1);
                a.jo(labels[bc.size()]);
                a.shl(acc, 1);
                a.or_(acc, 1);
                break;
            }

            // ----- AddSmiConst/SubSmiConst -----
            // acc += Smi(imm8) / acc -= Smi(imm8). Baked immediate.
            case Op::AddSmiConst: {
                uint8_t imm = bc[i + 1];
                Label no_overflow = a.new_label();
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.add(acc, static_cast<uint64_t>(imm) << kSmiShift);
                a.jno(no_overflow);
                a.mov(x86::ptr(regs, 0), acc);
                a.mov(acc, static_cast<uint64_t>(0xDEAD));
                a.pop(r14); a.pop(r12);
                a.ret();
                a.bind(no_overflow);
                break;
            }
            case Op::SubSmiConst: {
                uint8_t imm = bc[i + 1];
                Label no_overflow = a.new_label();
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.sub(acc, static_cast<uint64_t>(imm) << kSmiShift);
                a.jno(no_overflow);
                a.mov(x86::ptr(regs, 0), acc);
                a.mov(acc, static_cast<uint64_t>(0xDEAD));
                a.pop(r14); a.pop(r12);
                a.ret();
                a.bind(no_overflow);
                break;
            }

            // ----- Bitwise ops (always Smi, no overflow) -----
            case Op::BitAnd: {
                uint8_t r = bc[i + 1];
                a.and_(acc, x86::ptr(regs, r * 8));
                break;
            }
            case Op::BitOr: {
                uint8_t r = bc[i + 1];
                a.or_(acc, x86::ptr(regs, r * 8));
                break;
            }
            case Op::BitXor: {
                uint8_t r = bc[i + 1];
                a.xor_(acc, x86::ptr(regs, r * 8));
                break;
            }

            // ----- Inc/Dec -----
            case Op::Inc: {
                // Smi increment: tagged value += 2 (not 1!)
                // Smi(n) = (n << 1) | 1. Smi(n+1) = ((n+1) << 1) | 1 = Smi(n) + 2.
                a.add(acc, 2);
                a.jo(labels[bc.size()]);
                break;
            }
            case Op::Dec: {
                a.sub(acc, 2);
                a.jo(labels[bc.size()]);
                break;
            }
            case Op::IncReg: {
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                a.add(scratch1, 2);
                a.jo(labels[bc.size()]);
                a.mov(x86::ptr(regs, r * 8), scratch1);
                a.mov(acc, scratch1);
                break;
            }
            case Op::DecReg: {
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                a.sub(scratch1, 2);
                a.jo(labels[bc.size()]);
                a.mov(x86::ptr(regs, r * 8), scratch1);
                a.mov(acc, scratch1);
                break;
            }

            // ----- Comparisons (Smi fast path) -----
            // Result is the true/false HeapObject singleton (not Smi 0/1).
            // We embed the raw bits of iso->true_value() / false_value()
            // as immediates (fetched at JIT compile time).
            case Op::TestLessThan:
            case Op::TestGreaterThan:
            case Op::TestLessThanOrEqual:
            case Op::TestGreaterThanOrEqual:
            case Op::TestEqStrict: {
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                // Check both are Smis.
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.test(scratch1, 1);
                a.jz(labels[bc.size()]);
                // Compare unshifted values (tags still on, but both have
                // the same tag so the comparison result is the same).
                a.cmp(acc, scratch1);
                // Default: acc = false singleton.
                a.mov(acc, false_bits);
                // setcc sets the low byte of scratch2 based on the condition.
                switch (op) {
                    case Op::TestLessThan:     a.setl(scratch2.r8()); break;
                    case Op::TestGreaterThan:  a.setg(scratch2.r8()); break;
                    case Op::TestLessThanOrEqual:    a.setle(scratch2.r8()); break;
                    case Op::TestGreaterThanOrEqual: a.setge(scratch2.r8()); break;
                    case Op::TestEqStrict:     a.sete(scratch2.r8()); break;
                    default: break;
                }
                // Zero-extend the byte to full 64-bit.
                a.movzx(scratch2, scratch2.r8());
                // If condition true (scratch2 != 0), acc = true singleton.
                a.test(scratch2, scratch2);
                Label done = a.new_label();
                a.jz(done);
                a.mov(acc, true_bits);
                a.bind(done);
                break;
            }

            // ----- Jumps -----
            case Op::Jump:
            case Op::JumpLoop: {
                uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                                  (static_cast<uint32_t>(bc[i+2]) << 8) |
                                  (static_cast<uint32_t>(bc[i+3]) << 16) |
                                  (static_cast<uint32_t>(bc[i+4]) << 24);
                a.jmp(labels[target]);
                break;
            }
            case Op::JumpIfTrue: {
                uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                                  (static_cast<uint32_t>(bc[i+2]) << 8) |
                                  (static_cast<uint32_t>(bc[i+3]) << 16) |
                                  (static_cast<uint32_t>(bc[i+4]) << 24);
                // IsTruthy: Smi != 0, or heap object (truthy unless
                // undefined/null/false).
                // Fast path 1: if acc is Smi, jump if acc != Smi(0) = 1.
                a.test(acc, 1);
                a.jnz(labels[target]);  // Smi and non-zero → truthy
                // Fast path 2: Smi(0) = 1 is falsy → don't jump.
                a.cmp(acc, kSmiTag);
                a.je(labels[i + oi.length]);  // Smi(0) → falsy, continue
                // Heap object path: compare against false/undefined/null.
                // All three are falsy; any other heap object is truthy.
                a.cmp(acc, false_bits);
                a.je(labels[i + oi.length]);  // false → falsy
                a.cmp(acc, undefined_bits);
                a.je(labels[i + oi.length]);  // undefined → falsy
                a.cmp(acc, null_bits);
                a.je(labels[i + oi.length]);  // null → falsy
                // Any other heap object → truthy.
                a.jmp(labels[target]);
                break;
            }
            case Op::JumpIfFalse: {
                uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                                  (static_cast<uint32_t>(bc[i+2]) << 8) |
                                  (static_cast<uint32_t>(bc[i+3]) << 16) |
                                  (static_cast<uint32_t>(bc[i+4]) << 24);
                // IsFalsy: Smi(0), false, undefined, null.
                // Fast path 1: if acc is Smi(0) = 1, jump (falsy).
                a.cmp(acc, kSmiTag);
                a.je(labels[target]);
                // Fast path 2: if acc is Smi and non-zero, don't jump.
                a.test(acc, 1);
                a.jnz(labels[i + oi.length]);  // Smi non-zero → truthy
                // Heap object path: compare against false/undefined/null.
                a.cmp(acc, false_bits);
                a.je(labels[target]);
                a.cmp(acc, undefined_bits);
                a.je(labels[target]);
                a.cmp(acc, null_bits);
                a.je(labels[target]);
                // Any other heap object → truthy, don't jump.
                a.jmp(labels[i + oi.length]);
                break;
            }

            // ----- Returns -----
            // Return protocol: store acc in regs[0], return 0 (normal return).
            // The interpreter reads acc from regs[0] on normal return.
            case Op::Return:
                a.mov(x86::ptr(regs, 0), acc);
                a.xor_(acc, acc);  // return 0 = normal return
                a.pop(r14);
                a.pop(r12);
                a.ret();
                break;

            case Op::ReturnUndefined: {
                uintptr_t undef_bits = Isolate::Current()->undefined_value()
                                          .raw().raw_bits();
                a.mov(x86::ptr(regs, 0), static_cast<uint64_t>(undef_bits));
                a.xor_(acc, acc);  // return 0 = normal return
                a.pop(r14);
                a.pop(r12);
                a.ret();
                break;
            }

            default:
                // Unsupported opcode: deopt.
                // Save acc to rbx, return deopt offset+1 in rax.
                a.mov(x86::ptr(frame, 48), acc);  // frame->jit_deopt_acc = acc
                a.mov(acc, static_cast<uint64_t>(i + 1));
                a.pop(r14);
                a.pop(r12);
                a.ret();
                break;
        }

        i += oi.length;
    }

    // Deopt label (used by Smi-check failures and overflow).
    // Save acc to rbx, return 0xDEAD (deopt sentinel).
    a.bind(labels[bc.size()]);
    a.mov(x86::ptr(frame, 48), acc);  // frame->jit_deopt_acc = acc
    a.mov(acc, static_cast<uint64_t>(0xDEAD));
    a.pop(r14);
    a.pop(r12);
    a.ret();

    // Get the emitted code.
    CodeBuffer& buf = code.text_section()->buffer();
    size_t code_size = buf.size();
    if (code_size == 0) return nullptr;

    // Copy into executable memory.
    size_t page_size = 4096;
    size_t alloc_size = (code_size + page_size - 1) & ~(page_size - 1);
    void* exec_mem = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (exec_mem == MAP_FAILED) return nullptr;
    std::memcpy(exec_mem, buf.data(), code_size);
    if (mprotect(exec_mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(exec_mem, alloc_size);
        return nullptr;
    }

    auto co = std::make_unique<CodeObject>();
    co->SetCode(std::vector<uint8_t>(buf.data(), buf.data() + code_size));
    co->SetEntryPointOffset(0);
    co->set_executable_memory(exec_mem);
    co->set_source_function(fi);
    co->set_tier(CodeObject::Tier::kBaseline);
    co->set_instruction_count(static_cast<uint32_t>(code_size));
    return co;
}

}  // namespace v12
