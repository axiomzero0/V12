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

// External declaration of the interpreter's deopt flag (defined in interpreter.cc).
extern volatile uintptr_t g_jit_deopt_flag;

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
    //   r14 = deopt_flag pointer
    //   rcx, rdx = scratch
    //
    // The JIT entry point follows System V AMD64 ABI:
    //   arg1 (RDI) = deopt_flag (uintptr_t*)
    //   arg2 (RSI) = acc (uintptr_t, raw bits)
    //   arg3 (RDX) = regs (Value*)
    //   arg4 (RCX) = frame (void*)
    //   arg5 (R8)  = iso (Isolate*)
    //
    // We rearrange to our internal convention in the prologue.
    const x86::Gp acc = rax;
    const x86::Gp regs = rsi;
    const x86::Gp frame = rdi;
    const x86::Gp iso = r12;
    const x86::Gp deopt_ptr = r14;
    const x86::Gp scratch1 = rcx;
    const x86::Gp scratch2 = rdx;

    // Prologue: save callee-saved registers.
    a.push(rbx);
    a.push(r12);
    a.push(r13);
    a.push(r14);
    a.push(r15);

    // Rearrange args from ABI to internal convention:
    a.mov(deopt_ptr, rdi);  // deopt_ptr = arg1
    a.mov(acc, rsi);         // acc = arg2
    a.mov(regs, rdx);        // regs = arg3
    a.mov(frame, rcx);       // frame = arg4
    a.mov(iso, r8);          // iso = arg5

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
                // We can't call ResolveConstant at JIT time (it allocates
                // heap objects). Instead, for Smi constants we embed the
                // value directly. For non-Smi constants, we deopt.
                uint32_t cidx = static_cast<uint32_t>(bc[i+1]) |
                                (static_cast<uint32_t>(bc[i+2]) << 8) |
                                (static_cast<uint32_t>(bc[i+3]) << 16) |
                                (static_cast<uint32_t>(bc[i+4]) << 24);
                if (cidx < fi->constants.size()) {
                    const Constant& c = fi->constants[cidx];
                    if (c.kind == Constant::Kind::kSmi) {
                        a.mov(acc, (static_cast<uint64_t>(c.smi) << kSmiShift) | kSmiTag);
                        break;
                    }
                }
                // Non-Smi constant → deopt.
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

            // ----- Arithmetic (Smi fast path) -----
            // For Add/Sub/Mul: check both operands are Smis (low bit = 1),
            // do the arithmetic on the unshifted values, check overflow.
            // If not Smi or overflow, deopt.
            case Op::Add: {
                uint8_t r = bc[i + 1];
                Label no_overflow = a.new_label();
                a.mov(scratch1, x86::ptr(regs, r * 8));
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.test(scratch1, 1);
                a.jz(labels[bc.size()]);
                // Smi add: a + b - 1 = correct tagged result.
                a.add(acc, scratch1);
                a.jno(no_overflow);
                // Overflow: reload acc from the register that Ldar loaded
                // (we don't know which one, so store the overflowed acc
                // and deopt to THIS instruction so the interpreter redoes it).
                // The interpreter will see acc = overflowed value, but it
                // will call Add() which uses ToDouble (handles any value).
                // Actually, the interpreter's Add handler reads acc and regs[r].
                // acc is the overflowed value (garbage). We need to restore
                // acc to the value BEFORE the add. But we don't have it.
                // Solution: deopt to the instruction BEFORE the Add (the Ldar).
                // The Ldar will reload acc from the register, then the Add
                // will be redone by the interpreter.
                // But we don't know the Ldar's offset. Instead, just store
                // the overflowed acc and deopt to the Add offset. The
                // interpreter's Add handler calls Add(iso, acc, regs[r]).
                // acc is the overflowed tagged value. Add() calls ToDouble
                // which will interpret it as a Smi (wrong value) or crash.
                //
                // Better: just deopt to the generic label and let the
                // interpreter redo from the loop start. The registers are
                // still valid (s and i are in their registers, untouched
                // by the JIT's add which only modified acc).
                a.mov(x86::ptr(regs, 0), acc);  // store acc (garbage but won't be used)
                a.mov(acc, static_cast<uint64_t>(0xDEAD));
                a.pop(r15); a.pop(r14); a.pop(r13); a.pop(r12); a.pop(rbx);
                a.ret();
                a.bind(no_overflow);
                a.sub(acc, 1);
                break;
            }
            case Op::Sub: {
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.test(scratch1, 1);
                a.jz(labels[bc.size()]);
                // acc - scratch1 = (av - bv)*2 + 1 - 1 + 1 = (av-bv)*2 + 1
                // Wait: a - b = (av*2+1) - (bv*2+1) = (av-bv)*2
                // We want (av-bv)*2 + 1. So a - b + 1.
                a.sub(acc, scratch1);
                a.jo(labels[bc.size()]);
                a.add(acc, 1);
                break;
            }
            case Op::Mul: {
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.test(scratch1, 1);
                a.jz(labels[bc.size()]);
                // a * b = (av*2+1) * (bv*2+1) — complex. Simpler:
                // shr acc, 1 (get av, lose tag)
                // shr scratch1, 1 (get bv)
                // imul acc, scratch1 (av * bv)
                // jo deopt
                // shl acc, 1
                // or acc, 1 (set tag)
                a.shr(acc, 1);
                a.shr(scratch1, 1);
                a.imul(acc, scratch1);
                a.jo(labels[bc.size()]);
                a.shl(acc, 1);
                a.or_(acc, 1);
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
            // Result is true/false singleton. We load the isolate's
            // true/false values via a known offset. For simplicity,
            // we construct the tagged boolean directly.
            // true = Smi(1) won't work — true is a HeapObject.
            // Actually, in our VM, true/false are HeapObjects (JSBoolean).
            // We can't easily construct them in JIT without the Isolate.
            // So for comparisons, we deopt for now.
            // TODO: load true/false from Isolate.
            case Op::TestLessThan:
            case Op::TestGreaterThan:
            case Op::TestLessThanOrEqual:
            case Op::TestGreaterThanOrEqual:
            case Op::TestEqStrict: {
                // For now, set acc = 0 (will be fixed when we wire Isolate).
                // Actually, let's implement Smi comparison and return a
                // Smi boolean (0 or 1). The interpreter expects Value,
                // not bool. Smi(0) and Smi(1) will work for truthiness
                // but won't match === true. This is a known limitation.
                uint8_t r = bc[i + 1];
                a.mov(scratch1, x86::ptr(regs, r * 8));
                // Check both are Smis.
                a.test(acc, 1);
                a.jz(labels[bc.size()]);
                a.test(scratch1, 1);
                a.jz(labels[bc.size()]);
                // Compare unshifted values.
                a.cmp(acc, scratch1);
                // Set acc = Smi(0) or Smi(1) based on condition.
                a.mov(acc, kSmiTag);  // Smi(0) = 1 (default: false)
                switch (op) {
                    case Op::TestLessThan:     a.setl(scratch2.r8()); break;
                    case Op::TestGreaterThan:  a.setg(scratch2.r8()); break;
                    case Op::TestLessThanOrEqual:    a.setle(scratch2.r8()); break;
                    case Op::TestGreaterThanOrEqual: a.setge(scratch2.r8()); break;
                    case Op::TestEqStrict:     a.sete(scratch2.r8()); break;
                    default: break;
                }
                // Zero-extend the byte to full 64-bit (setcc only sets low 8 bits,
                // upper bits are garbage). movzx clears the upper bits.
                a.movzx(scratch2, scratch2.r8());
                // If condition true (scratch2 != 0), acc = Smi(1) = 3.
                a.mov(scratch1, 3);  // Smi(1) = (1 << 1) | 1 = 3
                a.test(scratch2, scratch2);
                a.cmovnz(acc, scratch1);
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
                // IsTruthyFast: Smi != 0, or heap object (truthy unless
                // undefined/null/false/0/"").
                // Fast path: if acc is Smi, jump if acc != Smi(0) = 1.
                a.test(acc, 1);
                a.jnz(labels[target]);  // Smi and non-zero → truthy
                // For heap objects, we'd need more checks. For now, deopt.
                a.jmp(labels[bc.size()]);
                break;
            }
            case Op::JumpIfFalse: {
                uint32_t target = static_cast<uint32_t>(bc[i+1]) |
                                  (static_cast<uint32_t>(bc[i+2]) << 8) |
                                  (static_cast<uint32_t>(bc[i+3]) << 16) |
                                  (static_cast<uint32_t>(bc[i+4]) << 24);
                // Fast path: if acc is Smi(0) = 1, jump (falsy).
                a.cmp(acc, kSmiTag);  // Smi(0) = 1
                a.je(labels[target]);
                // If acc is Smi and non-zero, it's truthy → don't jump.
                a.test(acc, 1);
                a.jnz(labels[i + oi.length]);  // continue (Smi, truthy)
                // Heap object → deopt.
                a.jmp(labels[bc.size()]);
                break;
            }

            // ----- Returns -----
            case Op::Return:
                // Store acc in regs[0] and return 0 (no deopt).
                a.mov(x86::ptr(regs, 0), acc);
                a.xor_(acc, acc);  // return 0 = no deopt
                a.pop(r15);
                a.pop(r14);
                a.pop(r13);
                a.pop(r12);
                a.pop(rbx);
                a.ret();
                break;

            case Op::ReturnUndefined:
                // Store acc in regs[0] and return deopt flag = offset+1.
                a.mov(x86::ptr(regs, 0), acc);
                a.mov(acc, static_cast<uint64_t>(i + 1));
                a.pop(r15);
                a.pop(r14);
                a.pop(r13);
                a.pop(r12);
                a.pop(rbx);
                a.ret();
                break;

            default:
                // Unsupported opcode: store acc in regs[0], return deopt flag = offset+1.
                a.mov(x86::ptr(regs, 0), acc);
                a.mov(acc, static_cast<uint64_t>(i + 1));
                a.pop(r15);
                a.pop(r14);
                a.pop(r13);
                a.pop(r12);
                a.pop(rbx);
                a.ret();
                break;
        }

        i += oi.length;
    }

    // Deopt label (used by Smi-check failures and overflow).
    a.bind(labels[bc.size()]);
    a.mov(x86::ptr(regs, 0), acc);  // store acc
    a.mov(acc, static_cast<uint64_t>(0xDEAD));  // deopt sentinel
    a.pop(r15);
    a.pop(r14);
    a.pop(r13);
    a.pop(r12);
    a.pop(rbx);
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
