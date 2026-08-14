// =============================================================================
// src/frontend/ast/ast.h
// =============================================================================
// AST node definitions.
//
// Design:
//   - All AST nodes are arena-allocated. They are not individually freed.
//   - Nodes use a tagged kind enum (no virtual functions) for fast dispatch.
//   - Each node knows its source location for diagnostics.
//   - The AST is mutable because we do several passes over it (scope
//     resolution, hoisting, etc.).
//
// Why no visitor pattern?
//   - A visitor adds a virtual call per node. For our small ASTs the overhead
//     is negligible, but the boilerplate is large. We use a switch on the
//     kind enum instead, which the compiler turns into a jump table.

#ifndef V12_FRONTEND_AST_AST_H_
#define V12_FRONTEND_AST_AST_H_

#include <cstdint>
#include <string_view>
#include <vector>

#include "base/arena.h"
#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

// Source location: a (start, end) pair of byte offsets into the source.
struct SourceRange {
    uint32_t start = 0;
    uint32_t end = 0;
};

enum class AstKind : uint8_t {
    // Expressions
    kNumberLiteral,
    kStringLiteral,
    kBoolLiteral,
    kNullLiteral,
    kUndefinedLiteral,
    kIdentifier,
    kThis,
    kSuper,
    kBinaryOp,
    kUnaryOp,
    kUpdateOp,            // ++x, x++, --x, x--
    kAssignment,
    kLogicalOp,
    kConditional,         // ?:
    kSequence,            // comma
    kCall,
    kNew,
    kMember,              // a.b or a[b]
    kOptionalChain,
    kArrayLiteral,
    kObjectLiteral,
    kFunctionExpr,
    kArrowFunction,
    kClassExpr,
    kTemplateLiteral,     // reserved for future
    kSpread,
    kYield,

    // Statements
    kBlock,
    kVarDecl,
    kLetDecl,
    kConstDecl,
    kFunctionDecl,
    kClassDecl,
    kIf,
    kWhile,
    kDoWhile,
    kFor,
    kForIn,
    kForOf,
    kBreak,
    kContinue,
    kReturn,
    kThrow,
    kTry,
    kSwitch,
    kLabeled,
    kEmpty,
    kExpressionStatement,
    kDebugger,

    // Programs / modules
    kProgram,
    kModule,
};

const char* AstKindName(AstKind kind);

// Forward declarations
struct AstNode;
struct Expr;
struct Stmt;
struct Decl;

// Base
struct AstNode {
    AstKind kind;
    SourceRange range;

    AstNode(AstKind k, SourceRange r) : kind(k), range(r) {}

    bool Is(AstKind k) const { return kind == k; }
    template <typename T> bool Is() const { return T::Matches(kind); }

    // Convenience casts - we don't use dynamic_cast because nodes are not
    // polymorphic (no vtable).
    Expr* AsExpr();
    Stmt* AsStmt();
    Decl* AsDecl();
};

// ============================================================================
// Expressions
// ============================================================================
struct Expr : public AstNode {
    using AstNode::AstNode;
    static bool Matches(AstKind k) {
        return k >= AstKind::kNumberLiteral && k <= AstKind::kYield;
    }
};

struct NumberLiteral : public Expr {
    double value;
    NumberLiteral(SourceRange r, double v) : Expr(AstKind::kNumberLiteral, r), value(v) {}
};

struct StringLiteral : public Expr {
    std::string_view value;
    StringLiteral(SourceRange r, std::string_view v)
        : Expr(AstKind::kStringLiteral, r), value(v) {}
};

struct BoolLiteral : public Expr {
    bool value;
    BoolLiteral(SourceRange r, bool v) : Expr(AstKind::kBoolLiteral, r), value(v) {}
};

struct NullLiteral : public Expr {
    explicit NullLiteral(SourceRange r) : Expr(AstKind::kNullLiteral, r) {}
};

struct UndefinedLiteral : public Expr {
    explicit UndefinedLiteral(SourceRange r) : Expr(AstKind::kUndefinedLiteral, r) {}
};

