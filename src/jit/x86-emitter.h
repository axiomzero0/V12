// =============================================================================
// src/jit/x86-emitter.h
// =============================================================================
// Minimal x86-64 machine code emitter for the baseline JIT.
//
// Emits instructions into a growable buffer. Supports the subset of x86-64
// needed for the baseline JIT: mov, add, sub, cmp, test, jcc, call, ret,
// and the REX prefix handling for 64-bit operations.
//
// Register usage (System V AMD64 ABI):
//   RAX = accumulator / return value
//   RSI = register file base pointer
//   RDI = Frame* pointer
//   R12 = Isolate*
//   R13 = bytecode base pointer
//   R14 = Interp*
//   R15 = FunctionInfo*
//   RBX, RCX, RDX = scratch registers

#ifndef V12_JIT_X86_EMITTER_H_
#define V12_JIT_X86_EMITTER_H_

#include <cstdint>
#include <cstring>
#include <vector>

#include "base/macros.h"

namespace v12 {

// x86-64 registers we use.
enum class X86Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

class X86Emitter {
public:
    X86Emitter() { code_.reserve(4096); }

    const std::vector<uint8_t>& code() const { return code_; }
    size_t size() const { return code_.size(); }
    size_t here() const { return code_.size(); }

    // ----- Low-level emit -----
    void emit_byte(uint8_t b) { code_.push_back(b); }
    void emit_word(uint16_t w) {
        code_.push_back(static_cast<uint8_t>(w & 0xFF));
        code_.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    }
    void emit_dword(uint32_t d) {
        code_.push_back(static_cast<uint8_t>(d & 0xFF));
        code_.push_back(static_cast<uint8_t>((d >> 8) & 0xFF));
        code_.push_back(static_cast<uint8_t>((d >> 16) & 0xFF));
        code_.push_back(static_cast<uint8_t>((d >> 24) & 0xFF));
    }
    void emit_qword(uint64_t q) {
        for (int i = 0; i < 8; ++i)
            code_.push_back(static_cast<uint8_t>((q >> (i * 8)) & 0xFF));
    }

    // REX prefix: W=64-bit, R=reg extension, B=r/m extension
    void emit_rex(bool w, bool r, bool x, bool b) {
        uint8_t rex = 0x40;
        if (w) rex |= 0x08;
        if (r) rex |= 0x04;
        if (x) rex |= 0x02;
        if (b) rex |= 0x01;
        if (rex != 0x40) emit_byte(rex);
    }

    // ----- MOV instructions -----
    // mov r64, imm64 (load 64-bit immediate)
    void mov_imm64(X86Reg dst, uint64_t imm) {
        emit_rex(true, false, false, static_cast<uint8_t>(dst) >= 8);
        emit_byte(0xB8 + (static_cast<uint8_t>(dst) & 7));
        emit_qword(imm);
    }

    // mov r64, r64
    void mov_reg(X86Reg dst, X86Reg src) {
        bool r = static_cast<uint8_t>(dst) >= 8;
        bool b = static_cast<uint8_t>(src) >= 8;
        emit_rex(true, r, false, b);
        emit_byte(0x89);
        emit_byte(0xC0 | ((static_cast<uint8_t>(dst) & 7) << 3) |
                  (static_cast<uint8_t>(src) & 7));
    }

    // mov r64, [base + disp]  (load from memory)
    void mov_mem(X86Reg dst, X86Reg base, int32_t disp) {
        bool r = static_cast<uint8_t>(dst) >= 8;
        bool b = static_cast<uint8_t>(base) >= 8;
        emit_rex(true, r, false, b);
        emit_byte(0x8B);
        emit_modrm_sib_disp(static_cast<uint8_t>(dst) & 7,
                            static_cast<uint8_t>(base) & 7, disp);
    }

    // mov [base + disp], r64  (store to memory)
    void mov_mem_store(X86Reg base, int32_t disp, X86Reg src) {
        bool r = static_cast<uint8_t>(src) >= 8;
        bool b = static_cast<uint8_t>(base) >= 8;
        emit_rex(true, r, false, b);
        emit_byte(0x89);
        emit_modrm_sib_disp(static_cast<uint8_t>(src) & 7,
                            static_cast<uint8_t>(base) & 7, disp);
    }

