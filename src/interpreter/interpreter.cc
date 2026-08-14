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
    reg_storage_.reserve(1024);   // pre-allocate to avoid early reallocs
    frames_.reserve(64);
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
// -----------------------------------------------------------------------------
InterpResult Interp::ExecuteTop() {
    Frame* frame = &frames_.back();
    // Local copies for speed. We re-sync `frame->regs` etc. after any
    // operation that may invalidate `frame` (i.e. recursive calls that
    // grow frames_).
    const uint8_t* pc = frame->pc;
    Value acc = iso_->undefined_value();
    FunctionInfo* info = frame->info;
    Value* regs = frame->regs;
    Context* ctx = frame->context;

    // Lambda to re-sync locals after a recursive call.
    // `pc` is preserved by the caller via `frame->pc = pc;` before the call.
    auto sync = [&]() {
        frame = &frames_.back();
        regs = frame->regs;
        ctx = frame->context;
        info = frame->info;
    };

    while (true) {
        Op op = static_cast<Op>(*pc);
        ++pc;
        switch (op) {
            // ----- Loading constants -----
            case Op::LdaConst: {
                uint32_t idx = ReadU32(&pc);
                acc = info->ResolveConstant(iso_, idx);
                break;
            }
            case Op::LdaSmi: {
                uint8_t v = ReadU8(&pc);
                acc = Value::FromSmi(static_cast<intptr_t>(v));
                break;
            }
            case Op::LdaZero:
                acc = Value::FromSmi(0);
                break;
            case Op::LdaUndefined:
                acc = iso_->undefined_value();
                break;
            case Op::LdaNull:
                acc = iso_->null_value();
                break;
            case Op::LdaTrue:
                acc = iso_->true_value();
                break;
            case Op::LdaFalse:
                acc = iso_->false_value();
                break;
            case Op::LdaThis:
                acc = frame->this_val;
                break;

            // ----- Register moves -----
            case Op::Ldar: {
                uint8_t r = ReadU8(&pc);
                acc = regs[r];
                break;
            }
            case Op::Star: {
                uint8_t r = ReadU8(&pc);
                regs[r] = acc;
                break;
            }
            case Op::Mov: {
                uint8_t dst = ReadU8(&pc);
                uint8_t src = ReadU8(&pc);
                regs[dst] = regs[src];
                break;
            }

            // ----- Binary arithmetic -----
            // Format: op  r:8  idx:16   ->   acc = acc <op> regs[r]
            case Op::Add: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);  // feedback slot, unused for now
                acc = Add(iso_, acc, regs[r]);
                break;
            }
            case Op::Sub: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Sub(iso_, acc, regs[r]);
                break;
            }
            case Op::Mul: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Mul(iso_, acc, regs[r]);
                break;
            }
            case Op::Div: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Div(iso_, acc, regs[r]);
                break;
            }
            case Op::Mod: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Mod(iso_, acc, regs[r]);
                break;
            }
            case Op::Exp: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Exp(iso_, acc, regs[r]);
                break;
            }
            case Op::BitOr: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = BitOr(iso_, acc, regs[r]);
                break;
            }
            case Op::BitAnd: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = BitAnd(iso_, acc, regs[r]);
                break;
            }
            case Op::BitXor: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = BitXor(iso_, acc, regs[r]);
                break;
            }
            case Op::Shl: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Shl(iso_, acc, regs[r]);
                break;
            }
            case Op::Shr: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Shr(iso_, acc, regs[r]);
                break;
            }
            case Op::Ushr: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = Ushr(iso_, acc, regs[r]);
                break;
            }

            // ----- Binary with constant -----
            // Format: op  const:32  idx:16
            case Op::AddConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Add(iso_, acc, info->ResolveConstant(iso_, cidx));
                break;
            }
            case Op::SubConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Sub(iso_, acc, info->ResolveConstant(iso_, cidx));
                break;
            }
            case Op::MulConst: {
                uint32_t cidx = ReadU32(&pc);
                (void)ReadU16(&pc);
                acc = Mul(iso_, acc, info->ResolveConstant(iso_, cidx));
                break;
            }

            // ----- Unary -----
            // Format: op  idx:16   (idx is a feedback slot)
            case Op::Negate: {
                (void)ReadU16(&pc);
                acc = Negate(iso_, acc);
                break;
            }
            case Op::BitNot: {
                (void)ReadU16(&pc);
                acc = BitNot(iso_, acc);
                break;
            }
            case Op::LogicalNot:
                acc = IsTruthy(iso_, acc) ? iso_->false_value() : iso_->true_value();
                break;
            case Op::Typeof:
                acc = Typeof(iso_, acc);
                break;

            // ----- Comparison -----
            // Format: op  r:8  idx:16
            case Op::TestEqual: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = LooseEquals(iso_, acc, regs[r]);
                break;
            }
            case Op::TestNotEqual: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = LooseEquals(iso_, acc, regs[r]).AsHeapObject() == iso_->true_object()
                      ? iso_->false_value() : iso_->true_value();
                break;
            }
            case Op::TestEqStrict: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = StrictEquals(iso_, acc, regs[r]);
                break;
            }
            case Op::TestNotEqStrict: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = StrictEquals(iso_, acc, regs[r]).AsHeapObject() == iso_->true_object()
                      ? iso_->false_value() : iso_->true_value();
                break;
            }
            case Op::TestLessThan: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = LessThan(iso_, acc, regs[r]);
                break;
            }
            case Op::TestGreaterThan: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = GreaterThan(iso_, acc, regs[r]);
                break;
            }
            case Op::TestLessThanOrEqual: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = LessThanOrEqual(iso_, acc, regs[r]);
                break;
            }
            case Op::TestGreaterThanOrEqual: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                acc = GreaterThanOrEqual(iso_, acc, regs[r]);
                break;
            }
            case Op::TestInstanceOf: {
                uint8_t r = ReadU8(&pc);
                (void)ReadU16(&pc);
                // instanceof not yet implemented; default to false.
                acc = iso_->false_value();
                break;
            }
            case Op::TestIn: {
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
                break;
            }

            // ----- Inc / Dec -----
            case Op::Inc: {
                (void)ReadU16(&pc);
                acc = Inc(iso_, acc);
                break;
            }
            case Op::Dec: {
                (void)ReadU16(&pc);
                acc = Dec(iso_, acc);
                break;
            }

            // ----- Control flow -----
            // Format: op  target:32
            case Op::Jump: {
                uint32_t target = ReadU32(&pc);
                pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfTrue: {
                uint32_t target = ReadU32(&pc);
                if (IsTruthy(iso_, acc)) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfFalse: {
                uint32_t target = ReadU32(&pc);
                if (!IsTruthy(iso_, acc)) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfNull: {
                uint32_t target = ReadU32(&pc);
                if (acc.IsNull()) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfUndefined: {
                uint32_t target = ReadU32(&pc);
                if (acc.IsUndefined()) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfNotNullOrUndefined: {
                uint32_t target = ReadU32(&pc);
                if (!acc.IsNullOrUndefined()) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfToBooleanTrue: {
                uint32_t target = ReadU32(&pc);
                if (IsTruthy(iso_, acc)) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpIfToBooleanFalse: {
                uint32_t target = ReadU32(&pc);
                if (!IsTruthy(iso_, acc)) pc = info->bytecode.data() + target;
                break;
            }
            case Op::JumpLoop: {
                uint32_t target = ReadU32(&pc);
                pc = info->bytecode.data() + target;
                break;
            }

            // ----- Property access -----
            // LoadProperty: op  name_idx:8  idx:16
            case Op::LoadProperty: {
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
                break;
            }
            case Op::LoadIndexed: {
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
                break;
            }
            // StoreProperty: op  val_reg:8  name_idx:8  idx:16
            case Op::StoreProperty: {
                uint8_t val_reg = ReadU8(&pc);
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                Value value = regs[val_reg];
                if (acc.IsObject()) {
                    acc.AsObject()->SetProperty(iso_, name, value);
                }
                break;
            }
            case Op::StoreIndexed: {
                uint8_t idx_reg = ReadU8(&pc);
                uint8_t val_reg = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value idx_val = regs[idx_reg];
                Value value = regs[val_reg];
                if (acc.IsArray()) {
                    uint32_t idx = static_cast<uint32_t>(ToDouble(iso_, idx_val));
                    acc.AsArray()->SetElement(iso_, idx, value);
                }
                break;
            }

            // ----- Variables -----
            // LoadGlobal: op  name_idx:8  idx:16
            case Op::LoadGlobal: {
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                acc = iso_->GetGlobal(name);
                break;
            }
            // StoreGlobal: op  r:8  name_idx:8  idx:16
            case Op::StoreGlobal: {
                (void)ReadU8(&pc);   // dummy register
                uint8_t name_idx = ReadU8(&pc);
                (void)ReadU16(&pc);
                std::string_view name = info->property_names[name_idx];
                iso_->SetGlobal(name, acc);
                break;
            }
            // LoadContext: op  depth:16  index:16  idx:16
            case Op::LoadContext: {
                uint16_t depth = ReadU16(&pc);
                uint16_t index = ReadU16(&pc);
                (void)ReadU16(&pc);
                if (ctx != nullptr) {
                    acc = ctx->LoadAt(depth, index);
                } else {
                    acc = iso_->undefined_value();
                }
                break;
            }
            // StoreContext: op  r:8  depth:16  index:16  idx:16
            case Op::StoreContext: {
                (void)ReadU8(&pc);
                uint16_t depth = ReadU16(&pc);
                uint16_t index = ReadU16(&pc);
                (void)ReadU16(&pc);
                if (ctx != nullptr) {
                    ctx->StoreAt(depth, index, acc);
                }
                break;
            }

            // ----- Calls -----
            // Call: op  argc:16  first_arg:8  idx:16
            //   acc holds the callee.
            // CallProperty: op  argc:16  prop_idx:8  idx:16
            //   acc holds the receiver; the property name is prop_idx.
            //   The interpreter looks up the property on acc to get the
            //   callee, then calls it with this = acc.
            case Op::Call: {
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
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                break;
            }
            case Op::CallProperty: {
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
                        break;
                    } else if (method_name == "pop") {
                        JSArray* arr = receiver.AsArray();
                        if (arr->length() > 0) {
                            acc = arr->GetElement(arr->length() - 1);
                        } else {
                            acc = iso_->undefined_value();
                        }
                        break;
                    } else if (method_name == "length") {
                        acc = Value::FromSmi(static_cast<intptr_t>(receiver.AsArray()->length()));
                        break;
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
                        break;
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
                        break;
                    } else if (method_name == "toUpperCase") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        std::string out(s->data(), s->length());
                        for (auto& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        acc = Value::FromHeap(JSString::New(iso_, out));
                        break;
                    } else if (method_name == "toLowerCase") {
                        JSString* s = static_cast<JSString*>(receiver.AsHeapObject());
                        std::string out(s->data(), s->length());
                        for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        acc = Value::FromHeap(JSString::New(iso_, out));
                        break;
                    }
                    callee = iso_->undefined_value();
                } else {
                    callee = iso_->undefined_value();
                }

                if (!callee.IsFunction()) {
                    Value exc = Value::FromHeap(JSString::New(iso_,
                        "TypeError: method is not a function"));
                    pending_exception_ = exc;
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
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                break;
            }
            case Op::Call0:
            case Op::Call1:
            case Op::Call2: {
                Value callee = acc;
                Value this_val = iso_->undefined_value();
                Value argbuf[2];
                uint32_t argc = 0;
                if (op == Op::Call1) {
                    uint8_t r = ReadU8(&pc);
                    (void)ReadU16(&pc);
                    argbuf[0] = regs[r];
                    argc = 1;
                } else if (op == Op::Call2) {
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
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    return r;
                }
                sync();
                pc = frame->pc;
                acc = r.value;
                break;
            }
            case Op::Construct: {
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
                    return {InterpStatus::kThrew, exc};
                }
                if (r.status == InterpStatus::kThrew) {
                    pending_exception_ = r.value;
                    return r;
                }
                sync();
                pc = frame->pc;
                // If the constructor returned an object, use that; otherwise
                // use the new object.
                if (r.value.IsObject()) {
                    acc = r.value;
                } else {
                    acc = new_obj;
                }
                break;
            }
            case Op::CallBuiltin: {
                (void)ReadU8(&pc);
                (void)ReadU16(&pc);
                (void)ReadU8(&pc);
                acc = iso_->undefined_value();
                break;
            }

            // ----- Object / array creation -----
            case Op::NewObject:
                acc = Value::FromHeap(JSObject::New(iso_));
                break;
            case Op::NewArray: {
                uint16_t cap = ReadU16(&pc);
                acc = Value::FromHeap(JSArray::New(iso_, cap));
                break;
            }
            // DefineProperty: op  val_reg:8  name_idx:8
            case Op::DefineProperty: {
                uint8_t val_reg = ReadU8(&pc);
                uint8_t name_idx = ReadU8(&pc);
                std::string_view name = info->property_names[name_idx];
                Value value = regs[val_reg];
                if (acc.IsObject()) {
                    acc.AsObject()->SetProperty(iso_, name, value);
                }
                break;
            }
            // CreateClosure: op  const_idx:32
            case Op::CreateClosure: {
                uint32_t cidx = ReadU32(&pc);
                V12_CHECK(current_program_ != nullptr, "no current program");
                const Constant& c = info->constants[cidx];
                V12_DCHECK(c.kind == Constant::Kind::kFunctionInfo, "not a function constant");
                FunctionInfo* inner_info = current_program_->functions[c.index].get();
                acc = Value::FromHeap(JSFunction::New(iso_, inner_info, ctx));
                break;
            }
            // PushArray: op  arr_reg:8  idx:16
            case Op::PushArray: {
                uint8_t arr_reg = ReadU8(&pc);
                (void)ReadU16(&pc);
                Value arr = regs[arr_reg];
                if (arr.IsArray()) {
                    arr.AsArray()->Push(iso_, acc);
                }
                break;
            }
            // LoadArrayLength: op  idx:16
            case Op::LoadArrayLength: {
                (void)ReadU16(&pc);
                if (acc.IsArray()) {
                    acc = Value::FromSmi(static_cast<intptr_t>(acc.AsArray()->length()));
                } else {
                    acc = iso_->undefined_value();
                }
                break;
            }
            case Op::StoreArrayLength: {
                (void)ReadU8(&pc);
                (void)ReadU16(&pc);
                // Setting .length is a no-op for now.
                break;
            }

            // ----- Context allocation -----
            // CreateContext: op  slot_count:16  idx:16
            case Op::CreateContext: {
                uint16_t slot_count = ReadU16(&pc);
                (void)ReadU16(&pc);
                acc = Value::FromHeap(Context::New(iso_, ctx, slot_count));
                break;
            }
            // PushContext: op  idx:16
            case Op::PushContext: {
                (void)ReadU16(&pc);
                if (acc.IsHeapObject()) {
                    ctx = static_cast<Context*>(acc.AsHeapObject());
                    frame->context = ctx;
                }
                break;
            }
            // PopContext: op  idx:16
            case Op::PopContext: {
                (void)ReadU16(&pc);
                if (ctx != nullptr) {
                    ctx = ctx->parent();
                    frame->context = ctx;
                }
                break;
            }

            // ----- Iteration -----
            case Op::ForInPrepare: {
                (void)ReadU8(&pc);
                // Not yet implemented.
                acc = iso_->undefined_value();
                break;
            }
            case Op::ForInNext: {
                (void)ReadU8(&pc);
                (void)ReadU8(&pc);
                acc = iso_->undefined_value();
                break;
            }
            case Op::ForInDone: {
                (void)ReadU8(&pc);
                acc = iso_->true_value();
                break;
            }

            // ----- Returns -----
            case Op::Return:
                frame->pc = pc;
                return {InterpStatus::kReturned, acc};
            case Op::ReturnUndefined:
                frame->pc = pc;
                return {InterpStatus::kReturned, iso_->undefined_value()};

            // ----- Exceptions -----
            case Op::Throw: {
                pending_exception_ = acc;
                frame->pc = pc;
                return {InterpStatus::kThrew, acc};
            }
            case Op::TryCatch: {
                (void)ReadU32(&pc);
                // Not yet implemented.
                break;
            }
            case Op::TryFinally: {
                (void)ReadU32(&pc);
                (void)ReadU32(&pc);
                break;
            }
            case Op::Exception:
                acc = pending_exception_;
                break;

            // ----- Debugger -----
            case Op::Debugger:
                // No-op in non-debug builds.
                break;

            // ----- Misc -----
            case Op::Pop:
                acc = iso_->undefined_value();
                break;
            case Op::Dup:
                // No-op (acc is already acc).
                break;
            case Op::Nop:
                break;
            case Op::Illegal:
                V12_CHECK(false, "Illegal bytecode executed");
                break;

            default:
                V12_CHECK(false, "unknown opcode %d", static_cast<int>(op));
                break;
        }
    }
}

}  // namespace v12
