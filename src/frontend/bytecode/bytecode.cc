// =============================================================================
// src/frontend/bytecode/bytecode.cc
// =============================================================================

#include "frontend/bytecode/bytecode.h"

namespace v12 {

const char* OpName(Op op) {
    return GetOpInfo(op).name;
}

namespace {

// The order of entries MUST match the Op enum.
constexpr OpInfo kOpInfos[] = {
    {Op::LdaConst,           "LdaConst",           OperandKind::kConst,    OperandKind::kNone, OperandKind::kNone, 5},
    {Op::LdaSmi,             "LdaSmi",             OperandKind::kImm8,     OperandKind::kNone, OperandKind::kNone, 2},
    {Op::LdaZero,            "LdaZero",            OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::LdaUndefined,       "LdaUndefined",       OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::LdaNull,            "LdaNull",            OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::LdaTrue,            "LdaTrue",            OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::LdaFalse,           "LdaFalse",           OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::LdaThis,            "LdaThis",            OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},

    {Op::Ldar,               "Ldar",               OperandKind::kReg,      OperandKind::kNone, OperandKind::kNone, 2},
    {Op::Star,               "Star",               OperandKind::kReg,      OperandKind::kNone, OperandKind::kNone, 2},
    {Op::Mov,                "Mov",                OperandKind::kReg,      OperandKind::kReg,  OperandKind::kNone, 3},

    {Op::Add,                "Add",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Sub,                "Sub",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Mul,                "Mul",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Div,                "Div",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Mod,                "Mod",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Exp,                "Exp",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::BitOr,              "BitOr",              OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::BitAnd,             "BitAnd",             OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::BitXor,             "BitXor",             OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Shl,                "Shl",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Shr,                "Shr",                OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Ushr,               "Ushr",               OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},

    {Op::AddConst,           "AddConst",           OperandKind::kConst,    OperandKind::kIdx,  OperandKind::kNone, 6},
    {Op::SubConst,           "SubConst",           OperandKind::kConst,    OperandKind::kIdx,  OperandKind::kNone, 6},
    {Op::MulConst,           "MulConst",           OperandKind::kConst,    OperandKind::kIdx,  OperandKind::kNone, 6},

    {Op::Negate,             "Negate",             OperandKind::kIdx,      OperandKind::kNone, OperandKind::kNone, 3},
    {Op::BitNot,             "BitNot",             OperandKind::kIdx,      OperandKind::kNone, OperandKind::kNone, 3},
    {Op::LogicalNot,         "LogicalNot",         OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::Typeof,             "Typeof",             OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},

    {Op::TestEqual,          "TestEqual",          OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestNotEqual,       "TestNotEqual",       OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestEqStrict,       "TestEqStrict",       OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestNotEqStrict,    "TestNotEqStrict",    OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestLessThan,       "TestLessThan",       OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestGreaterThan,    "TestGreaterThan",    OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestLessThanOrEqual,"TestLessThanOrEqual",OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestGreaterThanOrEqual,"TestGreaterThanOrEqual",OperandKind::kReg,OperandKind::kIdx,OperandKind::kNone, 4},
    {Op::TestInstanceOf,     "TestInstanceOf",     OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::TestIn,             "TestIn",             OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},

    {Op::Inc,                "Inc",                OperandKind::kIdx,      OperandKind::kNone, OperandKind::kNone, 3},
    {Op::Dec,                "Dec",                OperandKind::kIdx,      OperandKind::kNone, OperandKind::kNone, 3},

    {Op::Jump,               "Jump",               OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfTrue,         "JumpIfTrue",         OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfFalse,        "JumpIfFalse",        OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfNull,         "JumpIfNull",         OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfUndefined,    "JumpIfUndefined",    OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfNotNullOrUndefined,"JumpIfNotNullOrUndefined",OperandKind::kJump,OperandKind::kNone,OperandKind::kNone,5},
    {Op::JumpIfToBooleanTrue,"JumpIfToBooleanTrue",OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpIfToBooleanFalse,"JumpIfToBooleanFalse",OperandKind::kJump,   OperandKind::kNone, OperandKind::kNone, 5},
    {Op::JumpLoop,           "JumpLoop",           OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},

    {Op::LoadProperty,       "LoadProperty",       OperandKind::kPropertyIdx, OperandKind::kIdx,OperandKind::kNone, 4},
    {Op::LoadIndexed,        "LoadIndexed",        OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::StoreProperty,      "StoreProperty",      OperandKind::kReg,      OperandKind::kPropertyIdx, OperandKind::kIdx, 5},
    {Op::StoreIndexed,       "StoreIndexed",       OperandKind::kReg,      OperandKind::kReg,  OperandKind::kIdx, 5},

    {Op::LoadGlobal,         "LoadGlobal",         OperandKind::kPropertyIdx, OperandKind::kIdx,OperandKind::kNone, 4},
    {Op::StoreGlobal,        "StoreGlobal",        OperandKind::kReg,      OperandKind::kPropertyIdx, OperandKind::kIdx, 5},
    {Op::LoadContext,        "LoadContext",        OperandKind::kImm16,    OperandKind::kIdx,  OperandKind::kNone, 5},
    {Op::StoreContext,       "StoreContext",       OperandKind::kReg,      OperandKind::kImm16,OperandKind::kIdx,  6},

    {Op::Call,               "Call",               OperandKind::kArgCount, OperandKind::kReg,  OperandKind::kIdx, 6},
    {Op::CallProperty,       "CallProperty",       OperandKind::kArgCount, OperandKind::kPropertyIdx, OperandKind::kIdx, 6},
    {Op::Call0,              "Call0",              OperandKind::kIdx,      OperandKind::kNone, OperandKind::kNone, 3},
    {Op::Call1,              "Call1",              OperandKind::kReg,      OperandKind::kIdx,  OperandKind::kNone, 4},
    {Op::Call2,              "Call2",              OperandKind::kReg,      OperandKind::kReg,  OperandKind::kIdx, 5},
    {Op::Construct,          "Construct",          OperandKind::kArgCount, OperandKind::kReg,  OperandKind::kIdx, 6},
    {Op::CallBuiltin,        "CallBuiltin",        OperandKind::kImm8,     OperandKind::kArgCount, OperandKind::kReg, 5},

    {Op::NewObject,          "NewObject",          OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::NewArray,           "NewArray",           OperandKind::kImm16,    OperandKind::kNone, OperandKind::kNone, 3},
    {Op::DefineProperty,     "DefineProperty",     OperandKind::kReg,      OperandKind::kPropertyIdx, OperandKind::kNone, 4},
    {Op::CreateClosure,      "CreateClosure",      OperandKind::kConst,    OperandKind::kNone, OperandKind::kNone, 5},

    {Op::ForInPrepare,       "ForInPrepare",       OperandKind::kReg,      OperandKind::kNone, OperandKind::kNone, 2},
    {Op::ForInNext,          "ForInNext",          OperandKind::kReg,      OperandKind::kReg,  OperandKind::kNone, 3},
    {Op::ForInDone,          "ForInDone",          OperandKind::kReg,      OperandKind::kNone, OperandKind::kNone, 2},

    {Op::Return,             "Return",             OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::ReturnUndefined,    "ReturnUndefined",    OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},

    {Op::Throw,              "Throw",              OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::TryCatch,           "TryCatch",           OperandKind::kJump,     OperandKind::kNone, OperandKind::kNone, 5},
    {Op::TryFinally,         "TryFinally",         OperandKind::kJump,     OperandKind::kJump, OperandKind::kNone, 9},
    {Op::Exception,          "Exception",          OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},

    {Op::Debugger,           "Debugger",           OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},

    {Op::Pop,                "Pop",                OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::Dup,                "Dup",                OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::Nop,                "Nop",                OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
    {Op::Illegal,            "Illegal",            OperandKind::kNone,     OperandKind::kNone, OperandKind::kNone, 1},
};

static_assert(sizeof(kOpInfos) / sizeof(kOpInfos[0]) == static_cast<size_t>(Op::kCount),
              "kOpInfos size must match Op::kCount");

}  // namespace

const OpInfo& GetOpInfo(Op op) {
    V12_DCHECK(static_cast<size_t>(op) < static_cast<size_t>(Op::kCount), "invalid opcode");
    return kOpInfos[static_cast<size_t>(op)];
}

}  // namespace v12
