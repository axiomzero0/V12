// =============================================================================
// src/frontend/bytecode/bytecode.h
// =============================================================================
// Bytecode definitions.
//
// Design:
//   - Compact 1-byte opcodes.
//   - Operands are 8-bit, 16-bit, or 32-bit unsigned integers.
//   - Operands reference into per-Function constant pool, register file, or
//     bytecode offset table.
//
// Register-based vs stack-based:
//   We use a register-based bytecode (like LuaJIT, Dalvik, BEAM) rather
//   than a stack-based one (like V8 Ignition). Reasons:
//     1. Register-based is more compact (fewer push/pop instructions).
//     2. Maps more naturally to a register machine IR.
//     3. Type feedback is more naturally attached to a register slot than
//        to a stack position (which moves around).
//   The trade-off is that the bytecode generator must do register allocation
//   (liveness analysis) up front. We do this in the bytecode generator.

#ifndef V12_FRONTEND_BYTECODE_BYTECODE_H_
#define V12_FRONTEND_BYTECODE_BYTECODE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

class Value;   // forward declaration; full definition in vm/values/value.h
class Isolate; // forward declaration; full definition in vm/isolate/isolate.h

// Register reference: 0..255 are local registers; higher values are special.
// We use 8-bit register IDs to keep bytecode compact.
using Reg = uint8_t;
using BytecodeOffset = uint32_t;

constexpr Reg kInvalidReg = 0xFF;
constexpr Reg kAccumulatorReg = 0xFE;   // special: the accumulator
constexpr Reg kThisReg = 0xFD;          // special: `this`
constexpr int kNumSpecialRegs = 3;

// Operand kinds for the bytecode printer / verifier.
enum class OperandKind : uint8_t {
    kNone,
    kReg,           // 8-bit register id
    kRegPair,       // 16-bit (two registers, for call with N args)
    kImm8,          // 8-bit immediate
    kImm16,         // 16-bit immediate
    kImm32,         // 32-bit immediate
    kConst,         // index into constant pool
    kIdx,           // index into feedback vector
    kJump,          // 32-bit bytecode offset
    kArgCount,      // 16-bit argument count
    kPropertyIdx,   // index into property name table
};

// Opcodes. Keep this in sync with kOpcodeNames in bytecode.cc.
enum class Op : uint8_t {
    // Loading constants
    LdaConst,           // acc = constant[idx]
    LdaSmi,             // acc = Smi(imm8)  - small int shortcut (0..255)
    LdaSmi16,           // acc = Smi(imm16) - medium int (-32768..32767)
    LdaZero,            // acc = 0
    LdaUndefined,       // acc = undefined
    LdaNull,            // acc = null
    LdaTrue,            // acc = true
    LdaFalse,           // acc = false
    LdaThis,            // acc = this

    // Register moves
    Ldar,               // acc = reg
    Star,               // reg = acc
    Mov,                // dst = src

    // Arithmetic - binary
    Add,                // acc = acc <op> reg
    Sub,
    Mul,
    Div,
    Mod,
    Exp,
    BitOr,
    BitAnd,
    BitXor,
    Shl,
    Shr,
    Ushr,

    // Arithmetic - binary with constant operand
    AddConst,           // acc = acc + const[idx]
    SubConst,
    MulConst,

    // Arithmetic - unary
    Negate,             // acc = -acc
    BitNot,             // acc = ~acc
    LogicalNot,         // acc = !acc
    Typeof,             // acc = typeof acc

    // Comparison
    TestEqual,
    TestNotEqual,
    TestEqStrict,
    TestNotEqStrict,
    TestLessThan,
    TestGreaterThan,
    TestLessThanOrEqual,
    TestGreaterThanOrEqual,
    TestInstanceOf,
    TestIn,

    // Increment / decrement
    Inc,
    Dec,
    IncReg,             // R[imm8] = R[imm8] + 1  (fused Ldar+Inc+Star)
    DecReg,             // R[imm8] = R[imm8] - 1
    AddConstToReg,      // R[r1:imm8] += K[r2:imm8]  (fused for += const)

