// =============================================================================
// src/frontend/bytecode/scope.h
// =============================================================================
// Scope analysis: resolve every identifier reference to one of:
//   - a local register slot (in the current FunctionInfo's register file)
//   - a context slot (depth, index) — captured by a closure
//   - a global property (resolved at runtime via LoadGlobal/StoreGlobal)
//
// The analyzer walks the AST twice:
//   1. Declaration pass: assign every declared variable (var/let/const/
//      function) to a scope (function-local, block, or context-captured).
//      var declarations are hoisted to the enclosing function.
//   2. Reference pass: for every Identifier reference, look up which scope
//      holds the binding and emit either:
//        - a register index (local)
//        - a (depth, index) pair (context)
//        - a global lookup (name)
//
// Closures:
//   When a FunctionExpr/FunctionDecl/ArrowFunction references a variable
//   declared in an enclosing function, that variable is "captured": it
//   moves from a register slot to a Context slot at runtime. The bytecode
//   generator emits a CreateContext bytecode that allocates a Context
//   with the right number of slots, and StaContext/LoadContext bytecodes
//   to read/write captured variables through the context chain.
//
// This is essentially the same model as V8's "context chain" used by
// Ignition + TurboFan.

#ifndef V12_FRONTEND_BYTECODE_SCOPE_H_
#define V12_FRONTEND_BYTECODE_SCOPE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/arena.h"
#include "base/macros.h"
#include "base/small-vector.h"
#include "frontend/ast/ast.h"

namespace v12 {

class Isolate;

// Where a variable lives.
enum class VarLocation : uint8_t {
    kUnresolved,    // not yet resolved
    kLocal,         // in a register slot of the current function
    kContext,       // in a Context slot (captured by a closure)
    kGlobal,        // on the global object
};

// A resolved variable reference. Used by the bytecode generator.
struct ResolvedVar {
    VarLocation location = VarLocation::kUnresolved;
    uint8_t reg = 0;          // when kLocal
    uint16_t context_depth = 0;  // when kContext
    uint16_t context_index = 0;  // when kContext
    std::string_view name;    // when kGlobal (or for diagnostics)
};

// A variable binding inside a Scope.
struct Binding {
    std::string_view name;
    VarLocation location = VarLocation::kUnresolved;
    uint8_t reg = 0;          // valid when kLocal
    uint16_t context_index = 0;  // valid when kContext
    bool is_captured = false;    // true if a nested function references this
    bool is_const = false;       // for assignment checks (TODO)
};

// A Scope corresponds to a lexical scope in the source: the global scope,
// a function scope, or a block scope.
class Scope {
public:
    enum class Kind : uint8_t {
        kGlobal,
        kFunction,
        kBlock,
    };

    Scope(Kind kind, Scope* parent, Arena* arena)
        : kind_(kind), parent_(parent), arena_(arena) {}

    Kind kind() const { return kind_; }
    Scope* parent() const { return parent_; }

    // Declare a variable in this scope. Returns the Binding.
    // If the variable is already declared here, returns the existing binding.
    Binding* Declare(std::string_view name, bool is_const = false);

    // Declare a parameter in this scope. Parameters occupy registers
    // 0..num_params-1 (assigned in order). Must be called before Declare
    // for any local variable.
    Binding* DeclareParameter(std::string_view name);

    // Look up a variable, walking the scope chain. Returns nullptr if not
    // found anywhere (which means it's a global).
    Binding* Lookup(std::string_view name);

    // Mark this binding as captured by a nested function. The variable's
    // location transitions from kLocal to kContext (the bytecode generator
    // will allocate a context slot for it).
    void MarkCaptured(Binding* b);

    // Iterate bindings (used by the bytecode generator to allocate context
    // slots for captured variables).
    const SmallVector<Binding, 8>& bindings() const { return bindings_; }

    // Context slot bookkeeping (filled in by the bytecode generator).
    uint16_t context_slot_count() const { return context_slot_count_; }
    void set_context_slot_count(uint16_t n) { context_slot_count_ = n; }

    // Allocate the next local register index. Register 0..num_params-1 are
    // reserved for parameters; locals start at num_params.
    uint8_t AllocLocal() {
        V12_CHECK(next_local_ < 0xFE, "register file overflow (max 254 locals)");
        return next_local_++;
    }
    void SetParameterCount(uint16_t n) {
        num_params_ = static_cast<uint8_t>(n);
        next_local_ = static_cast<uint8_t>(n);
    }
    uint8_t next_local() const { return next_local_; }
    uint8_t num_params() const { return num_params_; }
    uint8_t next_param() const { return next_param_; }

private:
    Kind kind_;
    Scope* parent_;
    Arena* arena_;
    SmallVector<Binding, 8> bindings_;
    uint16_t context_slot_count_ = 0;
    uint8_t next_local_ = 0;
    uint8_t num_params_ = 0;
    uint8_t next_param_ = 0;
};

// ScopeAnalyzer: builds a tree of Scopes for a Program, marking captured
// variables. The result is queried by the bytecode generator via Lookup.
class ScopeAnalyzer {
public:
    ScopeAnalyzer(Arena* arena, Isolate* iso, Program* prog);

    // Run analysis. Returns true on success. After this, Resolve() can be
    // called for any Identifier in the AST.
    void Analyze();

    // Resolve an identifier reference at the current scope. The bytecode
    // generator calls this while emitting code.
    ResolvedVar Resolve(std::string_view name, Scope* current_scope);

    // Get the Function scope (or Global scope) for a given AST node.
    // We attach Scopes to AST nodes via a side table because the AST is
    // not annotated directly.
    Scope* GetScope(AstNode* node) { return node_scopes_[node]; }
    void SetScope(AstNode* node, Scope* s) { node_scopes_[node] = s; }

    Scope* global_scope() { return global_; }

private:
    // Walk the AST and build scopes. Each function/block gets its own Scope.
    void VisitStmt(Stmt* s, Scope* current);
    void VisitExpr(Expr* e, Scope* current);
    void VisitFunction(FunctionExpr* fn, Scope* current, std::string_view name);
    void VisitArrow(ArrowFunction* fn, Scope* current);
    void VisitClass(ClassExpr* cls, Scope* current);

    // Second pass: walk again, resolving every Identifier reference. If
    // the identifier resolves to a binding in an enclosing Function scope
    // (not the current one), mark that binding as captured.
    void ResolveReferences(Stmt* s, Scope* current);
    void ResolveReferences(Expr* e, Scope* current);

    Arena* arena_;
    Isolate* iso_;
    Program* program_;
    Scope* global_;
    std::unordered_map<AstNode*, Scope*> node_scopes_;
};

}  // namespace v12

#endif  // V12_FRONTEND_BYTECODE_SCOPE_H_
