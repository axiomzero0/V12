// =============================================================================
// src/interpreter/interpreter.cc
// =============================================================================
// The register-based bytecode interpreter.
//
// Dispatch loop:
//   The loop reads an opcode byte, switches on it, and executes the handler.
//   Each handler reads its operands from the PC and advances the PC past
//   them. The accumulator (acc) is held in a hot C++ local variable to
//   avoid a memory load on every operation.
//
// Implementation notes:
//   - All heap allocations go through Isolate::Allocate (which goes through
//     the Heap). The GC is currently a no-op mark-sweep; see heap.cc.
//   - Calling a JSFunction re-enters ExecuteTop recursively. This keeps
//     the C++ call stack in sync with the JS call stack, which makes
//     stack traces trivial. The downside is that we can't do tail calls
//     (a future "bottom-of-loop call" optimization would fix this).
//   - Throw is implemented by returning a kThrew result up the call chain.
//     TryCatch (when implemented) will catch the throw at the appropriate
//     frame. For now, an uncaught throw terminates execution with the
//     exception value dumped to stderr.

#include "interpreter/interpreter.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(V12_OS_LINUX) || defined(V12_OS_MACOS)
#include <sys/mman.h>
#endif

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
#ifndef V12_NO_JIT
#include "ir/opt/bytecode-optimizer.h"
#include "jit/baseline-jit.h"
#include "jit/optimizing-jit.h"
#endif
#include "vm/isolate/isolate.h"
#include "vm/objects/context.h"
#include "vm/objects/object.h"
#include "vm/objects/primitives.h"
#include "vm/runtime/runtime.h"

