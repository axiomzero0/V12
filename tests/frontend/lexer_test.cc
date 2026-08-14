// =============================================================================
// tests/frontend/lexer_test.cc
// =============================================================================
// Tests for the lexer.

#include "tests/test-framework.h"

#include "base/arena.h"
#include "frontend/lexer/lexer.h"
#include "frontend/lexer/tokens.h"

using namespace v12;
using namespace v12_test;

namespace {
Token LexSingle(Arena* arena, std::string_view src) {
    Lexer lex(arena, src);
    return lex.Next();
}
}  // namespace

TEST(Lexer, BasicTokens) {
    Arena arena;
    EXPECT_EQ(LexSingle(&arena, "(").kind, TokenKind::kLParen);
    EXPECT_EQ(LexSingle(&arena, ")").kind, TokenKind::kRParen);
    EXPECT_EQ(LexSingle(&arena, "{").kind, TokenKind::kLBrace);
    EXPECT_EQ(LexSingle(&arena, "}").kind, TokenKind::kRBrace);
    EXPECT_EQ(LexSingle(&arena, "[").kind, TokenKind::kLBracket);
    EXPECT_EQ(LexSingle(&arena, "]").kind, TokenKind::kRBracket);
    EXPECT_EQ(LexSingle(&arena, ";").kind, TokenKind::kSemicolon);
    EXPECT_EQ(LexSingle(&arena, ",").kind, TokenKind::kComma);
    EXPECT_EQ(LexSingle(&arena, ".").kind, TokenKind::kDot);
}

TEST(Lexer, Operators) {
    Arena arena;
    EXPECT_EQ(LexSingle(&arena, "+").kind, TokenKind::kPlus);
    EXPECT_EQ(LexSingle(&arena, "-").kind, TokenKind::kMinus);
    EXPECT_EQ(LexSingle(&arena, "*").kind, TokenKind::kStar);
    EXPECT_EQ(LexSingle(&arena, "/").kind, TokenKind::kSlash);
    EXPECT_EQ(LexSingle(&arena, "==").kind, TokenKind::kEq);
    EXPECT_EQ(LexSingle(&arena, "===").kind, TokenKind::kEqEq);
    EXPECT_EQ(LexSingle(&arena, "!==").kind, TokenKind::kNotEqEq);
    EXPECT_EQ(LexSingle(&arena, "=>").kind, TokenKind::kArrow);
    EXPECT_EQ(LexSingle(&arena, "...").kind, TokenKind::kSpread);
    EXPECT_EQ(LexSingle(&arena, "**").kind, TokenKind::kExp);
    EXPECT_EQ(LexSingle(&arena, "??").kind, TokenKind::kNullCoalesce);
}

TEST(Lexer, Keywords) {
    Arena arena;
    EXPECT_EQ(LexSingle(&arena, "var").kind, TokenKind::kVar);
    EXPECT_EQ(LexSingle(&arena, "let").kind, TokenKind::kLet);
    EXPECT_EQ(LexSingle(&arena, "const").kind, TokenKind::kConst);
    EXPECT_EQ(LexSingle(&arena, "function").kind, TokenKind::kFunction);
    EXPECT_EQ(LexSingle(&arena, "if").kind, TokenKind::kIf);
    EXPECT_EQ(LexSingle(&arena, "else").kind, TokenKind::kElse);
    EXPECT_EQ(LexSingle(&arena, "while").kind, TokenKind::kWhile);
    EXPECT_EQ(LexSingle(&arena, "for").kind, TokenKind::kFor);
    EXPECT_EQ(LexSingle(&arena, "return").kind, TokenKind::kReturn);
    EXPECT_EQ(LexSingle(&arena, "true").kind, TokenKind::kTrue);
    EXPECT_EQ(LexSingle(&arena, "false").kind, TokenKind::kFalse);
    EXPECT_EQ(LexSingle(&arena, "null").kind, TokenKind::kNull);
    EXPECT_EQ(LexSingle(&arena, "undefined").kind, TokenKind::kUndefined);
}

