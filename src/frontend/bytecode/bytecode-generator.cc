// =============================================================================
// src/frontend/bytecode/bytecode-generator.cc
// =============================================================================
// Implementation of the AST -> bytecode compiler.
//
// See bytecode-generator.h for the design overview. This file is large
// because it has to handle every AST node kind, but each handler is small.
//
// Register allocation strategy:
//   Parameters occupy registers 0..N-1 (where N = num_parameters).
//   Local variables (var/let/const at function scope) occupy the next slots.
//   Captured variables are moved to Context slots at function entry.
//   Temporaries occupy the remaining slots. We use a simple bump allocator:
//   each EmitExpr that needs a temp register calls AllocTemp, which returns
//   the next free slot. We don't reuse temps within an expression — this
//   wastes some registers but keeps the code simple and correct.
//
// For a real production engine we'd do liveness analysis and reuse temps
// aggressively, but for the interpreter MVP this is fine.

#include "frontend/bytecode/bytecode-generator.h"

#include <cstring>

#include "frontend/lexer/tokens.h"
#include "vm/isolate/isolate.h"

namespace v12 {

BytecodeGenerator::BytecodeGenerator(Isolate* iso, Arena* arena)
    : iso_(iso), arena_(arena),
      program_(std::make_unique<BytecodeProgram>()),
      scope_analyzer_(std::make_unique<ScopeAnalyzer>(arena, iso, nullptr)) {}

BytecodeGenerator::~BytecodeGenerator() = default;

std::unique_ptr<BytecodeProgram> BytecodeGenerator::Compile(Program* prog) {
    // Re-create the scope analyzer with the actual program.
    scope_analyzer_ = std::make_unique<ScopeAnalyzer>(arena_, iso_, prog);
    scope_analyzer_->Analyze();

    // Compile the top-level program as a FunctionInfo with no parameters.
    FnState top_fs;
    top_fs.info = program_->NewFunction("<toplevel>");
    top_fs.info->is_toplevel = true;
    top_fs.scope = scope_analyzer_->global_scope();
    top_fs.next_temp = 0;

    // Compile each top-level statement.
    for (Stmt* s : prog->body) {
        EmitStmt(&top_fs, s);
    }
    // Top-level falls off the end -> return undefined.
    EmitOp(&top_fs, Op::ReturnUndefined);

    // Set register counts.
    top_fs.info->num_parameters = 0;
    top_fs.info->num_registers = top_fs.next_temp > 0 ? top_fs.next_temp : 1;

    // Count context vars (the top-level doesn't allocate a context).
    top_fs.info->num_context_vars = 0;

    program_->toplevel = top_fs.info;
    return std::move(program_);
}

// -----------------------------------------------------------------------------
// Register allocation
// -----------------------------------------------------------------------------
uint8_t BytecodeGenerator::AllocTemp(FnState* fs) {
    V12_CHECK(fs->next_temp < 0xFE, "register file overflow (max 254 temps+locals)");
    return fs->next_temp++;
}

// -----------------------------------------------------------------------------
// Bytecode emission primitives
// -----------------------------------------------------------------------------
void BytecodeGenerator::EmitByte(FnState* fs, uint8_t b) {
    fs->info->bytecode.push_back(b);
}
void BytecodeGenerator::EmitOp(FnState* fs, Op op) {
    EmitByte(fs, static_cast<uint8_t>(op));
}
void BytecodeGenerator::EmitReg(FnState* fs, Reg r) {
    EmitByte(fs, static_cast<uint8_t>(r));
}
void BytecodeGenerator::EmitImm8(FnState* fs, uint8_t v) {
    EmitByte(fs, v);
}
void BytecodeGenerator::EmitImm16(FnState* fs, uint16_t v) {
    EmitByte(fs, static_cast<uint8_t>(v & 0xFF));
    EmitByte(fs, static_cast<uint8_t>((v >> 8) & 0xFF));
}
void BytecodeGenerator::EmitImm32(FnState* fs, uint32_t v) {
    EmitByte(fs, static_cast<uint8_t>(v & 0xFF));
    EmitByte(fs, static_cast<uint8_t>((v >> 8) & 0xFF));
    EmitByte(fs, static_cast<uint8_t>((v >> 16) & 0xFF));
    EmitByte(fs, static_cast<uint8_t>((v >> 24) & 0xFF));
}
void BytecodeGenerator::EmitIdx(FnState* fs, uint16_t idx) {
    EmitImm16(fs, idx);
}

uint32_t BytecodeGenerator::EmitJump(FnState* fs, Op op) {
    EmitOp(fs, op);
    uint32_t patch_off = static_cast<uint32_t>(fs->info->bytecode.size());
    EmitImm32(fs, 0);   // placeholder; will be patched.
    return patch_off;
}

void BytecodeGenerator::PatchJump(FnState* fs, uint32_t patch_offset, uint32_t target_offset) {
    uint8_t* p = fs->info->bytecode.data() + patch_offset;
    p[0] = static_cast<uint8_t>(target_offset & 0xFF);
    p[1] = static_cast<uint8_t>((target_offset >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((target_offset >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((target_offset >> 24) & 0xFF);
}

// -----------------------------------------------------------------------------
// Constant pool / name tables
// -----------------------------------------------------------------------------
uint32_t BytecodeGenerator::AddConstSmi(FnState* fs, intptr_t v) {
    Constant c;
    c.kind = Constant::Kind::kSmi;
    c.smi = v;
    fs->info->constants.push_back(c);
    return static_cast<uint32_t>(fs->info->constants.size() - 1);
}
uint32_t BytecodeGenerator::AddConstNumber(FnState* fs, double v) {
    Constant c;
    c.kind = Constant::Kind::kNumber;
    c.number = v;
    fs->info->constants.push_back(c);
    return static_cast<uint32_t>(fs->info->constants.size() - 1);
}
uint32_t BytecodeGenerator::AddConstString(FnState* fs, std::string_view s) {
    // Strings are stored as indices into the property_names table.
    uint16_t idx = AddPropertyName(fs, s);
    Constant c;
    c.kind = Constant::Kind::kString;
    c.index = idx;
    fs->info->constants.push_back(c);
    return static_cast<uint32_t>(fs->info->constants.size() - 1);
}
uint32_t BytecodeGenerator::AddConstFunctionInfo(FnState* fs, FunctionInfo* fi) {
    // Find the index of fi in program_->functions.
    uint32_t idx = 0;
    for (auto& up : program_->functions) {
        if (up.get() == fi) {
            Constant c;
            c.kind = Constant::Kind::kFunctionInfo;
            c.index = idx;
            fs->info->constants.push_back(c);
            return static_cast<uint32_t>(fs->info->constants.size() - 1);
        }
        ++idx;
    }
    V12_CHECK(false, "AddConstFunctionInfo: FunctionInfo not in program");
    return 0;
}
uint16_t BytecodeGenerator::AddPropertyName(FnState* fs, std::string_view s) {
    // Dedup by string content.
    for (size_t i = 0; i < fs->info->property_names.size(); ++i) {
        if (fs->info->property_names[i] == s) {
            return static_cast<uint16_t>(i);
        }
    }
    fs->info->property_names.push_back(std::string(s));
    return static_cast<uint16_t>(fs->info->property_names.size() - 1);
}
uint16_t BytecodeGenerator::AddGlobalName(FnState* fs, std::string_view s) {
    for (size_t i = 0; i < fs->info->global_names.size(); ++i) {
        if (fs->info->global_names[i] == s) {
            return static_cast<uint16_t>(i);
        }
    }
    fs->info->global_names.push_back(std::string(s));
    return static_cast<uint16_t>(fs->info->global_names.size() - 1);
}

// -----------------------------------------------------------------------------
// Identifier load/store
// -----------------------------------------------------------------------------
void BytecodeGenerator::EmitLoadIdentifier(FnState* fs, Identifier* id) {
    ResolvedVar r = scope_analyzer_->Resolve(id->name, fs->scope);
    switch (r.location) {
        case VarLocation::kLocal:
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, r.reg);
            return;
        case VarLocation::kContext:
            EmitOp(fs, Op::LoadContext);
            EmitImm16(fs, r.context_depth);
            EmitImm16(fs, r.context_index);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        case VarLocation::kGlobal:
            EmitOp(fs, Op::LoadGlobal);
            EmitReg(fs, AddPropertyName(fs, id->name));
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        case VarLocation::kUnresolved:
            // Treat as global.
            EmitOp(fs, Op::LoadGlobal);
            EmitReg(fs, AddPropertyName(fs, id->name));
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
    }
}

void BytecodeGenerator::EmitStoreIdentifier(FnState* fs, Identifier* id) {
    ResolvedVar r = scope_analyzer_->Resolve(id->name, fs->scope);
    switch (r.location) {
        case VarLocation::kLocal:
            EmitOp(fs, Op::Star);
            EmitReg(fs, r.reg);
            return;
        case VarLocation::kContext:
            EmitOp(fs, Op::StoreContext);
            EmitReg(fs, 0);   // dummy: the value is in acc
            EmitImm16(fs, r.context_depth);
            EmitImm16(fs, r.context_index);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        case VarLocation::kGlobal:
        case VarLocation::kUnresolved:
            EmitOp(fs, Op::StoreGlobal);
            EmitReg(fs, 0);   // dummy
            EmitReg(fs, AddPropertyName(fs, id->name));
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
    }
}

// -----------------------------------------------------------------------------
// Function compilation
// -----------------------------------------------------------------------------
FunctionInfo* BytecodeGenerator::CompileFunction(FnState* parent_fs,
                                                   Scope* fn_scope,
                                                   std::string_view name,
                                                   SmallVector<Parameter, 4> params,
                                                   Stmt* body,
                                                   Expr* expr_body,
                                                   bool is_toplevel) {
    (void)parent_fs;
    FunctionInfo* fi = program_->NewFunction(std::string(name));
    FnState fs;
    fs.info = fi;
    fs.scope = fn_scope;
    // Parameters occupy registers 0..N-1.
    fs.next_temp = static_cast<uint8_t>(params.size());

    // Assign context slot indices for captured variables declared in this
    // function's scope. Walk fn_scope->bindings(); each binding with
    // is_captured=true gets a context slot.
    uint16_t ctx_slot = 0;
    for (auto& b : fn_scope->bindings()) {
        if (b.is_captured) {
            const_cast<Binding&>(b).context_index = ctx_slot++;
        }
    }
    fi->num_context_vars = ctx_slot;

    // If this function has captured variables, emit a CreateContext at
    // function entry. The new context becomes the running context for
    // the body. We also stash the parent's context by linking it.
    if (ctx_slot > 0) {
        // CreateContext imm16 slot_count, idx
        EmitOp(&fs, Op::CreateContext);
        EmitImm16(&fs, ctx_slot);
        EmitIdx(&fs, AllocFeedbackSlot(&fs));
        // The new context is in acc. Push it as the current context.
        EmitOp(&fs, Op::PushContext);
        EmitIdx(&fs, AllocFeedbackSlot(&fs));
        // Copy captured variables from their parent-context slots into
        // the new context. For variables declared *in this function*
        // that are captured, they start as locals — the body will
        // write to them via StoreContext, which writes to the local
        // context. So no copy is needed at function entry.
        // (Variables captured from enclosing functions are read via the
        //  parent context link; no copy needed.)
    }

    // Initialize parameters: each parameter already lives in its assigned
    // register (the caller placed it there). If a parameter has a default
    // value, we'd emit code to check for undefined and evaluate the default.
    // For now, no defaults are supported by the generator.

    // Compile the body.
    if (body) {
        EmitStmt(&fs, body);
    } else if (expr_body) {
        // Arrow function with expression body: evaluate and return.
        EmitExpr(&fs, expr_body);
    }
    // Fall-off-the-end: return undefined.
    EmitOp(&fs, Op::ReturnUndefined);

    fi->num_parameters = static_cast<uint16_t>(params.size());
    fi->num_registers = fs.next_temp > 0 ? fs.next_temp : 1;
    return fi;
}

// -----------------------------------------------------------------------------
// Statement emission
// -----------------------------------------------------------------------------
void BytecodeGenerator::EmitStmt(FnState* fs, Stmt* s) {
    if (s == nullptr) return;
    // If this node has an associated scope (set by the scope analyzer),
    // switch to it for the duration of this statement. This matters for
    // blocks, for-loops, catch clauses, etc. — they introduce a new
    // scope for let/const declarations.
    Scope* node_scope = scope_analyzer_->GetScope(s);
    Scope* saved_scope = nullptr;
    if (node_scope != nullptr && node_scope != fs->scope) {
        saved_scope = fs->scope;
        fs->scope = node_scope;
    }
    switch (s->kind) {
        case AstKind::kBlock:
            EmitBlock(fs, static_cast<Block*>(s));
            break;
        case AstKind::kVarDecl:
        case AstKind::kLetDecl:
        case AstKind::kConstDecl: {
            VarDeclStmt* vd = static_cast<VarDeclStmt*>(s);
            for (auto& decl : vd->declarations) {
                if (decl.init) {
                    EmitExpr(fs, decl.init);
                    Identifier id(decl.range, decl.name);
                    EmitStoreIdentifier(fs, &id);
                }
            }
            break;
        }
        case AstKind::kFunctionDecl: {
            FunctionDecl* fd = static_cast<FunctionDecl*>(s);
            // Compile the function body.
            Scope* fn_scope = scope_analyzer_->GetScope(fd);
            FunctionInfo* inner = CompileFunction(fs, fn_scope, fd->name,
                                                   fd->params, fd->body, nullptr, false);
            // CreateClosure: acc = closure of function_info[idx]
            uint32_t cidx = AddConstFunctionInfo(fs, inner);
            EmitOp(fs, Op::CreateClosure);
            EmitImm32(fs, cidx);
            // Bind the function name.
            Identifier id(fd->range, fd->name);
            EmitStoreIdentifier(fs, &id);
            break;
        }
        case AstKind::kClassDecl: {
            // Class declarations are simplified to a function-like compile.
            // For now we just emit a no-op (TODO: full class support).
            ClassDecl* cd = static_cast<ClassDecl*>(s);
            (void)cd;
            EmitOp(fs, Op::Nop);
            break;
        }
        case AstKind::kIf: {
            If* i = static_cast<If*>(s);
            EmitExpr(fs, i->cond);
            uint32_t jmp_to_else = EmitJump(fs, Op::JumpIfFalse);
            EmitStmt(fs, i->then_branch);
            if (i->else_branch) {
                uint32_t jmp_to_end = EmitJump(fs, Op::Jump);
                PatchJump(fs, jmp_to_else, Here(fs));
                EmitStmt(fs, i->else_branch);
                PatchJump(fs, jmp_to_end, Here(fs));
            } else {
                PatchJump(fs, jmp_to_else, Here(fs));
            }
            break;
        }
        case AstKind::kWhile: {
            While* w = static_cast<While*>(s);
            uint32_t loop_start = Here(fs);
            fs->loops.push_back({{}, {}, loop_start});
            EmitExpr(fs, w->cond);
            uint32_t jmp_to_end = EmitJump(fs, Op::JumpIfFalse);
            EmitStmt(fs, w->body);
            // Back-edge to the condition test.
            EmitOp(fs, Op::JumpLoop);
            EmitImm32(fs, loop_start);
            // Patch the exit jump and any break/continue jumps.
            uint32_t loop_end = Here(fs);
            PatchJump(fs, jmp_to_end, loop_end);
            for (uint32_t b : fs->loops.back().breaks) {
                PatchJump(fs, b, loop_end);
            }
            for (uint32_t c : fs->loops.back().continues) {
                PatchJump(fs, c, loop_start);
            }
            fs->loops.pop_back();
            break;
        }
        case AstKind::kDoWhile: {
            DoWhile* d = static_cast<DoWhile*>(s);
            uint32_t loop_start = Here(fs);
            fs->loops.push_back({{}, {}, loop_start});
            EmitStmt(fs, d->body);
            uint32_t cond_start = Here(fs);
            EmitExpr(fs, d->cond);
            uint32_t jmp_to_start = EmitJump(fs, Op::JumpIfTrue);
            PatchJump(fs, jmp_to_start, loop_start);
            uint32_t loop_end = Here(fs);
            for (uint32_t b : fs->loops.back().breaks) {
                PatchJump(fs, b, loop_end);
            }
            for (uint32_t c : fs->loops.back().continues) {
                PatchJump(fs, c, cond_start);
            }
            fs->loops.pop_back();
            break;
        }
        case AstKind::kFor: {
            For* f = static_cast<For*>(s);
            if (f->init) EmitStmt(fs, f->init);
            uint32_t loop_start = Here(fs);
            uint32_t cond_jmp = 0;
            if (f->cond) {
                EmitExpr(fs, f->cond);
                cond_jmp = EmitJump(fs, Op::JumpIfFalse);
            }
            fs->loops.push_back({{}, {}, loop_start});
            EmitStmt(fs, f->body);
            uint32_t update_target = Here(fs);
            if (f->update) EmitExpr(fs, f->update);
            EmitOp(fs, Op::JumpLoop);
            EmitImm32(fs, loop_start);
            uint32_t loop_end = Here(fs);
            if (f->cond) PatchJump(fs, cond_jmp, loop_end);
            for (uint32_t b : fs->loops.back().breaks) {
                PatchJump(fs, b, loop_end);
            }
            for (uint32_t c : fs->loops.back().continues) {
                PatchJump(fs, c, update_target);
            }
            fs->loops.pop_back();
            break;
        }
        case AstKind::kReturn: {
            Return* r = static_cast<Return*>(s);
            if (r->value) {
                EmitExpr(fs, r->value);
                EmitOp(fs, Op::Return);
            } else {
                EmitOp(fs, Op::ReturnUndefined);
            }
            break;
        }
        case AstKind::kThrow: {
            Throw* t = static_cast<Throw*>(s);
            EmitExpr(fs, t->value);
            EmitOp(fs, Op::Throw);
            break;
        }
        case AstKind::kBreak: {
            if (!fs->loops.empty()) {
                uint32_t j = EmitJump(fs, Op::Jump);
                fs->loops.back().breaks.push_back(j);
            }
            break;
        }
        case AstKind::kContinue: {
            if (!fs->loops.empty()) {
                uint32_t j = EmitJump(fs, Op::Jump);
                fs->loops.back().continues.push_back(j);
            }
            break;
        }
        case AstKind::kEmpty:
            break;
        case AstKind::kDebugger:
            EmitOp(fs, Op::Debugger);
            break;
        case AstKind::kExpressionStatement: {
            ExpressionStatement* es = static_cast<ExpressionStatement*>(s);
            EmitExpr(fs, es->expr);
            break;
        }
        case AstKind::kTry:
        case AstKind::kSwitch:
        case AstKind::kLabeled:
        case AstKind::kForIn:
        case AstKind::kForOf:
            // Not yet implemented; emit a nop.
            EmitOp(fs, Op::Nop);
            break;
        default:
            EmitOp(fs, Op::Nop);
            break;
    }
    // Restore the saved scope (if we switched).
    if (saved_scope != nullptr) {
        fs->scope = saved_scope;
    }
}

void BytecodeGenerator::EmitBlock(FnState* fs, Block* b) {
    for (Stmt* s : b->statements) {
        EmitStmt(fs, s);
    }
}

// -----------------------------------------------------------------------------
// Expression emission
// -----------------------------------------------------------------------------
void BytecodeGenerator::EmitExpr(FnState* fs, Expr* e) {
    if (e == nullptr) {
        EmitOp(fs, Op::LdaUndefined);
        return;
    }
    switch (e->kind) {
        case AstKind::kNumberLiteral: {
            NumberLiteral* n = static_cast<NumberLiteral*>(e);
            // Smi shortcut: if the value is an integer in Smi range, use LdaSmi.
            if (n->value == static_cast<double>(static_cast<intptr_t>(n->value)) &&
                n->value >= -1e18 && n->value <= 1e18) {
                intptr_t v = static_cast<intptr_t>(n->value);
                if (v >= 0 && v <= 127) {
                    if (v == 0) {
                        EmitOp(fs, Op::LdaZero);
                    } else {
                        EmitOp(fs, Op::LdaSmi);
                        EmitImm8(fs, static_cast<uint8_t>(v));
                    }
                } else {
                    uint32_t idx = AddConstSmi(fs, v);
                    EmitOp(fs, Op::LdaConst);
                    EmitImm32(fs, idx);
                }
            } else {
                uint32_t idx = AddConstNumber(fs, n->value);
                EmitOp(fs, Op::LdaConst);
                EmitImm32(fs, idx);
            }
            return;
        }
        case AstKind::kStringLiteral: {
            StringLiteral* s = static_cast<StringLiteral*>(e);
            uint32_t idx = AddConstString(fs, s->value);
            EmitOp(fs, Op::LdaConst);
            EmitImm32(fs, idx);
            return;
        }
        case AstKind::kBoolLiteral: {
            BoolLiteral* b = static_cast<BoolLiteral*>(e);
            EmitOp(fs, b->value ? Op::LdaTrue : Op::LdaFalse);
            return;
        }
        case AstKind::kNullLiteral:
            EmitOp(fs, Op::LdaNull);
            return;
        case AstKind::kUndefinedLiteral:
            EmitOp(fs, Op::LdaUndefined);
            return;
        case AstKind::kThis:
            EmitOp(fs, Op::LdaThis);
            return;
        case AstKind::kIdentifier:
            EmitLoadIdentifier(fs, static_cast<Identifier*>(e));
            return;
        case AstKind::kBinaryOp: {
            BinaryOp* b = static_cast<BinaryOp*>(e);
            // Evaluate left into acc, spill to a temp, evaluate right into acc,
            // then emit the binary op with the temp as the second operand.
            EmitExpr(fs, b->left);
            uint8_t tmp = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, tmp);
            EmitExpr(fs, b->right);
            // Now acc = right, tmp = left. The op form is `op reg, idx`.
            // But we want `acc = op(left, right) = op(tmp, acc)`. The bytecode
            // semantics is `acc = acc <op> reg`, which would compute
            // right <op> left — wrong for non-commutative ops like Sub.
            //
            // Solution: swap. Move acc (right) into a second temp, load tmp
            // (left) into acc, then emit `op right_tmp, idx`.
            uint8_t tmp2 = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, tmp2);
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, tmp);
            // Now acc = left, tmp2 = right.
            Op op = Op::Illegal;
            switch (b->op_token) {
                case static_cast<int>(TokenKind::kPlus):    op = Op::Add; break;
                case static_cast<int>(TokenKind::kMinus):   op = Op::Sub; break;
                case static_cast<int>(TokenKind::kStar):    op = Op::Mul; break;
                case static_cast<int>(TokenKind::kSlash):   op = Op::Div; break;
                case static_cast<int>(TokenKind::kPercent): op = Op::Mod; break;
                case static_cast<int>(TokenKind::kExp):     op = Op::Exp; break;
                case static_cast<int>(TokenKind::kBitOr):   op = Op::BitOr; break;
                case static_cast<int>(TokenKind::kBitAnd):  op = Op::BitAnd; break;
                case static_cast<int>(TokenKind::kBitXor):  op = Op::BitXor; break;
                case static_cast<int>(TokenKind::kShl):     op = Op::Shl; break;
                case static_cast<int>(TokenKind::kShr):     op = Op::Shr; break;
                case static_cast<int>(TokenKind::kUshr):    op = Op::Ushr; break;
                case static_cast<int>(TokenKind::kEq):      op = Op::TestEqual; break;
                case static_cast<int>(TokenKind::kNotEq):   op = Op::TestNotEqual; break;
                case static_cast<int>(TokenKind::kEqEq):    op = Op::TestEqStrict; break;
                case static_cast<int>(TokenKind::kNotEqEq): op = Op::TestNotEqStrict; break;
                case static_cast<int>(TokenKind::kLt):      op = Op::TestLessThan; break;
                case static_cast<int>(TokenKind::kGt):      op = Op::TestGreaterThan; break;
                case static_cast<int>(TokenKind::kLe):      op = Op::TestLessThanOrEqual; break;
                case static_cast<int>(TokenKind::kGe):      op = Op::TestGreaterThanOrEqual; break;
                default:
                    EmitOp(fs, Op::Illegal);
                    return;
            }
            EmitOp(fs, op);
            EmitReg(fs, tmp2);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        }
        case AstKind::kUnaryOp: {
            UnaryOp* u = static_cast<UnaryOp*>(e);
            EmitExpr(fs, u->operand);
            switch (u->op_token) {
                case static_cast<int>(TokenKind::kMinus):  EmitOp(fs, Op::Negate); EmitIdx(fs, AllocFeedbackSlot(fs)); break;
                case static_cast<int>(TokenKind::kPlus):   /* unary plus is ToNumber */
                    EmitOp(fs, Op::Negate); EmitIdx(fs, AllocFeedbackSlot(fs));
                    EmitOp(fs, Op::Negate); EmitIdx(fs, AllocFeedbackSlot(fs)); break;
                case static_cast<int>(TokenKind::kBitNot): EmitOp(fs, Op::BitNot); EmitIdx(fs, AllocFeedbackSlot(fs)); break;
                case static_cast<int>(TokenKind::kNot):    EmitOp(fs, Op::LogicalNot); break;
                case static_cast<int>(TokenKind::kTypeof): EmitOp(fs, Op::Typeof); break;
                default: EmitOp(fs, Op::Illegal); break;
            }
            return;
        }
        case AstKind::kUpdateOp: {
            // ++x / x++ / --x / x--
            UpdateOp* u = static_cast<UpdateOp*>(e);
            // Load current value.
            EmitExpr(fs, u->operand);
            // Save the original value if this is postfix.
            uint8_t tmp_old = 0;
            if (!u->is_prefix) {
                tmp_old = AllocTemp(fs);
                EmitOp(fs, Op::Star);
                EmitReg(fs, tmp_old);
            }
            // Compute new value: acc = acc + 1 (or -1).
            EmitOp(fs, static_cast<TokenKind>(u->op_token) == TokenKind::kInc ? Op::Inc : Op::Dec);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            // Store back to the operand (assignment).
            if (u->operand->kind == AstKind::kIdentifier) {
                EmitStoreIdentifier(fs, static_cast<Identifier*>(u->operand));
            }
            // For postfix, the result is the original value.
            if (!u->is_prefix) {
                EmitOp(fs, Op::Ldar);
                EmitReg(fs, tmp_old);
            }
            return;
        }
        case AstKind::kAssignment: {
            Assignment* a = static_cast<Assignment*>(e);
            EmitExpr(fs, a->value);
            // For compound assignments (+=, etc.), we need to evaluate the
            // target, then apply the op. For now, only handle plain `=`.
            if (static_cast<TokenKind>(a->op_token) == TokenKind::kAssign) {
                if (a->target->kind == AstKind::kIdentifier) {
                    // Store acc to the variable; acc is also the result.
                    Identifier* id = static_cast<Identifier*>(a->target);
                    EmitStoreIdentifier(fs, id);
                    // EmitStoreIdentifier leaves acc as the stored value
                    // (because Star/StoreGlobal don't clobber acc).
                    // Actually, Star DOES overwrite acc with the value
                    // being stored — which is what we want.
                } else if (a->target->kind == AstKind::kMember) {
                    Member* m = static_cast<Member*>(a->target);
                    // At this point acc = a->value (the RHS). We need to
                    // evaluate m->object without losing the value, so:
                    //   1. Spill the value to val_tmp.
                    //   2. Evaluate m->object into acc, spill to obj_tmp.
                    //   3. Ldar obj_tmp; StoreProperty val_tmp, name.
                    uint8_t val_tmp = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, val_tmp);
                    // Evaluate the object.
                    EmitExpr(fs, m->object);
                    uint8_t obj_tmp = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, obj_tmp);
                    if (m->is_computed) {
                        EmitExpr(fs, m->property);
                        uint8_t key_tmp = AllocTemp(fs);
                        EmitOp(fs, Op::Star);
                        EmitReg(fs, key_tmp);
                        // acc = obj_tmp; StoreIndexed key_tmp, val_tmp
                        EmitOp(fs, Op::Ldar);
                        EmitReg(fs, obj_tmp);
                        // We don't have a StoreIndexed with computed key
                        // in our opcode table — fall through to nop for now.
                        EmitOp(fs, Op::Nop);
                    } else {
                        // a.b = val
                        Identifier* key = static_cast<Identifier*>(m->property);
                        EmitOp(fs, Op::Ldar);
                        EmitReg(fs, obj_tmp);
                        EmitOp(fs, Op::StoreProperty);
                        EmitReg(fs, val_tmp);
                        EmitReg(fs, AddPropertyName(fs, key->name));
                        EmitIdx(fs, AllocFeedbackSlot(fs));
                    }
                    // Result is the assigned value.
                    EmitOp(fs, Op::Ldar);
                    EmitReg(fs, val_tmp);
                }
            } else {
                // Compound assignment: desugar `x op= y` to `x = x op y`.
                // For simplicity, only identifier targets supported.
                if (a->target->kind == AstKind::kIdentifier) {
                    Identifier* id = static_cast<Identifier*>(a->target);
                    // Save the RHS value into a temp.
                    uint8_t rhs_tmp = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, rhs_tmp);
                    // Load the current value of x.
                    EmitLoadIdentifier(fs, id);
                    // Spill to temp.
                    uint8_t lhs_tmp = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, lhs_tmp);
                    // Load RHS.
                    EmitOp(fs, Op::Ldar);
                    EmitReg(fs, rhs_tmp);
                    // Swap so acc = lhs, rhs_tmp = rhs.
                    uint8_t rhs_tmp2 = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, rhs_tmp2);
                    EmitOp(fs, Op::Ldar);
                    EmitReg(fs, lhs_tmp);
                    // Now acc = lhs, rhs_tmp2 = rhs.
                    Op op = Op::Illegal;
                    switch (a->op_token) {
                        case static_cast<int>(TokenKind::kAddAssign): op = Op::Add; break;
                        case static_cast<int>(TokenKind::kSubAssign): op = Op::Sub; break;
                        case static_cast<int>(TokenKind::kMulAssign): op = Op::Mul; break;
                        case static_cast<int>(TokenKind::kDivAssign): op = Op::Div; break;
                        case static_cast<int>(TokenKind::kModAssign): op = Op::Mod; break;
                        case static_cast<int>(TokenKind::kBitAndAssign): op = Op::BitAnd; break;
                        case static_cast<int>(TokenKind::kBitOrAssign):  op = Op::BitOr; break;
                        case static_cast<int>(TokenKind::kBitXorAssign): op = Op::BitXor; break;
                        case static_cast<int>(TokenKind::kShlAssign):    op = Op::Shl; break;
                        case static_cast<int>(TokenKind::kShrAssign):    op = Op::Shr; break;
                        case static_cast<int>(TokenKind::kUshrAssign):   op = Op::Ushr; break;
                        default: EmitOp(fs, Op::Illegal); return;
                    }
                    EmitOp(fs, op);
                    EmitReg(fs, rhs_tmp2);
                    EmitIdx(fs, AllocFeedbackSlot(fs));
                    // Store back.
                    EmitStoreIdentifier(fs, id);
                }
            }
            return;
        }
        case AstKind::kLogicalOp: {
            LogicalOp* l = static_cast<LogicalOp*>(e);
            EmitExpr(fs, l->left);
            if (static_cast<TokenKind>(l->op_token) == TokenKind::kAnd) {
                // short-circuit: if acc is falsy, skip right.
                uint32_t j = EmitJump(fs, Op::JumpIfToBooleanFalse);
                EmitExpr(fs, l->right);
                PatchJump(fs, j, Here(fs));
            } else if (static_cast<TokenKind>(l->op_token) == TokenKind::kOr) {
                uint32_t j = EmitJump(fs, Op::JumpIfToBooleanTrue);
                EmitExpr(fs, l->right);
                PatchJump(fs, j, Here(fs));
            } else {
                // Nullish coalescing: if acc is null or undefined, eval right.
                uint32_t j = EmitJump(fs, Op::JumpIfNotNullOrUndefined);
                EmitExpr(fs, l->right);
                PatchJump(fs, j, Here(fs));
            }
            return;
        }
        case AstKind::kConditional: {
            Conditional* c = static_cast<Conditional*>(e);
            EmitExpr(fs, c->cond);
            uint32_t j_else = EmitJump(fs, Op::JumpIfToBooleanFalse);
            EmitExpr(fs, c->then_expr);
            uint32_t j_end = EmitJump(fs, Op::Jump);
            PatchJump(fs, j_else, Here(fs));
            EmitExpr(fs, c->else_expr);
            PatchJump(fs, j_end, Here(fs));
            return;
        }
        case AstKind::kSequence: {
            Sequence* sq = static_cast<Sequence*>(e);
            for (size_t i = 0; i < sq->expressions.size(); ++i) {
                EmitExpr(fs, sq->expressions[i]);
            }
            return;
        }
        case AstKind::kCall: {
            Call* c = static_cast<Call*>(e);
            // Special case: if the callee is a Member (obj.method), use
            // CallProperty so that `this` is bound correctly.
            if (c->callee->kind == AstKind::kMember) {
                Member* m = static_cast<Member*>(c->callee);
                if (!m->is_computed) {
                    // obj.method(args)
                    // Evaluate the object into a temp (this becomes `this`
                    // for the call, and also the lookup target).
                    EmitExpr(fs, m->object);
                    uint8_t recv_tmp = AllocTemp(fs);
                    EmitOp(fs, Op::Star);
                    EmitReg(fs, recv_tmp);
                    // Reserve arg slots.
                    uint8_t first_arg = fs->next_temp;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        (void)AllocTemp(fs);
                    }
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        EmitExpr(fs, c->args[i]);
                        EmitOp(fs, Op::Star);
                        EmitReg(fs, static_cast<uint8_t>(first_arg + i));
                    }
                    uint16_t argc = static_cast<uint16_t>(c->args.size());
                    // Load the receiver back into acc.
                    EmitOp(fs, Op::Ldar);
                    EmitReg(fs, recv_tmp);
                    // CallProperty format: argc:16  prop_idx:8  first_arg:8  idx:16
                    Identifier* key = static_cast<Identifier*>(m->property);
                    EmitOp(fs, Op::CallProperty);
                    EmitImm16(fs, argc);
                    EmitReg(fs, AddPropertyName(fs, key->name));
                    EmitReg(fs, first_arg);
                    EmitIdx(fs, AllocFeedbackSlot(fs));
                    return;
                }
                // Computed member call: obj[expr](args) — fall through to
                // regular Call after evaluating obj[expr] into acc.
                EmitExpr(fs, c->callee);
                // ... then regular call path below.
            }
            // Regular call: evaluate callee into acc, spill, evaluate args,
            // then Call.
            // Regular call: evaluate callee into acc, spill, evaluate args,
            // then Call.
            EmitExpr(fs, c->callee);
            uint8_t callee_tmp = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, callee_tmp);
            // Reserve arg slots BEFORE evaluating the args.
            uint8_t first_arg = fs->next_temp;
            for (size_t i = 0; i < c->args.size(); ++i) {
                (void)AllocTemp(fs);
            }
            for (size_t i = 0; i < c->args.size(); ++i) {
                EmitExpr(fs, c->args[i]);
                EmitOp(fs, Op::Star);
                EmitReg(fs, static_cast<uint8_t>(first_arg + i));
            }
            uint16_t argc = static_cast<uint16_t>(c->args.size());
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, callee_tmp);
            EmitOp(fs, Op::Call);
            EmitImm16(fs, argc);
            EmitReg(fs, first_arg);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        }
        case AstKind::kNew: {
            NewExpr* n = static_cast<NewExpr*>(e);
            EmitExpr(fs, n->callee);
            uint8_t callee_tmp = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, callee_tmp);
            uint8_t first_arg = fs->next_temp;
            for (size_t i = 0; i < n->args.size(); ++i) {
                (void)AllocTemp(fs);
            }
            for (size_t i = 0; i < n->args.size(); ++i) {
                EmitExpr(fs, n->args[i]);
                EmitOp(fs, Op::Star);
                EmitReg(fs, static_cast<uint8_t>(first_arg + i));
            }
            uint16_t argc = static_cast<uint16_t>(n->args.size());
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, callee_tmp);
            EmitOp(fs, Op::Construct);
            EmitImm16(fs, argc);
            EmitReg(fs, first_arg);
            EmitIdx(fs, AllocFeedbackSlot(fs));
            return;
        }
        case AstKind::kMember: {
            Member* m = static_cast<Member*>(e);
            EmitExpr(fs, m->object);
            if (m->is_computed) {
                // a[b]: evaluate b, then LoadIndexed.
                // We need both operands in registers.
                uint8_t obj_tmp = AllocTemp(fs);
                EmitOp(fs, Op::Star);
                EmitReg(fs, obj_tmp);
                EmitExpr(fs, m->property);
                uint8_t key_tmp = AllocTemp(fs);
                EmitOp(fs, Op::Star);
                EmitReg(fs, key_tmp);
                EmitOp(fs, Op::Ldar);
                EmitReg(fs, obj_tmp);
                EmitOp(fs, Op::LoadIndexed);
                EmitReg(fs, key_tmp);
                EmitIdx(fs, AllocFeedbackSlot(fs));
            } else {
                // a.b
                Identifier* key = static_cast<Identifier*>(m->property);
                EmitOp(fs, Op::LoadProperty);
                EmitReg(fs, AddPropertyName(fs, key->name));
                EmitIdx(fs, AllocFeedbackSlot(fs));
            }
            return;
        }
        case AstKind::kArrayLiteral: {
            ArrayLiteral* a = static_cast<ArrayLiteral*>(e);
            // NewArray imm16 initial_capacity
            uint16_t cap = static_cast<uint16_t>(a->elements.size());
            if (cap == 0) cap = 4;
            EmitOp(fs, Op::NewArray);
            EmitImm16(fs, cap);
            // Spill to a temp so we can push elements.
            uint8_t arr_tmp = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, arr_tmp);
            // For each element, evaluate into acc and PushArray.
            for (Expr* el : a->elements) {
                if (el->kind == AstKind::kSpread) {
                    // Spread not yet supported.
                    EmitOp(fs, Op::Nop);
                    continue;
                }
                EmitExpr(fs, el);
                // PushArray: reg = arr_tmp, idx feedback
                EmitOp(fs, Op::PushArray);
                EmitReg(fs, arr_tmp);
                EmitIdx(fs, AllocFeedbackSlot(fs));
            }
            // Result is the array.
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, arr_tmp);
            return;
        }
        case AstKind::kObjectLiteral: {
            ObjectLiteral* o = static_cast<ObjectLiteral*>(e);
            EmitOp(fs, Op::NewObject);
            uint8_t obj_tmp = AllocTemp(fs);
            EmitOp(fs, Op::Star);
            EmitReg(fs, obj_tmp);
            for (auto& p : o->properties) {
                if (p.is_computed) {
                    continue;
                }
                if (p.is_get || p.is_set) {
                    continue;
                }
                // Evaluate the value.
                EmitExpr(fs, p.value);
                uint8_t val_tmp = AllocTemp(fs);
                EmitOp(fs, Op::Star);
                EmitReg(fs, val_tmp);
                // The key: if it's an Identifier, use its name; if a
                // StringLiteral, use its value.
                std::string_view key_name;
                if (p.key->kind == AstKind::kIdentifier) {
                    key_name = static_cast<Identifier*>(p.key)->name;
                } else if (p.key->kind == AstKind::kStringLiteral) {
                    key_name = static_cast<StringLiteral*>(p.key)->value;
                } else {
                    continue;
                }
                EmitOp(fs, Op::Ldar);
                EmitReg(fs, obj_tmp);
                EmitOp(fs, Op::DefineProperty);
                EmitReg(fs, val_tmp);
                EmitReg(fs, AddPropertyName(fs, key_name));
            }
            EmitOp(fs, Op::Ldar);
            EmitReg(fs, obj_tmp);
            return;
        }
        case AstKind::kFunctionExpr: {
            FunctionExpr* fn = static_cast<FunctionExpr*>(e);
            Scope* fn_scope = scope_analyzer_->GetScope(fn);
            std::string_view name = fn->name;
            FunctionInfo* inner = CompileFunction(fs, fn_scope, name,
                                                   fn->params, fn->body, nullptr, false);
            uint32_t cidx = AddConstFunctionInfo(fs, inner);
            EmitOp(fs, Op::CreateClosure);
            EmitImm32(fs, cidx);
            return;
        }
        case AstKind::kArrowFunction: {
            ArrowFunction* af = static_cast<ArrowFunction*>(e);
            Scope* fn_scope = scope_analyzer_->GetScope(af);
            FunctionInfo* inner = CompileFunction(fs, fn_scope, "<arrow>",
                                                   af->params, af->block_body, af->expr_body,
                                                   false);
            uint32_t cidx = AddConstFunctionInfo(fs, inner);
            EmitOp(fs, Op::CreateClosure);
            EmitImm32(fs, cidx);
            return;
        }
        case AstKind::kClassExpr:
        case AstKind::kOptionalChain:
        case AstKind::kSpread:
        case AstKind::kYield:
        case AstKind::kSuper:
            // Not yet implemented.
            EmitOp(fs, Op::LdaUndefined);
            return;
        default:
            EmitOp(fs, Op::LdaUndefined);
            return;
    }
}

}  // namespace v12