    // Control flow
    Jump,               // unconditional
    JumpIfTrue,         // jump if acc is truthy
    JumpIfFalse,
    JumpIfNull,
    JumpIfUndefined,
    JumpIfNotNullOrUndefined,
    JumpIfToBooleanTrue,
    JumpIfToBooleanFalse,
    JumpLoop,           // backward jump (for loops)

    // Property access
    LoadProperty,       // acc = acc.name
    LoadIndexed,        // acc = acc[idx]   (idx in next reg or imm)
    StoreProperty,      // acc.name = reg
    StoreIndexed,       // acc[idx] = reg

    // Variables
    LoadGlobal,         // acc = global[name]
    StoreGlobal,        // global[name] = acc
    LoadContext,        // acc = context-var[idx]
    StoreContext,       // context-var[idx] = acc

    // Calls
    Call,               // acc = acc(args...)
    CallProperty,       // acc = recv.method(args...)
    Call0,              // acc = acc()  (no args)
    Call1,
    Call2,
    Construct,          // acc = new acc(args...)
    CallBuiltin,        // builtin call

    // Object / array creation
    NewObject,          // acc = new {}
    NewArray,           // acc = new [] (initial capacity imm16)
    DefineProperty,     // acc.name = reg (used in object literal)
    CreateClosure,      // acc = closure of function_info[idx]
    PushArray,          // arr.reg = arr.reg, push acc  (acc is the value)
    LoadArrayLength,    // acc = (acc as Array).length
    StoreArrayLength,   // (acc as Array).length = reg

    // Context allocation (for closure capture)
    CreateContext,      // acc = new Context(slot_count: imm16)  parent = current
    PushContext,        // current = acc  (set the running frame's context)
    PopContext,         // current = current.parent  (restore prior context)

    // Iteration helpers
    ObjectKeys,         // acc = array of key strings from acc (object)
    GetIterator,        // acc = iterator for acc (array/string) — returns the
                        // receiver itself for arrays/strings (for-of uses
                        // indexed access with a counter)

    // Iteration
    ForInPrepare,
    ForInNext,
    ForInDone,

    // Returns
    Return,             // return acc
    ReturnUndefined,    // return undefined

    // Exceptions
    Throw,              // throw acc
    TryCatch,           // sets up try-catch
    TryFinally,
    Exception,

    // Debugger
    Debugger,

    // Misc
    Pop,                // discard acc (replace with undefined)
    Dup,                // acc = acc (no-op logically, but signals to JIT)
    Nop,
    Illegal,

    kCount,
};

const char* OpName(Op op);

// Operand format for each opcode. Used by the bytecode printer and verifier.
struct OpInfo {
    Op op;
    const char* name;
    OperandKind operand1;
    OperandKind operand2;
    OperandKind operand3;
    uint8_t length;   // total bytecode length including opcode byte
};

const OpInfo& GetOpInfo(Op op);

// A constant pool entry. Constants are eagerly deduplicated by the bytecode
// generator.
struct Constant {
    enum class Kind : uint8_t {
        kSmi,
        kNumber,
        kString,
        kBoolean,
        kUndefined,
        kNull,
        kFunctionInfo,  // index into a separate FunctionInfo table
    };
    Kind kind;
    union {
        int64_t smi;
        double number;
        bool boolean;
        uint32_t index;   // for strings and function infos
    };
};

// ----- Inline cache (IC) storage -----
// Each feedback slot (the idx:16 operand on property access AND arithmetic
// opcodes) maps to an ICEntry. For property access, the IC caches the
// (Shape, slot) pair. For arithmetic, the IC caches the type seen.
struct ICEntry {
    // The shape seen on the last execution. 0 means "uninitialized".
    uintptr_t shape = 0;
    // The property slot index for this shape.
    uint16_t slot = 0xFFFF;
    // Is this IC entry initialized?
    bool initialized = false;
    // For LoadGlobal/StoreGlobal: cache the direct pointer to the
    // property slot in the global object's properties array. This
    // eliminates the shape compare + array index on every access.
    // Set on first IC hit, valid as long as the global shape doesn't
    // change (which it doesn't after startup).
    uintptr_t value_ptr = 0;

