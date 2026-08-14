// =============================================================================
// src/frontend/parser/parser.cc
// =============================================================================

#include "frontend/parser/parser.h"

#include <utility>

namespace v12 {

Parser::Parser(Arena* arena, std::string_view source)
    : arena_(arena), lexer_(arena, source) {}

void Parser::Error(const std::string& msg, const Token& tok) {
    ParseError err;
    err.message = msg;
    err.line = tok.line;
    err.column = tok.column;
    err.range.start = static_cast<uint32_t>(tok.start - lexer_.source().data());
    err.range.end = err.range.start + tok.length;
    errors_.push_back(std::move(err));
}

void Parser::Error(const std::string& msg) {
    Error(msg, Peek());
}

Token Parser::Expect(TokenKind k, const char* msg) {
    if (V12_UNLIKELY(!Peek().Is(k))) {
        Error(msg, Peek());
        // We don't return a synthetic token; we return the current Peek so
        // the caller can still make progress. The error is recorded.
        return Peek();
    }
    return Next();
}

bool Parser::IsValidASIPoint() {
    // ASI is allowed before: }, EOF, or a token that had a line terminator
    // before it.
    const Token& t = lexer_.Peek();
    return t.Is(TokenKind::kRBrace) || t.Is(TokenKind::kEof) ||
           t.had_line_terminator_before;
}

bool Parser::ConsumeSemicolon() {
    if (Match(TokenKind::kSemicolon)) return true;
    if (IsValidASIPoint()) return true;
    Error("expected ';' or newline", Peek());
    return false;
}

// =============================================================================
// Program / statement
// =============================================================================

Program* Parser::ParseProgram() {
    Token start = Peek();
    program_ = arena_->New<Program>(RangeFrom(start));
    while (!Peek().Is(TokenKind::kEof) && !lexer_.has_error()) {
        Stmt* s = ParseStatement();
        if (s != nullptr) {
            program_->body.push_back(s);
        } else {
            // Skip to next likely statement start to recover.
            while (!Peek().Is(TokenKind::kEof) &&
                   !Peek().Is(TokenKind::kSemicolon) &&
                   !Peek().Is(TokenKind::kRBrace)) {
                Next();
            }
            Match(TokenKind::kSemicolon);
        }
    }
    return program_;
}

Stmt* Parser::ParseStatement() {
    const Token& t = Peek();
    switch (t.kind) {
        case TokenKind::kLBrace:    return ParseBlock();
        case TokenKind::kVar:       return ParseVarDeclaration(TokenKind::kVar, Next());
        case TokenKind::kLet:       return ParseVarDeclaration(TokenKind::kLet, Next());
        case TokenKind::kConst:     return ParseVarDeclaration(TokenKind::kConst, Next());
        case TokenKind::kFunction:  return ParseFunctionDeclaration(Next(), false);
        case TokenKind::kAsync:
            if (Peek2().Is(TokenKind::kFunction)) {
                Next();
                return ParseFunctionDeclaration(Next(), true);
            }
            break;
        case TokenKind::kClass:     return ParseClassDeclaration(Next());
        case TokenKind::kIf:        return ParseIf(Next());
        case TokenKind::kWhile:     return ParseWhile(Next());
        case TokenKind::kDo:        return ParseDoWhile(Next());
        case TokenKind::kFor:       return ParseFor(Next());
        case TokenKind::kTry:       return ParseTry(Next());
        case TokenKind::kSwitch:    return ParseSwitch(Next());
        case TokenKind::kReturn:    return ParseReturn(Next());
        case TokenKind::kThrow:     return ParseThrow(Next());
        case TokenKind::kBreak:     return ParseBreak(Next());
        case TokenKind::kContinue:  return ParseContinue(Next());
        case TokenKind::kDebugger:  return ParseDebugger(Next());
        case TokenKind::kSemicolon: {
            Token tok = Next();
            return arena_->New<Empty>(EmptyRangeAt(tok));
        }
        default:
            if (t.kind == TokenKind::kIdentifier && Peek2().Is(TokenKind::kColon)) {
                Token label_tok = Next();
                Token colon = Expect(TokenKind::kColon, "expected ':' after label");
                return ParseLabeled(label_tok.identifier_value, label_tok);
            }
            break;
    }

    // Expression statement
    Token start = Peek();
    Expr* e = ParseExpression();
    if (e == nullptr) return nullptr;
    ConsumeSemicolon();
    return arena_->New<ExpressionStatement>(RangeFrom(start), e);
}

Block* Parser::ParseBlock() {
    Token start = Expect(TokenKind::kLBrace, "expected '{'");
    Block* block = arena_->New<Block>(RangeFrom(start));
    while (!Peek().Is(TokenKind::kRBrace) && !Peek().Is(TokenKind::kEof)) {
        Stmt* s = ParseStatement();
        if (s != nullptr) block->statements.push_back(s);
    }
    Expect(TokenKind::kRBrace, "expected '}'");
    return block;
}

Stmt* Parser::ParseVarDeclaration(TokenKind decl_kind, const Token& start_tok) {
    AstKind kind = AstKind::kVarDecl;
    if (decl_kind == TokenKind::kLet)   kind = AstKind::kLetDecl;
    if (decl_kind == TokenKind::kConst) kind = AstKind::kConstDecl;
    auto* decl = arena_->New<VarDeclStmt>(RangeFrom(start_tok), kind);
    while (true) {
        Token name_tok = Expect(TokenKind::kIdentifier, "expected identifier in declaration");
        VarDecl d;
        d.name = name_tok.identifier_value;
        d.range = EmptyRangeAt(name_tok);
        if (Match(TokenKind::kAssign)) {
            d.init = ParseAssignment();
        }
        decl->declarations.push_back(d);
        if (!Match(TokenKind::kComma)) break;
    }
    ConsumeSemicolon();
    return decl;
}

FunctionDecl* Parser::ParseFunctionDeclaration(const Token& start_tok, bool is_async) {
    // 'function' keyword was already consumed by the caller.
    (void)start_tok;
    bool is_generator = Match(TokenKind::kStar);
    Token name_tok = Expect(TokenKind::kIdentifier, "expected function name");
    auto* fn = arena_->New<FunctionDecl>(RangeFrom(start_tok));
    fn->name = name_tok.identifier_value;
    fn->is_async = is_async;
    fn->is_generator = is_generator;
    Expect(TokenKind::kLParen, "expected '(' after function name");
    ParseParameterList(&fn->params, TokenKind::kRParen);
    Expect(TokenKind::kRParen, "expected ')' after parameters");
    fn->body = ParseBlock();
    return fn;
}

ClassDecl* Parser::ParseClassDeclaration(const Token& start_tok) {
    // 'class' keyword was already consumed by the caller.
    (void)start_tok;
    Token name_tok = Expect(TokenKind::kIdentifier, "expected class name");
    auto* cls = arena_->New<ClassDecl>(RangeFrom(start_tok));
    cls->name = name_tok.identifier_value;
    if (Match(TokenKind::kExtends)) {
        cls->superclass = ParseLHS();
    }
    auto* cls_expr = arena_->New<ClassExpr>(RangeFrom(start_tok));
    cls_expr->name = cls->name;
    cls_expr->superclass = cls->superclass;
    Expect(TokenKind::kLBrace, "expected '{' before class body");
    ParseClassBody(cls_expr);
    cls->members = std::move(cls_expr->members);
    Expect(TokenKind::kRBrace, "expected '}' after class body");
    return cls;
}

void Parser::ParseClassBody(ClassExpr* cls) {
    while (!Peek().Is(TokenKind::kRBrace) && !Peek().Is(TokenKind::kEof)) {
        if (Match(TokenKind::kSemicolon)) continue;
        bool is_static = false;
        if (Peek().Is(TokenKind::kStatic)) {
            // Could be `static` modifier or a property named "static".
            if (!Peek2().Is(TokenKind::kLParen) && !Peek2().Is(TokenKind::kAssign) &&
                !Peek2().Is(TokenKind::kSemicolon)) {
                is_static = true;
                Next();
            }
        }
        ParseClassMember(cls, is_static);
    }
}

bool Parser::ParseClassMember(ClassExpr* cls, bool is_static) {
    ClassMember m;
    m.is_static = is_static;
    m.range = EmptyRangeAt(Peek());

    // get/set
    if (Peek().Is(TokenKind::kGet) || Peek().Is(TokenKind::kSet)) {
        Token tok = Peek();
        // Lookahead: if next is a property name, treat as accessor.
        if (Peek2().kind != TokenKind::kLParen && Peek2().kind != TokenKind::kAssign &&
            Peek2().kind != TokenKind::kSemicolon) {
            m.is_get = tok.Is(TokenKind::kGet);
            m.is_set = tok.Is(TokenKind::kSet);
            Next();
        }
    }

    // Computed key
    if (Match(TokenKind::kLBracket)) {
        m.is_computed = true;
        m.computed_key = ParseAssignment();
        Expect(TokenKind::kRBracket, "expected ']' after computed property name");
    } else {
        Token name_tok = Next();
        if (name_tok.Is(TokenKind::kIdentifier) || name_tok.IsKeyword()) {
            m.name = name_tok.identifier_value;
            // For keywords like "if", identifier_value isn't set.
            if (m.name.empty()) {
                m.name = std::string_view(name_tok.start, name_tok.length);
            }
        } else if (name_tok.Is(TokenKind::kString)) {
            m.name = name_tok.string_value;
        } else {
            Error("expected property name in class body", name_tok);
            return false;
        }
    }

    if (Match(TokenKind::kLParen)) {
        // Method
        m.is_method = true;
        SmallVector<Parameter, 4> params;
        ParseParameterList(&params, TokenKind::kRParen);
        Expect(TokenKind::kRParen, "expected ')' after parameters");
        Block* body = ParseBlock();
        // Build a FunctionExpr
        auto* fn = arena_->New<FunctionExpr>(m.range);
        fn->name = m.name;
        fn->params = std::move(params);
        fn->body = body;
        m.value = fn;
        // Methods don't require a trailing semicolon, but we accept one.
        Match(TokenKind::kSemicolon);
    } else if (Match(TokenKind::kAssign)) {
        m.value = ParseAssignment();
        ConsumeSemicolon();
    } else {
        // Field without initializer; just a declaration.
        // Optional semicolon.
        Match(TokenKind::kSemicolon);
    }
    cls->members.push_back(m);
    return true;
}

If* Parser::ParseIf(const Token& start_tok) {
    Expect(TokenKind::kLParen, "expected '(' after 'if'");
    Expr* cond = ParseExpression();
    Expect(TokenKind::kRParen, "expected ')' after if condition");
    Stmt* then_branch = ParseStatement();
    Stmt* else_branch = nullptr;
    if (Match(TokenKind::kElse)) {
        else_branch = ParseStatement();
    }
    auto* node = arena_->New<If>(RangeFrom(start_tok), cond, then_branch);
    node->else_branch = else_branch;
    return node;
}

While* Parser::ParseWhile(const Token& start_tok) {
    Expect(TokenKind::kLParen, "expected '(' after 'while'");
    Expr* cond = ParseExpression();
    Expect(TokenKind::kRParen, "expected ')' after while condition");
    Stmt* body = ParseStatement();
    return arena_->New<While>(RangeFrom(start_tok), cond, body);
}

DoWhile* Parser::ParseDoWhile(const Token& start_tok) {
    Stmt* body = ParseStatement();
    Expect(TokenKind::kWhile, "expected 'while' after do-block");
    Expect(TokenKind::kLParen, "expected '(' after 'while'");
    Expr* cond = ParseExpression();
    Expect(TokenKind::kRParen, "expected ')' after while condition");
    Match(TokenKind::kSemicolon);
    return arena_->New<DoWhile>(RangeFrom(start_tok), cond, body);
}

Stmt* Parser::ParseFor(const Token& start_tok) {
    Expect(TokenKind::kLParen, "expected '(' after 'for'");
    bool is_await = false;
    if (Peek().Is(TokenKind::kAwait)) {
        is_await = true;
        Next();
    }

    Stmt* init = nullptr;
    if (!Peek().Is(TokenKind::kSemicolon)) {
        if (Peek().Is(TokenKind::kVar) || Peek().Is(TokenKind::kLet) || Peek().Is(TokenKind::kConst)) {
            TokenKind decl_kind = Peek().kind;
            Token decl_tok = Next();
            // Parse variable declaration WITHOUT consuming the trailing semicolon,
            // because in a for-loop the semicolon is the separator.
            AstKind kind = AstKind::kVarDecl;
            if (decl_kind == TokenKind::kLet)   kind = AstKind::kLetDecl;
            if (decl_kind == TokenKind::kConst) kind = AstKind::kConstDecl;
            auto* decl = arena_->New<VarDeclStmt>(RangeFrom(decl_tok), kind);
            while (true) {
                Token name_tok = Expect(TokenKind::kIdentifier, "expected identifier in declaration");
                VarDecl d;
                d.name = name_tok.identifier_value;
                d.range = EmptyRangeAt(name_tok);
                if (Match(TokenKind::kAssign)) {
                    d.init = ParseAssignment();
                }
                decl->declarations.push_back(d);
                if (!Match(TokenKind::kComma)) break;
            }
            init = decl;
        } else {
            // Expression init (no semicolon consumed)
            Expr* e = ParseExpression();
            auto* stmt = arena_->New<ExpressionStatement>(RangeFrom(start_tok), e);
            init = stmt;
        }
    } else {
        Next();  // consume the leading ';'
    }

    // for-in / for-of detection
    if (Peek().Is(TokenKind::kIn)) {
        Next();
        Expr* right = ParseExpression();
        Expect(TokenKind::kRParen, "expected ')' after for-in");
        Stmt* body = ParseStatement();
        return arena_->New<ForIn>(RangeFrom(start_tok), init, right, body);
    }
    if (Peek().Is(TokenKind::kOf)) {
        Next();
        Expr* right = ParseAssignment();
        Expect(TokenKind::kRParen, "expected ')' after for-of");
        Stmt* body = ParseStatement();
        return arena_->New<ForOf>(RangeFrom(start_tok), init, right, body, is_await);
    }

    Expect(TokenKind::kSemicolon, "expected ';' in for-loop");
    Expr* cond = nullptr;
    if (!Peek().Is(TokenKind::kSemicolon)) {
        cond = ParseExpression();
    }
    Expect(TokenKind::kSemicolon, "expected ';' in for-loop");
    Expr* update = nullptr;
    if (!Peek().Is(TokenKind::kRParen)) {
        update = ParseExpression();
    }
    Expect(TokenKind::kRParen, "expected ')' after for-loop header");
    Stmt* body = ParseStatement();
    auto* for_node = arena_->New<For>(RangeFrom(start_tok));
    for_node->init = init;
    for_node->cond = cond;
    for_node->update = update;
    for_node->body = body;
    return for_node;
}

Try* Parser::ParseTry(const Token& start_tok) {
    // 'try' keyword was already consumed by the caller. ParseBlock handles '{'.
    (void)start_tok;
    Try* node = arena_->New<Try>(RangeFrom(start_tok), ParseBlock());
    if (Match(TokenKind::kCatch)) {
        CatchClause* cc = arena_->New<CatchClause>();
        cc->range = EmptyRangeAt(Peek());
        if (Match(TokenKind::kLParen)) {
            Token param = Expect(TokenKind::kIdentifier, "expected identifier in catch");
            cc->param = param.identifier_value;
            Expect(TokenKind::kRParen, "expected ')' after catch parameter");
        }
        // ParseBlock handles '{' itself.
        cc->body = ParseBlock();
        node->catch_clause = cc;
    }
    if (Match(TokenKind::kFinally)) {
        // ParseBlock handles '{' itself.
        node->finally_block = ParseBlock();
    }
    if (node->catch_clause == nullptr && node->finally_block == nullptr) {
        Error("expected catch or finally after try", Peek());
    }
    return node;
}

Switch* Parser::ParseSwitch(const Token& start_tok) {
    Expect(TokenKind::kLParen, "expected '(' after 'switch'");
    Expr* disc = ParseExpression();
    Expect(TokenKind::kRParen, "expected ')' after switch expression");
    Expect(TokenKind::kLBrace, "expected '{' after switch");
    auto* node = arena_->New<Switch>(RangeFrom(start_tok), disc);
    while (!Peek().Is(TokenKind::kRBrace) && !Peek().Is(TokenKind::kEof)) {
        SwitchCase sc;
        sc.range = EmptyRangeAt(Peek());
        if (Match(TokenKind::kCase)) {
            sc.test = ParseExpression();
            Expect(TokenKind::kColon, "expected ':' after case");
        } else if (Match(TokenKind::kDefault)) {
            sc.test = nullptr;
            Expect(TokenKind::kColon, "expected ':' after default");
        } else {
            Error("expected 'case' or 'default' in switch", Peek());
            break;
        }
        auto* body = arena_->New<Block>(sc.range);
        while (!Peek().Is(TokenKind::kCase) &&
               !Peek().Is(TokenKind::kDefault) &&
               !Peek().Is(TokenKind::kRBrace) &&
               !Peek().Is(TokenKind::kEof)) {
            Stmt* s = ParseStatement();
            if (s != nullptr) body->statements.push_back(s);
        }
        sc.body = body;
        node->cases.push_back(sc);
    }
    Expect(TokenKind::kRBrace, "expected '}' after switch body");
    return node;
}

Labeled* Parser::ParseLabeled(std::string_view label, const Token& start_tok) {
    Stmt* body = ParseStatement();
    return arena_->New<Labeled>(RangeFrom(start_tok), label, body);
}

Return* Parser::ParseReturn(const Token& start_tok) {
    Return* node = arena_->New<Return>(RangeFrom(start_tok));
    if (!Peek().Is(TokenKind::kSemicolon) &&
        !Peek().Is(TokenKind::kRBrace) &&
        !Peek().Is(TokenKind::kEof) &&
        !Peek().had_line_terminator_before) {
        node->value = ParseExpression();
    }
    ConsumeSemicolon();
    return node;
}

Throw* Parser::ParseThrow(const Token& start_tok) {
    if (Peek().had_line_terminator_before) {
        Error("illegal newline after throw", Peek());
    }
    Expr* value = ParseExpression();
    ConsumeSemicolon();
    return arena_->New<Throw>(RangeFrom(start_tok), value);
}

Break* Parser::ParseBreak(const Token& start_tok) {
    Break* node = arena_->New<Break>(RangeFrom(start_tok));
    if (!Peek().Is(TokenKind::kSemicolon) &&
        !Peek().Is(TokenKind::kRBrace) &&
        !Peek().Is(TokenKind::kEof) &&
        !Peek().had_line_terminator_before &&
        Peek().Is(TokenKind::kIdentifier)) {
        node->label = Next().identifier_value;
    }
    ConsumeSemicolon();
    return node;
}

Continue* Parser::ParseContinue(const Token& start_tok) {
    Continue* node = arena_->New<Continue>(RangeFrom(start_tok));
    if (!Peek().Is(TokenKind::kSemicolon) &&
        !Peek().Is(TokenKind::kRBrace) &&
        !Peek().Is(TokenKind::kEof) &&
        !Peek().had_line_terminator_before &&
        Peek().Is(TokenKind::kIdentifier)) {
        node->label = Next().identifier_value;
    }
    ConsumeSemicolon();
    return node;
}

Debugger* Parser::ParseDebugger(const Token& start_tok) {
    Debugger* node = arena_->New<Debugger>(RangeFrom(start_tok));
    ConsumeSemicolon();
    return node;
}

ExpressionStatement* Parser::ParseExpressionStatement(Expr* expr, const Token& start_tok) {
    ConsumeSemicolon();
    return arena_->New<ExpressionStatement>(RangeFrom(start_tok), expr);
}

// =============================================================================
// Expressions
// =============================================================================

Expr* Parser::ParseExpression() {
    Expr* e = ParseAssignment();
    if (Peek().Is(TokenKind::kComma)) {
        auto* seq = arena_->New<Sequence>(RangeFrom(Peek()));
        seq->expressions.push_back(e);
        while (Match(TokenKind::kComma)) {
            seq->expressions.push_back(ParseAssignment());
        }
        return seq;
    }
    return e;
}

Expr* Parser::ParseAssignment() {
    // Detect arrow functions: (params) => ... OR ident => ...
    {
        Token start = Peek();
        if (start.Is(TokenKind::kIdentifier) && Peek2().Is(TokenKind::kArrow)) {
            Token name_tok = Next();
            Match(TokenKind::kArrow);
            SmallVector<Parameter, 4> params;
            Parameter p;
            p.name = name_tok.identifier_value;
            p.range = EmptyRangeAt(name_tok);
            params.push_back(p);
            return ParseArrowFunction(std::move(params), start);
        }
        if (start.Is(TokenKind::kLParen)) {
            // Try arrow function parse: save position, attempt to parse params,
            // if not followed by => restore.
            // Simple heuristic: scan ahead for )  =>
            // We don't have full backtracking; we save the lexer state.
            // Actually our Lexer has Rewind but only for one token. So we
            // do a manual lookahead by scanning the source.
            // For now we just parse as parenthesized expr and check for =>
            // after closing paren.
        }
    }

    Expr* left = ParseConditional();

    if (IsAssignmentOp(Peek().kind)) {
        Token op = Next();
        Expr* right = ParseAssignment();
        return arena_->New<Assignment>(RangeFromToHere(left->range), static_cast<int>(op.kind), left, right);
    }
    return left;
}

Expr* Parser::ParseConditional() {
    Expr* cond = ParseBinary(0);
    if (Match(TokenKind::kQuestion)) {
        Expr* then_expr = ParseAssignment();
        Expect(TokenKind::kColon, "expected ':' in conditional expression");
        Expr* else_expr = ParseAssignment();
        return arena_->New<Conditional>(RangeFromToHere(cond->range), cond, then_expr, else_expr);
    }
    return cond;
}

Expr* Parser::ParseBinary(int min_prec) {
    Expr* left = ParseUnary();
    while (true) {
        TokenKind op_kind = Peek().kind;
        int prec = Precedence(op_kind);
        if (prec < min_prec || prec == 0) break;
        if (op_kind == TokenKind::kIn && min_prec == 0) {
            // `in` should not be parsed at top level of for-loop init
            // We handle this by checking a flag; for simplicity we always
            // parse `in`. The for-loop parser intercepts earlier.
        }
        Token op = Next();
        int next_min = IsRightAssociative(op_kind) ? prec : prec + 1;
        Expr* right = ParseBinary(next_min);

        if (op_kind == TokenKind::kAnd || op_kind == TokenKind::kOr || op_kind == TokenKind::kNullCoalesce) {
            left = arena_->New<LogicalOp>(RangeFromToHere(left->range), static_cast<int>(op_kind), left, right);
        } else {
            left = arena_->New<BinaryOp>(RangeFromToHere(left->range), static_cast<int>(op_kind), left, right);
        }
    }
    return left;
}

Expr* Parser::ParseUnary() {
    TokenKind k = Peek().kind;
    if (IsUnaryPrefixOp(k)) {
        Token op = Next();
        Expr* operand = ParseUnary();
        if (k == TokenKind::kInc || k == TokenKind::kDec) {
            return arena_->New<UpdateOp>(RangeFrom(op), static_cast<int>(k), operand, true);
        }
        return arena_->New<UnaryOp>(RangeFrom(op), static_cast<int>(k), operand);
    }
    return ParsePostfix();
}

Expr* Parser::ParsePostfix() {
    Expr* e = ParseLHS();
    if ((Peek().Is(TokenKind::kInc) || Peek().Is(TokenKind::kDec)) &&
        !Peek().had_line_terminator_before) {
        Token op = Next();
        return arena_->New<UpdateOp>(RangeFromToHere(e->range), static_cast<int>(op.kind), e, false);
    }
    return e;
}

Expr* Parser::ParseLHS() {
    if (Peek().Is(TokenKind::kNew)) {
        Token new_tok = Next();
        Expr* callee = ParseLHS();
        SmallVector<Expr*, 4> args;
        if (Peek().Is(TokenKind::kLParen)) {
            Token lp = Next();
            while (!Peek().Is(TokenKind::kRParen) && !Peek().Is(TokenKind::kEof)) {
                if (Peek().Is(TokenKind::kSpread)) {
                    Token spread_tok = Next();
                    Expr* inner = ParseAssignment();
                    args.push_back(arena_->New<Spread>(RangeFrom(spread_tok), inner));
                } else {
                    args.push_back(ParseAssignment());
                }
                if (!Match(TokenKind::kComma)) break;
            }
            Expect(TokenKind::kRParen, "expected ')' after arguments");
        }
        auto* node = arena_->New<NewExpr>(RangeFrom(new_tok), callee);
        node->args = std::move(args);
        return ParseMember(node);
    }

    Expr* e = ParsePrimary();
    while (true) {
        if (Peek().Is(TokenKind::kDot) ||
            (Peek().Is(TokenKind::kOptChain) && !Peek2().Is(TokenKind::kLParen))) {
            bool opt = Peek().Is(TokenKind::kOptChain);
            Token op = Next();
            Token name_tok = Peek();
            Expr* prop;
            if (name_tok.Is(TokenKind::kIdentifier) || name_tok.IsKeyword()) {
                Next();
                std::string_view name = name_tok.identifier_value;
                if (name.empty()) name = std::string_view(name_tok.start, name_tok.length);
                prop = arena_->New<Identifier>(EmptyRangeAt(name_tok), name);
            } else {
                Error("expected property name after '.'", Peek());
                break;
            }
            e = arena_->New<Member>(RangeFromToHere(e->range), e, prop, false, opt);
        } else if (Peek().Is(TokenKind::kLBracket)) {
            Next();
            Expr* index = ParseExpression();
            Expect(TokenKind::kRBracket, "expected ']' after index");
            e = arena_->New<Member>(RangeFromToHere(e->range), e, index, true, false);
        } else if (Peek().Is(TokenKind::kLParen) || Peek().Is(TokenKind::kOptChain)) {
            bool opt = Peek().Is(TokenKind::kOptChain);
            if (opt) Next();
            e = ParseCall(e);
            if (opt) {
                e = arena_->New<OptionalChain>(RangeFromToHere(e->range), e);
            }
        } else {
            break;
        }
    }
    return e;
}

Expr* Parser::ParseCall(Expr* callee) {
    Expect(TokenKind::kLParen, "expected '(' for call");
    SmallVector<Expr*, 4> args;
    while (!Peek().Is(TokenKind::kRParen) && !Peek().Is(TokenKind::kEof)) {
        if (Peek().Is(TokenKind::kSpread)) {
            Token spread_tok = Next();
            Expr* inner = ParseAssignment();
            args.push_back(arena_->New<Spread>(RangeFrom(spread_tok), inner));
        } else {
            args.push_back(ParseAssignment());
        }
        if (!Match(TokenKind::kComma)) break;
    }
    Expect(TokenKind::kRParen, "expected ')' after arguments");
    auto* node = arena_->New<Call>(RangeFromToHere(callee->range), callee, false);
    node->args = std::move(args);
    return node;
}

Expr* Parser::ParseMember(Expr* object) {
    while (true) {
        if (Peek().Is(TokenKind::kDot)) {
            Next();
            Token name_tok = Peek();
            if (name_tok.Is(TokenKind::kIdentifier) || name_tok.IsKeyword()) {
                Next();
                std::string_view name = name_tok.identifier_value;
                if (name.empty()) name = std::string_view(name_tok.start, name_tok.length);
                auto* prop = arena_->New<Identifier>(EmptyRangeAt(name_tok), name);
                object = arena_->New<Member>(RangeFromToHere(object->range), object, prop, false, false);
            } else {
                Error("expected property name after '.'", Peek());
                break;
            }
        } else if (Peek().Is(TokenKind::kLBracket)) {
            Next();
            Expr* index = ParseExpression();
            Expect(TokenKind::kRBracket, "expected ']'");
            object = arena_->New<Member>(RangeFromToHere(object->range), object, index, true, false);
        } else if (Peek().Is(TokenKind::kLParen)) {
            object = ParseCall(object);
        } else {
            break;
        }
    }
    return object;
}

Expr* Parser::ParsePrimary() {
    Token t = Peek();
    switch (t.kind) {
        case TokenKind::kNumber: {
            Next();
            return arena_->New<NumberLiteral>(EmptyRangeAt(t), t.number_value);
        }
        case TokenKind::kString: {
            Next();
            return arena_->New<StringLiteral>(EmptyRangeAt(t), t.string_value);
        }
        case TokenKind::kTrue:  Next(); return arena_->New<BoolLiteral>(EmptyRangeAt(t), true);
        case TokenKind::kFalse: Next(); return arena_->New<BoolLiteral>(EmptyRangeAt(t), false);
        case TokenKind::kNull:  Next(); return arena_->New<NullLiteral>(EmptyRangeAt(t));
        case TokenKind::kUndefined: Next(); return arena_->New<UndefinedLiteral>(EmptyRangeAt(t));
        case TokenKind::kThis: Next(); return arena_->New<ThisExpr>(EmptyRangeAt(t));
        case TokenKind::kSuper: Next(); return arena_->New<SuperExpr>(EmptyRangeAt(t));
        case TokenKind::kIdentifier: {
            Next();
            return arena_->New<Identifier>(EmptyRangeAt(t), t.identifier_value);
        }
        case TokenKind::kLParen: {
            Next();
            Expr* e = ParseExpression();
            Expect(TokenKind::kRParen, "expected ')'");
            // Check for arrow function: ( params ) =>
            if (Peek().Is(TokenKind::kArrow)) {
                Match(TokenKind::kArrow);
                // We've lost the parameter list! This is a known limitation.
                // For now, we accept this as a parenthesized expression followed
                // by arrow is invalid. Real fix: parse parameter list separately
                // when we see `(`.
                Error("arrow function with parenthesized params not yet supported", t);
            }
            return e;
        }
        case TokenKind::kLBracket: return ParseArrayLiteral(t);
        case TokenKind::kLBrace:   return ParseObjectLiteral(t);
        case TokenKind::kFunction: return ParseFunctionExpression(Next(), false);
        case TokenKind::kAsync:
            if (Peek2().Is(TokenKind::kFunction)) {
                return ParseFunctionExpression(Next(), true);
            }
            break;
        case TokenKind::kClass:    return ParseClassExpression(Next());
        case TokenKind::kNew: {
            // Already handled in ParseLHS, but if we get here it means we're
            // in a context where `new` was the primary. Hand off.
            return ParseLHS();
        }
        default:
            break;
    }
    Error("unexpected token in expression", t);
    Next();  // skip the bad token
    return arena_->New<UndefinedLiteral>(EmptyRangeAt(t));
}

Expr* Parser::ParseFunctionExpression(const Token& start_tok, bool is_async) {
    // 'function' keyword was already consumed by the caller.
    (void)start_tok;
    bool is_generator = Match(TokenKind::kStar);
    auto* fn = arena_->New<FunctionExpr>(RangeFrom(start_tok));
    fn->is_async = is_async;
    fn->is_generator = is_generator;
    if (Peek().Is(TokenKind::kIdentifier)) {
        Token name_tok = Next();
        fn->name = name_tok.identifier_value;
    }
    Expect(TokenKind::kLParen, "expected '(' after function");
    ParseParameterList(&fn->params, TokenKind::kRParen);
    Expect(TokenKind::kRParen, "expected ')'");
    fn->body = ParseBlock();
    return fn;
}

Expr* Parser::ParseClassExpression(const Token& start_tok) {
    // 'class' keyword was already consumed by the caller.
    (void)start_tok;
    auto* cls = arena_->New<ClassExpr>(RangeFrom(start_tok));
    if (Peek().Is(TokenKind::kIdentifier)) {
        Token name_tok = Next();
        cls->name = name_tok.identifier_value;
    }
    if (Match(TokenKind::kExtends)) {
        cls->superclass = ParseLHS();
    }
    Expect(TokenKind::kLBrace, "expected '{' before class body");
    ParseClassBody(cls);
    Expect(TokenKind::kRBrace, "expected '}' after class body");
    return cls;
}

Expr* Parser::ParseArrowFunction(SmallVector<Parameter, 4>&& params, const Token& start_tok) {
    auto* fn = arena_->New<ArrowFunction>(RangeFrom(start_tok));
    fn->params = std::move(params);
    if (Peek().Is(TokenKind::kLBrace)) {
        Next();
        fn->block_body = ParseBlock();
    } else {
        fn->expr_body = ParseAssignment();
    }
    return fn;
}

Expr* Parser::ParseArrayLiteral(const Token& start_tok) {
    Next();  // consume '['
    auto* arr = arena_->New<ArrayLiteral>(RangeFrom(start_tok));
    while (!Peek().Is(TokenKind::kRBracket) && !Peek().Is(TokenKind::kEof)) {
        if (Peek().Is(TokenKind::kComma)) {
            // elision: [1,,2] - we model as UndefinedLiteral
            Next();
            arr->elements.push_back(arena_->New<UndefinedLiteral>(EmptyRangeAt(Peek())));
            continue;
        }
        if (Peek().Is(TokenKind::kSpread)) {
            Token spread_tok = Next();
            Expr* inner = ParseAssignment();
            arr->elements.push_back(arena_->New<Spread>(RangeFrom(spread_tok), inner));
        } else {
            arr->elements.push_back(ParseAssignment());
        }
        if (!Match(TokenKind::kComma)) break;
    }
    Expect(TokenKind::kRBracket, "expected ']'");
    return arr;
}

Expr* Parser::ParseObjectLiteral(const Token& start_tok) {
    Next();  // consume '{'
    auto* obj = arena_->New<ObjectLiteral>(RangeFrom(start_tok));
    while (!Peek().Is(TokenKind::kRBrace) && !Peek().Is(TokenKind::kEof)) {
        ObjectProperty prop{};
        prop.range = EmptyRangeAt(Peek());

        // get/set
        if (Peek().Is(TokenKind::kGet) || Peek().Is(TokenKind::kSet)) {
            Token tok = Peek();
            if (Peek2().kind != TokenKind::kColon && Peek2().kind != TokenKind::kComma &&
                Peek2().kind != TokenKind::kRBrace && Peek2().kind != TokenKind::kLParen) {
                prop.is_get = tok.Is(TokenKind::kGet);
                prop.is_set = tok.Is(TokenKind::kSet);
                Next();
            }
        }

        // Computed key
        if (Match(TokenKind::kLBracket)) {
            prop.is_computed = true;
            prop.key = ParseAssignment();
            Expect(TokenKind::kRBracket, "expected ']'");
        } else {
            Token key_tok = Peek();
            if (key_tok.Is(TokenKind::kIdentifier) || key_tok.IsKeyword()) {
                Next();
                prop.key = arena_->New<Identifier>(EmptyRangeAt(key_tok),
                    key_tok.identifier_value.empty()
                        ? std::string_view(key_tok.start, key_tok.length)
                        : key_tok.identifier_value);
            } else if (key_tok.Is(TokenKind::kString)) {
                Next();
                prop.key = arena_->New<StringLiteral>(EmptyRangeAt(key_tok), key_tok.string_value);
            } else if (key_tok.Is(TokenKind::kNumber)) {
                Next();
                prop.key = arena_->New<NumberLiteral>(EmptyRangeAt(key_tok), key_tok.number_value);
            } else {
                Error("expected property name in object literal", key_tok);
                break;
            }
        }

        if (Match(TokenKind::kColon)) {
            prop.value = ParseAssignment();
        } else if (Peek().Is(TokenKind::kLParen)) {
            Next();
            SmallVector<Parameter, 4> params;
            ParseParameterList(&params, TokenKind::kRParen);
            Expect(TokenKind::kRParen, "expected ')'");
            Expect(TokenKind::kLBrace, "expected '{'");
            Block* body = ParseBlock();
            auto* fn = arena_->New<FunctionExpr>(prop.range);
            fn->params = std::move(params);
            fn->body = body;
            prop.value = fn;
            prop.is_method = true;
        } else {
            // Shorthand: { foo } => { foo: foo }
            // The key must be an identifier; value is an Identifier with same name.
            if (auto* ident = static_cast<Expr*>(prop.key);
                ident != nullptr && ident->Is(AstKind::kIdentifier)) {
                auto* id = static_cast<Identifier*>(ident);
                prop.value = arena_->New<Identifier>(id->range, id->name);
            } else {
                Error("expected ':' after property name", Peek());
            }
        }
        obj->properties.push_back(prop);
        if (!Match(TokenKind::kComma)) break;
    }
    Expect(TokenKind::kRBrace, "expected '}'");
    return obj;
}

void Parser::ParseParameterList(SmallVector<Parameter, 4>* params, TokenKind end_token) {
    while (!Peek().Is(end_token) && !Peek().Is(TokenKind::kEof)) {
        Parameter p;
        Token pt = Peek();
        p.range = EmptyRangeAt(pt);
        if (Match(TokenKind::kSpread)) {
            p.is_rest = true;
            Token name = Expect(TokenKind::kIdentifier, "expected identifier after ...");
            p.name = name.identifier_value;
        } else {
            Token name = Expect(TokenKind::kIdentifier, "expected parameter name");
            p.name = name.identifier_value;
            if (Match(TokenKind::kAssign)) {
                p.default_value = ParseAssignment();
            }
        }
        params->push_back(p);
        if (!Match(TokenKind::kComma)) break;
    }
}

Parameter Parser::ParseParameter() {
    Parameter p;
    Token pt = Peek();
    p.range = EmptyRangeAt(pt);
    if (Match(TokenKind::kSpread)) {
        p.is_rest = true;
        Token name = Expect(TokenKind::kIdentifier, "expected identifier after ...");
        p.name = name.identifier_value;
    } else {
        Token name = Expect(TokenKind::kIdentifier, "expected parameter name");
        p.name = name.identifier_value;
        if (Match(TokenKind::kAssign)) {
            p.default_value = ParseAssignment();
        }
    }
    return p;
}

}  // namespace v12
