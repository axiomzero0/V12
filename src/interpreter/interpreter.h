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
    };

    Isolate* iso_;

    // The currently-running program. Used by CreateClosure to look up
    // the FunctionInfo referenced by a constant pool entry.
    BytecodeProgram* current_program_ = nullptr;

    // The register storage backing all frame register files. We grow this
    // dynamically as frames are pushed.
    std::vector<Value> reg_storage_;

    // The frame stack.
    std::vector<Frame> frames_;

    // Pending exception (set by Throw, cleared by TryCatch).
    Value pending_exception_;

    uint32_t max_depth_ = 1000;

    // Execute the top frame on the frame stack until it returns or throws.
    InterpResult ExecuteTop();

    // Push a new frame for `fn` with `argc` arguments. Returns a pointer
    // to the new frame's register file. The caller fills registers 0..argc-1
    // with the arguments before calling ExecuteTop.
    Value* PushFrame(FunctionInfo* info, JSFunction* fn, Value this_val,
                     Context* closure_ctx, uint32_t argc, const Value* args);

    // Pop the top frame. Returns the frame's saved register base so the
    // caller can shrink reg_storage_.
    void PopFrame();

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
