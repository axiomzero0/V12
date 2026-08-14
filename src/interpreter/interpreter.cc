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

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "frontend/bytecode/bytecode-generator.h"
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
    // Pre-allocate enough capacity for the frame stack and register storage
    // so that recursive calls don't trigger vector reallocation (which would
    // invalidate the `frame` pointer and `regs` in ExecuteTop). The max_depth_
    // limit is 1000, so we reserve 1024 frames. Each frame's register file
    // averages ~16 slots, so we reserve 16384 Value slots.
    reg_storage_.reserve(16384);
    frames_.reserve(1024);
}

Interp::~Interp() = default;

// -----------------------------------------------------------------------------
// Frame management
// -----------------------------------------------------------------------------
Value* Interp::PushFrame(FunctionInfo* info, JSFunction* fn, Value this_val,
                          Context* closure_ctx, uint32_t argc, const Value* args) {
    V12_CHECK(frames_.size() < max_depth_, "maximum call stack depth exceeded (%u)",
              max_depth_);

    // Allocate the register file for this frame at the end of reg_storage_.
    uint16_t nregs = info->num_registers;
    if (nregs == 0) nregs = 1;   // ensure at least one slot for safety
    size_t base = reg_storage_.size();
    reg_storage_.resize(base + nregs, iso_->undefined_value());

    Frame f;
    f.info = info;
    f.regs = reg_storage_.data() + base;
    f.pc = info->bytecode.data();
    f.context = closure_ctx;
    f.this_val = this_val;
    f.function = fn;
    frames_.push_back(f);

    // Place arguments into the first `argc` slots.
    // Missing slots (argc < num_parameters) stay undefined (set by resize).
    // Extra slots (argc > num_parameters) are dropped.
    uint16_t nparams = info->num_parameters;
    uint16_t to_copy = (argc < nparams) ? static_cast<uint16_t>(argc) : nparams;
    for (uint16_t i = 0; i < to_copy; ++i) {
        f.regs[i] = args[i];
    }
    return f.regs;
}

