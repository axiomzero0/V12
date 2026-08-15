// =============================================================================
// src/frontend/lexer/tokens.cc
// =============================================================================

#include "frontend/lexer/tokens.h"

namespace v12 {

const char* TokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::kEof:           return "EOF";
        case TokenKind::kError:         return "ERROR";
        case TokenKind::kNumber:        return "NUMBER";
        case TokenKind::kString:        return "STRING";
        case TokenKind::kIdentifier:    return "IDENT";
        case TokenKind::kLParen:        return "(";
        case TokenKind::kRParen:        return ")";
        case TokenKind::kLBrace:        return "{";
        case TokenKind::kRBrace:        return "}";
        case TokenKind::kLBracket:      return "[";
        case TokenKind::kRBracket:      return "]";
        case TokenKind::kComma:         return ",";
        case TokenKind::kSemicolon:     return ";";
        case TokenKind::kColon:         return ":";
        case TokenKind::kDot:           return ".";
        case TokenKind::kArrow:         return "=>";
        case TokenKind::kSpread:        return "...";
        case TokenKind::kQuestion:      return "?";
        case TokenKind::kNullCoalesce:  return "??";
        case TokenKind::kOptChain:      return "?.";
        case TokenKind::kBacktick:      return "`";
        case TokenKind::kAssign:        return "=";
        case TokenKind::kAddAssign:     return "+=";
        case TokenKind::kSubAssign:     return "-=";
        case TokenKind::kMulAssign:     return "*=";
        case TokenKind::kDivAssign:     return "/=";
        case TokenKind::kModAssign:     return "%=";
        case TokenKind::kBitAndAssign:  return "&=";
        case TokenKind::kBitOrAssign:   return "|=";
        case TokenKind::kBitXorAssign:  return "^=";
        case TokenKind::kShlAssign:     return "<<=";
        case TokenKind::kShrAssign:     return ">>=";
        case TokenKind::kUshrAssign:    return ">>>=";
        case TokenKind::kExpAssign:     return "**=";
        case TokenKind::kAndAssign:     return "&&=";
        case TokenKind::kOrAssign:      return "||=";
        case TokenKind::kNullishAssign: return "??=";
        case TokenKind::kPlus:          return "+";
        case TokenKind::kMinus:         return "-";
        case TokenKind::kStar:          return "*";
        case TokenKind::kSlash:         return "/";
        case TokenKind::kPercent:       return "%";
        case TokenKind::kExp:           return "**";
        case TokenKind::kEq:            return "==";
        case TokenKind::kNotEq:         return "!=";
        case TokenKind::kEqEq:          return "===";
        case TokenKind::kNotEqEq:       return "!==";
        case TokenKind::kLt:            return "<";
        case TokenKind::kGt:            return ">";
        case TokenKind::kLe:            return "<=";
        case TokenKind::kGe:            return ">=";
        case TokenKind::kAnd:           return "&&";
        case TokenKind::kOr:            return "||";
        case TokenKind::kNot:           return "!";
        case TokenKind::kBitAnd:        return "&";
        case TokenKind::kBitOr:         return "|";
        case TokenKind::kBitXor:        return "^";
        case TokenKind::kBitNot:        return "~";
        case TokenKind::kShl:           return "<<";
        case TokenKind::kShr:           return ">>";
        case TokenKind::kUshr:          return ">>>";
        case TokenKind::kInc:           return "++";
        case TokenKind::kDec:           return "--";
        case TokenKind::kVar:           return "var";
        case TokenKind::kLet:           return "let";
        case TokenKind::kConst:         return "const";
        case TokenKind::kFunction:      return "function";
        case TokenKind::kReturn:        return "return";
        case TokenKind::kIf:            return "if";
        case TokenKind::kElse:          return "else";
        case TokenKind::kWhile:         return "while";
        case TokenKind::kFor:           return "for";
        case TokenKind::kDo:            return "do";
        case TokenKind::kBreak:         return "break";
        case TokenKind::kContinue:      return "continue";
        case TokenKind::kSwitch:        return "switch";
        case TokenKind::kCase:          return "case";
        case TokenKind::kDefault:       return "default";
        case TokenKind::kTrue:          return "true";
        case TokenKind::kFalse:         return "false";
        case TokenKind::kNull:          return "null";
        case TokenKind::kUndefined:     return "undefined";
        case TokenKind::kNew:           return "new";
        case TokenKind::kTypeof:        return "typeof";
        case TokenKind::kInstanceof:    return "instanceof";
        case TokenKind::kIn:            return "in";
        case TokenKind::kOf:            return "of";
        case TokenKind::kDelete:        return "delete";
        case TokenKind::kVoid:          return "void";
        case TokenKind::kTry:           return "try";
        case TokenKind::kCatch:         return "catch";
        case TokenKind::kFinally:       return "finally";
        case TokenKind::kThrow:         return "throw";
        case TokenKind::kClass:         return "class";
        case TokenKind::kExtends:       return "extends";
        case TokenKind::kSuper:         return "super";
        case TokenKind::kThis:          return "this";
        case TokenKind::kImport:        return "import";
        case TokenKind::kExport:        return "export";
        case TokenKind::kFrom:          return "from";
        case TokenKind::kAs:            return "as";
        case TokenKind::kAsync:         return "async";
        case TokenKind::kAwait:         return "await";
        case TokenKind::kYield:         return "yield";
        case TokenKind::kStatic:        return "static";
        case TokenKind::kGet:           return "get";
        case TokenKind::kSet:           return "set";
        case TokenKind::kDebugger:      return "debugger";
        case TokenKind::kCount:         return "<COUNT>";
    }
    return "<unknown>";
}

}  // namespace v12
