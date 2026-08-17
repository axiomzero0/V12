// =============================================================================
// src/interpreter/interpreter.h
// =============================================================================
// Register-based bytecode interpreter (tier 0).
//
// This is the entry point for executing JavaScript in V12. It runs
// bytecode produced by the BytecodeGenerator. The interpreter is the
// baseline tier — all functions start here, and hot ones are later
// promoted to the JIT (when the JIT is implemented).
//
// Frame layout:
//   Each call pushes a Frame on the call stack. A Frame contains:
//     - The FunctionInfo being executed.
//     - A register file: an array of Value with `num_registers` slots.
//     - A program counter (PC): pointer into the bytecode.
//     - The current Context (for closure variable access).
//     - The `this` value.
//     - The callee function (for stack traces).
//
// The accumulator is a virtual register: it lives in slot kAccumulatorReg
// of the register file, but we keep a hot reference to it via the `acc`
// local in the dispatch loop. (This means Star/Ldar are just register-
// file writes/reads, and arithmetic ops read acc and a register operand.)
//
// Dispatch:
//   We use a switch-based dispatch loop. Computed-goto dispatch can be
//   added later as a performance optimization; it's a one-line change
//   in the loop body.
//
// Calling convention:
//   The caller places arguments in consecutive registers of the callee's
//   register file (slots 0..argc-1). The callee's parameters occupy
//   those slots. Extra arguments beyond num_parameters are ignored;
//   missing arguments are filled with undefined.
//
// Recursion limit:
//   We enforce a maximum call stack depth (default 1000) to prevent
//   stack overflow from infinite recursion.

#ifndef V12_INTERPRETER_INTERPRETER_H_
#define V12_INTERPRETER_INTERPRETER_H_

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "base/macros.h"
#include "frontend/bytecode/bytecode.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/context.h"
#include "vm/values/value.h"

namespace v12 {

class Isolate;
class BytecodeProgram;
class JSFunction;
class JSObject;
class JSArray;

// InterpreterStatus: the result of executing a function.
enum class InterpStatus : uint8_t {
    kReturned,    // function returned normally; result is in result_value
    kThrew,       // function threw an exception; result_value is the exception
};

struct InterpResult {
    InterpStatus status;
    Value value;
};

class Interp {
public:
    explicit Interp(Isolate* iso);
    ~Interp();

    // Run a compiled program. Executes the toplevel function and returns
    // its result. The toplevel function takes no arguments.
    InterpResult Run(BytecodeProgram* program);

    // Call a JSFunction with the given arguments. Used by host code to
    // invoke JS functions, and recursively by the interpreter for nested
    // calls. `this_val` is the `this` for the call.
    InterpResult CallFunction(JSFunction* fn, Value this_val,
                              const Value* args, uint32_t argc);

    // Call a HostFunction. Used internally when invoking host-builtins.
    InterpResult CallHostFunction(HostFunction* fn, Value this_val,
                                  const Value* args, uint32_t argc);

    Isolate* isolate() { return iso_; }

    // Maximum call stack depth. Can be raised/lowered per-interpreter.
    void set_max_depth(uint32_t d) { max_depth_ = d; }
    uint32_t max_depth() const { return max_depth_; }

    // The pending exception (set by Throw; read by host code or a future
    // TryCatch handler).
    Value pending_exception() const { return pending_exception_; }
    void clear_pending_exception() { pending_exception_ = Value::FromSmi(0); }

private:
    struct Frame {
        FunctionInfo* info;
        Value* regs;          // points into reg_storage_
        const uint8_t* pc;
        Context* context;     // current context (for closure access)
        Value this_val;
        JSFunction* function; // the JSFunction being executed (or nullptr for toplevel)
        uintptr_t jit_deopt_acc = 0;  // JIT stores acc here on deopt
    };

    // DispatchState: the local variables used by ExecuteTop's dispatch loop.
    // Passed to HandleException so it can update the frame state when a
    // catch handler is found.
    struct DispatchState {
        Frame* frame;
        const uint8_t* pc;
        Value acc;
        FunctionInfo* info;
        Value* regs;
        Context* ctx;
        const uint8_t* bytecode_base;
    };

    Isolate* iso_;

    // The currently-running program. Used by CreateClosure to look up
    // the FunctionInfo referenced by a constant pool entry.
    BytecodeProgram* current_program_ = nullptr;

    // The register storage backing all frame register files. We use a
    // large mmap'd region (lazily committed by the OS) so that the
    // pointer never moves — no realloc, no stale pointers, no limit.
    // 256 MB of virtual address space = 32M Value slots = ~2M frames.
    Value* reg_base_ = nullptr;
    size_t reg_stack_top_ = 0;
    static constexpr size_t kRegRegionSize = 256 * 1024 * 1024;  // 256 MB

    // The frame stack. We use a raw array + top pointer instead of
    // std::vector to eliminate push_back/pop_back overhead (capacity
    // checks, size updates, back() dereferences). Pre-allocated to
    // max_depth_ entries.
    Frame* frames_ = nullptr;
    size_t frame_top_ = 0;  // index of next free slot (= current depth)

