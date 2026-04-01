#pragma once
/*
 * ir_gen.hpp  –  REXC IR Generator (AST → SSA IR)
 *
 * Walks the typed AST produced by the parser / semantic analyser and
 * emits the SSA-form IR defined in ir.hpp.  Every local variable is
 * lowered with the alloca-load-store pattern:
 *
 *   %ptr = alloca T          // VarDecl
 *   store %init, %ptr        // initialiser (if any)
 *   %v   = load  %ptr        // every read of the variable
 *   store %new,  %ptr        // every write (assignment)
 *
 * Pipeline:  AST → SemanticAnalyzer → IRGenerator → IRModule
 *                                                   → IROptimizer → Emitter
 *
 * Phase 1 (v0.5): handles functions, scalar variables, arithmetic,
 * comparisons, if/while/for, calls, and return.
 *
 * Phase 2 (v0.6): adds class layout, constructors, destructors,
 * virtual method dispatch, and new/delete operators.
 */

#include "ir.hpp"
#include "ast.hpp"
#include "semantic.hpp"
#include "object_layout.hpp"
#include "mangler.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cassert>
#include <cstdint>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  IRGenerator
// ─────────────────────────────────────────────────────────────────

class IRGenerator {
public:
    explicit IRGenerator(const SemanticContext& ctx)
        : sema_ctx_(ctx) {}

    /// Convert a list of top-level declarations into an IRModule.
    IRModule generate(const std::vector<NodePtr>& decls) {
        module_ = IRModule{};
        for (auto& d : decls)
            emit_top_level(*d);
        return std::move(module_);
    }

private:
    // ── State ────────────────────────────────────────────────────
    const SemanticContext& sema_ctx_;
    IRModule              module_;
    IRFunction*           cur_fn_     = nullptr;  // function being built
    IRBlock*              cur_block_  = nullptr;  // block being appended to
    uint32_t              label_seq_  = 0;        // for unique label names

    // ── Class support (Phase 2 / v0.6) ───────────────────────────
    ObjectLayoutBuilder               layout_builder_;
    Mangler                           mangler_;
    std::unordered_map<std::string, ClassLayout> class_layouts_;
    std::string                       current_class_;  // class being compiled

    // Scope stack: variable name → alloca'd IRValue (pointer).
    // Each scope is a map; pushing/popping is done via vector index.
    struct VarScope {
        std::unordered_map<std::string, IRValue> vars;
    };
    std::vector<VarScope> scopes_;

    // ── Scope management ─────────────────────────────────────────

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope()  { scopes_.pop_back(); }

    void define_var(const std::string& name, IRValue alloca_ptr) {
        assert(!scopes_.empty());
        scopes_.back().vars[name] = alloca_ptr;
    }