    // ----- Type feedback for arithmetic opcodes -----
    // Records the type seen on the last execution of Add/Sub/Mul/Div.
    // The baseline JIT reads this to emit a single type guard instead
    // of assuming Smi and deopting on the first non-Smi.
    //   0 = uninitialized (no feedback yet)
    //   1 = Smi (both operands Smi, no overflow)
    //   2 = Number (HeapNumber or Smi overflow)
    //   3 = String (string concatenation)
    //   4 = Other (boolean, object, etc.)
    uint8_t type_feedback = 0;
};

// FunctionInfo: per-function metadata. One per JS function (and one for
// the top-level program). Owned by a BytecodeProgram (see below).
//
// LAYOUT NOTE: Fields are deliberately ordered by access frequency to
// maximize cache-line utilization. The first 64 bytes (1 cache line)
// contain the hottest fields: bytecode vector (accessed on every Call/
// JumpLoop), num_registers/num_parameters/num_context_vars (PushFrame),
// ic_entries (every property access), and hotness/deopt counters (JumpLoop).
// Cold fields (name, source_positions, handlers, lazy_* fields) are pushed
// to later cache lines so they don't displace hot data.
struct FunctionInfo {
    // ===== CACHE LINE 0 (hot fields, accessed on every dispatch) =====
    std::vector<uint8_t> bytecode;          // 24 bytes — every Call/JumpLoop
    uint16_t num_registers = 0;             //  2 bytes — PushFrame
    uint16_t num_parameters = 0;            //  2 bytes — PushFrame
    uint16_t num_context_vars = 0;          //  2 bytes — PushFrame/CreateContext
    uint16_t feedback_vector_length = 0;    //  2 bytes — EnsureICCapacity (compile)
    // 4 bytes padding to align ic_entries to 8-byte boundary
    std::vector<ICEntry> ic_entries;        // 24 bytes — every property access
    uint32_t hotness_counter = 0;           //  4 bytes — JumpLoop (JIT tier-up)
    uint32_t deopt_count = 0;              //  4 bytes — JumpLoop (JIT check)
    // ===== END CACHE LINE 0 (64 bytes) =====

    // ===== CACHE LINE 1+ (moderate fields) =====
    uintptr_t jit_code = 0;                 //  8 bytes — JumpLoop (JIT entry)
    uintptr_t* resolved_constants = nullptr;//  8 bytes — LdaConst fast path
    std::vector<Constant> constants;        // 24 bytes — LdaConst slow path
    bool is_compiled = true;                //  1 byte  — CreateClosure

    // ===== COLD FIELDS (rarely accessed) =====
    std::string name;                       // 32 bytes — stack traces only
    std::vector<std::string> property_names;// 24 bytes — IC miss only
    std::vector<std::string> global_names;  // 24 bytes — IC miss only

    bool strict = false;
    bool is_async = false;
    bool is_generator = false;
    bool is_toplevel = false;       // true for the program-level FunctionInfo

    // Source position table (compressed): for each bytecode offset, the
    // (line, column) of the source. Used for stack traces.
    struct SourcePosition {
        BytecodeOffset bytecode_offset;
        uint32_t line;
        uint32_t column;
    };
    std::vector<SourcePosition> source_positions;

    // Pre-allocate the IC entries vector to feedback_vector_length. Call this
    // once at the end of compilation (after all feedback slots have been
    // allocated). After this, GetIC's bounds-check branch is always
    // not-taken (predicted correctly), eliminating the resize() path.
    void EnsureICCapacity() {
        if (ic_entries.size() < feedback_vector_length) {
            ic_entries.resize(feedback_vector_length);
        }
    }

    // Get the IC entry for a feedback slot index. The vector is pre-allocated
    // by EnsureICCapacity at compile time; the bounds check remains as a
    // safety net (and is predicted not-taken after warmup).
    // Marked always_inline to eliminate the function call overhead on the
    // property access hot path while keeping the bounds check (which helps
    // the compiler generate better code for the large ExecuteTop function).
    [[gnu::always_inline]] ICEntry& GetIC(uint16_t idx) {
        if (idx >= ic_entries.size()) ic_entries.resize(idx + 1);
        return ic_entries[idx];
    }