namespace v12 {

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------
Interp::Interp(Isolate* iso) : iso_(iso) {
#if defined(V12_OS_LINUX) || defined(V12_OS_MACOS)
    void* mem = mmap(nullptr, kRegRegionSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    V12_CHECK(mem != MAP_FAILED, "failed to mmap register storage region");
    reg_base_ = static_cast<Value*>(mem);
#else
    reg_base_ = static_cast<Value*>(std::malloc(kRegRegionSize));
    V12_CHECK(reg_base_ != nullptr, "failed to allocate register storage");
#endif
    reg_stack_top_ = 0;
    // Pre-allocate the frame array. 100k frames * 48 bytes = 4.8 MB.
    frames_ = static_cast<Frame*>(std::malloc(max_depth_ * sizeof(Frame)));
    V12_CHECK(frames_ != nullptr, "failed to allocate frame stack");
    frame_top_ = 0;
    // Pre-fill the first page of registers with undefined so that
    // missing parameters don't need to be filled per-call.
    Value undef = iso->undefined_value();
    for (size_t i = 0; i < 1024; ++i) reg_base_[i] = undef;
}

Interp::~Interp() {
#if defined(V12_OS_LINUX) || defined(V12_OS_MACOS)
    if (reg_base_) munmap(reg_base_, kRegRegionSize);
#else
    std::free(reg_base_);
#endif
    std::free(frames_);
}

// -----------------------------------------------------------------------------
// Top-level entry points
// -----------------------------------------------------------------------------
InterpResult Interp::Run(BytecodeProgram* program) {
    FunctionInfo* top = program->toplevel;
    if (top == nullptr) {
        return {InterpStatus::kReturned, iso_->undefined_value()};
    }
    BytecodeProgram* prev_program = current_program_;
    current_program_ = program;
    PushFrame(top, nullptr, iso_->undefined_value(), nullptr, 0, nullptr);
    InterpResult r = ExecuteTop();
    PopFrame();
    current_program_ = prev_program;
    return r;
}

InterpResult Interp::CallFunction(JSFunction* fn, Value this_val,
                                   const Value* args, uint32_t argc) {
    FunctionInfo* info = fn->shared_info();
    if (info == nullptr) {
        // Shouldn't happen for real JSFunctions.
        Value exc = Value::FromHeap(JSString::New(iso_,
            "TypeError: not a function"));
        return {InterpStatus::kThrew, exc};
    }
    PushFrame(info, fn, this_val, fn->closure_context(), argc, args);
    InterpResult r = ExecuteTop();
    PopFrame();
    return r;
}

InterpResult Interp::CallHostFunction(HostFunction* fn, Value this_val,
                                       const Value* args, uint32_t argc) {
    Value result = fn->fn()(this, this_val, const_cast<Value*>(args), argc);
    return {InterpStatus::kReturned, result};
}

// -----------------------------------------------------------------------------
// Dispatch loop
//
// We use computed-goto dispatch (GCC/Clang's "labels as values" extension).
// This is significantly faster than a switch statement because:
//   1. Each opcode handler ends with `goto *dispatch[*pc++]` — a single
//      indirect branch — instead of `break; loop-check; switch-jump`.
//   2. The branch predictor can learn the pattern of opcodes (e.g. "Add
//      is usually followed by Star") and predict the indirect branch
//      correctly, which is impossible with a single switch jump table.
//
// This requires GCC or Clang. MSVC is not supported.
// -----------------------------------------------------------------------------

// Macro: throw an exception and either jump to a catch handler or return.
// Uses HandleException (noinline) to keep the cold exception-handling code
// out of ExecuteTop, reducing function size and improving register allocation.
// `pc_off_expr` is evaluated to get the bytecode offset of the throwing
// instruction (for handler-table lookup).
#define V12_THROW(exc_val, pc_off_expr) do { \
    Value _exc = (exc_val); \
    DispatchState _ds{frame, pc, acc, info, regs, ctx, bytecode_base}; \
    if (HandleException(_exc, (pc_off_expr), _ds)) { \
        frame = _ds.frame; pc = _ds.pc; acc = _ds.acc; \
        info = _ds.info; regs = _ds.regs; ctx = _ds.ctx; \
        bytecode_base = _ds.bytecode_base; \
        V12_DISPATCH(); \
    } \
    return {InterpStatus::kThrew, _exc}; \
} while(0)

// Cold exception-handling helper. Called from Call/CallProperty/Construct
// handlers when a TypeError is thrown or a callee throws. Searches the
// handler table for a catch clause. Returns true if a handler was found
// (ds is updated to jump to the catch handler); false if no handler was
// found (caller should return kThrew).
//
// Marked [[gnu::noinline]] to keep this cold code out of ExecuteTop.
bool Interp::HandleException(Value exc, uint32_t pc_offset, DispatchState& ds) {
    pending_exception_ = exc;
    uint32_t catch_off = ds.info->FindHandler(pc_offset);
    if (catch_off != 0xFFFFFFFF) {
        ds.frame = &frames_[frame_top_ - 1];
        ds.regs = ds.frame->regs;
        ds.ctx = ds.frame->context;
        ds.info = ds.frame->info;
        ds.bytecode_base = ds.info->bytecode.data();
        ds.pc = ds.bytecode_base + catch_off;
        ds.acc = exc;
        return true;
    }
    return false;
}

// ----- Cold opcode handlers (extracted from ExecuteTop) -----
// These are rarely-executed opcodes with complex handlers. Extracting them
// into [[gnu::noinline]] functions reduces ExecuteTop's code size, improving
// register allocation for hot paths.

// Construct (new operator) — allocates a new object, then calls the callee.
// Returns true on success, false if an exception was thrown (ds.acc contains
// the exception value, ds is updated to jump to the catch handler if found).
bool Interp::HandleConstruct(DispatchState& ds) {
    Frame* frame = ds.frame;
    const uint8_t* pc = ds.pc;
    Value acc = ds.acc;
    FunctionInfo* info = ds.info;
    Value* regs = ds.regs;
    Context* ctx = ds.ctx;
    const uint8_t* bytecode_base = ds.bytecode_base;

    uint16_t argc = ReadU16(&pc);
    uint8_t first_arg = ReadU8(&pc);
    pc += 2;
    Value callee = acc;
    Value* args = regs + first_arg;
    Value new_obj = Value::FromHeap(JSObject::New(iso_));
    // Host function: call directly.
    if (callee.IsHostFunction()) {
        Value result = callee.AsHostFunction()->fn()(
            this, new_obj, const_cast<Value*>(args), argc);
        acc = result.IsObject() ? result : new_obj;
        ds.frame = frame; ds.pc = pc; ds.acc = acc;
        return true;
    }
    // JS function: inline call.
    if (callee.IsFunction()) {
        JSFunction* fn = callee.AsFunction();
        FunctionInfo* callee_info = fn->shared_info();
        if (callee_info == nullptr) {
            Value exc = Value::FromHeap(JSString::New(iso_,
                "TypeError: not a constructor"));
            uint32_t call_off = static_cast<uint32_t>((pc - bytecode_base) - 1);
            if (HandleException(exc, call_off, ds)) return true;
            ds.acc = exc;
            return false;
        }
        frame->pc = pc;
        Value* new_regs = PushFrame(callee_info, fn, new_obj,
                                    fn->closure_context(), argc, args);
        frame = &frames_[frame_top_ - 1];
        regs = new_regs;
        ctx = frame->context;
        info = callee_info;
        bytecode_base = info->bytecode.data();
        pc = bytecode_base;
        ds.frame = frame; ds.pc = pc; ds.acc = acc;
        ds.info = info; ds.regs = regs; ds.ctx = ctx;
        ds.bytecode_base = bytecode_base;
        return true;
    }
    // Not callable.
    {
        Value exc = Value::FromHeap(JSString::New(iso_,
            "TypeError: value is not a constructor"));
        uint32_t call_off = static_cast<uint32_t>((pc - bytecode_base) - 1);
        if (HandleException(exc, call_off, ds)) return true;
        ds.acc = exc;
        return false;
    }
}

// ObjectKeys — creates an array of property name strings.
void Interp::HandleObjectKeys(DispatchState& ds) {
    ds.pc += 2;  // skip feedback slot
    JSArray* arr = JSArray::New(iso_, 4);
    if (ds.acc.IsObject()) {
        JSObject* obj = ds.acc.AsObject();
        Shape* shape = obj->shape();
        for (uint16_t i = 0; i < shape->property_count(); ++i) {
            std::string_view name = shape->PropertyNameAt(i);
            Value key = Value::FromHeap(JSString::New(iso_, name));
            arr->Push(iso_, key);
        }
    } else if (ds.acc.IsArray()) {
        JSArray* a = ds.acc.AsArray();
        for (uint32_t i = 0; i < a->length(); ++i) {
            Value key = Value::FromHeap(JSString::NewFromSmi(iso_, static_cast<intptr_t>(i)));
            arr->Push(iso_, key);
        }
    }
    ds.acc = Value::FromHeap(arr);
}

// Throw — walks the call stack looking for a catch handler.
// Returns true if a handler was found (ds updated to jump to catch),
// false if uncaught (caller should return kThrew).
bool Interp::HandleThrow(DispatchState& ds) {
    pending_exception_ = ds.acc;
    Frame* frame = ds.frame;
    const uint8_t* pc = ds.pc;
    Value acc = ds.acc;
    FunctionInfo* info = ds.info;
    Value* regs = ds.regs;
    Context* ctx = ds.ctx;
    const uint8_t* bytecode_base = ds.bytecode_base;

    uint32_t throw_off = static_cast<uint32_t>((pc - bytecode_base) - 1);
    while (true) {
        uint32_t catch_off = info->FindHandler(throw_off);
        if (catch_off != 0xFFFFFFFF) {
            pc = bytecode_base + catch_off;
            acc = pending_exception_;
            ds.frame = frame; ds.pc = pc; ds.acc = acc;
            ds.info = info; ds.regs = regs; ds.ctx = ctx;
            ds.bytecode_base = bytecode_base;
            return true;
        }
        if (frame_top_ <= 1) {
            frame->pc = pc;
            ds.acc = acc;
            return false;
        }
        PopFrame();
        frame = &frames_[frame_top_ - 1];
        regs = frame->regs;
        ctx = frame->context;
        info = frame->info;
        bytecode_base = info->bytecode.data();
        throw_off = static_cast<uint32_t>((frame->pc - bytecode_base) - 1);
    }
}

// CallBuiltin — stub (not yet fully implemented).
void Interp::HandleCallBuiltin(DispatchState& ds) {
    (void)ReadU8(&ds.pc);
    ds.pc += 2;
    (void)ReadU8(&ds.pc);
    ds.acc = iso_->undefined_value();
}

#ifdef V12_OPCODE_STATS
// Global opcode dispatch counters (for profiling). Indexed by opcode byte.
// Declared here and defined in the header so the benchmark tool can read them.
uint64_t g_opcode_dispatch_counts[256] = {};
#define V12_DISPATCH()  do { g_opcode_dispatch_counts[*pc]++; goto *dispatch_table[*pc++]; } while(0)
#else
#define V12_DISPATCH()  goto *dispatch_table[*pc++]
#endif

InterpResult Interp::ExecuteTop() {
    Frame* frame = &frames_[frame_top_ - 1];
    const uint8_t* pc = frame->pc;
    Value acc = iso_->undefined_value();
    FunctionInfo* info = frame->info;
    Value* regs = frame->regs;
    Context* ctx = frame->context;
    // Cache the bytecode base pointer — used by every Jump handler and by
    // Call for handler-table lookup. Avoids repeated `info->bytecode.data()`
    // calls (which are vector data accesses).
    const uint8_t* bytecode_base = info->bytecode.data();

    // Computed-goto dispatch table. Each entry is the address of the label
    // for that opcode. Indexed by the opcode byte.
    static const void* dispatch_table[] = {
        &&L_LdaConst, &&L_LdaSmi, &&L_LdaSmi16, &&L_LdaZero, &&L_LdaUndefined,
        &&L_LdaNull, &&L_LdaTrue, &&L_LdaFalse, &&L_LdaThis,
        &&L_Ldar, &&L_Star, &&L_Mov,
        &&L_Add, &&L_Sub, &&L_Mul, &&L_Div, &&L_Mod, &&L_Exp,
        &&L_BitOr, &&L_BitAnd, &&L_BitXor, &&L_Shl, &&L_Shr, &&L_Ushr,
        &&L_AddConst, &&L_SubConst, &&L_MulConst, &&L_AddSmiConst, &&L_SubSmiConst,
        &&L_Negate, &&L_BitNot, &&L_LogicalNot, &&L_Typeof,
        &&L_TestEqual, &&L_TestNotEqual, &&L_TestEqStrict, &&L_TestNotEqStrict,
        &&L_TestLessThan, &&L_TestGreaterThan, &&L_TestLessThanOrEqual,
        &&L_TestGreaterThanOrEqual, &&L_TestInstanceOf, &&L_TestIn,
        &&L_Inc, &&L_Dec, &&L_IncReg, &&L_DecReg, &&L_AddConstToReg,
        &&L_Jump, &&L_JumpIfTrue, &&L_JumpIfFalse, &&L_JumpIfNull,
        &&L_JumpIfUndefined, &&L_JumpIfNotNullOrUndefined,
        &&L_JumpIfToBooleanTrue, &&L_JumpIfToBooleanFalse, &&L_JumpLoop,
        &&L_LoadProperty, &&L_LoadIndexed, &&L_StoreProperty, &&L_StoreIndexed,
        &&L_LoadGlobal, &&L_StoreGlobal, &&L_LoadContext, &&L_StoreContext,
        &&L_Call, &&L_CallProperty, &&L_Call0, &&L_Call1, &&L_Call2,
        &&L_Construct, &&L_CallBuiltin,
        &&L_NewObject, &&L_NewArray, &&L_DefineProperty, &&L_CreateClosure,
        &&L_PushArray, &&L_LoadArrayLength, &&L_StoreArrayLength,
        &&L_CreateContext, &&L_PushContext, &&L_PopContext,
        &&L_ObjectKeys, &&L_GetIterator,
        &&L_ForInPrepare, &&L_ForInNext, &&L_ForInDone,
        &&L_Return, &&L_ReturnUndefined,
        &&L_Throw, &&L_TryCatch, &&L_TryFinally, &&L_Exception,
        &&L_Debugger,
        &&L_Pop, &&L_Dup, &&L_Nop, &&L_Illegal,
    };
    V12_DISPATCH();
            // ----- Loading constants -----
            L_LdaConst: {
                uint32_t idx = ReadU32(&pc);
                acc = info->ResolveConstant(iso_, idx);
                V12_DISPATCH();
            }
            L_LdaSmi: {
                uint8_t v = ReadU8(&pc);
                acc = Value::FromSmi(static_cast<intptr_t>(v));
                V12_DISPATCH();
            }
            L_LdaSmi16: {
                int16_t v = static_cast<int16_t>(ReadU16(&pc));
                acc = Value::FromSmi(static_cast<intptr_t>(v));
                V12_DISPATCH();
            }
            L_LdaZero:
                acc = Value::FromSmi(0);
                V12_DISPATCH();
            L_LdaUndefined:
                acc = iso_->undefined_value();
                V12_DISPATCH();
            L_LdaNull:
                acc = iso_->null_value();
                V12_DISPATCH();
            L_LdaTrue:
                acc = iso_->true_value();
                V12_DISPATCH();
            L_LdaFalse:
                acc = iso_->false_value();
                V12_DISPATCH();
            L_LdaThis:
                acc = frame->this_val;
                V12_DISPATCH();

            // ----- Register moves -----
            L_Ldar: {
                uint8_t r = ReadU8(&pc);
                acc = regs[r];
                V12_DISPATCH();
            }
            L_Star: {
                uint8_t r = ReadU8(&pc);
                regs[r] = acc;
                V12_DISPATCH();
            }
            L_Mov: {
                uint8_t dst = ReadU8(&pc);
                uint8_t src = ReadU8(&pc);
                regs[dst] = regs[src];
                V12_DISPATCH();
            }

            // ----- Binary arithmetic -----
            // Format: op  r:8  idx:16   ->   acc = acc <op> regs[r]
            // Hot path: try Smi fast path first, fall back to slow path.
            // Type feedback: 0 = uninit, 1 = Smi, 2 = Number, 3 = String.
            // On the fast path, we set feedback to kSmi (1) once — this lets
            // the JIT skip the Smi tag checks on recompilation.
            // On the slow path, we record the actual type.
            L_Add: {
                uint8_t r = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                Value lhs = acc, rhs = regs[r];
                Value result;
                if (V12_LIKELY(TrySmiAdd(lhs, rhs, &result))) {
                    acc = result;
                    // Set feedback to Smi (only if not already set — avoids
                    // per-iteration store overhead).
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_UNLIKELY(ic.type_feedback == 0)) ic.type_feedback = 1;
                } else {
                    acc = Add(iso_, lhs, rhs);
                    info->GetIC(ic_slot).type_feedback =
                        static_cast<uint8_t>(ClassifyBinaryOp(lhs, rhs));
                }
                V12_DISPATCH();
            }
            L_Sub: {
                uint8_t r = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                Value lhs = acc, rhs = regs[r];
                Value result;
                if (V12_LIKELY(TrySmiSub(lhs, rhs, &result))) {
                    acc = result;
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_UNLIKELY(ic.type_feedback == 0)) ic.type_feedback = 1;
                } else {
                    acc = Sub(iso_, lhs, rhs);
                    info->GetIC(ic_slot).type_feedback =
                        static_cast<uint8_t>(ClassifyBinaryOp(lhs, rhs));
                }
                V12_DISPATCH();
            }
            L_Mul: {
                uint8_t r = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                Value lhs = acc, rhs = regs[r];
                Value result;
                if (V12_LIKELY(TrySmiMul(lhs, rhs, &result))) {
                    acc = result;
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_UNLIKELY(ic.type_feedback == 0)) ic.type_feedback = 1;
                } else {
                    acc = Mul(iso_, lhs, rhs);
                    info->GetIC(ic_slot).type_feedback =
                        static_cast<uint8_t>(ClassifyBinaryOp(lhs, rhs));
                }
                V12_DISPATCH();
            }
            L_Div: {
                uint8_t r = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                Value lhs = acc, rhs = regs[r];
                Value result;
                if (V12_LIKELY(TrySmiDiv(lhs, rhs, &result))) {
                    acc = result;
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_UNLIKELY(ic.type_feedback == 0)) ic.type_feedback = 1;
                } else {
                    acc = Div(iso_, lhs, rhs);
                    info->GetIC(ic_slot).type_feedback =
                        static_cast<uint8_t>(ClassifyBinaryOp(lhs, rhs));
                }
                V12_DISPATCH();
            }
            L_Mod: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                acc = Mod(iso_, acc, regs[r]);
                V12_DISPATCH();
            }
            L_Exp: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                acc = Exp(iso_, acc, regs[r]);
                V12_DISPATCH();
            }
            L_BitOr: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value result;
                if (V12_LIKELY(TrySmiBitOr(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = BitOr(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_BitAnd: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value result;
                if (V12_LIKELY(TrySmiBitAnd(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = BitAnd(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_BitXor: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value result;
                if (V12_LIKELY(TrySmiBitXor(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = BitXor(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Shl: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value result;
                if (V12_LIKELY(TrySmiShl(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = Shl(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Shr: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value result;
                if (V12_LIKELY(TrySmiShr(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = Shr(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Ushr: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                acc = Ushr(iso_, acc, regs[r]);
                V12_DISPATCH();
            }

            // ----- Binary with constant -----
            // Format: op  const:32  idx:16
            L_AddConst: {
                uint32_t cidx = ReadU32(&pc);
                pc += 2;
                acc = Add(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }
            L_SubConst: {
                uint32_t cidx = ReadU32(&pc);
                pc += 2;
                acc = Sub(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }
            L_MulConst: {
                uint32_t cidx = ReadU32(&pc);
                pc += 2;
                acc = Mul(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }
            // AddSmiConst/SubSmiConst: acc += Smi(imm8) / acc -= Smi(imm8).
            // Baked small-int immediate — no ResolveConstant, no Add() call.
            L_AddSmiConst: {
                uint8_t imm = ReadU8(&pc);
                pc += 2;  // skip feedback slot
                Value smi_val = Value::FromSmi(static_cast<intptr_t>(imm));
                Value result;
                if (V12_LIKELY(TrySmiAdd(acc, smi_val, &result))) {
                    acc = result;
                } else {
                    acc = Add(iso_, acc, smi_val);
                }
                V12_DISPATCH();
            }
            L_SubSmiConst: {
                uint8_t imm = ReadU8(&pc);
                pc += 2;  // skip feedback slot
                Value smi_val = Value::FromSmi(static_cast<intptr_t>(imm));
                Value result;
                if (V12_LIKELY(TrySmiSub(acc, smi_val, &result))) {
                    acc = result;
                } else {
                    acc = Sub(iso_, acc, smi_val);
                }
                V12_DISPATCH();
            }

            // ----- Unary -----
            // Format: op  idx:16   (idx is a feedback slot)
            L_Negate: {
                pc += 2;
                acc = Negate(iso_, acc);
                V12_DISPATCH();
            }
            L_BitNot: {
                pc += 2;
                acc = BitNot(iso_, acc);
                V12_DISPATCH();
            }
            L_LogicalNot:
                acc = IsTruthyFast(acc) ? iso_->false_value() : iso_->true_value();
                V12_DISPATCH();
            L_Typeof:
                acc = Typeof(iso_, acc);
                V12_DISPATCH();

            // ----- Comparison -----
            // Format: op  r:8  idx:16
            L_TestEqual: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = LooseEquals(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestNotEqual: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->false_value() : iso_->true_value();
                } else {
                    acc = LooseEquals(iso_, acc, regs[r]) == iso_->true_value()
                          ? iso_->false_value() : iso_->true_value();
                }
                V12_DISPATCH();
            }
            L_TestEqStrict: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = StrictEquals(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestNotEqStrict: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->false_value() : iso_->true_value();
                } else {
                    acc = StrictEquals(iso_, acc, regs[r]) == iso_->true_value()
                          ? iso_->false_value() : iso_->true_value();
                }
                V12_DISPATCH();
            }
            L_TestLessThan: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiLessThan(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = LessThan(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestGreaterThan: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiGreaterThan(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = GreaterThan(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestLessThanOrEqual: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiLessThanOrEqual(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = LessThanOrEqual(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestGreaterThanOrEqual: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                bool result;
                if (V12_LIKELY(TrySmiGreaterThanOrEqual(acc, regs[r], &result))) {
                    acc = result ? iso_->true_value() : iso_->false_value();
                } else {
                    acc = GreaterThanOrEqual(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_TestInstanceOf: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                // instanceof not yet implemented; default to false.
                acc = iso_->false_value();
                V12_DISPATCH();
            }
            L_TestIn: {
                uint8_t r = ReadU8(&pc);
                pc += 2;
                Value key = acc;
                Value obj = regs[r];
                if (obj.IsObject()) {
                    if (key.IsString()) {
                        std::string_view name = static_cast<JSString*>(key.AsHeapObject())->view();
                        acc = obj.AsObject()->HasProperty(name)
                              ? iso_->true_value() : iso_->false_value();
                    } else if (key.IsNumber()) {
                        uint32_t idx = static_cast<uint32_t>(ToDouble(iso_, key));
                        if (obj.IsArray()) {
                            acc = idx < obj.AsArray()->length()
                                  ? iso_->true_value() : iso_->false_value();
                        } else {
                            acc = iso_->false_value();
                        }
                    } else {
                        acc = iso_->false_value();
                    }
                } else {
                    acc = iso_->false_value();
                }
                V12_DISPATCH();
            }

            // ----- Inc / Dec -----
            L_Inc: {
                pc += 2;
                // Fast path for Smi.
                if (V12_LIKELY(acc.IsSmi())) {
                    intptr_t x = acc.AsSmi();
                    if (V12_LIKELY(SmiFitsFast(x + 1))) {
                        acc = Value::FromSmi(x + 1);
                        V12_DISPATCH();
                    }
                }
                acc = Inc(iso_, acc);
                V12_DISPATCH();
            }
            L_Dec: {
                pc += 2;
                if (V12_LIKELY(acc.IsSmi())) {
                    intptr_t x = acc.AsSmi();
                    if (V12_LIKELY(SmiFitsFast(x - 1))) {
                        acc = Value::FromSmi(x - 1);
                        V12_DISPATCH();
                    }
                }
                acc = Dec(iso_, acc);
                V12_DISPATCH();
            }
            // IncReg: R[r] = R[r] + 1. Fused Ldar+Inc+Star — one dispatch
            // instead of three. This is the hot path in for-loops (i++).
            L_IncReg: {
                uint8_t r = ReadU8(&pc);
                Value& v = regs[r];
                if (V12_LIKELY(v.IsSmi())) {
                    intptr_t x = v.AsSmi();
                    if (V12_LIKELY(SmiFitsFast(x + 1))) {
                        v = Value::FromSmi(x + 1);
                        V12_DISPATCH();
                    }
                }
                v = Inc(iso_, v);
                V12_DISPATCH();
            }
            L_DecReg: {
                uint8_t r = ReadU8(&pc);
                Value& v = regs[r];
                if (V12_LIKELY(v.IsSmi())) {
                    intptr_t x = v.AsSmi();
                    if (V12_LIKELY(SmiFitsFast(x - 1))) {
                        v = Value::FromSmi(x - 1);
                        V12_DISPATCH();
                    }
                }
                v = Dec(iso_, v);
                V12_DISPATCH();
            }
            // AddConstToReg: R[r1] = R[r1] + K[r2]. Fused for `x += const`.
            // r2 is an index into the constant pool.
            L_AddConstToReg: {
                uint8_t r1 = ReadU8(&pc);
                uint8_t r2 = ReadU8(&pc);
                Value& v = regs[r1];
                Value c = info->ResolveConstant(iso_, r2);
                Value result;
                if (V12_LIKELY(TrySmiAdd(v, c, &result))) {
                    v = result;
                } else {
                    v = Add(iso_, v, c);
                }
                V12_DISPATCH();
            }

            // ----- Control flow -----
            // Format: op  target:32
            L_Jump: {
                uint32_t target = ReadU32(&pc);
                pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfTrue: {
                uint32_t target = ReadU32(&pc);
                if (IsTruthyFast(acc)) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfFalse: {
                uint32_t target = ReadU32(&pc);
                if (!IsTruthyFast(acc)) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfNull: {
                uint32_t target = ReadU32(&pc);
                if (acc.IsNull()) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfUndefined: {
                uint32_t target = ReadU32(&pc);
                if (acc.IsUndefined()) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfNotNullOrUndefined: {
                uint32_t target = ReadU32(&pc);
                if (!acc.IsNullOrUndefined()) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfToBooleanTrue: {
                uint32_t target = ReadU32(&pc);
                if (IsTruthyFast(acc)) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpIfToBooleanFalse: {
                uint32_t target = ReadU32(&pc);
                if (!IsTruthyFast(acc)) pc = bytecode_base + target;
                V12_DISPATCH();
            }
            L_JumpLoop: {
                uint32_t target = ReadU32(&pc);
                info->hotness_counter++;
                info->ir_hotness_counter++;
#ifndef V12_NO_JIT
                // Tier 1: Baseline JIT at kOSRThreshold (500 iterations).
                // DISABLED: the baseline JIT has a correctness bug where
                // the JumpIfFalse handler incorrectly jumps for truthy
                // comparison results in OSR-compiled loops. The interpreter
                // with type feedback is fast enough. Re-enable when fixed.
                // if (V12_UNLIKELY(info->hotness_counter == BaselineJIT::kOSRThreshold &&
                //                  info->jit_code == 0)) {
                //     auto co = BaselineJIT::Compile(info, target);
                //     if (co) {
                //         info->jit_code = reinterpret_cast<uintptr_t>(co.release());
                //     }
                // }
                // Tier 1.5: IR optimization at kIROptThreshold (1000 iterations).
                // Runs the 21-pass IR optimizer to produce better bytecode
                // and type feedback, then recompiles the JIT.
                if (V12_UNLIKELY(info->ir_hotness_counter == kIROptThreshold &&
                                 !info->ir_optimized)) {
                    Arena ir_arena;
                    OptimizeBytecode(iso_, &ir_arena, info);
                }
                // JIT execution DISABLED — see comment above.
                // The interpreter with IR optimization + type feedback
                // produces correct results and competitive performance.
#endif // V12_NO_JIT
                pc = bytecode_base + target;
                V12_DISPATCH();
            }

            // ----- Property access -----
            // LoadProperty: op  name_idx:8  idx:16
            // Fast path: check the inline cache. If the receiver's shape
            // matches the cached shape, read the property directly from
            // the cached slot — no string comparison, no shape lookup.
            L_LoadProperty: {
                uint8_t name_idx = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                // Try the IC fast path for objects.
                if (V12_LIKELY(acc.IsObject())) {
                    JSObject* obj = acc.AsObject();
                    auto& ic = info->GetIC(ic_slot);
                    Shape* obj_shape = obj->shape();
                    if (V12_LIKELY(ic.initialized &&
                                   ic.shape == reinterpret_cast<uintptr_t>(obj_shape))) {
                        // IC hit: read directly from the cached slot.
                        acc = obj->properties()[ic.slot];
                        V12_DISPATCH();
                    }
                    // IC miss: check polymorphic IC if present (up to 4 shapes).
                    uintptr_t shape_bits = reinterpret_cast<uintptr_t>(obj_shape);
                    if (ic.poly_count >= 1 && ic.poly_count <= 4 &&
                        ic.poly_entries != nullptr) {
                        for (uint8_t p = 0; p < ic.poly_count; ++p) {
                            if (ic.poly_entries[p].shape == shape_bits) {
                                acc = obj->properties()[ic.poly_entries[p].slot];
                                V12_DISPATCH();
                            }
                        }
                    }
                    // Poly miss or megamorphic: do a full lookup.
                    // Use LookupInterned for pointer-compare (2-3x faster).
                    Shape::Slot slot = obj_shape->LookupInterned(
                        iso_->Intern(info->property_names[name_idx]));
                    if (slot != Shape::kInvalidSlot) {
                        // Update the IC: uninit → mono → poly(2-4) → mega.
                        if (!ic.initialized) {
                            // First shape: go monomorphic.
                            ic.shape = shape_bits;
                            ic.slot = slot;
                            ic.initialized = true;
                        } else if (ic.shape != shape_bits) {
                            // Second+ shape seen: upgrade to polymorphic.
                            if (ic.poly_count == 0) {
                                // First poly entry: allocate array of 4.
                                ic.poly_entries = new PolyIC[4];
                                ic.poly_entries[0].shape = ic.shape;
                                ic.poly_entries[0].slot = ic.slot;
                                ic.poly_count = 1;
                            }
                            if (ic.poly_count < 4) {
                                ic.poly_entries[ic.poly_count].shape = shape_bits;
                                ic.poly_entries[ic.poly_count].slot = slot;
                                ic.poly_count++;
                            } else {
                                // 5th shape: go megamorphic.
                                ic.poly_count = 5;
                            }
                        }
                        acc = obj->properties()[slot];
                    } else {
                        acc = iso_->undefined_value();
                    }
                    V12_DISPATCH();
                }
                // Slow path: non-object receivers (string/array .length, etc.)
                std::string_view name = info->property_names[name_idx];
                if (acc.IsString()) {
                    if (name == "length") {
                        acc = Value::FromSmi(static_cast<intptr_t>(
                            static_cast<JSString*>(acc.AsHeapObject())->length()));
                    } else {
                        acc = iso_->undefined_value();
                    }
                } else if (acc.IsArray()) {
                    if (name == "length") {
                        acc = Value::FromSmi(static_cast<intptr_t>(acc.AsArray()->length()));
                    } else {
                        acc = iso_->undefined_value();
                    }
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            L_LoadIndexed: {
                uint8_t idx_reg = ReadU8(&pc);
                pc += 2;
                Value idx_val = regs[idx_reg];
                if (acc.IsArray()) {
                    uint32_t idx = static_cast<uint32_t>(ToDouble(iso_, idx_val));
                    acc = acc.AsArray()->GetElement(idx);
                } else if (acc.IsString()) {
                    uint32_t idx = static_cast<uint32_t>(ToDouble(iso_, idx_val));
                    JSString* s = static_cast<JSString*>(acc.AsHeapObject());
                    if (idx < s->length()) {
                        char ch = s->data()[idx];
                        acc = Value::FromHeap(JSString::New(iso_, std::string_view(&ch, 1)));
                    } else {
                        acc = iso_->undefined_value();
                    }
                } else if (acc.IsObject()) {
                    Value key = ToString(iso_, idx_val);
                    std::string_view name = static_cast<JSString*>(key.AsHeapObject())->view();
                    acc = acc.AsObject()->GetProperty(iso_, name);
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            // StoreProperty: op  val_reg:8  name_idx:8  idx:16
            // Uses IC for the fast path (object with known shape).
            L_StoreProperty: {
                uint8_t val_reg = ReadU8(&pc);
                uint8_t name_idx = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                Value value = regs[val_reg];
                if (V12_LIKELY(acc.IsObject())) {
                    JSObject* obj = acc.AsObject();
                    Shape* obj_shape = obj->shape();
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_LIKELY(ic.initialized &&
                                   ic.shape == reinterpret_cast<uintptr_t>(obj_shape))) {
                        obj->properties()[ic.slot] = value;
                        V12_DISPATCH();
                    }
                    // IC miss: lookup and update cache.
                    std::string_view name = info->property_names[name_idx];
                    Shape::Slot slot = obj_shape->LookupInterned(iso_->Intern(name));
                    if (slot != Shape::kInvalidSlot) {
                        ic.shape = reinterpret_cast<uintptr_t>(obj_shape);
                        ic.slot = slot;
                        ic.initialized = true;
                        obj->properties()[slot] = value;
                    } else {
                        // New property — shape transition.
                        obj->SetProperty(iso_, name, value);
                        // Invalidate IC since shape changed.
                        ic.initialized = false;
                    }
                }
                V12_DISPATCH();
            }
            L_StoreIndexed: {
                uint8_t idx_reg = ReadU8(&pc);
                uint8_t val_reg = ReadU8(&pc);
                pc += 2;
                Value idx_val = regs[idx_reg];
                Value value = regs[val_reg];
                if (acc.IsArray()) {
                    uint32_t idx = static_cast<uint32_t>(ToDouble(iso_, idx_val));
                    acc.AsArray()->SetElement(iso_, idx, value);
                }
                V12_DISPATCH();
            }

            // ----- Variables -----
            // LoadGlobal: op  name_idx:8  idx:16
            // Uses an inline cache: the first call looks up the slot and
            // caches (shape, slot). Subsequent calls are a single shape
            // compare + array load.
            L_LoadGlobal: {
                uint8_t name_idx = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                auto& ic = info->GetIC(ic_slot);
                // Fast path: if we have a cached value pointer AND the
                // global shape hasn't changed, just load it.
                // We MUST check the shape because GrowProperties can
                // reallocate the properties array, making value_ptr dangling.
                JSObject* g = iso_->global_object();
                Shape* gshape = g->shape();
                if (V12_LIKELY(ic.value_ptr != 0 &&
                               ic.shape == reinterpret_cast<uintptr_t>(gshape))) {
                    acc = *reinterpret_cast<Value*>(ic.value_ptr);
                    V12_DISPATCH();
                }
                // Slow path: first-time lookup or shape changed.
                std::string_view name = info->property_names[name_idx];
                Shape::Slot slot = gshape->LookupInterned(iso_->Intern(name));
                if (slot != Shape::kInvalidSlot) {
                    ic.shape = reinterpret_cast<uintptr_t>(gshape);
                    ic.slot = slot;
                    ic.initialized = true;
                    ic.value_ptr = reinterpret_cast<uintptr_t>(&g->properties()[slot]);
                    acc = g->properties()[slot];
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            // StoreGlobal: op  r:8  name_idx:8  idx:16
            L_StoreGlobal: {
                (void)ReadU8(&pc);   // dummy register
                uint8_t name_idx = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);
                auto& ic = info->GetIC(ic_slot);
                // Fast path: direct store via cached pointer, but ONLY if
                // the global shape hasn't changed (GrowProperties can
                // reallocate the properties array).
                JSObject* g = iso_->global_object();
                Shape* gshape = g->shape();
                if (V12_LIKELY(ic.value_ptr != 0 &&
                               ic.shape == reinterpret_cast<uintptr_t>(gshape))) {
                    *reinterpret_cast<Value*>(ic.value_ptr) = acc;
                    V12_DISPATCH();
                }
                std::string_view name = info->property_names[name_idx];
                Shape::Slot slot = gshape->LookupInterned(iso_->Intern(name));
                if (slot != Shape::kInvalidSlot) {
                    ic.shape = reinterpret_cast<uintptr_t>(gshape);
                    ic.slot = slot;
                    ic.initialized = true;
                    ic.value_ptr = reinterpret_cast<uintptr_t>(&g->properties()[slot]);
                    g->properties()[slot] = acc;
                } else {
                    g->SetProperty(iso_, name, acc);
                    // Invalidate the IC — the shape changed.
                    ic.value_ptr = 0;
                    ic.initialized = false;
                }
                V12_DISPATCH();
            }
            // LoadContext: op  depth:16  index:16  idx:16
            L_LoadContext: {
                uint16_t depth = ReadU16(&pc);
                uint16_t index = ReadU16(&pc);
                pc += 2;
                if (ctx != nullptr) {
                    acc = ctx->LoadAt(depth, index);
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            // StoreContext: op  r:8  depth:16  index:16  idx:16
            L_StoreContext: {
                (void)ReadU8(&pc);
                uint16_t depth = ReadU16(&pc);
                uint16_t index = ReadU16(&pc);
                pc += 2;
                if (ctx != nullptr) {
                    ctx->StoreAt(depth, index, acc);
                }
                V12_DISPATCH();
            }

            // ----- Calls -----
            // Call: op  argc:16  first_arg:8  idx:16
            //   acc holds the callee.
            // CallProperty: op  argc:16  prop_idx:8  idx:16
            //   acc holds the receiver; the property name is prop_idx.
            //   The interpreter looks up the property on acc to get the
            //   callee, then calls it with this = acc.
            L_Call: {
                uint16_t argc = ReadU16(&pc);
                uint8_t first_arg = ReadU8(&pc);
                pc += 2;  // skip feedback slot

                Value callee = acc;
                Value* args = regs + first_arg;

                // Fast path: JS function (the common case).
                // Check kind directly to avoid two separate Is* calls.
                if (V12_LIKELY(callee.IsHeapObject())) {
                    HeapObjectKind kind = callee.AsHeapObject()->kind();
                    if (V12_LIKELY(kind == HeapObjectKind::kFunction)) {
                        JSFunction* fn = static_cast<JSFunction*>(callee.AsHeapObject());
                        FunctionInfo* callee_info = fn->shared_info();
                        // Save return PC.
                        frame->pc = pc;
                        // Push frame and switch to callee.
                        regs = PushFrame(callee_info, fn, iso_->undefined_value(),
                                         fn->closure_context(), argc, args);
                        frame = &frames_[frame_top_ - 1];
                        ctx = frame->context;
                        info = callee_info;
                        bytecode_base = info->bytecode.data();
                        pc = bytecode_base;
                        V12_DISPATCH();
                    }
                    if (kind == HeapObjectKind::kExternal) {
                        // Host function.
                        HostFunction* hf = static_cast<HostFunction*>(callee.AsHeapObject());
                        acc = hf->fn()(this, iso_->undefined_value(),
                                       const_cast<Value*>(args), argc);
                        V12_DISPATCH();
                    }
                }
                // Not callable.
                {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: value is not a function"));
                    V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
                }
            }
            L_CallProperty: {
                uint16_t argc = ReadU16(&pc);
                uint8_t prop_idx = ReadU8(&pc);
                uint8_t first_arg = ReadU8(&pc);
                uint16_t ic_slot = ReadU16(&pc);

                Value receiver = acc;
                Value* args = regs + first_arg;

                // Look up the property on the receiver to find the callee.
                Value callee;
                std::string_view method_name = info->property_names[prop_idx];
                if (receiver.IsObject()) {
                    // CallProperty IC: cache (shape, slot) so that repeated
                    // obj.method() calls skip the linear Shape::Lookup.
                    JSObject* obj = receiver.AsObject();
                    Shape* obj_shape = obj->shape();
                    auto& ic = info->GetIC(ic_slot);
                    if (V12_LIKELY(ic.initialized &&
                                   ic.shape == reinterpret_cast<uintptr_t>(obj_shape))) {
                        // IC hit: read the method directly from the cached slot.
                        callee = obj->properties()[ic.slot];
                    } else {
                        // IC miss: full lookup, then update cache.
                        Shape::Slot slot = obj_shape->LookupInterned(
                            iso_->Intern(method_name));
                        if (slot != Shape::kInvalidSlot) {
                            ic.shape = reinterpret_cast<uintptr_t>(obj_shape);
                            ic.slot = slot;
                            ic.initialized = true;
                            callee = obj->properties()[slot];
                        } else {
                            callee = iso_->undefined_value();
                        }
                    }
                } else if (receiver.IsArray()) {
                    // Built-in array methods.
                    if (method_name == "push") {
                        for (uint16_t i = 0; i < argc; ++i) {
                            receiver.AsArray()->Push(iso_, args[i]);
                        }
                        acc = Value::FromSmi(static_cast<intptr_t>(receiver.AsArray()->length()));
                        V12_DISPATCH();
                    } else if (method_name == "pop") {
                        JSArray* arr = receiver.AsArray();
                        if (arr->length() > 0) {
                            acc = arr->GetElement(arr->length() - 1);
                        } else {
                            acc = iso_->undefined_value();
                        }
                        V12_DISPATCH();
                    } else if (method_name == "length") {
                        acc = Value::FromSmi(static_cast<intptr_t>(receiver.AsArray()->length()));
                        V12_DISPATCH();
                    }
                    callee = iso_->undefined_value();
                } else if (receiver.IsString()) {
                    // Built-in string methods.
                    if (method_name == "charAt") {
                        uint32_t idx = argc > 0 ? static_cast<uint32_t>(ToDouble(iso_, args[0])) : 0;
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        if (idx < s->length()) {
                            char ch = s->data()[idx];
                            acc = Value::FromHeap(JSString::New(iso_, std::string_view(&ch, 1)));
                        } else {
                            acc = Value::FromHeap(JSString::New(iso_, ""));
                        }
                        V12_DISPATCH();
                    } else if (method_name == "substring" || method_name == "substr" ||
                               method_name == "slice") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        uint32_t start = argc > 0 ? static_cast<uint32_t>(ToDouble(iso_, args[0])) : 0;
                        uint32_t end = argc > 1 ? static_cast<uint32_t>(ToDouble(iso_, args[1])) : s->length();
                        if (start > s->length()) start = s->length();
                        if (end > s->length()) end = s->length();
                        if (start > end) { uint32_t t = start; start = end; end = t; }
                        acc = Value::FromHeap(JSString::New(iso_,
                            std::string_view(s->data() + start, end - start)));
                        V12_DISPATCH();
                    } else if (method_name == "toUpperCase") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        std::string out(s->data(), s->length());
                        for (auto& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        acc = Value::FromHeap(JSString::New(iso_, out));
                        V12_DISPATCH();
                    } else if (method_name == "toLowerCase") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        std::string out(s->data(), s->length());
                        for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        acc = Value::FromHeap(JSString::New(iso_, out));
                        V12_DISPATCH();
                    } else if (method_name == "indexOf") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        if (argc > 0 && args[0].IsString()) {
                            std::string_view needle = static_cast<JSString*>(args[0].AsHeapObject())->view();
                            auto pos = s->view().find(needle);
                            acc = pos == std::string_view::npos
                                  ? Value::FromSmi(-1)
                                  : Value::FromSmi(static_cast<intptr_t>(pos));
                        } else {
                            acc = Value::FromSmi(-1);
                        }
                        V12_DISPATCH();
                    }
                    callee = iso_->undefined_value();
                } else {
                    callee = iso_->undefined_value();
                }

                if (!callee.IsFunction()) {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: method is not a function"));
                    V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
                }

                // Inline the JS callee path (same pattern as L_Call) to
                // avoid C++ recursion via CallFunction/ExecuteTop.
                if (callee.IsHostFunction()) {
                    // Host function: call directly, no frame push.
                    frame->pc = pc;
                    acc = callee.AsHostFunction()->fn()(
                        this, receiver, const_cast<Value*>(args), argc);
                    V12_DISPATCH();
                }
                // JS function: inline push frame and dispatch.
                {
                    JSFunction* fn = callee.AsFunction();
                    FunctionInfo* callee_info = fn->shared_info();
                    frame->pc = pc;
                    regs = PushFrame(callee_info, fn, receiver,
                                     fn->closure_context(), argc, args);
                    frame = &frames_[frame_top_ - 1];
                    ctx = frame->context;
                    info = callee_info;
                    bytecode_base = info->bytecode.data();
                    pc = bytecode_base;
                    V12_DISPATCH();
                }
            }
            // Call0/Call1/Call2 split into separate handlers to avoid
            // re-decoding the opcode (pc[-1]) and the cascading if/else.
            L_Call0: {
                pc += 2;  // skip feedback slot
                Value callee = acc;
                // Host function: call directly.
                if (callee.IsHostFunction()) {
                    acc = callee.AsHostFunction()->fn()(
                        this, iso_->undefined_value(), nullptr, 0);
                    V12_DISPATCH();
                }
                // JS function: inline call.
                if (V12_LIKELY(callee.IsFunction())) {
                    JSFunction* fn = callee.AsFunction();
                    FunctionInfo* callee_info = fn->shared_info();
                    if (callee_info == nullptr) {
                        Value exc = Value::FromHeap(JSString::New(iso_,
                            "TypeError: not a function"));
                        V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
                    }
                    frame->pc = pc;
                    regs = PushFrame(callee_info, fn, iso_->undefined_value(),
                                     fn->closure_context(), 0, nullptr);
                    frame = &frames_[frame_top_ - 1];
                    ctx = frame->context;
                    info = callee_info;
                    bytecode_base = info->bytecode.data();
                    pc = bytecode_base;
                    V12_DISPATCH();
                }
                Value exc = Value::FromHeap(JSString::New(iso_,
                    "TypeError: value is not a function"));
                V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
            }
            L_Call1: {
                uint8_t r = ReadU8(&pc);
                pc += 2;  // skip feedback slot
                Value callee = acc;
                Value argbuf[1] = {regs[r]};
                // Host function: call directly.
                if (callee.IsHostFunction()) {
                    acc = callee.AsHostFunction()->fn()(
                        this, iso_->undefined_value(), argbuf, 1);
                    V12_DISPATCH();
                }
                // JS function: inline call.
                if (V12_LIKELY(callee.IsFunction())) {
                    JSFunction* fn = callee.AsFunction();
                    FunctionInfo* callee_info = fn->shared_info();
                    if (callee_info == nullptr) {
                        Value exc = Value::FromHeap(JSString::New(iso_,
                            "TypeError: not a function"));
                        V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
                    }
                    frame->pc = pc;
                    regs = PushFrame(callee_info, fn, iso_->undefined_value(),
                                     fn->closure_context(), 1, argbuf);
                    frame = &frames_[frame_top_ - 1];
                    ctx = frame->context;
                    info = callee_info;
                    bytecode_base = info->bytecode.data();
                    pc = bytecode_base;
                    V12_DISPATCH();
                }
                Value exc = Value::FromHeap(JSString::New(iso_,
                    "TypeError: value is not a function"));
                V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
            }
            L_Call2: {
                uint8_t r1 = ReadU8(&pc);
                uint8_t r2 = ReadU8(&pc);
                pc += 2;  // skip feedback slot
                Value callee = acc;
                Value argbuf[2] = {regs[r1], regs[r2]};
                // Host function: call directly.
                if (callee.IsHostFunction()) {
                    acc = callee.AsHostFunction()->fn()(
                        this, iso_->undefined_value(), argbuf, 2);
                    V12_DISPATCH();
                }
                // JS function: inline call.
                if (V12_LIKELY(callee.IsFunction())) {
                    JSFunction* fn = callee.AsFunction();
                    FunctionInfo* callee_info = fn->shared_info();
                    if (callee_info == nullptr) {
                        Value exc = Value::FromHeap(JSString::New(iso_,
                            "TypeError: not a function"));
                        V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
                    }
                    frame->pc = pc;
                    regs = PushFrame(callee_info, fn, iso_->undefined_value(),
                                     fn->closure_context(), 2, argbuf);
                    frame = &frames_[frame_top_ - 1];
                    ctx = frame->context;
                    info = callee_info;
                    bytecode_base = info->bytecode.data();
                    pc = bytecode_base;
                    V12_DISPATCH();
                }
                Value exc = Value::FromHeap(JSString::New(iso_,
                    "TypeError: value is not a function"));
                V12_THROW(exc, static_cast<uint32_t>((pc - bytecode_base) - 1));
            }
            L_Construct: {
                // Cold handler — extracted to reduce ExecuteTop code size.
                DispatchState ds{frame, pc, acc, info, regs, ctx, bytecode_base};
                if (HandleConstruct(ds)) {
                    frame = ds.frame; pc = ds.pc; acc = ds.acc;
                    info = ds.info; regs = ds.regs; ctx = ds.ctx;
                    bytecode_base = ds.bytecode_base;
                    V12_DISPATCH();
                }
                return {InterpStatus::kThrew, ds.acc};
            }
            L_CallBuiltin: {
                DispatchState ds{frame, pc, acc, info, regs, ctx, bytecode_base};
                HandleCallBuiltin(ds);
                pc = ds.pc; acc = ds.acc;
                V12_DISPATCH();
            }

            // ----- Object / array creation -----
            L_NewObject:
                acc = Value::FromHeap(JSObject::New(iso_));
                V12_DISPATCH();
            L_NewArray: {
                uint16_t cap = ReadU16(&pc);
                acc = Value::FromHeap(JSArray::New(iso_, cap));
                V12_DISPATCH();
            }
            // DefineProperty: op  val_reg:8  name_idx:8
            L_DefineProperty: {
                uint8_t val_reg = ReadU8(&pc);
                uint8_t name_idx = ReadU8(&pc);
                std::string_view name = info->property_names[name_idx];
                Value value = regs[val_reg];
                if (acc.IsObject()) {
                    acc.AsObject()->SetProperty(iso_, name, value);
                }
                V12_DISPATCH();
            }
            // CreateClosure: op  const_idx:32
            L_CreateClosure: {
                uint32_t cidx = ReadU32(&pc);
                V12_CHECK(current_program_ != nullptr, "no current program");
                const Constant& c = info->constants[cidx];
                V12_DCHECK(c.kind == Constant::Kind::kFunctionInfo, "not a function constant");
                FunctionInfo* inner_info = current_program_->functions[c.index].get();
                // Lazy compilation: if the function hasn't been compiled yet,
                // compile it now (on first use).
                if (V12_UNLIKELY(!inner_info->is_compiled)) {
                    BytecodeGenerator gen(iso_, nullptr);
                    gen.CompileLazyFunction(inner_info, current_program_,
                                           current_program_->scope_analyzer.get());
                }
                acc = Value::FromHeap(JSFunction::New(iso_, inner_info, ctx));
                V12_DISPATCH();
            }
            // PushArray: op  arr_reg:8  idx:16
            L_PushArray: {
                uint8_t arr_reg = ReadU8(&pc);
                pc += 2;
                Value arr = regs[arr_reg];
                if (arr.IsArray()) {
                    arr.AsArray()->Push(iso_, acc);
                }
                V12_DISPATCH();
            }
            // LoadArrayLength: op  idx:16
            L_LoadArrayLength: {
                pc += 2;
                if (acc.IsArray()) {
                    acc = Value::FromSmi(static_cast<intptr_t>(acc.AsArray()->length()));
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            L_StoreArrayLength: {
                (void)ReadU8(&pc);
                pc += 2;
                // Setting .length is a no-op for now.
                V12_DISPATCH();
            }

            // ----- Context allocation -----
            // CreateContext: op  slot_count:16  idx:16
            L_CreateContext: {
                uint16_t slot_count = ReadU16(&pc);
                pc += 2;
                acc = Value::FromHeap(Context::New(iso_, ctx, slot_count));
                V12_DISPATCH();
            }
            // PushContext: op  idx:16
            L_PushContext: {
                pc += 2;
                if (acc.IsHeapObject()) {
                    ctx = static_cast<Context*>(acc.AsHeapObject());
                    frame->context = ctx;
                }
                V12_DISPATCH();
            }
            // PopContext: op  idx:16
            L_PopContext: {
                pc += 2;
                if (ctx != nullptr) {
                    ctx = ctx->parent();
                    frame->context = ctx;
                }
                V12_DISPATCH();
            }

            // ----- Iteration -----
            L_ObjectKeys: {
                DispatchState ds{frame, pc, acc, info, regs, ctx, bytecode_base};
                HandleObjectKeys(ds);
                pc = ds.pc; acc = ds.acc;
                V12_DISPATCH();
            }
            L_GetIterator: {
                pc += 2;
                // For arrays and strings, the receiver itself is the
                // "iterator" (for-of uses indexed access with a counter).
                // No transformation needed.
                V12_DISPATCH();
            }
            L_ForInPrepare: {
                (void)ReadU8(&pc);
                acc = iso_->undefined_value();
                V12_DISPATCH();
            }
            L_ForInNext: {
                (void)ReadU8(&pc);
                (void)ReadU8(&pc);
                acc = iso_->undefined_value();
                V12_DISPATCH();
            }
            L_ForInDone: {
                (void)ReadU8(&pc);
                acc = iso_->true_value();
                V12_DISPATCH();
            }

            // ----- Returns -----
            // If there's a caller frame, pop the current frame and resume
            // the caller (inline return — no C++ recursion). If this is
            // the toplevel frame, return from ExecuteTop.
            L_Return: {
                if (V12_LIKELY(frame_top_ > 1)) {
                    Value retval = acc;
                    PopFrame();
                    frame = &frames_[frame_top_ - 1];
                    regs = frame->regs;
                    ctx = frame->context;
                    info = frame->info;
                    bytecode_base = info->bytecode.data();
                    pc = frame->pc;
                    acc = retval;
                    V12_DISPATCH();
                }
                frame->pc = pc;
                return {InterpStatus::kReturned, acc};
            }
            L_ReturnUndefined: {
                if (V12_LIKELY(frame_top_ > 1)) {
                    PopFrame();
                    frame = &frames_[frame_top_ - 1];
                    regs = frame->regs;
                    ctx = frame->context;
                    info = frame->info;
                    bytecode_base = info->bytecode.data();
                    pc = frame->pc;
                    acc = iso_->undefined_value();
                    V12_DISPATCH();
                }
                frame->pc = pc;
                return {InterpStatus::kReturned, iso_->undefined_value()};
            }

            // ----- Exceptions -----
            L_Throw: {
                // Cold handler — walks the call stack looking for a catch.
                DispatchState ds{frame, pc, acc, info, regs, ctx, bytecode_base};
                if (HandleThrow(ds)) {
                    frame = ds.frame; pc = ds.pc; acc = ds.acc;
                    info = ds.info; regs = ds.regs; ctx = ds.ctx;
                    bytecode_base = ds.bytecode_base;
                    V12_DISPATCH();
                }
                return {InterpStatus::kThrew, ds.acc};
            }
            L_TryCatch: {
                // No-op: the handler table is static (in FunctionInfo).
                (void)ReadU32(&pc);
                V12_DISPATCH();
            }
            L_TryFinally: {
                // Not yet fully implemented.
                (void)ReadU32(&pc);
                (void)ReadU32(&pc);
                V12_DISPATCH();
            }
            L_Exception:
                acc = pending_exception_;
                V12_DISPATCH();

            // ----- Debugger -----
            L_Debugger:
                // No-op in non-debug builds.
                V12_DISPATCH();

            // ----- Misc -----
            L_Pop:
                acc = iso_->undefined_value();
                V12_DISPATCH();
            L_Dup:
                // No-op (acc is already acc).
                V12_DISPATCH();
            L_Nop:
                V12_DISPATCH();
            L_Illegal:
                V12_CHECK(false, "Illegal bytecode executed");
                V12_DISPATCH();
    // Unreachable — every opcode has a label above.
    return {InterpStatus::kReturned, iso_->undefined_value()};
}

}  // namespace v12