struct Identifier : public Expr {
    std::string_view name;
    Identifier(SourceRange r, std::string_view n) : Expr(AstKind::kIdentifier, r), name(n) {}
};

struct ThisExpr : public Expr {
    explicit ThisExpr(SourceRange r) : Expr(AstKind::kThis, r) {}
};

struct SuperExpr : public Expr {
    explicit SuperExpr(SourceRange r) : Expr(AstKind::kSuper, r) {}
};

struct BinaryOp : public Expr {
    int op_token;        // TokenKind as int (to avoid circular include)
    Expr* left;
    Expr* right;
    BinaryOp(SourceRange r, int op, Expr* l, Expr* r2)
        : Expr(AstKind::kBinaryOp, r), op_token(op), left(l), right(r2) {}
};

struct UnaryOp : public Expr {
    int op_token;
    Expr* operand;
    UnaryOp(SourceRange r, int op, Expr* o) : Expr(AstKind::kUnaryOp, r), op_token(op), operand(o) {}
};

struct UpdateOp : public Expr {
    int op_token;       // kInc or kDec
    Expr* operand;
    bool is_prefix;
    UpdateOp(SourceRange r, int op, Expr* o, bool prefix)
        : Expr(AstKind::kUpdateOp, r), op_token(op), operand(o), is_prefix(prefix) {}
};

struct Assignment : public Expr {
    int op_token;       // TokenKind::kAssign or compound
    Expr* target;
    Expr* value;
    Assignment(SourceRange r, int op, Expr* t, Expr* v)
        : Expr(AstKind::kAssignment, r), op_token(op), target(t), value(v) {}
};

struct LogicalOp : public Expr {
    int op_token;       // kAnd, kOr, kNullCoalesce
    Expr* left;
    Expr* right;
    LogicalOp(SourceRange r, int op, Expr* l, Expr* r2)
        : Expr(AstKind::kLogicalOp, r), op_token(op), left(l), right(r2) {}
};

struct Conditional : public Expr {
    Expr* cond;
    Expr* then_expr;
    Expr* else_expr;
    Conditional(SourceRange r, Expr* c, Expr* t, Expr* e)
        : Expr(AstKind::kConditional, r), cond(c), then_expr(t), else_expr(e) {}
};

struct Sequence : public Expr {
    SmallVector<Expr*, 2> expressions;
    explicit Sequence(SourceRange r) : Expr(AstKind::kSequence, r) {}
};

struct Call : public Expr {
    Expr* callee;
    SmallVector<Expr*, 4> args;
    bool is_optional;   // for ?.()
    Call(SourceRange r, Expr* c, bool opt)
        : Expr(AstKind::kCall, r), callee(c), is_optional(opt) {}
};

struct NewExpr : public Expr {
    Expr* callee;
    SmallVector<Expr*, 4> args;
    NewExpr(SourceRange r, Expr* c) : Expr(AstKind::kNew, r), callee(c) {}
};

struct Member : public Expr {
    Expr* object;
    Expr* property;     // for a[b] - an Identifier for a.b
    bool is_computed;   // a[b] vs a.b
    bool is_optional;   // a?.b vs a.b
    Member(SourceRange r, Expr* obj, Expr* prop, bool comp, bool opt)
        : Expr(AstKind::kMember, r), object(obj), property(prop),
          is_computed(comp), is_optional(opt) {}
};

struct OptionalChain : public Expr {
    Expr* base;
    OptionalChain(SourceRange r, Expr* b) : Expr(AstKind::kOptionalChain, r), base(b) {}
};

struct ArrayLiteral : public Expr {
    SmallVector<Expr*, 4> elements;
    explicit ArrayLiteral(SourceRange r) : Expr(AstKind::kArrayLiteral, r) {}
};

struct ObjectProperty {
    Expr* key;          // Identifier or computed expr
    Expr* value;
    bool is_computed;
    bool is_method;     // shorthand method
    bool is_get;
    bool is_set;
    SourceRange range;
};