    // Pending exception (set by Throw, cleared by TryCatch).
    Value pending_exception_;

    // Deopt accumulator: the JIT saves acc here on deopt (via RBX).
    // The interpreter reads it back after the JIT call returns nonzero.
    uintptr_t jit_deopt_acc_ = 0;

    uint32_t max_depth_ = 100000;

    // Execute the top frame on the frame stack until it returns or throws.
    // Marked [[gnu::hot]] so the compiler prioritizes this function for
    // optimization. We intentionally do NOT use [[gnu::flatten]] — flattening
    // a 1500-line function causes excessive code size and register pressure.
    [[gnu::hot]] InterpResult ExecuteTop();

    // Cold exception-handling helper. Called when a TypeError/etc. is thrown
    // from a Call/CallProperty/Construct handler. Searches the handler table
    // for a catch clause. Returns true if a handler was found (in which case
    // the frame state is updated to jump to the catch handler); false if no
    // handler was found (caller should return kThrew).
    //
    // Marked [[gnu::noinline]] to keep the cold exception-handling code out
    // of the hot dispatch loop in ExecuteTop. This significantly reduces
    // ExecuteTop's code size and improves register allocation.
    struct DispatchState;
    [[gnu::noinline]] bool HandleException(Value exc, uint32_t pc_offset,
                                           DispatchState& ds);

    // ----- Cold opcode handlers (extracted from ExecuteTop) -----
    // These are rarely-executed opcodes with complex handlers. Extracting
    // them into [[gnu::noinline]] functions reduces ExecuteTop's code size,
    // improving register allocation for hot paths. Each takes a DispatchState&
    // and returns true if execution should continue (V12_DISPATCH), or false
    // if ExecuteTop should return (with the result in ds.acc or via return).

    // Construct (new operator) — cold because it allocates a new object.
    // Returns true on success, false if an exception was thrown (ds.acc
    // contains the exception value).
    [[gnu::noinline]] bool HandleConstruct(DispatchState& ds);
    // ObjectKeys — cold (for-in iteration setup, allocates an array).
    [[gnu::noinline]] void HandleObjectKeys(DispatchState& ds);
    // Throw — cold (exception handling, walks the call stack).
    // Returns true if a handler was found (ds updated), false if uncaught.
    [[gnu::noinline]] bool HandleThrow(DispatchState& ds);
    // CallBuiltin — cold (stub, not yet fully implemented).
    [[gnu::noinline]] void HandleCallBuiltin(DispatchState& ds);

    // Push a new frame — ALWAYS_INLINE for the hot call path.
    [[gnu::always_inline]] Value* PushFrame(FunctionInfo* info, JSFunction* fn,
                     Value this_val, Context* closure_ctx,
                     uint32_t argc, const Value* args) {
        V12_CHECK(frame_top_ < max_depth_, "maximum call stack depth exceeded");
        uint16_t nregs = info->num_registers;
        if (nregs == 0) nregs = 1;
        size_t base = reg_stack_top_;
        reg_stack_top_ += nregs;
        Frame* f = &frames_[frame_top_++];
        f->info = info;
        f->regs = reg_base_ + base;
        f->pc = info->bytecode.data();
        f->context = closure_ctx;
        f->this_val = this_val;
        f->function = fn;
        uint16_t nparams = info->num_parameters;
        uint16_t to_copy = (argc < nparams) ? static_cast<uint16_t>(argc) : nparams;
        Value* r = f->regs;
        for (uint16_t i = 0; i < to_copy; ++i) r[i] = args[i];
        Value undef = iso_->undefined_value();
        for (uint16_t i = to_copy; i < nparams; ++i) r[i] = undef;
        return r;
    }

    // Pop the top frame — ALWAYS_INLINE for the hot return path.
    [[gnu::always_inline]] void PopFrame() {
        --frame_top_;
        Frame* f = &frames_[frame_top_];
        uint16_t nregs = f->info->num_registers;
        if (nregs == 0) nregs = 1;
        reg_stack_top_ -= nregs;
    }

    // Access the top frame.
    Frame* TopFrame() { return &frames_[frame_top_ - 1]; }

    // Read an operand at the current PC, advancing PC.
    // These use memcpy for multi-byte reads, which the compiler optimizes
    // to a single load instruction on x86/ARM (much faster than byte-by-byte
    // assembly with shifts).
    uint8_t ReadU8(const uint8_t** pc) {
        uint8_t v = **pc; ++(*pc); return v;
    }
    uint16_t ReadU16(const uint8_t** pc) {
        uint16_t v;
        std::memcpy(&v, *pc, 2);
        *pc += 2;
        return v;
    }
    uint32_t ReadU32(const uint8_t** pc) {
        uint32_t v;
        std::memcpy(&v, *pc, 4);
        *pc += 4;
        return v;
    }

    // Read a register from the current frame.
    Value& Reg(Frame* f, uint8_t idx) {
        V12_DCHECK(idx < f->info->num_registers, "register index out of range");
        return f->regs[idx];
    }
};

}  // namespace v12

#endif  // V12_INTERPRETER_INTERPRETER_H_
