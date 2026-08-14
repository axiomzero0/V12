// =============================================================================
// src/frontend/parser/parser.h
// =============================================================================
// Recursive-descent parser for the JS subset supported by the lexer.
//
// Operator precedence (lowest to highest):
//   1. comma (sequence)        - only inside parens
//   2. assignment (= += etc.)  - right-associative
//   3. conditional (?:)        - right-associative
//   4. ??                       - nullish coalescing
//   5. ||                       - logical or
//   6. &&                       - logical and
//   7. | ^ &
//   8. == != === !==
//   9. < <= > >= instanceof in
//  10. << >> >>>
//  11. + -
//  12. * / %
//  13. **                        - right-associative
//  14. unary (+ - ! ~ typeof void delete ++ --)
//  15. postfix (++ --)
//  16. call / member / new
//  17. primary
//
// We do NOT do automatic semicolon insertion in the parser. Instead:
//   - After most statements, we accept an optional `;`.
//   - If a `;` is missing AND the next token starts a new statement on a
//     new line, we treat it as ASI. This matches V8's behavior for the
//     common cases.
//   - We always require `;` after some constructs (var/let/const decls,
//     return, break, continue, throw) unless ASI applies.

#ifndef V12_FRONTEND_PARSER_PARSER_H_
#define V12_FRONTEND_PARSER_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/arena.h"
#include "base/macros.h"
#include "base/small-vector.h"
#include "frontend/ast/ast.h"
#include "frontend/lexer/lexer.h"
#include "frontend/lexer/tokens.h"

namespace v12 {

struct ParseError {
    std::string message;
    uint32_t line = 0;
    uint32_t column = 0;
    SourceRange range;
};

class Parser {
public:
    Parser(Arena* arena, std::string_view source);

    // Parse the entire source as a Program. Returns nullptr on fatal error.
    // On success, the program is owned by the parser's arena.
    Program* ParseProgram();

    bool has_error() const { return !errors_.empty(); }
    const std::vector<ParseError>& errors() const { return errors_; }

    Arena* arena() { return arena_; }

private:
    // Token helpers
    const Token& Peek() { return lexer_.Peek(); }
    const Token& Peek2() { return lexer_.Peek2(); }
    Token Next() { return lexer_.Next(); }
    bool Check(TokenKind k) { return Peek().Is(k); }
    bool Match(TokenKind k) {
        if (Peek().Is(k)) { Next(); return true; }
        return false;
    }
    Token Expect(TokenKind k, const char* msg);
    void Error(const std::string& msg, const Token& tok);
    void Error(const std::string& msg);

    // Automatic semicolon insertion
    bool ConsumeSemicolon();
    bool IsValidASIPoint();

    // Source range helpers
    SourceRange RangeFrom(const Token& start) {
        SourceRange r;
        r.start = static_cast<uint32_t>(start.start - lexer_.source().data());
        r.end = static_cast<uint32_t>(lexer_.Peek().start - lexer_.source().data());
        return r;
    }
    SourceRange RangeFromTo(const Token& start, const Token& end) {
        SourceRange r;
        r.start = static_cast<uint32_t>(start.start - lexer_.source().data());
        r.end = static_cast<uint32_t>(end.start + end.length - lexer_.source().data());
        return r;
    }
    SourceRange EmptyRangeAt(const Token& tok) {
        SourceRange r;
        r.start = r.end = static_cast<uint32_t>(tok.start - lexer_.source().data());
        return r;
    }
    // Range starting at `start` and ending at the current position.
    SourceRange RangeFromToHere(SourceRange start) {
        SourceRange r;
        r.start = start.start;
        r.end = static_cast<uint32_t>(lexer_.Peek().start - lexer_.source().data());
        return r;
    }

    // ----- Parsing functions -----
    Stmt* ParseStatement();
    Block* ParseBlock();
    Stmt* ParseVarDeclaration(TokenKind decl_kind, const Token& start_tok);
    FunctionDecl* ParseFunctionDeclaration(const Token& start_tok, bool is_async);
    ClassDecl* ParseClassDeclaration(const Token& start_tok);
    If* ParseIf(const Token& start_tok);
    While* ParseWhile(const Token& start_tok);
    DoWhile* ParseDoWhile(const Token& start_tok);
    Stmt* ParseFor(const Token& start_tok);
    Try* ParseTry(const Token& start_tok);
    Switch* ParseSwitch(const Token& start_tok);
    Labeled* ParseLabeled(std::string_view label, const Token& start_tok);
    Return* ParseReturn(const Token& start_tok);
    Throw* ParseThrow(const Token& start_tok);
    Break* ParseBreak(const Token& start_tok);
    Continue* ParseContinue(const Token& start_tok);
    Debugger* ParseDebugger(const Token& start_tok);
    ExpressionStatement* ParseExpressionStatement(Expr* expr, const Token& start_tok);

    // Expressions
    Expr* ParseExpression();        // top-level (includes comma)
    Expr* ParseAssignment();
    Expr* ParseConditional();
    Expr* ParseBinary(int min_prec);
    Expr* ParseUnary();
    Expr* ParsePostfix();
    Expr* ParseLHS();
    Expr* ParseCall(Expr* callee);
    Expr* ParseMember(Expr* object);
    Expr* ParsePrimary();
    Expr* ParseFunctionExpression(const Token& start_tok, bool is_async);
    Expr* ParseClassExpression(const Token& start_tok);
    Expr* ParseArrowFunction(SmallVector<Parameter, 4>&& params, const Token& start_tok);
    Expr* ParseArrayLiteral(const Token& start_tok);
    Expr* ParseObjectLiteral(const Token& start_tok);

    // Parameters
    void ParseParameterList(SmallVector<Parameter, 4>* params, TokenKind end_token);
    Parameter ParseParameter();

    // Class members
    void ParseClassBody(ClassExpr* cls);
    bool ParseClassMember(ClassExpr* cls, bool is_static);

    Arena* arena_;
    Lexer lexer_;
    Program* program_ = nullptr;
    std::vector<ParseError> errors_;
};

}  // namespace v12

#endif  // V12_FRONTEND_PARSER_PARSER_H_
