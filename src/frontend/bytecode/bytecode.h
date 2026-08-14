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
#include <string>
#include <string_view>
#include <vector>

#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

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
    LdaSmi,             // acc = Smi(imm8)  - small int shortcut
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
    NewArray,           // acc = new []
    DefineProperty,     // acc.name = reg (used in object literal)
    CreateClosure,      // acc = closure of function[idx]

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

// FunctionInfo: per-function metadata. One per JS function.
struct FunctionInfo {
    std::string_view name;
    std::vector<uint8_t> bytecode;
    std::vector<Constant> constants;
    std::vector<std::string_view> property_names;  // for property access ops
    std::vector<std::string_view> global_names;
    uint16_t num_parameters = 0;
    uint16_t num_registers = 0;     // locals + temporaries
    uint16_t num_context_vars = 0;
    bool strict = false;
    bool is_async = false;
    bool is_generator = false;

    // Feedback vector size (number of slots).
    uint16_t feedback_vector_length = 0;

    // Source position table (compressed): for each bytecode offset, the
    // (line, column) of the source. Used for stack traces.
    struct SourcePosition {
        BytecodeOffset bytecode_offset;
        uint32_t line;
        uint32_t column;
    };
    std::vector<SourcePosition> source_positions;
};

}  // namespace v12

#endif  // V12_FRONTEND_BYTECODE_BYTECODE_H_