    // ----- Exception handler table -----
    // Maps a bytecode range (the try block) to a catch handler offset.
    // When an exception is thrown, the interpreter searches this table
    // for an entry whose [try_start, try_end) range contains the current
    // PC. If found, execution jumps to catch_start with the exception in
    // the accumulator.
    struct HandlerEntry {
        uint32_t try_start;    // bytecode offset of the try block start
        uint32_t try_end;      // bytecode offset just past the try block
        uint32_t catch_start;  // bytecode offset of the catch handler
    };
    std::vector<HandlerEntry> handlers;

    // Search the handler table for an entry containing `pc_offset`.
    // Returns the catch_start offset, or 0xFFFFFFFF if not found.
    uint32_t FindHandler(uint32_t pc_offset) const {
        for (const auto& h : handlers) {
            if (pc_offset >= h.try_start && pc_offset < h.try_end) {
                return h.catch_start;
            }
        }
        return 0xFFFFFFFF;
    }

    // Look up a constant's Value at runtime. Returns the appropriate Value
    // (Smi, HeapNumber, JSString, etc.) given an Isolate. After
    // PreResolveConstants has been called, this is a single array load for
    // all kinds except kFunctionInfo.
    Value ResolveConstant(Isolate* iso, uint32_t idx) const;

    // Pre-resolve all constants (except kFunctionInfo) into a heap-allocated
    // array parallel to `constants`. After this, ResolveConstant never
    // allocates heap objects — it just loads from resolved_constants[idx].
    // Uses a raw pointer (8 bytes) instead of a vector (24 bytes) to minimize
    // the FunctionInfo size increase.
    void PreResolveConstants(Isolate* iso);

    // ----- Lazy compilation -----
    // (is_compiled is declared in cache line 1 above.)
    // If false, this function's bytecode has not been compiled yet. The
    // interpreter triggers compilation on first CreateClosure.

    // Tier-up counter for OSR. Incremented on JumpLoop; when it crosses
    // a threshold, the interpreter triggers baseline JIT compilation.
    // (hotness_counter and deopt_count are declared in cache line 0 above.)

    // Pointer to the baseline JIT CodeObject (if compiled). nullptr means
    // no JIT code has been generated yet. Forward-declared — the full
    // definition is in contracts/code-object.h.
    class CodeObject;
    // (jit_code is declared in cache line 1 above.)

    // For lazy compilation: stores the AST node and scope needed to
    // compile this function on first use. Only set when is_compiled == false.
    // The AST node is either a FunctionDecl*, FunctionExpr*, or ArrowFunction*.
    // We store it as void* to avoid header dependencies.
    void* lazy_ast = nullptr;       // FunctionDecl* / FunctionExpr* / ArrowFunction*
    void* lazy_scope = nullptr;     // Scope*
    bool lazy_is_toplevel = false;

    // (resolved_constants is declared in cache line 1 above.)
};

// BytecodeProgram: the result of compiling a whole source file. Owns:
//   - All FunctionInfos (the toplevel + nested functions).
//   - The interner state needed to keep string_views alive.
// Lifetime: as long as the program is runnable.
struct BytecodeProgram {
    std::vector<std::unique_ptr<FunctionInfo>> functions;
    FunctionInfo* toplevel = nullptr;
    // The scope analyzer is kept alive here so that lazy compilation can
    // use it (it contains the scope tree built during the initial Compile).
    // Forward-declared to avoid header dependency. The destructor is defined
    // in bytecode.cc where ScopeAnalyzer is complete.
    std::unique_ptr<class ScopeAnalyzer> scope_analyzer;

    BytecodeProgram();
    ~BytecodeProgram();

    FunctionInfo* NewFunction(std::string name) {
        auto fi = std::make_unique<FunctionInfo>();
        fi->name = std::move(name);
        FunctionInfo* raw = fi.get();
        functions.push_back(std::move(fi));
        return raw;
    }
};

}  // namespace v12

#endif  // V12_FRONTEND_BYTECODE_BYTECODE_H_
