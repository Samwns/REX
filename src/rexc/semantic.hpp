#pragma once
/*
 * semantic.hpp  –  REXC Semantic Analyser
 *
 * Walks the AST produced by parser.hpp and:
 *   1. Builds a scoped symbol table
 *   2. Resolves type names (NamedType → canonical type)
 *   3. Infers `auto` variable types where the initialiser is visible
 *   4. Records which classes have virtual methods (vtable needed)
 *   5. Annotates FunctionDecl / VarDecl nodes with their enclosing class
 *   6. Reports undeclared identifiers and type mismatches as Diagnostics
 *
 * The analyser is intentionally conservative: unknown constructs are
 * allowed through so that downstream code-gen can handle them.
 */

#include "ast.hpp"
#include "parser.hpp"   // for Diagnostic

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <algorithm>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Type descriptor  (simplified type representation for analysis)
// ─────────────────────────────────────────────────────────────────
enum class TypeTag {
    Unknown,
    Void, Bool,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double, LongDouble,
    Char, WChar,
    Pointer, Reference, RValueRef,
    Array,
    Class, Enum, Typedef,
    FunctionType,
    Auto,           // unresolved auto
    Nullptr_t,
};

struct TypeDesc {
    TypeTag  tag      = TypeTag::Unknown;
    std::string class_name;         // for TypeTag::Class / Enum / Typedef
    std::shared_ptr<TypeDesc> inner; // for Pointer / Reference / Array
    bool     is_const    = false;
    bool     is_volatile = false;
    bool     is_unsigned = false;   // for integer types

    static TypeDesc make(TypeTag t) { TypeDesc d; d.tag = t; return d; }
    static TypeDesc make_int()    { return make(TypeTag::Int32); }
    static TypeDesc make_bool()   { return make(TypeTag::Bool); }
    static TypeDesc make_void()   { return make(TypeTag::Void); }
    static TypeDesc make_auto()   { return make(TypeTag::Auto); }
    static TypeDesc make_unknown(){ return make(TypeTag::Unknown); }
    static TypeDesc make_ptr(TypeDesc inner_type) {
        TypeDesc d; d.tag = TypeTag::Pointer;
        d.inner = std::make_shared<TypeDesc>(std::move(inner_type));
        return d;
    }
    static TypeDesc make_ref(TypeDesc inner_type) {
        TypeDesc d; d.tag = TypeTag::Reference;
        d.inner = std::make_shared<TypeDesc>(std::move(inner_type));
        return d;
    }
    static TypeDesc make_class(std::string name) {
        TypeDesc d; d.tag = TypeTag::Class; d.class_name = std::move(name);
        return d;
    }
    static TypeDesc make_array(TypeDesc elem_type) {
        TypeDesc d; d.tag = TypeTag::Array;
        d.inner = std::make_shared<TypeDesc>(std::move(elem_type));
        return d;
    }

    bool is_integral() const {
        return tag == TypeTag::Int8   || tag == TypeTag::Int16  ||
               tag == TypeTag::Int32  || tag == TypeTag::Int64  ||
               tag == TypeTag::UInt8  || tag == TypeTag::UInt16 ||
               tag == TypeTag::UInt32 || tag == TypeTag::UInt64 ||
               tag == TypeTag::Char   || tag == TypeTag::Bool;
    }
    bool is_numeric() const {
        return is_integral() || tag == TypeTag::Float ||
               tag == TypeTag::Double || tag == TypeTag::LongDouble;
    }
    bool is_pointer_like() const {
        return tag == TypeTag::Pointer || tag == TypeTag::Nullptr_t;
    }

    std::string str() const {
        switch (tag) {
            case TypeTag::Void:       return "void";
            case TypeTag::Bool:       return "bool";
            case TypeTag::Int8:       return "int8_t";
            case TypeTag::Int16:      return "int16_t";
            case TypeTag::Int32:      return "int";
            case TypeTag::Int64:      return "long long";
            case TypeTag::UInt8:      return "uint8_t";
            case TypeTag::UInt16:     return "uint16_t";
            case TypeTag::UInt32:     return "unsigned int";
            case TypeTag::UInt64:     return "unsigned long long";
            case TypeTag::Float:      return "float";
            case TypeTag::Double:     return "double";
            case TypeTag::LongDouble: return "long double";
            case TypeTag::Char:       return "char";
            case TypeTag::WChar:      return "wchar_t";
            case TypeTag::Pointer:    return (inner ? inner->str() : "?") + "*";
            case TypeTag::Reference:  return (inner ? inner->str() : "?") + "&";
            case TypeTag::Class:      return class_name;
            case TypeTag::Array:      return (inner ? inner->str() : "?") + "[]";
            case TypeTag::Auto:       return "auto";
            case TypeTag::Nullptr_t:  return "nullptr_t";
            default:                  return "<unknown>";
        }
    }
};

