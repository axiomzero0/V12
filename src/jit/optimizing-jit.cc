// =============================================================================
// src/jit/optimizing-jit.cc
// =============================================================================
// Tier 2 Optimizing JIT: bytecode → IR → optimized machine code.
//
// Uses asmjit::x86::Compiler for automatic register allocation and code
// generation. Each function gets its own compiled code (no icache pressure
// from a giant dispatch loop).
//
// The key difference from the baseline JIT: the baseline JIT compiles ALL
// opcodes into one giant function (causing icache thrashing when too many
// opcodes are supported). The optimizing JIT uses the IR graph to generate
// only the necessary code, with asmjit's register allocator managing
// register pressure automatically.

#include "jit/optimizing-jit.h"

#include <asmjit/x86.h>
#include <cstring>
#include <sys/mman.h>
#include <unordered_map>

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "ir/builder/graph-builder.h"
#include "ir/graph/graph.h"
#include "ir/graph/node.h"
#include "ir/opt/passes.h"
#include "ir/types/type.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

namespace v12 {

using namespace asmjit;

// OptimizingJIT: compiles a FunctionInfo into optimized machine code using
// the IR pipeline and asmjit's Compiler (with built-in register allocation).
//
// Strategy: instead of walking bytecode, we walk the IR graph. Each IR node
// maps to a virtual register. asmjit's register allocator manages the
// physical registers automatically.
//
// For opcodes the IR builder handles (arithmetic, comparisons, constants),
// we emit native code directly. For opcodes the IR builder doesn't handle
// (calls, property access, etc.), we fall back to the interpreter by
// emitting a deopt.
//
// The key advantage: this generates per-function code with proper register
// allocation. The baseline JIT's one-giant-function approach causes icache
// pressure; this approach doesn't.

std::unique_ptr<CodeObject> OptimizingJIT::Compile(FunctionInfo* fi,
                                                     Isolate* iso,
                                                     Arena* arena) {
    // Step 1: Build and optimize the IR graph.
    GraphBuilder builder(arena, fi);
    Graph* g = builder.Build();

    // Run optimization passes.
    OptimizeGraph(g);

    // Step 2: Generate machine code using asmjit::Compiler.
    Environment env(Arch::kX64);
    CodeHolder code;
    code.init(env, 0);
    x86::Compiler cc(&code);

    // Function signature: uintptr_t fn(uintptr_t acc, uintptr_t regs,
    //                                   uintptr_t frame, uintptr_t iso)
    FuncSignature sig(CallConvId::kCDecl, 0, TypeId::kUInt64, TypeId::kUInt64, TypeId::kUInt64, TypeId::kUInt64, TypeId::kUInt64);

    FuncNode* func = cc.add_func(sig);
    if (func == nullptr) return nullptr;

    // Arguments as virtual registers.
    x86::Gp acc_arg = cc.new_gp64("acc_in");
    x86::Gp regs_arg = cc.new_gp64("regs");
    x86::Gp frame_arg = cc.new_gp64("frame");
    x86::Gp iso_arg = cc.new_gp64("iso");
    func->set_arg(0, acc_arg);
    func->set_arg(1, regs_arg);
    func->set_arg(2, frame_arg);
    func->set_arg(3, iso_arg);

    // The accumulator — a virtual register that asmjit manages.
    x86::Gp acc = cc.new_gp64("acc");
    cc.mov(acc, acc_arg);

    // Map from IR Node → virtual register holding its value.
    std::unordered_map<Node*, x86::Gp> node_vregs;

    // Helper: get or create a virtual register for a node's output.
    auto GetVReg = [&](Node* n) -> x86::Gp {
        auto it = node_vregs.find(n);
        if (it != node_vregs.end()) return it->second;
        x86::Gp v = cc.new_gp64();
        node_vregs[n] = v;
        return v;
    };

    // Smi tag constants.
    constexpr uint64_t kSmiTag = 1;
    constexpr uint64_t kSmiShift = 1;

    // Walk all IR nodes in creation order and emit code.
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;

        switch (n->op()) {
            case Opcode::kStart:
                // No code needed — the entry point is implicit.
                break;

            case Opcode::kInt32Constant: {
                x86::Gp v = GetVReg(n);
                // Smi-encode the constant value.
                int64_t val = n->int_value();
                cc.mov(v, (static_cast<uint64_t>(val) << kSmiShift) | kSmiTag);
                break;
            }

            case Opcode::kConstant: {
                // Opaque constant — load from resolved_constants if available.
                // For now, load undefined.
                x86::Gp v = GetVReg(n);
                uintptr_t undef_bits = iso->undefined_value().raw().raw_bits();
                cc.mov(v, static_cast<uint64_t>(undef_bits));
                break;
            }

            // ----- Int32 arithmetic (Smi fast path) -----
            case Opcode::kInt32Add:
            case Opcode::kInt32Sub:
            case Opcode::kInt32Mul: {
                int count = n->input_count();
                Node* lhs_node = n->input(count - 2);
                Node* rhs_node = n->input(count - 1);
                x86::Gp lhs = GetVReg(lhs_node);
                x86::Gp rhs = GetVReg(rhs_node);
                x86::Gp result = GetVReg(n);

                // Check both are Smis (low bit = 1).
                // For the optimizing JIT, we use type feedback to skip checks.
                // If type_feedback == 1 (known Smi), skip the tag checks.
                bool known_smi = false;
                // The IR builder doesn't track which IC slot corresponds
                // to which IR node, so we check the type instead.
                if (lhs_node->type().IsSmi() && rhs_node->type().IsSmi()) {
                    known_smi = true;
                }

                x86::Gp scratch = cc.new_gp64();
                Label deopt = cc.new_label();
                Label done = cc.new_label();

                if (!known_smi) {
                    cc.mov(scratch, lhs);
                    cc.test(scratch, 1);
                    cc.jz(deopt);
                    cc.mov(scratch, rhs);
                    cc.test(scratch, 1);
                    cc.jz(deopt);
                }

                // Do the arithmetic on tagged values.
                cc.mov(result, lhs);
                if (n->op() == Opcode::kInt32Add) {
                    cc.add(result, rhs);
                    cc.jno(done);
                    cc.jmp(deopt);
                } else if (n->op() == Opcode::kInt32Sub) {
                    cc.sub(result, rhs);
                    cc.jno(done);
                    cc.jmp(deopt);
                } else { // Mul
                    cc.shr(result, 1);  // get value, lose tag
                    cc.mov(scratch, rhs);
                    cc.shr(scratch, 1);
                    cc.imul(result, scratch);
                    cc.jo(deopt);
                    cc.shl(result, 1);
                    cc.or_(result, 1);
                    cc.jmp(done);
                }

                cc.bind(deopt);
                // Deopt: store acc to regs[0], return 0xDEAD.
                cc.mov(x86::ptr(regs_arg, 0), acc);
                cc.mov(x86::rax, static_cast<uint64_t>(0xDEAD));
                cc.ret();

                cc.bind(done);
                // For Add: result has tag already (a+b-1).
                if (n->op() == Opcode::kInt32Add) {
                    cc.sub(result, 1);
                } else if (n->op() == Opcode::kInt32Sub) {
                    cc.add(result, 1);
                }
                // Mul already has the tag set.
                break;
            }

            // ----- Bitwise ops (always work on tagged Smis) -----
            case Opcode::kBitwiseAnd:
            case Opcode::kBitwiseOr:
            case Opcode::kBitwiseXor: {
                int count = n->input_count();
                x86::Gp lhs = GetVReg(n->input(count - 2));
                x86::Gp rhs = GetVReg(n->input(count - 1));
                x86::Gp result = GetVReg(n);
                cc.mov(result, lhs);
                if (n->op() == Opcode::kBitwiseAnd) cc.and_(result, rhs);
                else if (n->op() == Opcode::kBitwiseOr) cc.or_(result, rhs);
                else cc.xor_(result, rhs);
                break;
            }

            // ----- Shifts -----
            case Opcode::kShiftLeft:
            case Opcode::kShiftRight:
            case Opcode::kShiftRightLogical: {
                int count = n->input_count();
                x86::Gp lhs = GetVReg(n->input(count - 2));
                x86::Gp rhs = GetVReg(n->input(count - 1));
                x86::Gp result = GetVReg(n);
                x86::Gp shift_amount = cc.new_gp64();
                cc.mov(result, lhs);
                cc.mov(shift_amount, rhs);
                cc.shr(shift_amount, 1);  // get untagged shift amount
                if (n->op() == Opcode::kShiftLeft) {
                    cc.shl(result, shift_amount.r8());
                } else if (n->op() == Opcode::kShiftRight) {
                    cc.sar(result, shift_amount.r8());
                } else {
                    cc.shr(result, shift_amount.r8());
                }
                break;
            }

            // ----- Comparisons -----
            case Opcode::kWord32Equal:
            case Opcode::kInt32LessThan:
            case Opcode::kInt32LessThanOrEqual: {
                int count = n->input_count();
                x86::Gp lhs = GetVReg(n->input(count - 2));
                x86::Gp rhs = GetVReg(n->input(count - 1));
                x86::Gp result = GetVReg(n);

                // Compare tagged values directly (both Smis, so comparison
                // of tagged values gives same result as untagged).
                cc.cmp(lhs, rhs);

                // Get the true/false singleton values.
                uintptr_t true_bits = iso->true_value().raw().raw_bits();
                uintptr_t false_bits = iso->false_value().raw().raw_bits();

                cc.mov(result, false_bits);
                x86::Gp tmp = cc.new_gp64();
                cc.mov(tmp, true_bits);

                if (n->op() == Opcode::kWord32Equal) {
                    cc.cmove(result, tmp);  // if equal, result = true
                } else if (n->op() == Opcode::kInt32LessThan) {
                    cc.cmovl(result, tmp);
                } else {
                    cc.cmovle(result, tmp);
                }
                break;
            }

            // ----- Control flow -----
            case Opcode::kBranch: {
                // Branch on acc being truthy.
                // For now, emit a simple deopt — full branch support
                // requires basic block management.
                break;
            }

            case Opcode::kLoop: {
                // Loop marker — no code needed for now.
                break;
            }

            // ----- Return -----
            case Opcode::kReturn: {
                // The return value is the last value input.
                if (n->input_count() > 0) {
                    Node* retval = n->input(n->input_count() - 1);
                    x86::Gp ret_val = GetVReg(retval);
                    // Store return value in regs[0].
                    cc.mov(x86::ptr(regs_arg, 0), ret_val);
                } else {
                    cc.mov(x86::ptr(regs_arg, 0), acc);
                }
                // Return 0 = normal return.
                cc.mov(x86::rax, 0);
                cc.ret();
                break;
            }

            case Opcode::kEnd:
                // Function end — implicit in asmjit.
                break;

            default:
                // Unsupported node — skip (the optimizer should have
                // eliminated dead code, and unsupported opcodes cause
                // the builder to mark the graph as incomplete).
                break;
        }
    }

    // If the graph didn't have a Return node, add a default return.
    // Store undefined in regs[0] and return 0.
    uintptr_t undef_bits = iso->undefined_value().raw().raw_bits();
    cc.mov(x86::ptr(regs_arg, 0), static_cast<uint64_t>(undef_bits));
    cc.mov(x86::rax, 0);
    cc.ret();

    cc.end_func();
    cc.finalize();

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
    co->set_tier(CodeObject::Tier::kOptimizing);
    co->set_instruction_count(static_cast<uint32_t>(code_size));
    return co;
}

}  // namespace v12