void Interp::PopFrame() {
    Frame& f = frames_.back();
    // Shrink the register storage by the frame's register count.
    uint16_t nregs = f.info->num_registers;
    if (nregs == 0) nregs = 1;
    reg_storage_.resize(reg_storage_.size() - nregs);
    frames_.pop_back();
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
#define V12_DISPATCH()  goto *dispatch_table[*pc++]

InterpResult Interp::ExecuteTop() {
    Frame* frame = &frames_.back();
    const uint8_t* pc = frame->pc;
    Value acc = iso_->undefined_value();
    FunctionInfo* info = frame->info;
    Value* regs = frame->regs;
    Context* ctx = frame->context;
    // Cache the bytecode base pointer — used by every Jump handler and by
    // Call for handler-table lookup. Avoids repeated `info->bytecode.data()`
    // calls (which are vector data accesses).
    const uint8_t* bytecode_base = info->bytecode.data();
    auto sync = [&]() {
        frame = &frames_.back();
        regs = frame->regs;
        ctx = frame->context;
        info = frame->info;
        bytecode_base = info->bytecode.data();
    };

    // Computed-goto dispatch table. Each entry is the address of the label
    // for that opcode. Indexed by the opcode byte.
    static const void* dispatch_table[] = {
        &&L_LdaConst, &&L_LdaSmi, &&L_LdaZero, &&L_LdaUndefined,
        &&L_LdaNull, &&L_LdaTrue, &&L_LdaFalse, &&L_LdaThis,
        &&L_Ldar, &&L_Star, &&L_Mov,
        &&L_Add, &&L_Sub, &&L_Mul, &&L_Div, &&L_Mod, &&L_Exp,
        &&L_BitOr, &&L_BitAnd, &&L_BitXor, &&L_Shl, &&L_Shr, &&L_Ushr,
        &&L_AddConst, &&L_SubConst, &&L_MulConst,
        &&L_Negate, &&L_BitNot, &&L_LogicalNot, &&L_Typeof,
        &&L_TestEqual, &&L_TestNotEqual, &&L_TestEqStrict, &&L_TestNotEqStrict,
        &&L_TestLessThan, &&L_TestGreaterThan, &&L_TestLessThanOrEqual,
        &&L_TestGreaterThanOrEqual, &&L_TestInstanceOf, &&L_TestIn,
        &&L_Inc, &&L_Dec,
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
            L_Add: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value result;
                if (V12_LIKELY(TrySmiAdd(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = Add(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Sub: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value result;
                if (V12_LIKELY(TrySmiSub(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = Sub(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Mul: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value result;
                if (V12_LIKELY(TrySmiMul(acc, regs[r], &result))) {
                    acc = result;
                } else {
                    acc = Mul(iso_, acc, regs[r]);
                }
                V12_DISPATCH();
            }
            L_Div: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                // No Smi fast path for Div (result may be non-integer).
                acc = Div(iso_, acc, regs[r]);
                V12_DISPATCH();
            }
            L_Mod: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Mod(iso_, acc, regs[r]);
                V12_DISPATCH();
            }
            L_Exp: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Exp(iso_, acc, regs[r]);
                V12_DISPATCH();
            }
            L_BitOr: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
                acc = Ushr(iso_, acc, regs[r]);
                V12_DISPATCH();
            }

            // ----- Binary with constant -----
            // Format: op  const:32  idx:16
            L_AddConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Add(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }
            L_SubConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Sub(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }
            L_MulConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Mul(iso_, acc, info->ResolveConstant(iso_, cidx));
                V12_DISPATCH();
            }

            // ----- Unary -----
            // Format: op  idx:16   (idx is a feedback slot)
            L_Negate: {
                (void)ReadU16(&pc);
                acc = Negate(iso_, acc);
                V12_DISPATCH();
            }
            L_BitNot: {
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->false_value() : iso_->true_value();
                } else {
                    acc = LooseEquals(iso_, acc, regs[r]).AsHeapObject() == iso_->true_object()
                          ? iso_->false_value() : iso_->true_value();
                }
                V12_DISPATCH();
            }
            L_TestEqStrict: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
                bool result;
                if (V12_LIKELY(TrySmiStrictEquals(acc, regs[r], &result))) {
                    acc = result ? iso_->false_value() : iso_->true_value();
                } else {
                    acc = StrictEquals(iso_, acc, regs[r]).AsHeapObject() == iso_->true_object()
                          ? iso_->false_value() : iso_->true_value();
                }
                V12_DISPATCH();
            }
            L_TestLessThan: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
                // instanceof not yet implemented; default to false.
                acc = iso_->false_value();
                V12_DISPATCH();
            }
            L_TestIn: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                pc = bytecode_base + target;
                V12_DISPATCH();
            }

            // ----- Property access -----
            // LoadProperty: op  name_idx:8  idx:16
            L_LoadProperty: {
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                if (acc.IsObject()) {
                    acc = acc.AsObject()->GetProperty(iso_, name);
                } else if (acc.IsString()) {
                    if (name == "length") {
                        uint32_t len = static_cast<JSString*>(acc.AsHeapObject())->length();
                        acc = Value::FromSmi(static_cast<intptr_t>(len));
                    } else {
                        acc = iso_->undefined_value();
                    }
                } else if (acc.IsArray()) {
                    if (name == "length") {
                        uint32_t len = acc.AsArray()->length();
                        acc = Value::FromSmi(static_cast<intptr_t>(len));
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
                (void)ReadU16(&pc);
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
            L_StoreProperty: {
                uint8_t val_reg = ReadU8(&pc);
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                Value value = regs[val_reg];
                if (acc.IsObject()) {
                    acc.AsObject()->SetProperty(iso_, name, value);
                }
                V12_DISPATCH();
            }
            L_StoreIndexed: {
                uint8_t idx_reg = ReadU8(&pc);
                uint8_t val_reg = ReadU8(&pc);
                (void)ReadU16(&pc);
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
            L_LoadGlobal: {
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                acc = iso_->GetGlobal(name);
                V12_DISPATCH();
            }
            // StoreGlobal: op  r:8  name_idx:8  idx:16
            L_StoreGlobal: {
                (void)ReadU8(&pc);   // dummy register
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                iso_->SetGlobal(name, acc);
                V12_DISPATCH();
            }
            // LoadContext: op  depth:16  index:16  idx:16
            L_LoadContext: {
                uint16_t depth = ReadU16(&pc);
                uint16_t index = ReadU16(&pc);
                (void)ReadU16(&pc);
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
                (void)ReadU16(&pc);
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
                // Save the Call instruction's offset for handler lookup.
                uint32_t call_off = static_cast<uint32_t>(
                    (pc - bytecode_base) - 1);
                uint16_t argc = ReadU16(&pc);
                uint8_t first_arg = ReadU8(&pc);
                (void)ReadU16(&pc);

                Value this_val = iso_->undefined_value();
                Value callee = acc;
                Value* args = regs + first_arg;

                frame->pc = pc;
                InterpResult r;
                if (callee.IsHostFunction()) {
                    r = CallHostFunction(callee.AsHostFunction(), this_val,
                                         args, argc);
                } else if (callee.IsFunction()) {
                    r = CallFunction(callee.AsFunction(), this_val,
                                     args, argc);
                } else {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: value is not a function"));
                    pending_exception_ = exc;
                    uint32_t catch_off = info->FindHandler(call_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = exc;
                        V12_DISPATCH();
                    }
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    uint32_t catch_off = info->FindHandler(call_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = r.value;
                        V12_DISPATCH();
                    }
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                V12_DISPATCH();
            }
            L_CallProperty: {
                uint32_t call_off = static_cast<uint32_t>(
                    (pc - bytecode_base) - 1);
                uint16_t argc = ReadU16(&pc);
                uint8_t prop_idx = ReadU8(&pc);
                uint8_t first_arg = ReadU8(&pc);
                (void)ReadU16(&pc);

                Value receiver = acc;
                Value* args = regs + first_arg;

                // Look up the property on the receiver to find the callee.
                Value callee;
                std::string_view method_name = info->property_names[prop_idx];
                if (receiver.IsObject()) {
                    callee = receiver.AsObject()->GetProperty(iso_, method_name);
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
                    pending_exception_ = exc;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = exc;
                        V12_DISPATCH();
                    }
                    return {InterpStatus::kThrew, exc};
                }

                frame->pc = pc;
                InterpResult r;
                if (callee.IsHostFunction()) {
                    r = CallHostFunction(callee.AsHostFunction(), receiver, args, argc);
                } else {
                    r = CallFunction(callee.AsFunction(), receiver, args, argc);
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = r.value;
                        V12_DISPATCH();
                    }
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                V12_DISPATCH();
            }
            L_Call0:
            L_Call1:
            L_Call2: {
                uint32_t call_off = static_cast<uint32_t>(
                    (pc - bytecode_base) - 1);
                // Determine which variant (Call0/1/2) by looking at the
                // opcode byte we just consumed (pc[-1]).
                uint8_t opcode_byte = pc[-1];
                Value callee = acc;
                Value this_val = iso_->undefined_value();
                Value argbuf[2];
                uint32_t argc = 0;
                if (opcode_byte == static_cast<uint8_t>(Op::Call1)) {
                    uint8_t r = ReadU8(&pc);
                    (void)ReadU16(&pc);
                    argbuf[0] = regs[r];
                    argc = 1;
                } else if (opcode_byte == static_cast<uint8_t>(Op::Call2)) {
                    uint8_t r1 = ReadU8(&pc);
                    uint8_t r2 = ReadU8(&pc);
                    (void)ReadU16(&pc);
                    argbuf[0] = regs[r1];
                    argbuf[1] = regs[r2];
                    argc = 2;
                } else {
                    (void)ReadU16(&pc);
                }
                frame->pc = pc;
                InterpResult r;
                if (callee.IsHostFunction()) {
                    r = CallHostFunction(callee.AsHostFunction(), this_val,
                                         argbuf, argc);
                } else if (callee.IsFunction()) {
                    r = CallFunction(callee.AsFunction(), this_val,
                                     argbuf, argc);
                } else {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: value is not a function"));
                    pending_exception_ = exc;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = exc;
                        V12_DISPATCH();
                    }
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = r.value;
                        V12_DISPATCH();
                    }
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                V12_DISPATCH();
            }
            L_Construct: {
                uint32_t call_off = static_cast<uint32_t>(
                    (pc - bytecode_base) - 1);
                uint16_t argc = ReadU16(&pc);
                uint8_t first_arg = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value callee = acc;
                Value* args = regs + first_arg;
                Value new_obj = Value::FromHeap(JSObject::New(iso_));
                frame->pc = pc;
                InterpResult r;
                if (callee.IsFunction()) {
                    r = CallFunction(callee.AsFunction(), new_obj, args, argc);
                } else if (callee.IsHostFunction()) {
                    r = CallHostFunction(callee.AsHostFunction(), new_obj, args, argc);
                } else {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: value is not a constructor"));
                    pending_exception_ = exc;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = exc;
                        V12_DISPATCH();
                    }
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    uint32_t pc_off = call_off;
                    uint32_t catch_off = info->FindHandler(pc_off);
                    if (catch_off != 0xFFFFFFFF) {
                        sync();
                        pc = bytecode_base + catch_off;
                        acc = r.value;
                        V12_DISPATCH();
                    }
                    return r;
                }
                sync();
                pc = frame->pc;
                if (r.value.IsObject()) {
                    acc = r.value;
                } else {
                    acc = new_obj;
                }
                V12_DISPATCH();
            }
            L_CallBuiltin: {
                (void)ReadU8(&pc);
                (void)ReadU16(&pc);
                (void)ReadU8(&pc);
                acc = iso_->undefined_value();
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
                acc = Value::FromHeap(JSFunction::New(iso_, inner_info, ctx));
                V12_DISPATCH();
            }
            // PushArray: op  arr_reg:8  idx:16
            L_PushArray: {
                uint8_t arr_reg = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value arr = regs[arr_reg];
                if (arr.IsArray()) {
                    arr.AsArray()->Push(iso_, acc);
                }
                V12_DISPATCH();
            }
            // LoadArrayLength: op  idx:16
            L_LoadArrayLength: {
                (void)ReadU16(&pc);
                if (acc.IsArray()) {
                    acc = Value::FromSmi(static_cast<intptr_t>(acc.AsArray()->length()));
                } else {
                    acc = iso_->undefined_value();
                }
                V12_DISPATCH();
            }
            L_StoreArrayLength: {
                (void)ReadU8(&pc);
                (void)ReadU16(&pc);
                // Setting .length is a no-op for now.
                V12_DISPATCH();
            }

            // ----- Context allocation -----
            // CreateContext: op  slot_count:16  idx:16
            L_CreateContext: {
                uint16_t slot_count = ReadU16(&pc);
                (void)ReadU16(&pc);
                acc = Value::FromHeap(Context::New(iso_, ctx, slot_count));
                V12_DISPATCH();
            }
            // PushContext: op  idx:16
            L_PushContext: {
                (void)ReadU16(&pc);
                if (acc.IsHeapObject()) {
                    ctx = static_cast<Context*>(acc.AsHeapObject());
                    frame->context = ctx;
                }
                V12_DISPATCH();
            }
            // PopContext: op  idx:16
            L_PopContext: {
                (void)ReadU16(&pc);
                if (ctx != nullptr) {
                    ctx = ctx->parent();
                    frame->context = ctx;
                }
                V12_DISPATCH();
            }

            // ----- Iteration -----
            L_ObjectKeys: {
                (void)ReadU16(&pc);
                // Create an array of property name strings from acc.
                JSArray* arr = JSArray::New(iso_, 4);
                if (acc.IsObject()) {
                    JSObject* obj = acc.AsObject();
                    Shape* shape = obj->shape();
                    for (uint16_t i = 0; i < shape->property_count(); ++i) {
                        std::string_view name = shape->PropertyNameAt(i);
                        Value key = Value::FromHeap(JSString::New(iso_, name));
                        arr->Push(iso_, key);
                    }
                } else if (acc.IsArray()) {
                    // Arrays have numeric indices + "length".
                    JSArray* a = acc.AsArray();
                    for (uint32_t i = 0; i < a->length(); ++i) {
                        Value key = Value::FromHeap(JSString::NewFromSmi(iso_, static_cast<intptr_t>(i)));
                        arr->Push(iso_, key);
                    }
                }
                acc = Value::FromHeap(arr);
                V12_DISPATCH();
            }
            L_GetIterator: {
                (void)ReadU16(&pc);
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
            L_Return:
                frame->pc = pc;
                return {InterpStatus::kReturned, acc};
            L_ReturnUndefined:
                frame->pc = pc;
                return {InterpStatus::kReturned, iso_->undefined_value()};

            // ----- Exceptions -----
            L_Throw: {
                pending_exception_ = acc;
                frame->pc = pc;
                // Search for a handler in the current function. Use the
                // offset of the Throw instruction itself (pc has already
                // advanced past the opcode byte).
                uint32_t throw_off = static_cast<uint32_t>(
                    (pc - bytecode_base) - 1);
                uint32_t catch_off = info->FindHandler(throw_off);
                if (catch_off != 0xFFFFFFFF) {
                    pc = bytecode_base + catch_off;
                    acc = pending_exception_;
                    V12_DISPATCH();
                }
                return {InterpStatus::kThrew, acc};
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