struct ObjectLiteral : public Expr {
    SmallVector<ObjectProperty, 4> properties;
    explicit ObjectLiteral(SourceRange r) : Expr(AstKind::kObjectLiteral, r) {}
};

struct Parameter {
    std::string_view name;
    Expr* default_value = nullptr;   // for ES6 default parameters
    bool is_rest = false;
    SourceRange range;
};

struct FunctionExpr : public Expr {
    std::string_view name;          // may be empty for anonymous
    SmallVector<Parameter, 4> params;
    Stmt* body;                     // a Block
    bool is_async;
    bool is_generator;
    FunctionExpr(SourceRange r) : Expr(AstKind::kFunctionExpr, r) {}
};

struct ArrowFunction : public Expr {
    SmallVector<Parameter, 4> params;
    // body is either a Block (for { ... }) or an Expr (for x => x+1)
    Stmt* block_body = nullptr;
    Expr* expr_body = nullptr;
    bool is_async;
    ArrowFunction(SourceRange r) : Expr(AstKind::kArrowFunction, r) {}
};

struct ClassMember {
    std::string_view name;
    Expr* value = nullptr;
    bool is_static;
    bool is_method;
    bool is_get;
    bool is_set;
    bool is_computed;
    Expr* computed_key = nullptr;
    SourceRange range;
};

struct ClassExpr : public Expr {
    std::string_view name;
    Expr* superclass = nullptr;     // optional
    SmallVector<ClassMember, 4> members;
    ClassExpr(SourceRange r) : Expr(AstKind::kClassExpr, r) {}
};

struct Spread : public Expr {
    Expr* inner;
    Spread(SourceRange r, Expr* i) : Expr(AstKind::kSpread, r), inner(i) {}
};

struct Yield : public Expr {
    Expr* value = nullptr;  // null for `yield;`
    bool is_delegate;       // yield*
    Yield(SourceRange r, Expr* v, bool deleg)
        : Expr(AstKind::kYield, r), value(v), is_delegate(deleg) {}
};

// ============================================================================
// Statements
// ============================================================================
struct Stmt : public AstNode {
    using AstNode::AstNode;
    static bool Matches(AstKind k) {
        return k >= AstKind::kBlock && k <= AstKind::kDebugger;
    }
};

struct Block : public Stmt {
    SmallVector<Stmt*, 4> statements;
    explicit Block(SourceRange r) : Stmt(AstKind::kBlock, r) {}
};

struct VarDecl {
    std::string_view name;
    Expr* init = nullptr;
    SourceRange range;
};

struct VarDeclStmt : public Stmt {
    SmallVector<VarDecl, 2> declarations;
    explicit VarDeclStmt(SourceRange r, AstKind k) : Stmt(k, r) {}
};

struct FunctionDecl : public Stmt {
    std::string_view name;
    SmallVector<Parameter, 4> params;
    Stmt* body;     // a Block
    bool is_async;
    bool is_generator;
    FunctionDecl(SourceRange r) : Stmt(AstKind::kFunctionDecl, r) {}
};

struct ClassDecl : public Stmt {
    std::string_view name;
    Expr* superclass = nullptr;
    SmallVector<ClassMember, 4> members;
    ClassDecl(SourceRange r) : Stmt(AstKind::kClassDecl, r) {}
};

struct If : public Stmt {
    Expr* cond;
    Stmt* then_branch;
    Stmt* else_branch = nullptr;
    If(SourceRange r, Expr* c, Stmt* t) : Stmt(AstKind::kIf, r), cond(c), then_branch(t) {}
};

struct While : public Stmt {
    Expr* cond;
    Stmt* body;
    While(SourceRange r, Expr* c, Stmt* b) : Stmt(AstKind::kWhile, r), cond(c), body(b) {}
};

struct DoWhile : public Stmt {
    Expr* cond;
    Stmt* body;
    DoWhile(SourceRange r, Expr* c, Stmt* b) : Stmt(AstKind::kDoWhile, r), cond(c), body(b) {}
};

