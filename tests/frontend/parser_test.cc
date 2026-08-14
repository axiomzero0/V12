// =============================================================================
// tests/frontend/parser_test.cc
// =============================================================================

#include "tests/test-framework.h"

#include "base/arena.h"
#include "frontend/ast/ast.h"
#include "frontend/parser/parser.h"

using namespace v12;
using namespace v12_test;

namespace {
struct ParseResult {
    std::unique_ptr<Arena> arena;
    Parser* parser = nullptr;
    Program* program = nullptr;
};

ParseResult ParseSource(std::string_view src) {
    ParseResult r;
    r.arena = std::make_unique<Arena>();
    r.parser = new Parser(r.arena.get(), src);
    r.program = r.parser->ParseProgram();
    return r;
}
}  // namespace

TEST(Parser, EmptyProgram) {
    auto r = ParseSource("");
    ASSERT_FALSE(r.parser->has_error());
}

TEST(Parser, VariableDeclaration) {
    auto r = ParseSource("var x = 42;");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kVarDecl);
}

TEST(Parser, FunctionDeclaration) {
    auto r = ParseSource("function f(a, b) { return a + b; }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kFunctionDecl);
}

TEST(Parser, IfElse) {
    auto r = ParseSource("if (true) { 1; } else { 2; }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kIf);
}

TEST(Parser, WhileLoop) {
    auto r = ParseSource("while (i < 10) { i++; }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kWhile);
}

TEST(Parser, ForLoop) {
    auto r = ParseSource("for (let i = 0; i < 10; i++) { sum += i; }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kFor);
}

TEST(Parser, BinaryExpression) {
    auto r = ParseSource("1 + 2 * 3;");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kExpressionStatement);
}

TEST(Parser, ObjectLiteral) {
    auto r = ParseSource("var o = { a: 1, b: 2, c: 3 };");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
}

TEST(Parser, ArrayLiteral) {
    auto r = ParseSource("var a = [1, 2, 3];");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
}

TEST(Parser, ArrowFunction) {
    auto r = ParseSource("var f = x => x + 1;");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
}

TEST(Parser, FunctionExpression) {
    auto r = ParseSource("var f = function() { return 42; };");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
}

TEST(Parser, ClassDeclaration) {
    auto r = ParseSource("class Foo { constructor() {} bar() {} }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kClassDecl);
}

TEST(Parser, TryCatchFinally) {
    auto r = ParseSource("try { f(); } catch (e) { g(e); } finally { h(); }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kTry);
}

TEST(Parser, SwitchStatement) {
    auto r = ParseSource("switch (x) { case 1: y(); break; default: z(); }");
    ASSERT_FALSE(r.parser->has_error());
    ASSERT_EQ(r.program->body.size(), 1u);
    EXPECT_EQ(r.program->body[0]->kind, AstKind::kSwitch);
}

TEST(Parser, ErrorReporting) {
    auto r = ParseSource("var = ;");
    EXPECT_TRUE(r.parser->has_error());
}
