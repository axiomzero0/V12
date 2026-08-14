// =============================================================================
// src/frontend/bytecode/scope.cc
// =============================================================================

#include "frontend/bytecode/scope.h"

#include "vm/isolate/isolate.h"

namespace v12 {

// ----- Scope -----
Binding* Scope::DeclareParameter(std::string_view name) {
    V12_CHECK(kind_ == Kind::kFunction, "DeclareParameter on non-function scope");
    for (auto& b : bindings_) {
        if (b.name == name) return &b;
    }
    Binding b;
    b.name = name;
    b.location = VarLocation::kLocal;
    b.reg = next_param_++;
    bindings_.push_back(b);
    return &bindings_.back();
}

Binding* Scope::Declare(std::string_view name, bool is_const) {
    for (auto& b : bindings_) {
        if (b.name == name) return &b;
    }
    Binding b;
    b.name = name;
    b.location = (kind_ == Kind::kGlobal) ? VarLocation::kGlobal : VarLocation::kLocal;
    if (kind_ == Kind::kFunction) {
        b.reg = AllocLocal();
    } else if (kind_ == Kind::kBlock) {
        // Block-local variables share the function's register file.
        // The bytecode generator assigns register indices later when it
        // enters the block. We mark them kLocal with reg = 0xFF for now.
        b.reg = 0xFF;
    } else {
        // Global: no register.
        b.reg = 0;
    }
    b.is_const = is_const;
    bindings_.push_back(b);
    return &bindings_.back();
}

Binding* Scope::Lookup(std::string_view name) {
    for (auto& b : bindings_) {
        if (b.name == name) return &b;
    }
    if (parent_ != nullptr) return parent_->Lookup(name);
    return nullptr;
}

void Scope::MarkCaptured(Binding* b) {
    if (b->is_captured) return;
    b->is_captured = true;
    // The binding's location transitions to kContext. The actual context
    // slot index is assigned later by the bytecode generator when it
    // allocates the function's Context.
    b->location = VarLocation::kContext;
}

// ----- ScopeAnalyzer -----
ScopeAnalyzer::ScopeAnalyzer(Arena* arena, Isolate* iso, Program* prog)
    : arena_(arena), iso_(iso), program_(prog),
      global_(arena_->New<Scope>(Scope::Kind::kGlobal, nullptr, arena_)) {}

void ScopeAnalyzer::Analyze() {
    // Pass 1: declare all variables in their proper scopes.
    for (Stmt* s : program_->body) {
        VisitStmt(s, global_);
    }

    // Pass 2: resolve references, marking captured variables.
    for (Stmt* s : program_->body) {
        ResolveReferences(s, global_);
    }

    // Pass 3: assign context slot indices for captured variables in each
    // function scope. Done lazily as we walk the AST again.
    // (We skip this here; the bytecode generator assigns context slots
    //  when it enters each function.)
}

void ScopeAnalyzer::VisitStmt(Stmt* s, Scope* current) {
    if (s == nullptr) return;
    SetScope(s, current);
    switch (s->kind) {
        case AstKind::kBlock: {
            Block* b = static_cast<Block*>(s);
            Scope* block_scope = arena_->New<Scope>(Scope::Kind::kBlock, current, arena_);
            SetScope(s, block_scope);
            for (Stmt* inner : b->statements) {
                VisitStmt(inner, block_scope);
            }
            break;
        }
        case AstKind::kVarDecl:
        case AstKind::kLetDecl:
        case AstKind::kConstDecl: {
            VarDeclStmt* vd = static_cast<VarDeclStmt*>(s);
            bool is_const = (s->kind == AstKind::kConstDecl);
            // var declarations are hoisted to the enclosing function/global.
            // let/const are scoped to the enclosing block.
            Scope* decl_scope = (s->kind == AstKind::kVarDecl)
                                  ? current  // TODO: walk up to function scope
                                  : current;
            for (auto& decl : vd->declarations) {
                Binding* b = decl_scope->Declare(decl.name, is_const);
                (void)b;
                if (decl.init) VisitExpr(decl.init, current);
            }
            break;
        }
        case AstKind::kFunctionDecl: {
            FunctionDecl* fd = static_cast<FunctionDecl*>(s);
            // The function name is declared in the enclosing scope.
            current->Declare(fd->name);
            // The function body gets its own scope.
            Scope* fn_scope = arena_->New<Scope>(Scope::Kind::kFunction, current, arena_);
            fn_scope->SetParameterCount(static_cast<uint16_t>(fd->params.size()));
            for (auto& p : fd->params) {
                fn_scope->DeclareParameter(p.name);
            }
            // Visit the body in the function scope.
            VisitStmt(fd->body, fn_scope);
            SetScope(s, fn_scope);
            break;
        }
        case AstKind::kClassDecl: {
            ClassDecl* cd = static_cast<ClassDecl*>(s);
            current->Declare(cd->name);
            if (cd->superclass) VisitExpr(cd->superclass, current);
            // Visit class members in a new scope.
            VisitClass(nullptr, current);
            break;
        }
        case AstKind::kIf: {
            If* i = static_cast<If*>(s);
            VisitExpr(i->cond, current);
            VisitStmt(i->then_branch, current);
            VisitStmt(i->else_branch, current);
            break;
        }
        case AstKind::kWhile: {
            While* w = static_cast<While*>(s);
            VisitExpr(w->cond, current);
            VisitStmt(w->body, current);
            break;
        }
        case AstKind::kDoWhile: {
            DoWhile* d = static_cast<DoWhile*>(s);
            VisitStmt(d->body, current);
            VisitExpr(d->cond, current);
            break;
        }
        case AstKind::kFor: {
            For* f = static_cast<For*>(s);
            // for-let/for-const get their own block scope (per-iteration in
            // real JS, but we approximate with one scope for the whole loop).
            Scope* for_scope = arena_->New<Scope>(Scope::Kind::kBlock, current, arena_);
            SetScope(s, for_scope);
            if (f->init) VisitStmt(f->init, for_scope);
            if (f->cond) VisitExpr(f->cond, for_scope);
            if (f->update) VisitExpr(f->update, for_scope);
            VisitStmt(f->body, for_scope);
            break;
        }
        case AstKind::kForIn:
        case AstKind::kForOf: {
            // Simplified: treat the loop variable as a fresh binding.
            if (s->kind == AstKind::kForIn) {
                ForIn* fi = static_cast<ForIn*>(s);
                VisitStmt(fi->left, current);
                VisitExpr(fi->right, current);
                VisitStmt(fi->body, current);
            } else {
                ForOf* fo = static_cast<ForOf*>(s);
                VisitStmt(fo->left, current);
                VisitExpr(fo->right, current);
                VisitStmt(fo->body, current);
            }
            break;
        }
        case AstKind::kReturn: {
            Return* r = static_cast<Return*>(s);
            if (r->value) VisitExpr(r->value, current);
            break;
        }
        case AstKind::kThrow: {
            Throw* t = static_cast<Throw*>(s);
            VisitExpr(t->value, current);
            break;
        }
        case AstKind::kTry: {
            Try* t = static_cast<Try*>(s);
            VisitStmt(t->block, current);
            if (t->catch_clause) {
                Scope* catch_scope = arena_->New<Scope>(Scope::Kind::kBlock, current, arena_);
                if (!t->catch_clause->param.empty()) {
                    catch_scope->Declare(t->catch_clause->param);
                }
                VisitStmt(t->catch_clause->body, catch_scope);
            }
            if (t->finally_block) VisitStmt(t->finally_block, current);
            break;
        }
        case AstKind::kSwitch: {
            Switch* sw = static_cast<Switch*>(s);
            VisitExpr(sw->discriminant, current);
            for (auto& c : sw->cases) {
                if (c.test) VisitExpr(c.test, current);
                VisitStmt(c.body, current);
            }
            break;
        }
        case AstKind::kLabeled: {
            Labeled* l = static_cast<Labeled*>(s);
            VisitStmt(l->body, current);
            break;
        }
        case AstKind::kBreak:
        case AstKind::kContinue:
        case AstKind::kEmpty:
        case AstKind::kDebugger:
            break;
        case AstKind::kExpressionStatement: {
            ExpressionStatement* es = static_cast<ExpressionStatement*>(s);
            VisitExpr(es->expr, current);
            break;
        }
        default:
            break;
    }
}

void ScopeAnalyzer::VisitExpr(Expr* e, Scope* current) {
    if (e == nullptr) return;
    SetScope(e, current);
    switch (e->kind) {
        case AstKind::kNumberLiteral:
        case AstKind::kStringLiteral:
        case AstKind::kBoolLiteral:
        case AstKind::kNullLiteral:
        case AstKind::kUndefinedLiteral:
        case AstKind::kThis:
        case AstKind::kSuper:
        case AstKind::kIdentifier:
            break;
        case AstKind::kBinaryOp: {
            BinaryOp* b = static_cast<BinaryOp*>(e);
            VisitExpr(b->left, current);
            VisitExpr(b->right, current);
            break;
        }
        case AstKind::kUnaryOp: {
            UnaryOp* u = static_cast<UnaryOp*>(e);
            VisitExpr(u->operand, current);
            break;
        }
        case AstKind::kUpdateOp: {
            UpdateOp* u = static_cast<UpdateOp*>(e);
            VisitExpr(u->operand, current);
            break;
        }
        case AstKind::kAssignment: {
            Assignment* a = static_cast<Assignment*>(e);
            VisitExpr(a->target, current);
            VisitExpr(a->value, current);
            break;
        }
        case AstKind::kLogicalOp: {
            LogicalOp* l = static_cast<LogicalOp*>(e);
            VisitExpr(l->left, current);
            VisitExpr(l->right, current);
            break;
        }
        case AstKind::kConditional: {
            Conditional* c = static_cast<Conditional*>(e);
            VisitExpr(c->cond, current);
            VisitExpr(c->then_expr, current);
            VisitExpr(c->else_expr, current);
            break;
        }
        case AstKind::kSequence: {
            Sequence* sq = static_cast<Sequence*>(e);
            for (Expr* x : sq->expressions) VisitExpr(x, current);
            break;
        }
        case AstKind::kCall: {
            Call* c = static_cast<Call*>(e);
            VisitExpr(c->callee, current);
            for (Expr* a : c->args) VisitExpr(a, current);
            break;
        }
        case AstKind::kNew: {
            NewExpr* n = static_cast<NewExpr*>(e);
            VisitExpr(n->callee, current);
            for (Expr* a : n->args) VisitExpr(a, current);
            break;
        }
        case AstKind::kMember: {
            Member* m = static_cast<Member*>(e);
            VisitExpr(m->object, current);
            if (m->is_computed) VisitExpr(m->property, current);
            break;
        }
        case AstKind::kOptionalChain: {
            OptionalChain* o = static_cast<OptionalChain*>(e);
            VisitExpr(o->base, current);
            break;
        }
        case AstKind::kArrayLiteral: {
            ArrayLiteral* a = static_cast<ArrayLiteral*>(e);
            for (Expr* x : a->elements) VisitExpr(x, current);
            break;
        }
        case AstKind::kObjectLiteral: {
            ObjectLiteral* o = static_cast<ObjectLiteral*>(e);
            for (auto& p : o->properties) {
                if (p.is_computed) VisitExpr(p.key, current);
                VisitExpr(p.value, current);
            }
            break;
        }
        case AstKind::kFunctionExpr: {
            FunctionExpr* fn = static_cast<FunctionExpr*>(e);
            VisitFunction(fn, current, fn->name);
            break;
        }
        case AstKind::kArrowFunction: {
            ArrowFunction* af = static_cast<ArrowFunction*>(e);
            VisitArrow(af, current);
            break;
        }
        case AstKind::kClassExpr: {
            ClassExpr* cls = static_cast<ClassExpr*>(e);
            VisitClass(cls, current);
            break;
        }
        case AstKind::kSpread: {
            Spread* sp = static_cast<Spread*>(e);
            VisitExpr(sp->inner, current);
            break;
        }
        case AstKind::kYield: {
            Yield* y = static_cast<Yield*>(e);
            if (y->value) VisitExpr(y->value, current);
            break;
        }
        default:
            break;
    }
}

void ScopeAnalyzer::VisitFunction(FunctionExpr* fn, Scope* current, std::string_view name) {
    (void)name;
    if (fn == nullptr) return;
    Scope* fn_scope = arena_->New<Scope>(Scope::Kind::kFunction, current, arena_);
    fn_scope->SetParameterCount(static_cast<uint16_t>(fn->params.size()));
    for (auto& p : fn->params) {
        fn_scope->DeclareParameter(p.name);
    }
    if (fn->body) {
        SetScope(fn, fn_scope);
        VisitStmt(fn->body, fn_scope);
    }
}

void ScopeAnalyzer::VisitArrow(ArrowFunction* fn, Scope* current) {
    Scope* fn_scope = arena_->New<Scope>(Scope::Kind::kFunction, current, arena_);
    fn_scope->SetParameterCount(static_cast<uint16_t>(fn->params.size()));
    for (auto& p : fn->params) {
        fn_scope->DeclareParameter(p.name);
    }
    SetScope(fn, fn_scope);
    if (fn->block_body) VisitStmt(fn->block_body, fn_scope);
    if (fn->expr_body) VisitExpr(fn->expr_body, fn_scope);
}

void ScopeAnalyzer::VisitClass(ClassExpr* cls, Scope* current) {
    if (cls == nullptr) return;
    Scope* class_scope = arena_->New<Scope>(Scope::Kind::kBlock, current, arena_);
    SetScope(cls, class_scope);
    if (cls->superclass) VisitExpr(cls->superclass, current);
    // Members are visited as part of the class scope (simplified).
}

void ScopeAnalyzer::ResolveReferences(Stmt* s, Scope* current) {
    if (s == nullptr) return;
    Scope* node_scope = GetScope(s);
    if (node_scope != nullptr) current = node_scope;
    switch (s->kind) {
        case AstKind::kBlock: {
            Block* b = static_cast<Block*>(s);
            for (Stmt* inner : b->statements) ResolveReferences(inner, current);
            break;
        }
        case AstKind::kVarDecl:
        case AstKind::kLetDecl:
        case AstKind::kConstDecl: {
            VarDeclStmt* vd = static_cast<VarDeclStmt*>(s);
            for (auto& decl : vd->declarations) {
                if (decl.init) ResolveReferences(decl.init, current);
            }
            break;
        }
        case AstKind::kFunctionDecl:
        case AstKind::kClassDecl: {
            // The name was declared in the enclosing scope already; visit body.
            // (We don't recurse into the body here — the body's references
            //  will be resolved when the bytecode generator emits the inner
            //  function. This is fine because the inner function's scope
            //  chain points back to this scope.)
            FunctionDecl* fd = s->kind == AstKind::kFunctionDecl
                                 ? static_cast<FunctionDecl*>(s) : nullptr;
            ClassDecl* cd = s->kind == AstKind::kClassDecl
                              ? static_cast<ClassDecl*>(s) : nullptr;
            if (fd) {
                if (fd->body) ResolveReferences(fd->body, GetScope(s));
            } else if (cd) {
                if (cd->superclass) ResolveReferences(cd->superclass, current);
            }
            break;
        }
        case AstKind::kIf: {
            If* i = static_cast<If*>(s);
            ResolveReferences(i->cond, current);
            ResolveReferences(i->then_branch, current);
            ResolveReferences(i->else_branch, current);
            break;
        }
        case AstKind::kWhile: {
            While* w = static_cast<While*>(s);
            ResolveReferences(w->cond, current);
            ResolveReferences(w->body, current);
            break;
        }
        case AstKind::kDoWhile: {
            DoWhile* d = static_cast<DoWhile*>(s);
            ResolveReferences(d->body, current);
            ResolveReferences(d->cond, current);
            break;
        }
        case AstKind::kFor: {
            For* f = static_cast<For*>(s);
            if (f->init) ResolveReferences(f->init, current);
            if (f->cond) ResolveReferences(f->cond, current);
            if (f->update) ResolveReferences(f->update, current);
            ResolveReferences(f->body, current);
            break;
        }
        case AstKind::kForIn: {
            ForIn* fi = static_cast<ForIn*>(s);
            ResolveReferences(fi->left, current);
            ResolveReferences(fi->right, current);
            ResolveReferences(fi->body, current);
            break;
        }
        case AstKind::kForOf: {
            ForOf* fo = static_cast<ForOf*>(s);
            ResolveReferences(fo->left, current);
            ResolveReferences(fo->right, current);
            ResolveReferences(fo->body, current);
            break;
        }
        case AstKind::kReturn: {
            Return* r = static_cast<Return*>(s);
            if (r->value) ResolveReferences(r->value, current);
            break;
        }
        case AstKind::kThrow: {
            Throw* t = static_cast<Throw*>(s);
            ResolveReferences(t->value, current);
            break;
        }
        case AstKind::kTry: {
            Try* t = static_cast<Try*>(s);
            ResolveReferences(t->block, current);
            if (t->catch_clause) {
                Scope* catch_scope = GetScope(t);
                ResolveReferences(t->catch_clause->body,
                                   catch_scope ? catch_scope : current);
            }
            if (t->finally_block) ResolveReferences(t->finally_block, current);
            break;
        }
        case AstKind::kSwitch: {
            Switch* sw = static_cast<Switch*>(s);
            ResolveReferences(sw->discriminant, current);
            for (auto& c : sw->cases) {
                if (c.test) ResolveReferences(c.test, current);
                ResolveReferences(c.body, current);
            }
            break;
        }
        case AstKind::kLabeled: {
            Labeled* l = static_cast<Labeled*>(s);
            ResolveReferences(l->body, current);
            break;
        }
        case AstKind::kExpressionStatement: {
            ExpressionStatement* es = static_cast<ExpressionStatement*>(s);
            ResolveReferences(es->expr, current);
            break;
        }
        default:
            break;
    }
}

void ScopeAnalyzer::ResolveReferences(Expr* e, Scope* current) {
    if (e == nullptr) return;
    Scope* node_scope = GetScope(e);
    if (node_scope != nullptr) current = node_scope;
    switch (e->kind) {
        case AstKind::kIdentifier: {
            Identifier* id = static_cast<Identifier*>(e);
            // Find the enclosing function scope of `current`. Variables in
            // that scope (or in block scopes within it) are local to this
            // function. Variables in FURTHER-outer function scopes are
            // captured by this function's closure.
            Scope* enclosing_fn = current;
            while (enclosing_fn != nullptr &&
                   enclosing_fn->kind() != Scope::Kind::kFunction) {
                enclosing_fn = enclosing_fn->parent();
            }
            // Walk the scope chain looking for the binding.
            Scope* s = current;
            while (s != nullptr) {
                for (auto& x : s->bindings()) {
                    if (x.name == id->name) {
                        // Found it. If `s` is outside the enclosing function
                        // (i.e., `s` is an ancestor of `enclosing_fn`), and
                        // `s` is a function scope, then this binding is
                        // captured by our closure.
                        if (s != enclosing_fn && s->kind() == Scope::Kind::kFunction &&
                            x.location == VarLocation::kLocal) {
                            s->MarkCaptured(&const_cast<Binding&>(x));
                        }
                        // Also: if the binding is in the enclosing function
                        // itself but accessed from a nested function (not yet
                        // possible since we don't recurse into nested
                        // functions here), it would be captured. The nested
                        // function's own ResolveReferences pass handles that.
                        goto found;
                    }
                }
                s = s->parent();
            }
            found:
            break;
        }
        case AstKind::kBinaryOp: {
            BinaryOp* b = static_cast<BinaryOp*>(e);
            ResolveReferences(b->left, current);
            ResolveReferences(b->right, current);
            break;
        }
        case AstKind::kUnaryOp: {
            UnaryOp* u = static_cast<UnaryOp*>(e);
            ResolveReferences(u->operand, current);
            break;
        }
        case AstKind::kUpdateOp: {
            UpdateOp* u = static_cast<UpdateOp*>(e);
            ResolveReferences(u->operand, current);
            break;
        }
        case AstKind::kAssignment: {
            Assignment* a = static_cast<Assignment*>(e);
            ResolveReferences(a->target, current);
            ResolveReferences(a->value, current);
            break;
        }
        case AstKind::kLogicalOp: {
            LogicalOp* l = static_cast<LogicalOp*>(e);
            ResolveReferences(l->left, current);
            ResolveReferences(l->right, current);
            break;
        }
        case AstKind::kConditional: {
            Conditional* c = static_cast<Conditional*>(e);
            ResolveReferences(c->cond, current);
            ResolveReferences(c->then_expr, current);
            ResolveReferences(c->else_expr, current);
            break;
        }
        case AstKind::kSequence: {
            Sequence* sq = static_cast<Sequence*>(e);
            for (Expr* x : sq->expressions) ResolveReferences(x, current);
            break;
        }
        case AstKind::kCall: {
            Call* c = static_cast<Call*>(e);
            ResolveReferences(c->callee, current);
            for (Expr* a : c->args) ResolveReferences(a, current);
            break;
        }
        case AstKind::kNew: {
            NewExpr* n = static_cast<NewExpr*>(e);
            ResolveReferences(n->callee, current);
            for (Expr* a : n->args) ResolveReferences(a, current);
            break;
        }
        case AstKind::kMember: {
            Member* m = static_cast<Member*>(e);
            ResolveReferences(m->object, current);
            if (m->is_computed) ResolveReferences(m->property, current);
            break;
        }
        case AstKind::kArrayLiteral: {
            ArrayLiteral* a = static_cast<ArrayLiteral*>(e);
            for (Expr* x : a->elements) ResolveReferences(x, current);
            break;
        }
        case AstKind::kObjectLiteral: {
            ObjectLiteral* o = static_cast<ObjectLiteral*>(e);
            for (auto& p : o->properties) {
                if (p.is_computed) ResolveReferences(p.key, current);
                ResolveReferences(p.value, current);
            }
            break;
        }
        case AstKind::kFunctionExpr: {
            FunctionExpr* fn = static_cast<FunctionExpr*>(e);
            if (fn->body) ResolveReferences(fn->body, GetScope(e));
            break;
        }
        case AstKind::kArrowFunction: {
            ArrowFunction* af = static_cast<ArrowFunction*>(e);
            if (af->block_body) ResolveReferences(af->block_body, GetScope(e));
            if (af->expr_body) ResolveReferences(af->expr_body, GetScope(e));
            break;
        }
        case AstKind::kClassExpr: {
            ClassExpr* cls = static_cast<ClassExpr*>(e);
            if (cls->superclass) ResolveReferences(cls->superclass, current);
            break;
        }
        case AstKind::kSpread: {
            Spread* sp = static_cast<Spread*>(e);
            ResolveReferences(sp->inner, current);
            break;
        }
        default:
            break;
    }
}

ResolvedVar ScopeAnalyzer::Resolve(std::string_view name, Scope* current_scope) {
    ResolvedVar r;
    r.name = name;
    // Walk the scope chain looking for the binding.
    Scope* s = current_scope;
    uint16_t depth = 0;
    while (s != nullptr) {
        for (auto& b : s->bindings()) {
            if (b.name == name) {
                if (b.location == VarLocation::kContext) {
                    // Walk back to find the function scope that owns this
                    // context slot. The depth is the number of function
                    // scopes between current and the owner.
                    r.location = VarLocation::kContext;
                    r.context_depth = depth;
                    r.context_index = b.context_index;
                    return r;
                } else if (b.location == VarLocation::kLocal) {
                    r.location = VarLocation::kLocal;
                    r.reg = b.reg;
                    return r;
                } else if (b.location == VarLocation::kGlobal) {
                    r.location = VarLocation::kGlobal;
                    return r;
                }
            }
        }
        s = s->parent();
        ++depth;
    }
    // Not found anywhere — it's a global (declared implicitly).
    r.location = VarLocation::kGlobal;
    return r;
}

}  // namespace v12