struct For : public Stmt {
    Stmt* init = nullptr;          // VarDeclStmt or Expr
    Expr* cond = nullptr;
    Expr* update = nullptr;
    Stmt* body;
    For(SourceRange r) : Stmt(AstKind::kFor, r) {}
};

struct ForIn : public Stmt {
    Stmt* left;        // VarDeclStmt or Expr (the loop variable)
    Expr* right;
    Stmt* body;
    ForIn(SourceRange r, Stmt* l, Expr* r2, Stmt* b)
        : Stmt(AstKind::kForIn, r), left(l), right(r2), body(b) {}
};

struct ForOf : public Stmt {
    Stmt* left;
    Expr* right;
    Stmt* body;
    bool is_await;
    ForOf(SourceRange r, Stmt* l, Expr* r2, Stmt* b, bool aw)
        : Stmt(AstKind::kForOf, r), left(l), right(r2), body(b), is_await(aw) {}
};

struct Break : public Stmt {
    std::string_view label;   // empty if unlabeled
    explicit Break(SourceRange r) : Stmt(AstKind::kBreak, r) {}
};

struct Continue : public Stmt {
    std::string_view label;
    explicit Continue(SourceRange r) : Stmt(AstKind::kContinue, r) {}
};

struct Return : public Stmt {
    Expr* value = nullptr;
    explicit Return(SourceRange r) : Stmt(AstKind::kReturn, r) {}
};

struct Throw : public Stmt {
    Expr* value;
    Throw(SourceRange r, Expr* v) : Stmt(AstKind::kThrow, r), value(v) {}
};

struct CatchClause {
    std::string_view param;     // empty if `catch {}`
    Stmt* body;                 // a Block
    SourceRange range;
};

struct Try : public Stmt {
    Stmt* block;
    CatchClause* catch_clause = nullptr;
    Stmt* finally_block = nullptr;
    Try(SourceRange r, Stmt* b) : Stmt(AstKind::kTry, r), block(b) {}
};

struct SwitchCase {
    Expr* test = nullptr;       // null for `default`
    Stmt* body;                 // a Block
    SourceRange range;
};

struct Switch : public Stmt {
    Expr* discriminant;
    SmallVector<SwitchCase, 2> cases;
    Switch(SourceRange r, Expr* d) : Stmt(AstKind::kSwitch, r), discriminant(d) {}
};

struct Labeled : public Stmt {
    std::string_view label;
    Stmt* body;
    Labeled(SourceRange r, std::string_view l, Stmt* b)
        : Stmt(AstKind::kLabeled, r), label(l), body(b) {}
};

struct Empty : public Stmt {
    explicit Empty(SourceRange r) : Stmt(AstKind::kEmpty, r) {}
};

struct ExpressionStatement : public Stmt {
    Expr* expr;
    ExpressionStatement(SourceRange r, Expr* e) : Stmt(AstKind::kExpressionStatement, r), expr(e) {}
};

struct Debugger : public Stmt {
    explicit Debugger(SourceRange r) : Stmt(AstKind::kDebugger, r) {}
};

// ============================================================================
// Top-level
// ============================================================================
struct Program : public AstNode {
    SmallVector<Stmt*, 8> body;
    bool strict;
    explicit Program(SourceRange r) : AstNode(AstKind::kProgram, r), strict(false) {}
};

// Convenience inline casts
inline Expr* AstNode::AsExpr() {
    V12_DCHECK(Expr::Matches(kind), "AsExpr on non-expression");
    return static_cast<Expr*>(this);
}
inline Stmt* AstNode::AsStmt() {
    V12_DCHECK(Stmt::Matches(kind), "AsStmt on non-statement");
    return static_cast<Stmt*>(this);
}
inline Decl* AstNode::AsDecl() {
    // Decl is a structural concept; we return the node as Decl* if it's a
    // declaration. For simplicity we treat VarDeclStmt/FunctionDecl/ClassDecl
    // as "decls" via a static_cast; Decl itself is not a real type.
    return reinterpret_cast<Decl*>(this);
}

}  // namespace v12

#endif  // V12_FRONTEND_AST_AST_H_