    // ----- Arithmetic (64-bit) -----
    // add r64, r64
    void add_reg(X86Reg dst, X86Reg src) {
        bool r = static_cast<uint8_t>(src) >= 8;
        bool b = static_cast<uint8_t>(dst) >= 8;
        emit_rex(true, r, false, b);
        emit_byte(0x01);
        emit_byte(0xC0 | ((static_cast<uint8_t>(src) & 7) << 3) |
                  (static_cast<uint8_t>(dst) & 7));
    }

    // sub r64, r64
    void sub_reg(X86Reg dst, X86Reg src) {
        bool r = static_cast<uint8_t>(src) >= 8;
        bool b = static_cast<uint8_t>(dst) >= 8;
        emit_rex(true, r, false, b);
        emit_byte(0x29);
        emit_byte(0xC0 | ((static_cast<uint8_t>(src) & 7) << 3) |
                  (static_cast<uint8_t>(dst) & 7));
    }

    // cmp r64, r64
    void cmp_reg(X86Reg a, X86Reg b) {
        bool r = static_cast<uint8_t>(b) >= 8;
        bool bb = static_cast<uint8_t>(a) >= 8;
        emit_rex(true, r, false, bb);
        emit_byte(0x39);
        emit_byte(0xC0 | ((static_cast<uint8_t>(b) & 7) << 3) |
                  (static_cast<uint8_t>(a) & 7));
    }

    // test r64, r64 (for checking low bit = Smi tag)
    void test_reg(X86Reg a, X86Reg b) {
        bool r = static_cast<uint8_t>(b) >= 8;
        bool bb = static_cast<uint8_t>(a) >= 8;
        emit_rex(true, r, false, bb);
        emit_byte(0x85);
        emit_byte(0xC0 | ((static_cast<uint8_t>(b) & 7) << 3) |
                  (static_cast<uint8_t>(a) & 7));
    }

    // ----- Control flow -----
    // jmp rel32 (returns offset of the rel32 for patching)
    size_t jmp() {
        emit_byte(0xE9);
        size_t off = code_.size();
        emit_dword(0);
        return off;
    }

    // jcc rel32 (conditional jump). cond is the condition code (e.g., 0x84 = JE).
    size_t jcc(uint8_t cond) {
        emit_byte(0x0F);
        emit_byte(cond);
        size_t off = code_.size();
        emit_dword(0);
        return off;
    }

    // Patch a rel32 jump target.
    void patch_rel32(size_t patch_offset, int32_t target_offset) {
        int32_t rel = target_offset - static_cast<int32_t>(patch_offset) - 4;
        std::memcpy(code_.data() + patch_offset, &rel, 4);
    }

    // call r64 (indirect call through register)
    void call_reg(X86Reg r) {
        bool b = static_cast<uint8_t>(r) >= 8;
        emit_rex(false, false, false, b);
        emit_byte(0xFF);
        emit_byte(0xD0 | (static_cast<uint8_t>(r) & 7));
    }

    // ret
    void ret() { emit_byte(0xC3); }

    // nop
    void nop() { emit_byte(0x90); }

    // push/pop r64
    void push(X86Reg r) {
        if (static_cast<uint8_t>(r) >= 8) emit_byte(0x41);
        emit_byte(0x50 + (static_cast<uint8_t>(r) & 7));
    }
    void pop(X86Reg r) {
        if (static_cast<uint8_t>(r) >= 8) emit_byte(0x41);
        emit_byte(0x58 + (static_cast<uint8_t>(r) & 7));
    }

private:
    // Emit ModR/M + SIB (if needed) + displacement for [base + disp]
    void emit_modrm_sib_disp(uint8_t reg, uint8_t base, int32_t disp) {
        if (disp == 0 && base != 5) {  // RBP/R13 needs disp8=0
            emit_byte(0x00 | (reg << 3) | base);
        } else if (disp >= -128 && disp <= 127) {
            emit_byte(0x40 | (reg << 3) | base);
            emit_byte(static_cast<uint8_t>(disp));
        } else {
            emit_byte(0x80 | (reg << 3) | base);
            emit_dword(static_cast<uint32_t>(disp));
        }
    }

    std::vector<uint8_t> code_;
};

}  // namespace v12

#endif  // V12_JIT_X86_EMITTER_H_
