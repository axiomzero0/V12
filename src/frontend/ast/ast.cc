// =============================================================================
// src/frontend/ast/ast.cc
// =============================================================================

#include "frontend/ast/ast.h"

namespace v12 {

const char* AstKindName(AstKind kind) {
    switch (kind) {
        case AstKind::kNumberLiteral:       return "NumberLiteral";
        case AstKind::kStringLiteral:       return "StringLiteral";
        case AstKind::kBoolLiteral:         return "BoolLiteral";
        case AstKind::kNullLiteral:         return "NullLiteral";
        case AstKind::kUndefinedLiteral:    return "UndefinedLiteral";
        case AstKind::kIdentifier:          return "Identifier";
        case AstKind::kThis:                return "This";
        case AstKind::kSuper:               return "Super";
        case AstKind::kBinaryOp:            return "BinaryOp";
        case AstKind::kUnaryOp:             return "UnaryOp";
        case AstKind::kUpdateOp:            return "UpdateOp";
        case AstKind::kAssignment:          return "Assignment";
        case AstKind::kLogicalOp:           return "LogicalOp";
        case AstKind::kConditional:         return "Conditional";
        case AstKind::kSequence:            return "Sequence";
        case AstKind::kCall:                return "Call";
        case AstKind::kNew:                 return "New";
        case AstKind::kMember:              return "Member";
        case AstKind::kOptionalChain:       return "OptionalChain";
        case AstKind::kArrayLiteral:        return "ArrayLiteral";
        case AstKind::kObjectLiteral:       return "ObjectLiteral";
        case AstKind::kFunctionExpr:        return "FunctionExpr";
        case AstKind::kArrowFunction:       return "ArrowFunction";
        case AstKind::kClassExpr:           return "ClassExpr";
        case AstKind::kTemplateLiteral:     return "TemplateLiteral";
        case AstKind::kSpread:              return "Spread";
        case AstKind::kYield:               return "Yield";
        case AstKind::kBlock:               return "Block";
        case AstKind::kVarDecl:             return "VarDecl";
        case AstKind::kLetDecl:             return "LetDecl";
        case AstKind::kConstDecl:           return "ConstDecl";
        case AstKind::kFunctionDecl:        return "FunctionDecl";
        case AstKind::kClassDecl:           return "ClassDecl";
        case AstKind::kIf:                  return "If";
        case AstKind::kWhile:               return "While";
        case AstKind::kDoWhile:             return "DoWhile";
        case AstKind::kFor:                 return "For";
        case AstKind::kForIn:               return "ForIn";
        case AstKind::kForOf:               return "ForOf";
        case AstKind::kBreak:               return "Break";
        case AstKind::kContinue:            return "Continue";
        case AstKind::kReturn:              return "Return";
        case AstKind::kThrow:               return "Throw";
        case AstKind::kTry:                 return "Try";
        case AstKind::kSwitch:              return "Switch";
        case AstKind::kLabeled:             return "Labeled";
        case AstKind::kEmpty:               return "Empty";
        case AstKind::kExpressionStatement: return "ExpressionStatement";
        case AstKind::kDebugger:            return "Debugger";
        case AstKind::kProgram:             return "Program";
        case AstKind::kModule:              return "Module";
    }
    return "<unknown>";
}

}  // namespace v12