    /// Walk scopes from innermost to outermost; returns nullptr if not found.
    const IRValue* lookup_var(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end())
                return &found->second;
        }
        return nullptr;
    }

    // ── SSA helpers ──────────────────────────────────────────────

    /// Generate a unique basic-block label with an optional hint.
    std::string fresh_label(const std::string& hint = "bb") {
        return hint + "." + std::to_string(label_seq_++);
    }

    /// Allocate a fresh SSA value from the current function.
    IRValue fresh_value(IRType type, const std::string& name = "") {
        assert(cur_fn_);
        return cur_fn_->new_value(type, name);
    }

    /// Append an instruction to the current block.
    void emit(IRInstr instr) {
        assert(cur_block_);
        cur_block_->instrs.push_back(std::move(instr));
    }

    /// Start a new basic block (switch cur_block_ to it).
    /// Labels are generated via fresh_label() and are guaranteed unique.
    void start_block(const std::string& label) {
        assert(cur_fn_);
        // Debug-mode check: labels produced by fresh_label() must be unique.
        for ([[maybe_unused]] auto& b : cur_fn_->blocks)
            assert(b.label != label && "duplicate block label");
        cur_fn_->new_block(label);
        cur_block_ = &cur_fn_->current();
    }

    /// Emit an unconditional branch to `target` and start `target` block.
    void emit_br_and_start(const std::string& target) {
        IRInstr br;
        br.op         = IROp::Br;
        br.label_true = target;
        emit(br);
        start_block(target);
    }

    // ── AST type → IRType mapping ────────────────────────────────

    IRType map_type(const Node* type_node) const {
        if (!type_node) return IRType::Int32;  // default fallback

        switch (type_node->kind) {
            case NodeKind::PrimitiveType: {
                auto* pt = type_node->as<const PrimitiveType>();
                return map_prim(pt->prim_kind);
            }
            case NodeKind::PointerType:
            case NodeKind::ReferenceType:
            case NodeKind::RValueRefType:
                return IRType::Ptr;
            case NodeKind::QualifiedType: {
                auto* qt = type_node->as<const QualifiedType>();
                return map_type(qt->inner.get());
            }
            case NodeKind::NamedType: {
                // User-defined types become pointers at the IR level for now.
                return IRType::Ptr;
            }
            case NodeKind::ArrayType:
            case NodeKind::TemplateInstType:
                return IRType::Ptr;
            default:
                return IRType::Int32;
        }
    }

    static IRType map_prim(PrimitiveType::Kind k) {
        switch (k) {
            case PrimitiveType::Kind::Void:       return IRType::Void;
            case PrimitiveType::Kind::Bool:        return IRType::Bool;
            case PrimitiveType::Kind::Char:
            case PrimitiveType::Kind::SChar:
            case PrimitiveType::Kind::UChar:       return IRType::Int8;
            case PrimitiveType::Kind::Short:
            case PrimitiveType::Kind::UShort:      return IRType::Int16;
            case PrimitiveType::Kind::Int:
            case PrimitiveType::Kind::UInt:        return IRType::Int32;
            case PrimitiveType::Kind::Long:
            case PrimitiveType::Kind::ULong:
            case PrimitiveType::Kind::LongLong:
            case PrimitiveType::Kind::ULongLong:
            case PrimitiveType::Kind::SizeT:       return IRType::Int64;
            case PrimitiveType::Kind::Float:       return IRType::Float32;
            case PrimitiveType::Kind::Double:
            case PrimitiveType::Kind::LongDouble:  return IRType::Float64;
            case PrimitiveType::Kind::WChar:       return IRType::Int16;
            case PrimitiveType::Kind::Auto:        return IRType::Int32;  // unresolved
            case PrimitiveType::Kind::Nullptr_t:   return IRType::Ptr;
        }
        return IRType::Int32;
    }

    // ═════════════════════════════════════════════════════════════
    //  Top-level declaration dispatch
    // ═════════════════════════════════════════════════════════════

    void emit_top_level(const Node& node) {
        switch (node.kind) {
            case NodeKind::FunctionDecl:
                emit_function(*node.as<const FunctionDecl>());
                break;
            case NodeKind::VarDecl:
                emit_global_var(*node.as<const VarDecl>());
                break;
            case NodeKind::NamespaceDecl: {
                auto& ns = *node.as<const NamespaceDecl>();
                for (auto& d : ns.decls)
                    emit_top_level(*d);
                break;
            }
            case NodeKind::TranslationUnit: {
                auto& tu = *node.as<const TranslationUnit>();
                for (auto& d : tu.decls)
                    emit_top_level(*d);
                break;
            }
            case NodeKind::DeclStmt: {
                auto& ds = *node.as<const DeclStmt>();
                for (auto& d : ds.decls)
                    emit_top_level(*d);
                break;
            }
            // ── Class support (Phase 2 / v0.6) ─────────────────────
            case NodeKind::ClassDecl:
            case NodeKind::StructDecl:
                emit_class(*node.as<const ClassDecl>());
                break;
            case NodeKind::ConstructorDecl:
            case NodeKind::DestructorDecl:
                // Constructors/destructors are emitted as part of emit_class.
                break;
            case NodeKind::UnionDecl:
            case NodeKind::TemplateDecl:
            case NodeKind::EnumDecl:
            case NodeKind::UsingDecl:
            case NodeKind::TypedefDecl:
            case NodeKind::PreprocessorDecl:
            case NodeKind::StaticAssertDecl:
            case NodeKind::FriendDecl:
                break;
            default:
                break;
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  Declaration visitors
    // ═════════════════════════════════════════════════════════════

    void emit_function(const FunctionDecl& fn) {
        // Skip declarations without a body (forward declarations).
        if (!fn.body) return;
        // Skip deleted/defaulted functions.
        if (fn.is_deleted || fn.is_defaulted) return;

        IRFunction ir_fn;
        ir_fn.name        = fn.name;
        ir_fn.return_type = map_type(fn.return_type.get());

        // Build parameter list.
        for (auto& p : fn.params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            IRType pt = map_type(pd->type.get());
            ir_fn.params.emplace_back(pd->name, pt);
        }

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;

        // Create entry block.
        start_block(fresh_label("entry"));

        push_scope();

        // Alloca + Store for each parameter so they are mutable locals.
        for (auto& [pname, ptype] : cur_fn_->params) {
            if (pname.empty()) continue;
            IRValue alloca_ptr = fresh_value(IRType::Ptr, pname + ".addr");
            {
                IRInstr a;
                a.op     = IROp::Alloca;
                a.result = alloca_ptr;
                // Store the allocated type size hint in const_int.
                a.const_int = static_cast<int64_t>(ir_type_size(ptype));
                emit(a);
            }
            {
                IRValue param_val = fresh_value(ptype, pname);
                IRInstr s;
                s.op = IROp::Store;
                s.operands.push_back(param_val);   // value
                s.operands.push_back(alloca_ptr);   // destination ptr
                emit(s);
            }
            define_var(pname, alloca_ptr);
        }

        // Emit body.
        emit_stmt(*fn.body);

        // If the last block has no terminator, emit an implicit void return.
        if (cur_block_ && (cur_block_->instrs.empty() ||
            (cur_block_->instrs.back().op != IROp::Ret &&
             cur_block_->instrs.back().op != IROp::Br  &&
             cur_block_->instrs.back().op != IROp::BrCond))) {
            IRInstr ret;
            ret.op = IROp::Ret;
            emit(ret);
        }

        pop_scope();
        cur_fn_    = nullptr;
        cur_block_ = nullptr;
    }

    void emit_global_var(const VarDecl& var) {
        // Global variables are stored as named constant data for now.
        // Initialiser with an int literal is supported; everything else
        // is recorded as an uninitialised global.
        std::string value;
        if (var.init) {
            if (var.init->kind == NodeKind::IntLiteralExpr) {
                auto* lit = var.init->as<const IntLiteralExpr>();
                value = std::to_string(lit->value);
            } else if (var.init->kind == NodeKind::StringLiteralExpr) {
                auto* lit = var.init->as<const StringLiteralExpr>();
                value = lit->cooked;
            }
        }
        module_.globals.emplace_back(var.name, value);
    }

    // ── Phase 2 (v0.6): Class IR emission ────────────────────────

    /// Emit IR for a class declaration: compute layout, emit constructor,
    /// destructor, member functions, and vtable initialisation.
    void emit_class(const ClassDecl& cls) {
        // Build layout using ObjectLayoutBuilder.
        ClassLayout layout = layout_builder_.build(cls, sema_ctx_);
        class_layouts_[cls.name] = layout;

        // If the class has a vtable, emit a global vtable symbol.
        if (layout.has_vtable) {
            // Vtable is represented as a global constant (array of fn ptrs).
            // For now, store as a named global; the backend resolves addresses.
            std::string vtable_data;
            for (auto& vt_entry : layout.vtable) {
                if (!vtable_data.empty()) vtable_data += ',';
                vtable_data += vt_entry.mangled_name;
            }
            module_.globals.emplace_back(layout.vtable_symbol, vtable_data);
        }

        current_class_ = cls.name;

        // Walk members and emit each function/constructor/destructor.
        bool has_explicit_ctor = false;
        bool has_explicit_dtor = false;
        for (auto& member : cls.members) {
            if (!member) continue;
            switch (member->kind) {
                case NodeKind::ConstructorDecl: {
                    emit_class_ctor(*member->as<const FunctionDecl>(), layout);
                    has_explicit_ctor = true;
                    break;
                }
                case NodeKind::DestructorDecl: {
                    emit_class_dtor(*member->as<const FunctionDecl>(), layout);
                    has_explicit_dtor = true;
                    break;
                }
                case NodeKind::FunctionDecl: {
                    emit_class_method(*member->as<const FunctionDecl>(), layout);
                    break;
                }
                default:
                    break;  // FieldDecl, AccessSpecifier, etc. handled by layout
            }
        }

        // Emit implicit default constructor if none was provided.
        if (!has_explicit_ctor) {
            emit_default_ctor(cls.name, layout);
        }
        // Emit implicit destructor if none was provided.
        if (!has_explicit_dtor) {
            emit_default_dtor(cls.name, layout);
        }

        current_class_.clear();
    }

    /// Emit an explicit constructor as an IR function.
    void emit_class_ctor(const FunctionDecl& fn, const ClassLayout& layout) {
        IRFunction ir_fn;
        ir_fn.name = mangler_.mangle_ctor(
            current_class_,
            params_to_irtypes(fn.params));
        ir_fn.return_type = IRType::Void;
        // First param is always 'this' pointer.
        ir_fn.params.emplace_back("this", IRType::Ptr);
        for (auto& p : fn.params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            ir_fn.params.emplace_back(pd->name, map_type(pd->type.get()));
        }

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;
        start_block(fresh_label("entry"));
        push_scope();

        // Store 'this' parameter as local variable.
        IRValue this_alloca = fresh_value(IRType::Ptr, "this.addr");
        { IRInstr ai; ai.op = IROp::Alloca; ai.result = this_alloca; emit(ai); }
        {
            IRValue this_param;
            this_param.id   = 0; // placeholder — will be resolved by param binding
            this_param.type = IRType::Ptr;
            this_param.name = "this";
            // The parameter value is available as the 1st param.
            IRValue pv = cur_fn_->new_value(IRType::Ptr, "this.val");
            // Store through param binding.
            IRInstr si; si.op = IROp::Store;
            si.operands = {pv, this_alloca};
            emit(si);
        }
        define_var("this", this_alloca);

        // Store other params.
        for (auto& p : fn.params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            if (pd->name.empty()) continue;
            IRType pt = map_type(pd->type.get());
            IRValue pa = fresh_value(IRType::Ptr, pd->name + ".addr");
            { IRInstr ai; ai.op = IROp::Alloca; ai.result = pa; emit(ai); }
            IRValue pv = fresh_value(pt, pd->name + ".param");
            { IRInstr si; si.op = IROp::Store; si.operands = {pv, pa}; emit(si); }
            define_var(pd->name, pa);
        }

        // Initialise vtable pointer if class has one.
        if (layout.has_vtable) {
            emit_vtable_init(this_alloca, layout);
        }

        // Emit constructor body if present.
        if (fn.body) {
            emit_stmt(*fn.body);
        }

        // Implicit void return.
        if (cur_block_ && (cur_block_->instrs.empty() ||
            cur_block_->instrs.back().op != IROp::Ret)) {
            IRInstr ret; ret.op = IROp::Ret; emit(ret);
        }

        pop_scope();
        cur_fn_ = nullptr; cur_block_ = nullptr;
    }

    /// Emit an explicit destructor as an IR function.
    void emit_class_dtor(const FunctionDecl& fn, const ClassLayout& layout) {
        IRFunction ir_fn;
        ir_fn.name = mangler_.mangle_dtor(current_class_);
        ir_fn.return_type = IRType::Void;
        ir_fn.params.emplace_back("this", IRType::Ptr);

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;
        start_block(fresh_label("entry"));
        push_scope();

        IRValue this_alloca = fresh_value(IRType::Ptr, "this.addr");
        { IRInstr ai; ai.op = IROp::Alloca; ai.result = this_alloca; emit(ai); }
        { IRValue pv = cur_fn_->new_value(IRType::Ptr, "this.val");
          IRInstr si; si.op = IROp::Store; si.operands = {pv, this_alloca}; emit(si); }
        define_var("this", this_alloca);

        // Emit destructor body.
        if (fn.body) {
            emit_stmt(*fn.body);
        }

        if (cur_block_ && (cur_block_->instrs.empty() ||
            cur_block_->instrs.back().op != IROp::Ret)) {
            IRInstr ret; ret.op = IROp::Ret; emit(ret);
        }

        pop_scope();
        cur_fn_ = nullptr; cur_block_ = nullptr;
    }

    /// Emit a member function (non-ctor/dtor) as an IR function.
    void emit_class_method(const FunctionDecl& fn, const ClassLayout& layout) {
        if (!fn.body) return;
        if (fn.is_deleted || fn.is_defaulted) return;

        auto ptypes = params_to_irtypes(fn.params);
        IRFunction ir_fn;
        ir_fn.name = mangler_.mangle_method(current_class_, fn.name, ptypes);
        ir_fn.return_type = map_type(fn.return_type.get());
        // First param is 'this'.
        ir_fn.params.emplace_back("this", IRType::Ptr);
        for (auto& p : fn.params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            ir_fn.params.emplace_back(pd->name, map_type(pd->type.get()));
        }

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;
        start_block(fresh_label("entry"));
        push_scope();

        // Alloca + Store for 'this'.
        IRValue this_alloca = fresh_value(IRType::Ptr, "this.addr");
        { IRInstr ai; ai.op = IROp::Alloca; ai.result = this_alloca; emit(ai); }
        { IRValue pv = cur_fn_->new_value(IRType::Ptr, "this.val");
          IRInstr si; si.op = IROp::Store; si.operands = {pv, this_alloca}; emit(si); }
        define_var("this", this_alloca);

        // Other params.
        for (auto& p : fn.params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            if (pd->name.empty()) continue;
            IRType pt = map_type(pd->type.get());
            IRValue pa = fresh_value(IRType::Ptr, pd->name + ".addr");
            { IRInstr ai; ai.op = IROp::Alloca; ai.result = pa; emit(ai); }
            IRValue pv = fresh_value(pt, pd->name + ".param");
            { IRInstr si; si.op = IROp::Store; si.operands = {pv, pa}; emit(si); }
            define_var(pd->name, pa);
        }

        emit_stmt(*fn.body);

        if (cur_block_ && (cur_block_->instrs.empty() ||
            (cur_block_->instrs.back().op != IROp::Ret &&
             cur_block_->instrs.back().op != IROp::Br  &&
             cur_block_->instrs.back().op != IROp::BrCond))) {
            IRInstr ret; ret.op = IROp::Ret; emit(ret);
        }

        pop_scope();
        cur_fn_ = nullptr; cur_block_ = nullptr;
    }

    /// Emit an implicit default constructor.
    void emit_default_ctor(const std::string& class_name, const ClassLayout& layout) {
        IRFunction ir_fn;
        ir_fn.name = mangler_.mangle_ctor(class_name, {});
        ir_fn.return_type = IRType::Void;
        ir_fn.params.emplace_back("this", IRType::Ptr);

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;
        start_block(fresh_label("entry"));
        push_scope();

        IRValue this_alloca = fresh_value(IRType::Ptr, "this.addr");
        { IRInstr ai; ai.op = IROp::Alloca; ai.result = this_alloca; emit(ai); }
        { IRValue pv = cur_fn_->new_value(IRType::Ptr, "this.val");
          IRInstr si; si.op = IROp::Store; si.operands = {pv, this_alloca}; emit(si); }
        define_var("this", this_alloca);

        if (layout.has_vtable) {
            emit_vtable_init(this_alloca, layout);
        }

        IRInstr ret; ret.op = IROp::Ret; emit(ret);
        pop_scope();
        cur_fn_ = nullptr; cur_block_ = nullptr;
    }

    /// Emit an implicit default destructor.
    void emit_default_dtor(const std::string& class_name, const ClassLayout& layout) {
        IRFunction ir_fn;
        ir_fn.name = mangler_.mangle_dtor(class_name);
        ir_fn.return_type = IRType::Void;
        ir_fn.params.emplace_back("this", IRType::Ptr);

        module_.functions.push_back(std::move(ir_fn));
        cur_fn_    = &module_.functions.back();
        cur_block_ = nullptr;
        label_seq_ = 0;
        start_block(fresh_label("entry"));

        IRInstr ret; ret.op = IROp::Ret; emit(ret);
        cur_fn_ = nullptr; cur_block_ = nullptr;
    }

    /// Emit vtable pointer initialisation: store vtable address at offset 0 of 'this'.
    void emit_vtable_init(IRValue this_alloca, const ClassLayout& layout) {
        // Load 'this' pointer.
        IRValue this_ptr = fresh_value(IRType::Ptr, "this.ptr");
        { IRInstr li; li.op = IROp::Load; li.result = this_ptr;
          li.operands = {this_alloca}; emit(li); }

        // GEP to vtable pointer slot (offset 0).
        IRValue vptr_slot = fresh_value(IRType::Ptr, "vptr.slot");
        { IRInstr gi; gi.op = IROp::GetElementPtr; gi.result = vptr_slot;
          gi.operands = {this_ptr};
          IRValue zero = fresh_value(IRType::Int64);
          { IRInstr ci; ci.op = IROp::Const; ci.result = zero; ci.const_int = 0; emit(ci); }
          gi.gep_indices = {zero};
          emit(gi); }

        // Store vtable address (represented as a Const ptr to the vtable global).
        IRValue vtable_addr = fresh_value(IRType::Ptr, layout.vtable_symbol);
        { IRInstr ci; ci.op = IROp::Const; ci.result = vtable_addr;
          ci.const_int = 0; emit(ci); }  // Address resolved by backend.

        { IRInstr si; si.op = IROp::Store;
          si.operands = {vtable_addr, vptr_slot}; emit(si); }
    }

    /// Helper: extract IRType vector from parameter list.
    std::vector<IRType> params_to_irtypes(const std::vector<NodePtr>& params) {
        std::vector<IRType> result;
        for (auto& p : params) {
            if (p->kind != NodeKind::ParamDecl) continue;
            auto* pd = p->as<const ParamDecl>();
            result.push_back(map_type(pd->type.get()));
        }
        return result;
    }

    // ═════════════════════════════════════════════════════════════
    //  Statement visitors
    // ═════════════════════════════════════════════════════════════

    void emit_stmt(const Node& stmt) {
        switch (stmt.kind) {
            case NodeKind::CompoundStmt:  emit_compound(stmt);  break;
            case NodeKind::IfStmt:        emit_if(stmt);        break;
            case NodeKind::WhileStmt:     emit_while(stmt);     break;
            case NodeKind::DoWhileStmt:   emit_do_while(stmt);  break;
            case NodeKind::ForStmt:       emit_for(stmt);       break;
            case NodeKind::ReturnStmt:    emit_return(stmt);    break;
            case NodeKind::VarDecl:       emit_var_decl(stmt);  break;
            case NodeKind::DeclStmt:      emit_decl_stmt(stmt); break;
            case NodeKind::ExprStmt:      emit_expr_stmt(stmt); break;
            case NodeKind::NullStmt:      /* no-op */           break;
            case NodeKind::BreakStmt:     /* TODO: break/continue need loop context */ break;
            case NodeKind::ContinueStmt:  break;
            default:
                // Unsupported statement – silently skip.
                break;
        }
    }

    void emit_compound(const Node& stmt) {
        auto& cs = *stmt.as<const CompoundStmt>();
        push_scope();
        for (auto& s : cs.stmts)
            emit_stmt(*s);
        pop_scope();
    }

    void emit_if(const Node& s) {
        auto& ifs = *s.as<const IfStmt>();

        IRValue cond = emit_expr(*ifs.condition);

        std::string then_lbl  = fresh_label("if.then");
        std::string else_lbl  = fresh_label("if.else");
        std::string merge_lbl = fresh_label("if.merge");

        // Conditional branch.
        {
            IRInstr br;
            br.op          = IROp::BrCond;
            br.operands.push_back(cond);
            br.label_true  = then_lbl;
            br.label_false = ifs.else_branch ? else_lbl : merge_lbl;
            emit(br);
        }

        // Then block.
        start_block(then_lbl);
        emit_stmt(*ifs.then_branch);
        if (!block_terminated()) emit_br_and_start(merge_lbl);
        else start_block(ifs.else_branch ? else_lbl : merge_lbl);

        // Else block (if present).
        if (ifs.else_branch) {
            if (cur_block_->label != else_lbl)
                start_block(else_lbl);
            emit_stmt(*ifs.else_branch);
            if (!block_terminated()) emit_br_and_start(merge_lbl);
            else start_block(merge_lbl);
        }

        // Merge block is now current.
        if (cur_block_->label != merge_lbl)
            start_block(merge_lbl);
    }

    void emit_while(const Node& s) {
        auto& ws = *s.as<const WhileStmt>();

        std::string hdr_lbl  = fresh_label("while.hdr");
        std::string body_lbl = fresh_label("while.body");
        std::string exit_lbl = fresh_label("while.exit");

        // Jump to header.
        emit_br_and_start(hdr_lbl);

        // Header: evaluate condition.
        // (block was already started by emit_br_and_start)
        IRValue cond = emit_expr(*ws.condition);
        {
            IRInstr br;
            br.op          = IROp::BrCond;
            br.operands.push_back(cond);
            br.label_true  = body_lbl;
            br.label_false = exit_lbl;
            emit(br);
        }

        // Body.
        start_block(body_lbl);
        emit_stmt(*ws.body);
        if (!block_terminated()) {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = hdr_lbl;
            emit(br);
        }

        // Exit.
        start_block(exit_lbl);
    }

    void emit_do_while(const Node& s) {
        auto& dw = *s.as<const DoWhileStmt>();

        std::string body_lbl = fresh_label("do.body");
        std::string cond_lbl = fresh_label("do.cond");
        std::string exit_lbl = fresh_label("do.exit");

        emit_br_and_start(body_lbl);

        emit_stmt(*dw.body);
        if (!block_terminated()) {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = cond_lbl;
            emit(br);
        }

        start_block(cond_lbl);
        IRValue cond = emit_expr(*dw.condition);
        {
            IRInstr br;
            br.op          = IROp::BrCond;
            br.operands.push_back(cond);
            br.label_true  = body_lbl;
            br.label_false = exit_lbl;
            emit(br);
        }

        start_block(exit_lbl);
    }

    void emit_for(const Node& s) {
        auto& fs = *s.as<const ForStmt>();

        // Init.
        push_scope();
        if (fs.init) emit_stmt(*fs.init);

        std::string hdr_lbl   = fresh_label("for.hdr");
        std::string body_lbl  = fresh_label("for.body");
        std::string latch_lbl = fresh_label("for.latch");
        std::string exit_lbl  = fresh_label("for.exit");

        emit_br_and_start(hdr_lbl);

        // Header: evaluate condition (if any).
        if (fs.condition) {
            IRValue cond = emit_expr(*fs.condition);
            IRInstr br;
            br.op          = IROp::BrCond;
            br.operands.push_back(cond);
            br.label_true  = body_lbl;
            br.label_false = exit_lbl;
            emit(br);
        } else {
            // Infinite loop – unconditional jump to body.
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = body_lbl;
            emit(br);
        }

        // Body.
        start_block(body_lbl);
        if (fs.body) emit_stmt(*fs.body);
        if (!block_terminated()) {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = latch_lbl;
            emit(br);
        }

        // Latch: increment expression.
        start_block(latch_lbl);
        if (fs.increment) emit_expr(*fs.increment);
        {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = hdr_lbl;
            emit(br);
        }

        // Exit.
        start_block(exit_lbl);
        pop_scope();
    }

    void emit_return(const Node& s) {
        auto& rs = *s.as<const ReturnStmt>();
        IRInstr ret;
        ret.op = IROp::Ret;
        if (rs.value) {
            IRValue v = emit_expr(*rs.value);
            ret.operands.push_back(v);
        }
        emit(ret);
    }

    void emit_var_decl(const Node& s) {
        auto& vd = *s.as<const VarDecl>();
        IRType ty = map_type(vd.type.get());

        // Alloca.
        IRValue alloca_ptr = fresh_value(IRType::Ptr, vd.name + ".addr");
        {
            IRInstr a;
            a.op        = IROp::Alloca;
            a.result    = alloca_ptr;
            a.const_int = static_cast<int64_t>(ir_type_size(ty));
            emit(a);
        }
        define_var(vd.name, alloca_ptr);

        // Optional initialiser → Store.
        if (vd.init) {
            IRValue init_val = emit_expr(*vd.init);
            IRInstr st;
            st.op = IROp::Store;
            st.operands.push_back(init_val);
            st.operands.push_back(alloca_ptr);
            emit(st);
        }
    }

    void emit_decl_stmt(const Node& s) {
        auto& ds = *s.as<const DeclStmt>();
        for (auto& d : ds.decls)
            emit_stmt(*d);
    }

    void emit_expr_stmt(const Node& s) {
        auto& es = *s.as<const ExprStmt>();
        if (es.expr) emit_expr(*es.expr);
    }

    /// Check whether the current block already has a terminator.
    bool block_terminated() const {
        if (!cur_block_ || cur_block_->instrs.empty()) return false;
        auto op = cur_block_->instrs.back().op;
        return op == IROp::Ret || op == IROp::Br || op == IROp::BrCond;
    }

    // ═════════════════════════════════════════════════════════════
    //  Expression visitors (each returns an IRValue)
    // ═════════════════════════════════════════════════════════════

    IRValue emit_expr(const Node& expr) {
        switch (expr.kind) {
            case NodeKind::IntLiteralExpr:    return emit_int_literal(expr);
            case NodeKind::FloatLiteralExpr:  return emit_float_literal(expr);
            case NodeKind::StringLiteralExpr: return emit_string_literal(expr);
            case NodeKind::CharLiteralExpr:   return emit_char_literal(expr);
            case NodeKind::BoolLiteralExpr:   return emit_bool_literal(expr);
            case NodeKind::NullptrExpr:       return emit_nullptr_literal();
            case NodeKind::IdentifierExpr:    return emit_var(expr);
            case NodeKind::BinaryExpr:        return emit_binary(expr);
            case NodeKind::UnaryExpr:         return emit_unary(expr);
            case NodeKind::CallExpr:          return emit_call(expr);
            case NodeKind::AssignExpr:        return emit_assign(expr);
            case NodeKind::ParenExpr:
                return emit_expr(*expr.as<const ParenExpr>()->inner);
            case NodeKind::CastExpr:          return emit_cast_expr(expr);
            case NodeKind::TernaryExpr:       return emit_ternary(expr);
            case NodeKind::ScopeExpr:         return emit_scope_expr(expr);
            case NodeKind::IndexExpr:         return emit_index(expr);
            case NodeKind::CommaExpr:         return emit_comma(expr);
            default:
                // Unsupported expression – return a zero constant.
                return emit_literal_int(0);
        }
    }

    // ── Literals ─────────────────────────────────────────────────

    IRValue emit_literal_int(int64_t v, IRType ty = IRType::Int32) {
        IRValue result = fresh_value(ty);
        IRInstr ci;
        ci.op        = IROp::Const;
        ci.result    = result;
        ci.const_int = v;
        emit(ci);
        return result;
    }

    IRValue emit_literal_float(double v, IRType ty = IRType::Float64) {
        IRValue result = fresh_value(ty);
        IRInstr ci;
        ci.op          = IROp::Const;
        ci.result      = result;
        ci.const_float = v;
        emit(ci);
        return result;
    }

    IRValue emit_literal_str(const std::string& s) {
        // Create a global string constant and return a Ptr to it.
        std::string label = ".str." + std::to_string(module_.globals.size());
        module_.globals.emplace_back(label, s);

        IRValue result = fresh_value(IRType::Ptr, label);
        IRInstr ci;
        ci.op        = IROp::Const;
        ci.result    = result;
        ci.const_int = 0;  // address resolved at link time
        ci.callee    = label;  // reuse callee field for global label ref
        emit(ci);
        return result;
    }

    IRValue emit_int_literal(const Node& e) {
        auto& lit = *e.as<const IntLiteralExpr>();
        IRType ty = (lit.is_long || lit.is_long_long) ? IRType::Int64 : IRType::Int32;
        return emit_literal_int(static_cast<int64_t>(lit.value), ty);
    }

    IRValue emit_float_literal(const Node& e) {
        auto& lit = *e.as<const FloatLiteralExpr>();
        IRType ty = lit.is_float ? IRType::Float32 : IRType::Float64;
        return emit_literal_float(lit.value, ty);
    }

    IRValue emit_string_literal(const Node& e) {
        auto& lit = *e.as<const StringLiteralExpr>();
        return emit_literal_str(lit.cooked);
    }

    IRValue emit_char_literal(const Node& e) {
        auto& lit = *e.as<const CharLiteralExpr>();
        return emit_literal_int(static_cast<int64_t>(lit.char_val), IRType::Int8);
    }

    IRValue emit_bool_literal(const Node& e) {
        auto& lit = *e.as<const BoolLiteralExpr>();
        return emit_literal_int(lit.value ? 1 : 0, IRType::Bool);
    }

    IRValue emit_nullptr_literal() {
        return emit_literal_int(0, IRType::Ptr);
    }

    // ── Variable access (Load from alloca) ───────────────────────

    IRValue emit_var(const Node& e) {
        auto& id = *e.as<const IdentifierExpr>();
        const IRValue* alloca_ptr = lookup_var(id.name);
        if (!alloca_ptr) {
            // Unknown variable – treat as external symbol / zero.
            return emit_literal_int(0);
        }
        IRValue result = fresh_value(alloca_ptr->type == IRType::Ptr
                                         ? IRType::Int32  // default loaded type
                                         : alloca_ptr->type,
                                     id.name);
        IRInstr ld;
        ld.op     = IROp::Load;
        ld.result = result;
        ld.operands.push_back(*alloca_ptr);
        emit(ld);
        return result;
    }

    // ── Assignment (Store to alloca) ─────────────────────────────

    IRValue emit_assign(const Node& e) {
        auto& ae = *e.as<const AssignExpr>();

        // RHS value.
        IRValue rhs = emit_expr(*ae.value);

        // LHS must be an identifier for now.
        if (ae.target->kind == NodeKind::IdentifierExpr) {
            auto& id = *ae.target->as<const IdentifierExpr>();
            const IRValue* alloca_ptr = lookup_var(id.name);
            if (alloca_ptr) {
                // For compound assignments (+=, -=, etc.), load + op + store.
                if (ae.op != TokenKind::Assign) {
                    IRValue lhs = emit_var(*ae.target);
                    IROp bin_op = compound_assign_op(ae.op);
                    IRValue tmp = fresh_value(lhs.type);
                    IRInstr bin;
                    bin.op     = bin_op;
                    bin.result = tmp;
                    bin.operands.push_back(lhs);
                    bin.operands.push_back(rhs);
                    emit(bin);
                    rhs = tmp;
                }
                IRInstr st;
                st.op = IROp::Store;
                st.operands.push_back(rhs);
                st.operands.push_back(*alloca_ptr);
                emit(st);
            }
        }
        return rhs;
    }

    // ── Binary expression ────────────────────────────────────────

    IRValue emit_binary(const Node& e) {
        auto& be = *e.as<const BinaryExpr>();
        IRValue lhs = emit_expr(*be.left);
        IRValue rhs = emit_expr(*be.right);
        IROp op     = map_binary_op(be.op);

        // Determine result type – comparisons yield Bool.
        IRType res_type = is_comparison(op) ? IRType::Bool : lhs.type;

        IRValue result = fresh_value(res_type);
        IRInstr bin;
        bin.op     = op;
        bin.result = result;
        bin.operands.push_back(lhs);
        bin.operands.push_back(rhs);
        emit(bin);
        return result;
    }

    // ── Unary expression ─────────────────────────────────────────

    IRValue emit_unary(const Node& e) {
        auto& ue = *e.as<const UnaryExpr>();
        IRValue operand = emit_expr(*ue.operand);

        switch (ue.op) {
            case TokenKind::Minus: {
                IRValue result = fresh_value(operand.type);
                IRInstr neg;
                neg.op     = IROp::Neg;
                neg.result = result;
                neg.operands.push_back(operand);
                emit(neg);
                return result;
            }
            case TokenKind::Bang: {
                IRValue result = fresh_value(IRType::Bool);
                IRInstr n;
                n.op     = IROp::Not;
                n.result = result;
                n.operands.push_back(operand);
                emit(n);
                return result;
            }
            case TokenKind::Tilde: {
                IRValue result = fresh_value(operand.type);
                IRInstr n;
                n.op     = IROp::Not;
                n.result = result;
                n.operands.push_back(operand);
                emit(n);
                return result;
            }
            case TokenKind::Amp: {
                // Address-of: if operand was a load from an alloca, return
                // the alloca pointer directly.  Simplified – just return operand.
                return operand;
            }
            case TokenKind::Star: {
                // Dereference: emit a Load.
                IRValue result = fresh_value(IRType::Int32);  // loaded type unknown
                IRInstr ld;
                ld.op     = IROp::Load;
                ld.result = result;
                ld.operands.push_back(operand);
                emit(ld);
                return result;
            }
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus: {
                // ++x / --x / x++ / x--  →  load, add/sub 1, store
                // We need the alloca pointer.  If the operand came from an
                // identifier, look it up again.
                IROp arith = (ue.op == TokenKind::PlusPlus) ? IROp::Add : IROp::Sub;
                IRValue one = emit_literal_int(1, operand.type);
                IRValue tmp = fresh_value(operand.type);
                IRInstr bin;
                bin.op     = arith;
                bin.result = tmp;
                bin.operands.push_back(operand);
                bin.operands.push_back(one);
                emit(bin);

                // Try to store back.
                if (ue.operand->kind == NodeKind::IdentifierExpr) {
                    auto& id = *ue.operand->as<const IdentifierExpr>();
                    const IRValue* ptr = lookup_var(id.name);
                    if (ptr) {
                        IRInstr st;
                        st.op = IROp::Store;
                        st.operands.push_back(tmp);
                        st.operands.push_back(*ptr);
                        emit(st);
                    }
                }
                return ue.is_postfix ? operand : tmp;
            }
            default:
                return operand;
        }
    }

    // ── Function call ────────────────────────────────────────────

    IRValue emit_call(const Node& e) {
        auto& ce = *e.as<const CallExpr>();

        // Resolve callee name.
        std::string callee_name = resolve_callee(*ce.callee);

        // Emit arguments.
        std::vector<IRValue> args;
        args.reserve(ce.args.size());
        for (auto& a : ce.args)
            args.push_back(emit_expr(*a));

        // Result type is unknown at IR level – default to Int32.
        IRValue result = fresh_value(IRType::Int32, callee_name + ".ret");
        IRInstr call;
        call.op       = IROp::Call;
        call.result   = result;
        call.callee   = callee_name;
        call.operands = std::move(args);
        emit(call);
        return result;
    }

    /// Extract a printable callee name from a callee expression.
    std::string resolve_callee(const Node& callee) const {
        switch (callee.kind) {
            case NodeKind::IdentifierExpr:
                return callee.as<const IdentifierExpr>()->name;
            case NodeKind::ScopeExpr:
                return callee.as<const ScopeExpr>()->qualified();
            case NodeKind::MemberExpr:
                return callee.as<const MemberExpr>()->member;
            default:
                return "__indirect_call";
        }
    }

    // ── Cast expression ──────────────────────────────────────────

    IRValue emit_cast_expr(const Node& e) {
        auto& ce = *e.as<const CastExpr>();
        IRValue src = emit_expr(*ce.expr);
        IRType  dst = map_type(ce.type.get());
        return emit_cast(src, dst);
    }

    IRValue emit_cast(IRValue v, IRType to) {
        if (v.type == to) return v;
        IRValue result = fresh_value(to);
        IRInstr c;
        c.op      = IROp::Cast;
        c.result  = result;
        c.cast_to = to;
        c.operands.push_back(v);
        emit(c);
        return result;
    }

    // ── Ternary expression ───────────────────────────────────────

    IRValue emit_ternary(const Node& e) {
        auto& te = *e.as<const TernaryExpr>();

        IRValue cond = emit_expr(*te.condition);

        std::string then_lbl  = fresh_label("tern.then");
        std::string else_lbl  = fresh_label("tern.else");
        std::string merge_lbl = fresh_label("tern.merge");

        {
            IRInstr br;
            br.op          = IROp::BrCond;
            br.operands.push_back(cond);
            br.label_true  = then_lbl;
            br.label_false = else_lbl;
            emit(br);
        }

        // Then.
        start_block(then_lbl);
        IRValue then_val = emit_expr(*te.then_expr);
        std::string then_exit = cur_block_->label;
        {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = merge_lbl;
            emit(br);
        }

        // Else.
        start_block(else_lbl);
        IRValue else_val = emit_expr(*te.else_expr);
        std::string else_exit = cur_block_->label;
        {
            IRInstr br;
            br.op         = IROp::Br;
            br.label_true = merge_lbl;
            emit(br);
        }

        // Merge with Phi.
        start_block(merge_lbl);
        IRValue result = fresh_value(then_val.type);
        IRInstr phi;
        phi.op     = IROp::Phi;
        phi.result = result;
        phi.phi_incoming.emplace_back(then_exit, then_val);
        phi.phi_incoming.emplace_back(else_exit, else_val);
        emit(phi);
        return result;
    }

    // ── Scope expression (e.g. std::cout) ────────────────────────

    IRValue emit_scope_expr(const Node& e) {
        auto& se = *e.as<const ScopeExpr>();
        // Check if this is a known variable.
        const IRValue* ptr = lookup_var(se.qualified());
        if (ptr) {
            IRValue result = fresh_value(IRType::Int32, se.qualified());
            IRInstr ld;
            ld.op     = IROp::Load;
            ld.result = result;
            ld.operands.push_back(*ptr);
            emit(ld);
            return result;
        }
        // Otherwise treat as an external symbol constant.
        return emit_literal_int(0);
    }

    // ── Index expression (a[i]) ──────────────────────────────────

    IRValue emit_index(const Node& e) {
        auto& ie = *e.as<const IndexExpr>();
        IRValue base  = emit_expr(*ie.object);
        IRValue index = emit_expr(*ie.index);

        // GEP + Load.
        IRValue gep_result = fresh_value(IRType::Ptr);
        IRInstr gep;
        gep.op     = IROp::GetElementPtr;
        gep.result = gep_result;
        gep.operands.push_back(base);
        gep.gep_indices.push_back(index);
        emit(gep);

        IRValue loaded = fresh_value(IRType::Int32);  // element type unknown
        IRInstr ld;
        ld.op     = IROp::Load;
        ld.result = loaded;
        ld.operands.push_back(gep_result);
        emit(ld);
        return loaded;
    }

    // ── Comma expression ─────────────────────────────────────────

    IRValue emit_comma(const Node& e) {
        auto& ce = *e.as<const CommaExpr>();
        emit_expr(*ce.left);   // discard
        return emit_expr(*ce.right);
    }

    // ═════════════════════════════════════════════════════════════
    //  Operator mapping helpers
    // ═════════════════════════════════════════════════════════════

    static IROp map_binary_op(TokenKind tk) {
        switch (tk) {
            case TokenKind::Plus:     return IROp::Add;
            case TokenKind::Minus:    return IROp::Sub;
            case TokenKind::Star:     return IROp::Mul;
            case TokenKind::Slash:    return IROp::Div;
            case TokenKind::Percent:  return IROp::Mod;
            case TokenKind::Amp:      return IROp::And;
            case TokenKind::Pipe:     return IROp::Or;
            case TokenKind::Caret:    return IROp::Xor;
            case TokenKind::LShift:   return IROp::Shl;
            case TokenKind::RShift:   return IROp::Shr;
            case TokenKind::AmpAmp:   return IROp::And;
            case TokenKind::PipePipe: return IROp::Or;
            case TokenKind::Eq:       return IROp::CmpEq;
            case TokenKind::NotEq:    return IROp::CmpNe;
            case TokenKind::Lt:       return IROp::CmpLt;
            case TokenKind::LtEq:     return IROp::CmpLe;
            case TokenKind::Gt:       return IROp::CmpGt;
            case TokenKind::GtEq:     return IROp::CmpGe;
            default:                  return IROp::Add;
        }
    }

    static IROp compound_assign_op(TokenKind tk) {
        switch (tk) {
            case TokenKind::PlusAssign:    return IROp::Add;
            case TokenKind::MinusAssign:   return IROp::Sub;
            case TokenKind::StarAssign:    return IROp::Mul;
            case TokenKind::SlashAssign:   return IROp::Div;
            case TokenKind::PercentAssign: return IROp::Mod;
            case TokenKind::AmpAssign:     return IROp::And;
            case TokenKind::PipeAssign:    return IROp::Or;
            case TokenKind::CaretAssign:   return IROp::Xor;
            case TokenKind::LShiftAssign:  return IROp::Shl;
            case TokenKind::RShiftAssign:  return IROp::Shr;
            default:                       return IROp::Add;
        }
    }

    static bool is_comparison(IROp op) {
        return op == IROp::CmpEq || op == IROp::CmpNe ||
               op == IROp::CmpLt || op == IROp::CmpLe ||
               op == IROp::CmpGt || op == IROp::CmpGe;
    }
};

} // namespace rexc