TEST(Lexer, Identifiers) {
    Arena arena;
    Token t = LexSingle(&arena, "foo");
    EXPECT_EQ(t.kind, TokenKind::kIdentifier);
    EXPECT_EQ(t.identifier_value, "foo");

    t = LexSingle(&arena, "_bar");
    EXPECT_EQ(t.kind, TokenKind::kIdentifier);
    EXPECT_EQ(t.identifier_value, "_bar");

    t = LexSingle(&arena, "$baz");
    EXPECT_EQ(t.kind, TokenKind::kIdentifier);
    EXPECT_EQ(t.identifier_value, "$baz");

    t = LexSingle(&arena, "foo123");
    EXPECT_EQ(t.kind, TokenKind::kIdentifier);
    EXPECT_EQ(t.identifier_value, "foo123");
}

TEST(Lexer, Numbers) {
    Arena arena;
    Token t = LexSingle(&arena, "42");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 42.0);

    t = LexSingle(&arena, "3.14");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 3.14);

    t = LexSingle(&arena, "0");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 0.0);

    t = LexSingle(&arena, "1e5");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 100000.0);

    t = LexSingle(&arena, "0xFF");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 255.0);

    t = LexSingle(&arena, "0b101");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 5.0);

    t = LexSingle(&arena, "0o17");
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 15.0);
}

TEST(Lexer, Strings) {
    Arena arena;
    Token t = LexSingle(&arena, "\"hello\"");
    EXPECT_EQ(t.kind, TokenKind::kString);
    EXPECT_EQ(t.string_value, "hello");

    t = LexSingle(&arena, "'world'");
    EXPECT_EQ(t.kind, TokenKind::kString);
    EXPECT_EQ(t.string_value, "world");

    t = LexSingle(&arena, "\"hello\\nworld\"");
    EXPECT_EQ(t.kind, TokenKind::kString);
    EXPECT_EQ(t.string_value, "hello\nworld");

    t = LexSingle(&arena, "\"tab\\there\"");
    EXPECT_EQ(t.kind, TokenKind::kString);
    EXPECT_EQ(t.string_value, "tab\there");
}

TEST(Lexer, Comments) {
    Arena arena;
    Lexer lex(&arena, "// comment\n42");
    Token t = lex.Next();
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 42.0);

    lex = Lexer(&arena, "/* block comment */ 42");
    t = lex.Next();
    EXPECT_EQ(t.kind, TokenKind::kNumber);
    EXPECT_DOUBLE_EQ(t.number_value, 42.0);
}

TEST(Lexer, Eof) {
    Arena arena;
    Lexer lex(&arena, "");
    EXPECT_EQ(lex.Next().kind, TokenKind::kEof);
}

TEST(Lexer, Sequence) {
    Arena arena;
    Lexer lex(&arena, "var x = 42;");
    EXPECT_EQ(lex.Next().kind, TokenKind::kVar);
    EXPECT_EQ(lex.Next().kind, TokenKind::kIdentifier);
    EXPECT_EQ(lex.Next().kind, TokenKind::kAssign);
    EXPECT_EQ(lex.Next().kind, TokenKind::kNumber);
    EXPECT_EQ(lex.Next().kind, TokenKind::kSemicolon);
    EXPECT_EQ(lex.Next().kind, TokenKind::kEof);
}

TEST(Lexer, LineTracking) {
    Arena arena;
    Lexer lex(&arena, "var x\n= 42");
    Token t = lex.Next();   // var
    EXPECT_EQ(t.line, 1u);
    EXPECT_EQ(t.column, 1u);
    t = lex.Next();          // x
    EXPECT_EQ(t.line, 1u);
    t = lex.Next();          // =
    EXPECT_EQ(t.line, 2u);
    t = lex.Next();          // 42
    EXPECT_EQ(t.line, 2u);
}