// ─────────────────────────────────────────────────────────────────
//  Symbol  –  a named entity in the symbol table
// ─────────────────────────────────────────────────────────────────
enum class SymbolKind {
    Variable,
    Function,
    Class,
    Enum,
    EnumValue,
    Typedef,
    Namespace,
    Template,
    Parameter,
};

struct Symbol {
    std::string name;
    SymbolKind  kind      = SymbolKind::Variable;
    TypeDesc    type;
    Node*       decl_node = nullptr;   // back-pointer into AST
    std::string enclosing_class;       // for member functions / fields
    bool        is_static   = false;
    bool        is_virtual  = false;
    bool        is_override = false;
};

// ─────────────────────────────────────────────────────────────────
//  Scope  –  one level of the symbol table
// ─────────────────────────────────────────────────────────────────
struct Scope {
    enum class Kind { Global, Namespace, Class, Function, Block };
    Kind       kind        = Kind::Block;
    std::string class_name;  // set for Kind::Class
    std::string ns_name;     // set for Kind::Namespace

    std::unordered_map<std::string, Symbol> symbols;

    Scope* parent = nullptr;
    std::vector<std::unique_ptr<Scope>> children;

    void define(const std::string& name, Symbol sym) {
        symbols[name] = std::move(sym);
    }

    Symbol* lookup_local(const std::string& name) {
        auto it = symbols.find(name);
        return it != symbols.end() ? &it->second : nullptr;
    }

