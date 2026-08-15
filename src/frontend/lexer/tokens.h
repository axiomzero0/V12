// =============================================================================
// src/frontend/lexer/tokens.h
// =============================================================================
// Token types for the JavaScript lexer.
//
// We support a strict subset of ES2020+:
//   - var, let, const
//   - function, return
//   - if, else, while, for, do, break, continue, switch, case, default
//   - true, false, null, undefined
//   - new, typeof, instanceof, in, of, delete, void
//   - try, catch, finally, throw
//   - class, extends, super, this
//   - import, export (parsed but only basic forms supported)
//   - async, await (parsed, limited semantics)
//
// Not yet supported: template literals, computed property names in object
// literals, destructuring patterns (will be added in a later milestone).

#ifndef V12_FRONTEND_LEXER_TOKENS_H_
#define V12_FRONTEND_LEXER_TOKENS_H_

#include <cstdint>

namespace v12 {

enum class TokenKind : uint8_t {
    // Special
    kEof,
    kError,

    // Literals
    kNumber,        // 42, 3.14, 0x1F, 0b101, 1e5
    kString,        // "foo", 'bar'
    kIdentifier,    // foo, bar, _baz, $qux

    // Punctuation
    kLParen,        // (
    kRParen,        // )
    kLBrace,        // {
    kRBrace,        // }
    kLBracket,      // [
    kRBracket,      // ]
    kComma,         // ,
    kSemicolon,     // ;
    kColon,         // :
    kDot,           // .
    kArrow,         // =>
    kSpread,        // ...
    kQuestion,      // ?
    kNullCoalesce,  // ??
    kOptChain,      // ?.
    kBacktick,      // `

    // Operators - assignment
    kAssign,        // =
    kAddAssign,     // +=
    kSubAssign,     // -=
    kMulAssign,     // *=
    kDivAssign,     // /=
    kModAssign,     // %=
    kBitAndAssign,  // &=
    kBitOrAssign,   // |=
    kBitXorAssign,  // ^=
    kShlAssign,     // <<=
    kShrAssign,     // >>=
    kUshrAssign,    // >>>=
    kExpAssign,     // **=
    kAndAssign,     // &&=
    kOrAssign,      // ||=
    kNullishAssign, // ??=

    // Operators - arithmetic
    kPlus,          // +
    kMinus,         // -
    kStar,          // *
    kSlash,         // /
    kPercent,       // %
    kExp,           // **

    // Operators - comparison
    kEq,            // ==
    kNotEq,         // !=
    kEqEq,          // ===
    kNotEqEq,       // !==
    kLt,            // <
    kGt,            // >
    kLe,            // <=
    kGe,            // >=

    // Operators - logical
    kAnd,           // &&
    kOr,            // ||
    kNot,           // !

    // Operators - bitwise
    kBitAnd,        // &
    kBitOr,         // |
    kBitXor,        // ^
    kBitNot,        // ~
    kShl,           // <<
    kShr,           // >>
    kUshr,          // >>>

    // Operators - unary prefix
    kInc,           // ++
    kDec,           // --

    // Keywords
    kVar,
    kLet,
    kConst,
    kFunction,
    kReturn,
    kIf,
    kElse,
    kWhile,
    kFor,
    kDo,
    kBreak,
    kContinue,
    kSwitch,
    kCase,
    kDefault,
    kTrue,
    kFalse,
    kNull,
    kUndefined,
    kNew,
    kTypeof,
    kInstanceof,
    kIn,
    kOf,
    kDelete,
    kVoid,
    kTry,
    kCatch,
    kFinally,
    kThrow,
    kClass,
    kExtends,
    kSuper,
    kThis,
    kImport,
    kExport,
    kFrom,
    kAs,
    kAsync,
    kAwait,
    kYield,
    kStatic,
    kGet,
    kSet,
    kDebugger,

    kCount  // sentinel
};

const char* TokenKindName(TokenKind kind);

inline bool IsAssignmentOp(TokenKind k) {
    return k >= TokenKind::kAssign && k <= TokenKind::kNullishAssign;
}

inline bool IsBinaryOp(TokenKind k) {
    return (k >= TokenKind::kPlus && k <= TokenKind::kUshr) ||
           k == TokenKind::kInstanceof || k == TokenKind::kIn;
}

inline bool IsUnaryPrefixOp(TokenKind k) {
    return k == TokenKind::kPlus || k == TokenKind::kMinus ||
           k == TokenKind::kNot || k == TokenKind::kBitNot ||
           k == TokenKind::kTypeof || k == TokenKind::kVoid ||
           k == TokenKind::kDelete || k == TokenKind::kInc ||
           k == TokenKind::kDec || k == TokenKind::kAwait;
}

inline bool IsKeyword(TokenKind k) {
    return k >= TokenKind::kVar && k <= TokenKind::kSet;
}

inline int Precedence(TokenKind k) {
    switch (k) {
        case TokenKind::kNullCoalesce: return 1;
        case TokenKind::kOr:            return 2;
        case TokenKind::kAnd:           return 3;
        case TokenKind::kBitOr:         return 4;
        case TokenKind::kBitXor:        return 5;
        case TokenKind::kBitAnd:        return 6;
        case TokenKind::kEq: case TokenKind::kNotEq:
        case TokenKind::kEqEq: case TokenKind::kNotEqEq: return 7;
        case TokenKind::kLt: case TokenKind::kGt:
        case TokenKind::kLe: case TokenKind::kGe:
        case TokenKind::kInstanceof: case TokenKind::kIn: return 8;
        case TokenKind::kShl: case TokenKind::kShr: case TokenKind::kUshr: return 9;
        case TokenKind::kPlus: case TokenKind::kMinus: return 10;
        case TokenKind::kStar: case TokenKind::kSlash:
        case TokenKind::kPercent: return 11;
        case TokenKind::kExp: return 12;
        default: return 0;
    }
}

inline bool IsRightAssociative(TokenKind k) {
    return k == TokenKind::kExp || k == TokenKind::kAssign ||
           IsAssignmentOp(k);
}

}  // namespace v12

#endif  // V12_FRONTEND_LEXER_TOKENS_H_