    Symbol* lookup(const std::string& name) {
        auto* sym = lookup_local(name);
        if (sym) return sym;
        if (parent) return parent->lookup(name);
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────
//  ClassInfo  –  metadata collected during the first pass
// ─────────────────────────────────────────────────────────────────
struct ClassInfo {
    std::string              name;
    std::vector<std::string> bases;           // direct base class names
    bool                     has_vtable = false;
    bool                     needs_ctor = false;
    bool                     needs_dtor = false;
    std::vector<std::string> virtual_methods;  // names of virtual methods
    std::vector<std::string> fields;           // field names in order
    std::unordered_map<std::string, TypeDesc> field_types;
    std::unordered_set<std::string> methods;   // all method names
};

// ─────────────────────────────────────────────────────────────────
//  SemanticContext  –  output of the analysis pass
// ─────────────────────────────────────────────────────────────────
struct SemanticContext {
    std::unordered_map<std::string, ClassInfo>  classes;
    std::vector<std::string>                    using_namespaces; // e.g. "std"
    std::unordered_set<std::string>             used_std_types;   // "string","vector",...
    bool                                        uses_cout  = false;
    bool                                        uses_cerr  = false;
    bool                                        uses_cin   = false;
};

// ─────────────────────────────────────────────────────────────────
//  SemanticAnalyzer
// ─────────────────────────────────────────────────────────────────
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer() {
        // Push global scope
        auto g = std::make_unique<Scope>();
        g->kind = Scope::Kind::Global;
        current_ = g.get();
        global_  = g.get();
        scopes_.push_back(std::move(g));
        register_builtins();
    }

    void analyze(TranslationUnit& tu) {
        collect_top_level_names(tu);
        for (auto& decl : tu.decls) visit_decl(*decl);
    }

    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    SemanticContext& context() { return ctx_; }
    const SemanticContext& context() const { return ctx_; }

    bool has_errors() const {
        return std::any_of(diags_.begin(), diags_.end(),
            [](const Diagnostic& d){ return d.severity == Diagnostic::Severity::Error; });
    }

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope*       current_ = nullptr;
    Scope*       global_  = nullptr;
    std::vector<Diagnostic> diags_;
    SemanticContext ctx_;
    std::string  current_class_;   // non-empty when inside a class body
    std::string  current_function_;

    // ── Diagnostics helpers ──────────────────────────────────────
    void error(const std::string& msg, SourceLocation loc = {}) {
        diags_.push_back({Diagnostic::Severity::Error, msg, loc});
    }
    void warning(const std::string& msg, SourceLocation loc = {}) {
        diags_.push_back({Diagnostic::Severity::Warning, msg, loc});
    }
    void note(const std::string& msg, SourceLocation loc = {}) {
        diags_.push_back({Diagnostic::Severity::Note, msg, loc});
    }

    // ── Scope management ─────────────────────────────────────────
    Scope* push_scope(Scope::Kind k) {
        auto s = std::make_unique<Scope>();
        s->kind   = k;
        s->parent = current_;
        Scope* raw = s.get();
        current_->children.push_back(std::move(s));
        current_ = raw;
        return raw;
    }

    void pop_scope() {
        if (current_->parent) current_ = current_->parent;
    }

    // ── Builtin names ────────────────────────────────────────────
    void register_builtins() {
        // std:: namespace placeholder
        Symbol std_ns;
        std_ns.name = "std";
        std_ns.kind = SymbolKind::Namespace;
        global_->define("std", std_ns);

        // Common built-in functions
        for (auto& fn : {"printf","fprintf","sprintf","snprintf","puts","fputs",
                         "malloc","calloc","realloc","free","exit","abort","assert",
                         "strlen","strcpy","strncpy","strcmp","strcat","memcpy","memset"}) {
            Symbol s; s.name = fn; s.kind = SymbolKind::Function;
            s.type = TypeDesc::make_int();
            global_->define(fn, s);
        }

        // roll:: dice library builtins
        for (auto& fn : {"roll_dice","roll_dice_n",
                         "roll_d4","roll_d6","roll_d8","roll_d10",
                         "roll_d12","roll_d20","roll_d100",
                         "roll_d4_n","roll_d6_n","roll_d8_n","roll_d10_n",
                         "roll_d12_n","roll_d20_n","roll_d100_n",
                         "roll_roll_full","roll_result_to_string",
                         "roll_result_max","roll_result_min",
                         "roll_advantage","roll_disadvantage","roll_with_mod",
                         "roll_critical","roll_critical_fail","roll_chance",
                         "roll_pick_ptr","roll_shuffle_vec",
                         "roll_character_stats","roll_parse"}) {
            Symbol s; s.name = fn; s.kind = SymbolKind::Function;
            s.type = TypeDesc::make_int();
            global_->define(fn, s);
        }
    }

    // ── First pass: collect top-level names ─────────────────────
    void collect_top_level_names(TranslationUnit& tu) {
        for (auto& d : tu.decls) {
            if (!d) continue;
            switch (d->kind) {
                case NodeKind::ClassDecl: {
                    auto& cls = *d->as<ClassDecl>();
                    register_class(cls);
                    break;
                }
                case NodeKind::FunctionDecl: {
                    auto& fn = *d->as<FunctionDecl>();
                    Symbol s; s.name = fn.name; s.kind = SymbolKind::Function;
                    s.decl_node = d.get();
                    global_->define(fn.name, s);
                    break;
                }
                case NodeKind::TemplateDecl: {
                    auto& tmpl = *d->as<TemplateDecl>();
                    if (tmpl.body) {
                        if (tmpl.body->kind == NodeKind::ClassDecl)
                            register_class(*tmpl.body->as<ClassDecl>());
                        else if (tmpl.body->kind == NodeKind::FunctionDecl) {
                            auto& fn = *tmpl.body->as<FunctionDecl>();
                            Symbol s; s.name = fn.name; s.kind = SymbolKind::Function;
                            global_->define(fn.name, s);
                        }
                    }
                    break;
                }
                case NodeKind::UsingDecl: {
                    auto& u = *d->as<UsingDecl>();
                    if (u.form == UsingDecl::Form::Directive) {
                        // e.g.  using namespace std;
                        std::string ns;
                        for (auto& p : u.scope_path) ns += p;
                        ctx_.using_namespaces.push_back(ns);
                    }
                    break;
                }
                default: break;
            }
        }
    }

    void register_class(ClassDecl& cls) {
        ClassInfo info;
        info.name = cls.name;

        for (auto& base : cls.bases) info.bases.push_back(base.name);

        // Scan members
        for (auto& m : cls.members) {
            if (!m) continue;
            if (m->kind == NodeKind::FunctionDecl ||
                m->kind == NodeKind::ConstructorDecl ||
                m->kind == NodeKind::DestructorDecl) {
                if (m->kind == NodeKind::FunctionDecl) {
                    auto& fn = *m->as<FunctionDecl>();
                    info.methods.insert(fn.name);
                    if (fn.is_virtual || fn.is_override) {
                        info.has_vtable = true;
                        info.virtual_methods.push_back(fn.name);
                    }
                }
                if (m->kind == NodeKind::ConstructorDecl) info.needs_ctor = true;
                if (m->kind == NodeKind::DestructorDecl)  info.needs_dtor = true;
            } else if (m->kind == NodeKind::FieldDecl) {
                auto& fld = *m->as<FieldDecl>();
                info.fields.push_back(fld.name);
                info.field_types[fld.name] = resolve_type_desc(fld.type.get());
            } else if (m->kind == NodeKind::VarDecl) {
                // Class member variable
                auto& vd = *m->as<VarDecl>();
                info.fields.push_back(vd.name);
                info.field_types[vd.name] = resolve_type_desc(vd.type.get());
            } else if (m->kind == NodeKind::DeclStmt) {
                for (auto& d : m->as<DeclStmt>()->decls) {
                    if (!d || d->kind != NodeKind::VarDecl) continue;
                    auto& vd = *d->as<VarDecl>();
                    info.fields.push_back(vd.name);
                    info.field_types[vd.name] = resolve_type_desc(vd.type.get());
                }
            }
        }

        // Inherit vtable and methods from base classes
        for (auto& b : info.bases) {
            auto it = ctx_.classes.find(b);
            if (it != ctx_.classes.end()) {
                if (it->second.has_vtable) info.has_vtable = true;
                // Inherit virtual methods
                for (auto& vm : it->second.virtual_methods)
                    if (!info.methods.count(vm))
                        info.virtual_methods.push_back(vm);
                // Inherit all methods
                for (auto& mth : it->second.methods)
                    info.methods.insert(mth);
            }
        }

        ctx_.classes[cls.name] = std::move(info);

        // Register in symbol table
        Symbol s; s.name = cls.name; s.kind = SymbolKind::Class;
        s.type = TypeDesc::make_class(cls.name);
        global_->define(cls.name, s);
    }

    // ── Visitor dispatch ─────────────────────────────────────────
    void visit_decl(Node& n) {
        switch (n.kind) {
            case NodeKind::PreprocessorDecl:  visit_preprocessor(*n.as<PreprocessorDecl>()); break;
            case NodeKind::NamespaceDecl:     visit_namespace(*n.as<NamespaceDecl>()); break;
            case NodeKind::UsingDecl:         visit_using(*n.as<UsingDecl>()); break;
            case NodeKind::TypedefDecl:       visit_typedef(*n.as<TypedefDecl>()); break;
            case NodeKind::ClassDecl:         visit_class(*n.as<ClassDecl>()); break;
            case NodeKind::EnumDecl:          visit_enum(*n.as<EnumDecl>()); break;
            case NodeKind::FunctionDecl:
            case NodeKind::ConstructorDecl:
            case NodeKind::DestructorDecl:    visit_function(*n.as<FunctionDecl>()); break;
            case NodeKind::VarDecl:           visit_var(*n.as<VarDecl>()); break;
            case NodeKind::DeclStmt:
                for (auto& d : n.as<DeclStmt>()->decls) if (d) visit_decl(*d);
                break;
            case NodeKind::TemplateDecl:
                if (n.as<TemplateDecl>()->body) visit_decl(*n.as<TemplateDecl>()->body);
                break;
            case NodeKind::StaticAssertDecl:  break;
            case NodeKind::FriendDecl:
                if (n.as<FriendDecl>()->decl) visit_decl(*n.as<FriendDecl>()->decl);
                break;
            default: break;
        }
    }

    void visit_preprocessor(PreprocessorDecl& pp) {
        // Extract include names to identify used std types
        if (pp.directive.size() > 9 &&
            pp.directive.substr(1,7) == "include") {
            const std::string& line = pp.directive;
            auto get_name = [&]() {
                auto s = line.find('<');
                if (s == std::string::npos) s = line.find('"');
                if (s == std::string::npos) return std::string{};
                auto e = line.find_first_of(">\"", s+1);
                if (e == std::string::npos) return std::string{};
                return line.substr(s+1, e-s-1);
            };
            std::string hdr = get_name();
            if (hdr == "iostream")  { ctx_.uses_cout = true; ctx_.uses_cerr = true; ctx_.uses_cin = true; }
            if (hdr == "string")    ctx_.used_std_types.insert("string");
            if (hdr == "vector")    ctx_.used_std_types.insert("vector");
            if (hdr == "map")       ctx_.used_std_types.insert("map");
            if (hdr == "set")       ctx_.used_std_types.insert("set");
            if (hdr == "memory")    ctx_.used_std_types.insert("memory");
            if (hdr == "algorithm") ctx_.used_std_types.insert("algorithm");
            if (hdr == "functional")ctx_.used_std_types.insert("functional");
        }
    }

    void visit_namespace(NamespaceDecl& ns) {
        auto* prev = current_;
        push_scope(Scope::Kind::Namespace);
        current_->ns_name = ns.name;
        for (auto& d : ns.decls) if (d) visit_decl(*d);
        pop_scope();
    }

    void visit_using(UsingDecl& u) {
        if (u.form == UsingDecl::Form::Directive) {
            // Already handled in collect_top_level_names
        } else if (u.form == UsingDecl::Form::Alias) {
            Symbol s; s.name = u.name; s.kind = SymbolKind::Typedef;
            if (u.type_alias) s.type = resolve_type_desc(u.type_alias.get());
            current_->define(u.name, s);
        }
    }

    void visit_typedef(TypedefDecl& td) {
        Symbol s; s.name = td.name; s.kind = SymbolKind::Typedef;
        if (td.type) s.type = resolve_type_desc(td.type.get());
        current_->define(td.name, s);
    }

    void visit_class(ClassDecl& cls) {
        if (cls.name.empty()) return;
        std::string prev_class = current_class_;
        current_class_ = cls.name;

        push_scope(Scope::Kind::Class);
        current_->class_name = cls.name;

        // Define 'this' as pointer to class
        Symbol this_sym; this_sym.name = "this"; this_sym.kind = SymbolKind::Variable;
        this_sym.type = TypeDesc::make_ptr(TypeDesc::make_class(cls.name));
        current_->define("this", this_sym);

        for (auto& m : cls.members) {
            if (!m) continue;
            visit_decl(*m);
        }

        pop_scope();
        current_class_ = prev_class;
    }

    void visit_enum(EnumDecl& en) {
        Symbol s; s.name = en.name; s.kind = SymbolKind::Enum;
        s.type = TypeDesc::make_int();
        current_->define(en.name, s);

        for (auto& e : en.enumerators) {
            Symbol vs; vs.name = e.name; vs.kind = SymbolKind::EnumValue;
            vs.type = TypeDesc::make_int();
            vs.enclosing_class = en.name;
            current_->define(e.name, vs);
        }
    }

    void visit_function(FunctionDecl& fn) {
        // Register the function symbol
        Symbol sym;
        sym.name = fn.name;
        sym.kind = SymbolKind::Function;
        sym.is_virtual  = fn.is_virtual;
        sym.is_override = fn.is_override;
        sym.is_static   = fn.is_static;
        sym.enclosing_class = current_class_;
        sym.decl_node = &fn;
        current_->define(fn.name, sym);

        if (!fn.body) return;  // declaration only

        std::string prev_fn = current_function_;
        current_function_ = fn.name;

        push_scope(Scope::Kind::Function);

        // Register 'this' if inside a class (non-static)
        if (!current_class_.empty() && !fn.is_static) {
            Symbol ts; ts.name = "this"; ts.kind = SymbolKind::Variable;
            ts.type = TypeDesc::make_ptr(TypeDesc::make_class(current_class_));
            current_->define("this", ts);
        }

        // Register parameters
        for (auto& p : fn.params) {
            if (!p || p->kind != NodeKind::ParamDecl) continue;
            auto& param = *p->as<ParamDecl>();
            if (param.name.empty()) continue;
            Symbol ps; ps.name = param.name; ps.kind = SymbolKind::Parameter;
            ps.type = resolve_type_desc(param.type.get());
            current_->define(param.name, ps);
        }

        visit_stmt(*fn.body);
        pop_scope();
        current_function_ = prev_fn;
    }

    void visit_var(VarDecl& v) {
        TypeDesc td;
        if (v.type) {
            td = resolve_type_desc(v.type.get());
        } else {
            td = TypeDesc::make_auto();
        }

        // auto type inference
        if (td.tag == TypeTag::Auto && v.init) {
            td = infer_expr_type(*v.init);
        }

        Symbol s; s.name = v.name; s.kind = SymbolKind::Variable;
        s.type = std::move(td);
        s.is_static = v.is_static;
        s.enclosing_class = current_class_;
        s.decl_node = &v;
        current_->define(v.name, s);

        if (v.init) visit_expr(*v.init);
    }

    // ── Statement visitor ────────────────────────────────────────
    void visit_stmt(Node& n) {
        switch (n.kind) {
            case NodeKind::CompoundStmt: {
                push_scope(Scope::Kind::Block);
                for (auto& s : n.as<CompoundStmt>()->stmts)
                    if (s) visit_stmt(*s);
                pop_scope();
                break;
            }
            case NodeKind::IfStmt: {
                auto& s = *n.as<IfStmt>();
                if (s.init_stmt)    visit_stmt(*s.init_stmt);
                if (s.condition)    visit_expr(*s.condition);
                if (s.then_branch)  visit_stmt(*s.then_branch);
                if (s.else_branch)  visit_stmt(*s.else_branch);
                break;
            }
            case NodeKind::ForStmt: {
                auto& s = *n.as<ForStmt>();
                push_scope(Scope::Kind::Block);
                if (s.init)       visit_stmt(*s.init);
                if (s.condition)  visit_expr(*s.condition);
                if (s.increment)  visit_expr(*s.increment);
                if (s.body)       visit_stmt(*s.body);
                pop_scope();
                break;
            }
            case NodeKind::RangeForStmt: {
                auto& s = *n.as<RangeForStmt>();
                push_scope(Scope::Kind::Block);
                if (s.range_expr) visit_expr(*s.range_expr);
                // Register the loop variable
                if (s.range_decl) {
                    auto& vd = *s.range_decl->as<VarDecl>();
                    TypeDesc td = resolve_type_desc(vd.type.get());
                    // If auto, infer from range expression type's element type
                    Symbol vs; vs.name = vd.name; vs.kind = SymbolKind::Variable;
                    vs.type = td;
                    current_->define(vd.name, vs);
                }
                if (s.body) visit_stmt(*s.body);
                pop_scope();
                break;
            }
            case NodeKind::WhileStmt: {
                auto& s = *n.as<WhileStmt>();
                if (s.condition) visit_expr(*s.condition);
                if (s.body)      visit_stmt(*s.body);
                break;
            }
            case NodeKind::DoWhileStmt: {
                auto& s = *n.as<DoWhileStmt>();
                if (s.body)      visit_stmt(*s.body);
                if (s.condition) visit_expr(*s.condition);
                break;
            }
            case NodeKind::SwitchStmt: {
                auto& s = *n.as<SwitchStmt>();
                if (s.control) visit_expr(*s.control);
                for (auto& c : s.cases) if (c) visit_stmt(*c);
                break;
            }
            case NodeKind::CaseStmt: {
                auto& s = *n.as<CaseStmt>();
                if (s.value) visit_expr(*s.value);
                for (auto& st : s.stmts) if (st) visit_stmt(*st);
                break;
            }
            case NodeKind::DefaultStmt:
                for (auto& st : n.as<DefaultStmt>()->stmts) if (st) visit_stmt(*st);
                break;
            case NodeKind::ReturnStmt:
                if (n.as<ReturnStmt>()->value) visit_expr(*n.as<ReturnStmt>()->value);
                break;
            case NodeKind::ExprStmt:
                if (n.as<ExprStmt>()->expr) visit_expr(*n.as<ExprStmt>()->expr);
                break;
            case NodeKind::DeclStmt:
                for (auto& d : n.as<DeclStmt>()->decls) if (d) visit_decl(*d);
                break;
            case NodeKind::TryStmt: {
                auto& s = *n.as<TryStmt>();
                if (s.try_body) visit_stmt(*s.try_body);
                for (auto& c : s.catches) if (c) visit_stmt(*c);
                break;
            }
            case NodeKind::CatchClause: {
                auto& cl = *n.as<CatchClause>();
                push_scope(Scope::Kind::Block);
                if (cl.param) visit_decl(*cl.param);
                if (cl.body)  visit_stmt(*cl.body);
                pop_scope();
                break;
            }
            case NodeKind::ThrowStmt:
                if (n.as<ThrowStmt>()->expr) visit_expr(*n.as<ThrowStmt>()->expr);
                break;
            case NodeKind::LabelStmt:
                if (n.as<LabelStmt>()->stmt) visit_stmt(*n.as<LabelStmt>()->stmt);
                break;
            case NodeKind::VarDecl:
                visit_var(*n.as<VarDecl>());
                break;
            case NodeKind::ParamDecl:  break;
            case NodeKind::BreakStmt:
            case NodeKind::ContinueStmt:
            case NodeKind::NullStmt:
            case NodeKind::GotoStmt:   break;
            default: break;
        }
    }

    // ── Expression visitor (light analysis) ─────────────────────
    void visit_expr(Node& n) {
        switch (n.kind) {
            case NodeKind::BinaryExpr: {
                auto& e = *n.as<BinaryExpr>();
                if (e.left)  visit_expr(*e.left);
                if (e.right) visit_expr(*e.right);
                // Detect std::cout / std::cerr usage
                detect_cout_usage(e);
                break;
            }
            case NodeKind::AssignExpr: {
                auto& e = *n.as<AssignExpr>();
                if (e.target) visit_expr(*e.target);
                if (e.value)  visit_expr(*e.value);
                break;
            }
            case NodeKind::UnaryExpr: {
                auto& e = *n.as<UnaryExpr>();
                if (e.operand) visit_expr(*e.operand);
                break;
            }
            case NodeKind::TernaryExpr: {
                auto& e = *n.as<TernaryExpr>();
                if (e.condition) visit_expr(*e.condition);
                if (e.then_expr) visit_expr(*e.then_expr);
                if (e.else_expr) visit_expr(*e.else_expr);
                break;
            }
            case NodeKind::CallExpr: {
                auto& e = *n.as<CallExpr>();
                if (e.callee) visit_expr(*e.callee);
                for (auto& a : e.args) if (a) visit_expr(*a);
                break;
            }
            case NodeKind::MemberExpr:
                if (n.as<MemberExpr>()->object) visit_expr(*n.as<MemberExpr>()->object);
                break;
            case NodeKind::IndexExpr: {
                auto& e = *n.as<IndexExpr>();
                if (e.object) visit_expr(*e.object);
                if (e.index)  visit_expr(*e.index);
                break;
            }
            case NodeKind::NewExpr: {
                auto& e = *n.as<NewExpr>();
                for (auto& a : e.args) if (a) visit_expr(*a);
                break;
            }
            case NodeKind::DeleteExpr:
                if (n.as<DeleteExpr>()->expr) visit_expr(*n.as<DeleteExpr>()->expr);
                break;
            case NodeKind::CastExpr:
                if (n.as<CastExpr>()->expr) visit_expr(*n.as<CastExpr>()->expr);
                break;
            case NodeKind::ParenExpr:
                if (n.as<ParenExpr>()->inner) visit_expr(*n.as<ParenExpr>()->inner);
                break;
            case NodeKind::CommaExpr: {
                auto& e = *n.as<CommaExpr>();
                if (e.left)  visit_expr(*e.left);
                if (e.right) visit_expr(*e.right);
                break;
            }
            case NodeKind::IdentifierExpr: {
                auto& e = *n.as<IdentifierExpr>();
                // Check if declared
                auto* sym = current_->lookup(e.name);
                if (!sym) {
                    // Not necessarily an error (could be template param, etc.)
                    // Only warn for identifiers that look like user code
                }
                break;
            }
            case NodeKind::InitListExpr:
                for (auto& el : n.as<InitListExpr>()->elements) if (el) visit_expr(*el);
                break;
            case NodeKind::LambdaExpr: {
                auto& lam = *n.as<LambdaExpr>();
                push_scope(Scope::Kind::Function);
                for (auto& p : lam.params) if (p) visit_decl(*p);
                if (lam.body) visit_stmt(*lam.body);
                pop_scope();
                break;
            }
            case NodeKind::ScopeExpr: {
                auto& e = *n.as<ScopeExpr>();
                // Check for std:: usage
                if (!e.scope.empty() && e.scope[0] == "std") {
                    ctx_.used_std_types.insert(e.name);
                    if (e.name == "cout")  ctx_.uses_cout = true;
                    if (e.name == "cerr")  ctx_.uses_cerr = true;
                    if (e.name == "cin")   ctx_.uses_cin  = true;
                    if (e.name == "endl")  {} // handled
                }
                break;
            }
            default: break;
        }
    }

    void detect_cout_usage(BinaryExpr& e) {
        // Check if the left-most node is std::cout
        Node* n = e.left.get();
        if (!n) return;
        if (n->kind == NodeKind::ScopeExpr) {
            auto& se = *n->as<ScopeExpr>();
            if (se.name == "cout" || se.name == "cerr") {
                ctx_.uses_cout = true;
            }
        }
        if (n->kind == NodeKind::BinaryExpr) {
            detect_cout_usage(*n->as<BinaryExpr>());
        }
    }

    // ── Type resolution ──────────────────────────────────────────
    TypeDesc resolve_type_desc(const Node* n) const {
        if (!n) return TypeDesc::make_unknown();
        switch (n->kind) {
            case NodeKind::PrimitiveType: {
                auto& pt = *static_cast<const PrimitiveType*>(n);
                switch (pt.prim_kind) {
                    case PrimitiveType::Kind::Void:       return TypeDesc::make_void();
                    case PrimitiveType::Kind::Bool:       return TypeDesc::make_bool();
                    case PrimitiveType::Kind::Auto:       return TypeDesc::make_auto();
                    case PrimitiveType::Kind::Char:       return TypeDesc::make(TypeTag::Char);
                    case PrimitiveType::Kind::Short:      { auto d = TypeDesc::make(TypeTag::Int16); return d; }
                    case PrimitiveType::Kind::Int:        return TypeDesc::make_int();
                    case PrimitiveType::Kind::Long:       return TypeDesc::make(TypeTag::Int64);
                    case PrimitiveType::Kind::LongLong:   return TypeDesc::make(TypeTag::Int64);
                    case PrimitiveType::Kind::UInt:       return TypeDesc::make(TypeTag::UInt32);
                    case PrimitiveType::Kind::UShort:     return TypeDesc::make(TypeTag::UInt16);
                    case PrimitiveType::Kind::ULong:      return TypeDesc::make(TypeTag::UInt64);
                    case PrimitiveType::Kind::ULongLong:  return TypeDesc::make(TypeTag::UInt64);
                    case PrimitiveType::Kind::Float:      return TypeDesc::make(TypeTag::Float);
                    case PrimitiveType::Kind::Double:     return TypeDesc::make(TypeTag::Double);
                    case PrimitiveType::Kind::LongDouble: return TypeDesc::make(TypeTag::LongDouble);
                    default:                              return TypeDesc::make_unknown();
                }
            }
            case NodeKind::NamedType: {
                auto& nt = *static_cast<const NamedType*>(n);
                std::string qname = nt.qualified_name();
                // Check if it's a known class
                auto it = ctx_.classes.find(nt.name);
                if (it != ctx_.classes.end()) return TypeDesc::make_class(nt.name);
                // Check symbol table
                auto* sym = current_->lookup(nt.name);
                if (sym) return sym->type;
                // Assume it's a class type
                return TypeDesc::make_class(qname);
            }
            case NodeKind::TemplateInstType: {
                auto& ti = *static_cast<const TemplateInstType*>(n);
                TypeDesc d; d.tag = TypeTag::Class;
                d.class_name = ti.qualified_name();
                return d;
            }
            case NodeKind::QualifiedType: {
                auto& qt = *static_cast<const QualifiedType*>(n);
                auto inner = resolve_type_desc(qt.inner.get());
                inner.is_const    = qt.quals.is_const;
                inner.is_volatile = qt.quals.is_volatile;
                return inner;
            }
            case NodeKind::PointerType: {
                auto& pt = *static_cast<const PointerType*>(n);
                return TypeDesc::make_ptr(resolve_type_desc(pt.pointee.get()));
            }
            case NodeKind::ReferenceType: {
                auto& rt = *static_cast<const ReferenceType*>(n);
                return TypeDesc::make_ref(resolve_type_desc(rt.referee.get()));
            }
            case NodeKind::RValueRefType: {
                auto& rr = *static_cast<const RValueRefType*>(n);
                TypeDesc d; d.tag = TypeTag::RValueRef;
                d.inner = std::make_shared<TypeDesc>(resolve_type_desc(rr.referee.get()));
                return d;
            }
            case NodeKind::ArrayType: {
                auto& at = *static_cast<const ArrayType*>(n);
                return TypeDesc::make_array(resolve_type_desc(at.element.get()));
            }
            default:
                return TypeDesc::make_unknown();
        }
    }

    // ── Expression type inference (best-effort) ──────────────────
    TypeDesc infer_expr_type(const Node& n) const {
        switch (n.kind) {
            case NodeKind::IntLiteralExpr:
                return TypeDesc::make_int();
            case NodeKind::FloatLiteralExpr: {
                auto& fl = *static_cast<const FloatLiteralExpr*>(&n);
                return TypeDesc::make(fl.is_float ? TypeTag::Float : TypeTag::Double);
            }
            case NodeKind::StringLiteralExpr:
                return TypeDesc::make_ptr(TypeDesc::make(TypeTag::Char));
            case NodeKind::BoolLiteralExpr:
                return TypeDesc::make_bool();
            case NodeKind::NullptrExpr:
                return TypeDesc::make(TypeTag::Nullptr_t);
            case NodeKind::IdentifierExpr: {
                auto& e = *static_cast<const IdentifierExpr*>(&n);
                auto* sym = current_->lookup(e.name);
                return sym ? sym->type : TypeDesc::make_unknown();
            }
            case NodeKind::BinaryExpr: {
                auto& e = *static_cast<const BinaryExpr*>(&n);
                if (!e.left) return TypeDesc::make_unknown();
                return infer_expr_type(*e.left);  // rough approximation
            }
            case NodeKind::NewExpr: {
                auto& e = *static_cast<const NewExpr*>(&n);
                if (e.type) return TypeDesc::make_ptr(resolve_type_desc(e.type.get()));
                return TypeDesc::make_unknown();
            }
            case NodeKind::CallExpr:
                // Without full overload resolution, return unknown
                return TypeDesc::make_unknown();
            case NodeKind::MemberExpr:
                return TypeDesc::make_unknown();
            default:
                return TypeDesc::make_unknown();
        }
    }
};

// ─────────────────────────────────────────────────────────────────
//  Convenience wrapper
// ─────────────────────────────────────────────────────────────────
inline SemanticContext analyze(TranslationUnit& tu,
                                std::vector<Diagnostic>& out_diags) {
    SemanticAnalyzer sa;
    sa.analyze(tu);
    for (auto& d : sa.diagnostics()) out_diags.push_back(d);
    return sa.context();
}

} // namespace rexc
