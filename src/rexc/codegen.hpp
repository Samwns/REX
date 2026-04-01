#pragma once
/*
 * codegen.hpp  –  REXC C Code Generator
 *
 * Transforms a C++ AST into valid C11 source code that can be
 * compiled with  gcc -x c  or  cc.
 *
 * Transformation strategy (matching the features in the test program):
 *
 *   std::string         → rexc_str  (from runtime.h)
 *   std::vector<T*>     → rexc_vec  (from runtime.h, void* elements)
 *   std::cout << x      → rexc_cout_*(&rexc_stdout_stream, x)
 *   C++ class           → C struct + vtable struct (if virtual methods exist)
 *   Inheritance         → first struct member is base class (by value)
 *   Methods             → free functions with  Self* self  first parameter
 *   Constructors        → _ctor(Self*, ...)  functions
 *   Destructors         → _dtor(Self*)  functions
 *   new T(args)         → REXC_NEW(T); T_ctor(ptr, args)
 *   delete ptr          → T_dtor(ptr); REXC_DELETE(ptr)
 *   Range-for           → index-based for loop over rexc_vec
 *   Virtual calls       → self->__vt->method(self, ...)
 *   Namespaces          → name mangling  ns__name
 *   Templates           → emit specialised copies as they are instantiated
 *   Exceptions          → rexc_throw_msg()/try with longjmp stubs
 */

#include "ast.hpp"
#include "semantic.hpp"

#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────
namespace detail {

// Mangle a qualified C++ name to a valid C identifier
inline std::string mangle(const std::string& cpp_name) {
    std::string r;
    r.reserve(cpp_name.size());
    for (char c : cpp_name) {
        if (c == ':') { if (!r.empty() && r.back() != '_') r += '_'; }
        else if (std::isalnum(c) || c == '_') r += c;
        else r += '_';
    }
    return r;
}

// Operator name for C function  (e.g. "+" → "op_add")
inline std::string op_mangle(const std::string& op) {
    if (op == "+")   return "op_add";
    if (op == "-")   return "op_sub";
    if (op == "*")   return "op_mul";
    if (op == "/")   return "op_div";
    if (op == "%")   return "op_mod";
    if (op == "==")  return "op_eq";
    if (op == "!=")  return "op_ne";
    if (op == "<")   return "op_lt";
    if (op == ">")   return "op_gt";
    if (op == "<=")  return "op_le";
    if (op == ">=")  return "op_ge";
    if (op == "<<")  return "op_shl";
    if (op == ">>")  return "op_shr";
    if (op == "&&")  return "op_and";
    if (op == "||")  return "op_or";
    if (op == "!")   return "op_not";
    if (op == "&")   return "op_bitand";
    if (op == "|")   return "op_bitor";
    if (op == "^")   return "op_xor";
    if (op == "~")   return "op_bitnot";
    if (op == "[]")  return "op_index";
    if (op == "()")  return "op_call";
    if (op == "=")   return "op_assign";
    if (op == "+=")  return "op_add_assign";
    if (op == "-=")  return "op_sub_assign";
    if (op == "++")  return "op_inc";
    if (op == "--")  return "op_dec";
    // Conversion operators
    return "op_conv_" + mangle(op);
}

// Strip outer const/volatile from a type node (for variable declarations)
inline const Node* strip_qual(const Node* n) {
    if (!n) return nullptr;
    if (n->kind == NodeKind::QualifiedType) {
        return static_cast<const QualifiedType*>(n)->inner.get();
    }
    return n;
}

// Check if identifier is a known ANSI terminal color/style constant
inline bool is_ansi_identifier(const std::string& name) {
    static const std::unordered_set<std::string> ansi_ids = {
        "reset", "bold", "dim", "italic", "underline", "blink",
        "reverse", "strikethrough",
        "fg_black", "fg_red", "fg_green", "fg_yellow",
        "fg_blue", "fg_magenta", "fg_cyan", "fg_white",
        "fg_bright_black", "fg_bright_red", "fg_bright_green", "fg_bright_yellow",
        "fg_bright_blue", "fg_bright_magenta", "fg_bright_cyan", "fg_bright_white",
        "bg_black", "bg_red", "bg_green", "bg_yellow",
        "bg_blue", "bg_magenta", "bg_cyan", "bg_white",
        "bg_bright_black", "bg_bright_red", "bg_bright_green", "bg_bright_yellow",
        "bg_bright_blue", "bg_bright_magenta", "bg_bright_cyan", "bg_bright_white"
    };
    return ansi_ids.count(name) > 0;
}

// Check if a callee name is an ANSI helper function returning const char*
inline bool is_ansi_cstr_function(const std::string& name) {
    return name == "fg_rgb" || name == "bg_rgb"
        || name == "fg_color256" || name == "bg_color256";
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────
//  CodeGenerator
// ─────────────────────────────────────────────────────────────────
class CodeGenerator {
public:
    CodeGenerator(const SemanticContext& ctx,
                  const std::string& runtime_include_path)
        : ctx_(ctx), runtime_path_(runtime_include_path) {}

    // Generate C source for the entire translation unit
    std::string generate(const TranslationUnit& tu) {
        out_.str(""); out_.clear();
        fwd_.str(""); fwd_.clear();

        // Header
        emit_file_header();

        // First pass: collect class info and emit forward declarations
        collect_classes(tu);
        emit_vtable_structs();
        emit_class_forward_decls();
        emit_class_struct_defs();

        // Main emission pass
        for (auto& d : tu.decls) {
            if (d) emit_top_level_decl(*d);
        }

        // Compose final output
        std::string result;
        result  = out_.str();
        result += "\n/* === Forward declarations ===*/\n";
        result += fwd_.str();
        result += "\n/* === Implementations === */\n";
        result += impl_.str();

        return result;
    }

    // Generate complete C source as a single string (preferred entry point)
    std::string generate_source(const TranslationUnit& tu) {
        out_.str(""); out_.clear();
        fwd_.str(""); fwd_.clear();
        impl_.str(""); impl_.clear();

        // ── Preamble ─────────────────────────────────────────────
        out_ << "/* Generated by REXC - REX C++ Compiler */\n";
        out_ << "/* Do not edit this file manually       */\n\n";
        out_ << "#include \"runtime.h\"\n";  // found via -I include path
        out_ << "#include <stdio.h>\n";
        out_ << "#include <stdlib.h>\n";
        out_ << "#include <string.h>\n";
        out_ << "#include <stdint.h>\n";
        out_ << "#include <math.h>\n\n";

        // ── Preprocessor passthrough (includes / defines) ────────
        for (auto& d : tu.decls) {
            if (d && d->kind == NodeKind::PreprocessorDecl) {
                const std::string& dir = d->as<PreprocessorDecl>()->directive;
                // Pass through #define and #pragma; skip #include (use runtime)
                if (dir.size() > 1) {
                    std::string rest = dir.substr(1);
                    // ltrim
                    size_t s = rest.find_first_not_of(" \t");
                    if (s != std::string::npos) rest = rest.substr(s);
                    if (rest.substr(0,7) == "include") {
                        // Skip – replaced with runtime.h; keep as comment
                        out_ << "/* " << dir << " */\n";
                        continue;
                    }
                    if (rest.substr(0,6) == "define" || rest.substr(0,6) == "pragma" ||
                        rest.substr(0,5) == "undef" || rest.substr(0,5) == "ifdef" ||
                        rest.substr(0,6) == "ifndef" || rest.substr(0,5) == "endif" ||
                        rest.substr(0,4) == "else" || rest.substr(0,5) == "error") {
                        out_ << "#" << dir << "\n";
                    }
                }
            }
        }
        out_ << "\n";

        // ── Collect class metadata and forward-declare structs ────
        collect_classes(tu);
        emit_vtable_structs();
        emit_class_forward_decls();
        emit_class_struct_defs();

        // Flush to out_
        out_ << fwd_.str();
        fwd_.str(""); fwd_.clear();

        // ── Emit function prototypes ──────────────────────────────
        emit_function_prototypes(tu);
        out_ << fwd_.str();
        fwd_.str(""); fwd_.clear();

        out_ << "\n";

        // ── Emit implementation ───────────────────────────────────
        for (auto& d : tu.decls) {
            if (d && d->kind != NodeKind::PreprocessorDecl) {
                emit_top_level_decl(*d);
            }
        }
        out_ << impl_.str();

        return out_.str();
    }

private:
    const SemanticContext& ctx_;
    std::string            runtime_path_;

    std::ostringstream     out_;   // preamble / declarations
    std::ostringstream     fwd_;   // forward declarations
    std::ostringstream     impl_;  // function bodies
    int                    indent_ = 0;

    // ── Class layout cache ───────────────────────────────────────
    struct ClassLayout {
        std::string name;
        std::string c_struct_name;      // e.g. "Animal"
        std::string vtable_struct_name; // e.g. "Animal_VTable"
        bool        has_vtable  = false;
        std::vector<std::string> virtual_methods;
        std::vector<std::string> bases;
        std::vector<std::pair<std::string,std::string>> fields; // (c_type, name)
        bool        emitted_fwd   = false;
        bool        emitted_struct= false;
    };
    std::unordered_map<std::string, ClassLayout> class_layouts_;
    std::vector<std::string> class_order_;  // topological order

    // Lambda counter for anonymous struct names
    int lambda_counter_ = 0;
    int tmp_counter_    = 0;

    std::string next_tmp() {
        return "__rexc_tmp" + std::to_string(tmp_counter_++);
    }

    // Currently emitting which class (for member access resolution)
    std::string current_emit_class_;

    // Track variable types for cin >> type inference
    mutable std::unordered_map<std::string, std::string> cin_var_types_;

    // ── Indentation ──────────────────────────────────────────────
    std::string ind() const { return std::string(indent_ * 4, ' '); }

    // Emit to current active stream
    std::ostringstream* active_ = nullptr;
    void out(const std::string& s) {
        if (active_) *active_ << s;
        else          out_ << s;
    }

    // ── File header ──────────────────────────────────────────────
    void emit_file_header() {
        out_ << "/* Generated by REXC - REX C++ Compiler */\n";
        out_ << "#include \"" << runtime_path_ << "\"\n";
        out_ << "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n";
    }

    // ─────────────────────────────────────────────────────────────
    //  Class collection & layout
    // ─────────────────────────────────────────────────────────────
    void collect_classes(const TranslationUnit& tu) {
        for (auto& d : tu.decls) {
            if (!d) continue;
            if (d->kind == NodeKind::ClassDecl) {
                collect_class(*d->as<ClassDecl>());
            } else if (d->kind == NodeKind::TemplateDecl) {
                auto& tmpl = *d->as<TemplateDecl>();
                if (tmpl.body && tmpl.body->kind == NodeKind::ClassDecl) {
                    collect_class(*tmpl.body->as<ClassDecl>());
                }
            }
        }
    }

    void collect_class(const ClassDecl& cls) {
        if (class_layouts_.count(cls.name)) return;
        ClassLayout lay;
        lay.name              = cls.name;
        lay.c_struct_name     = cls.name;
        lay.vtable_struct_name= cls.name + "_VTable";

        // Collect base classes first
        for (auto& base : cls.bases) {
            lay.bases.push_back(base.name);
            // Look up the base in already-collected classes
            auto it = ctx_.classes.find(base.name);
            if (it != ctx_.classes.end() && it->second.has_vtable)
                lay.has_vtable = true;
        }

        // Collect virtual methods from semantic context
        auto it = ctx_.classes.find(cls.name);
        if (it != ctx_.classes.end()) {
            lay.has_vtable      = it->second.has_vtable;
            lay.virtual_methods = it->second.virtual_methods;
        }

        // Scan members for virtual methods (in case semantic missed some)
        for (auto& m : cls.members) {
            if (!m) continue;
            if (m->kind == NodeKind::FunctionDecl) {
                auto& fn = *m->as<FunctionDecl>();
                if (fn.is_virtual) {
                    lay.has_vtable = true;
                    if (std::find(lay.virtual_methods.begin(), lay.virtual_methods.end(),
                                  fn.name) == lay.virtual_methods.end()) {
                        lay.virtual_methods.push_back(fn.name);
                    }
                }
            }
        }

        class_layouts_[cls.name] = std::move(lay);
        class_order_.push_back(cls.name);
    }

    // Emit  struct ClassName_VTable { ... };  for every class with virtuals
    void emit_vtable_structs() {
        // We need full method signatures; scan the AST for each class.
        // We'll emit these after classes are collected.
    }

    // Emit  typedef struct X X;  for every class
    void emit_class_forward_decls() {
        for (auto& name : class_order_) {
            auto& lay = class_layouts_[name];
            fwd_ << "typedef struct " << lay.c_struct_name
                 << " " << lay.c_struct_name << ";\n";
            if (lay.has_vtable) {
                // Only emit a struct forward decl for the vtable if this class
                // does NOT reuse a base class vtable (derived classes emit a
                // typedef later in emit_class_decl; we must not pre-declare it
                // as a struct because that would conflict with the typedef).
                bool reuses_base_vtable = false;
                for (auto& b : lay.bases) {
                    auto bit = class_layouts_.find(b);
                    if (bit != class_layouts_.end() && bit->second.has_vtable) {
                        reuses_base_vtable = true;
                        break;
                    }
                }
                if (!reuses_base_vtable) {
                    fwd_ << "typedef struct " << lay.vtable_struct_name
                         << " " << lay.vtable_struct_name << ";\n";
                }
            }
        }
        if (!class_order_.empty()) fwd_ << "\n";
    }

    // Emit  struct ClassName { ... };  for every class
    void emit_class_struct_defs() {
        // We defer actual struct body emission to emit_class_decl
        // because we need the full member list from the AST.
    }

    // Emit function prototypes for non-member functions
    void emit_function_prototypes(const TranslationUnit& tu) {
        // Emitted inline when we process each decl
    }

    // ─────────────────────────────────────────────────────────────
    //  Top-level declaration emitter
    // ─────────────────────────────────────────────────────────────
    void emit_top_level_decl(const Node& n) {
        switch (n.kind) {
            case NodeKind::ClassDecl:
                emit_class_decl(*n.as<ClassDecl>());
                break;
            case NodeKind::FunctionDecl:
                emit_function_decl(*n.as<FunctionDecl>(), "");
                break;
            case NodeKind::VarDecl:
                emit_var_decl(*n.as<VarDecl>(), out_);
                out_ << ";\n";
                break;
            case NodeKind::DeclStmt:
                for (auto& d : n.as<DeclStmt>()->decls) {
                    if (d) emit_top_level_decl(*d);
                }
                break;
            case NodeKind::NamespaceDecl:
                // Emit contents (C has no namespaces, we flatten)
                for (auto& d : n.as<NamespaceDecl>()->decls)
                    if (d) emit_top_level_decl(*d);
                break;
            case NodeKind::UsingDecl:
                // using namespace → no output needed (already resolved)
                // using Type = ... → typedef
                if (n.as<UsingDecl>()->form == UsingDecl::Form::Alias &&
                    n.as<UsingDecl>()->type_alias) {
                    auto& u = *n.as<UsingDecl>();
                    out_ << "typedef ";
                    out_ << emit_type_str(u.type_alias.get()) << " " << u.name << ";\n";
                }
                break;
            case NodeKind::TypedefDecl: {
                auto& td = *n.as<TypedefDecl>();
                out_ << "typedef " << emit_type_str(td.type.get()) << " " << td.name << ";\n";
                break;
            }
            case NodeKind::EnumDecl:
                emit_enum_decl(*n.as<EnumDecl>(), out_);
                break;
            case NodeKind::TemplateDecl: {
                auto& tmpl = *n.as<TemplateDecl>();
                if (tmpl.body) {
                    // Emit the template body as a concrete definition
                    // (simplified: treat it as non-template)
                    emit_top_level_decl(*tmpl.body);
                }
                break;
            }
            case NodeKind::StaticAssertDecl:
                // emit as _Static_assert
                if (n.as<StaticAssertDecl>()->condition) {
                    out_ << "_Static_assert(";
                    out_ << emit_expr_str(*n.as<StaticAssertDecl>()->condition);
                    out_ << ", \"" << n.as<StaticAssertDecl>()->message << "\");\n";
                }
                break;
            case NodeKind::FriendDecl:
                if (n.as<FriendDecl>()->decl) emit_top_level_decl(*n.as<FriendDecl>()->decl);
                break;
            default: break;
        }
    }

    // ─────────────────────────────────────────────────────────────
    //  Class emission
    // ─────────────────────────────────────────────────────────────
    void emit_class_decl(const ClassDecl& cls) {
        if (cls.name.empty() || !cls.is_complete) return;

        auto& lay = class_layouts_[cls.name];

        // ── Vtable struct ─────────────────────────────────────────
        if (lay.has_vtable) {
            // Derived classes reuse root base vtable via typedef for layout compatibility
            std::string root_base;
            for (auto& b : lay.bases) {
                auto bit = class_layouts_.find(b);
                if (bit != class_layouts_.end() && bit->second.has_vtable) {
                    root_base = b; break;
                }
            }
            out_ << "/* VTable for " << cls.name << " */\n";
            if (!root_base.empty()) {
                out_ << "typedef " << root_base << "_VTable " << cls.name << "_VTable;\n\n";
            } else {
                out_ << "struct " << lay.vtable_struct_name << " {\n";
                for (auto& m : cls.members) {
                    if (!m || m->kind != NodeKind::FunctionDecl) continue;
                    auto& fn = *m->as<FunctionDecl>();
                    if (!fn.is_virtual && !fn.is_override) continue;
                    out_ << "    " << emit_type_str(fn.return_type.get()) << " (*"
                         << fn.name << ")(const " << cls.name << "*";
                    for (auto& p : fn.params) {
                        if (!p) continue;
                        out_ << ", " << emit_type_str(p->as<ParamDecl>()->type.get());
                    }
                    out_ << ");\n";
                }
                out_ << "};\n\n";
            }
        }
        // ── Struct definition ─────────────────────────────────────
        // Determine whether this class introduces the vtable or inherits one
        bool has_vtable_base = false;
        for (auto& b : lay.bases) {
            auto bit = class_layouts_.find(b);
            if (bit != class_layouts_.end() && bit->second.has_vtable) {
                has_vtable_base = true; break;
            }
        }

        out_ << "/* Struct for " << cls.name << " */\n";
        out_ << "struct " << cls.name << " {\n";

        // Embedded base class(es) come FIRST so that a pointer to the derived
        // struct is also a valid pointer to the base subobject (C++ layout rule).
        for (auto& base : cls.bases) {
            out_ << "    " << base.name << " __base_" << base.name << ";\n";
        }

        // Vtable pointer only if this class is the root of the vtable hierarchy
        // (derived classes share the vtable through the embedded base subobject)
        if (lay.has_vtable && !has_vtable_base) {
            out_ << "    " << lay.vtable_struct_name << "* __vt;\n";
        }

        // Fields (may be FieldDecl or VarDecl depending on parser path)
        for (auto& m : cls.members) {
            if (!m) continue;
            if (m->kind == NodeKind::FieldDecl) {
                auto& fld = *m->as<FieldDecl>();
                if (fld.is_static) continue;
                out_ << "    " << emit_type_str(fld.type.get()) << " " << fld.name << ";\n";
            } else if (m->kind == NodeKind::VarDecl) {
                auto& v = *m->as<VarDecl>();
                if (v.is_static) continue;
                out_ << "    " << emit_type_str(v.type.get()) << " " << v.name << ";\n";
            } else if (m->kind == NodeKind::DeclStmt) {
                for (auto& d : m->as<DeclStmt>()->decls) {
                    if (!d || d->kind != NodeKind::VarDecl) continue;
                    auto& v = *d->as<VarDecl>();
                    if (v.is_static) continue;
                    out_ << "    " << emit_type_str(v.type.get()) << " " << v.name << ";\n";
                }
            }
        }
        out_ << "};\n\n";

        // ── VTable instance (forward declared; filled after methods) ─
        if (lay.has_vtable) {
            out_ << "static " << lay.vtable_struct_name << " "
                 << cls.name << "__vtable;\n\n";
        }

        // ── Static field definitions ──────────────────────────────
        for (auto& m : cls.members) {
            if (!m || m->kind != NodeKind::FieldDecl) continue;
            auto& fld = *m->as<FieldDecl>();
            if (!fld.is_static) continue;
            out_ << "static " << emit_type_str(fld.type.get())
                 << " " << cls.name << "_" << fld.name;
            if (fld.init) out_ << " = " << emit_expr_str(*fld.init);
            out_ << ";\n";
        }

        // ── Methods (constructor, destructor, regular, virtual) ───
        for (auto& m : cls.members) {
            if (!m) continue;
            switch (m->kind) {
                case NodeKind::ConstructorDecl:
                    emit_constructor_def(*m->as<ConstructorDecl>(), cls);
                    break;
                case NodeKind::DestructorDecl:
                    emit_destructor_def(*m->as<DestructorDecl>(), cls);
                    break;
                case NodeKind::FunctionDecl:
                    emit_function_decl(*m->as<FunctionDecl>(), cls.name);
                    break;
                default: break;
            }
        }

        // ── VTable initialiser ────────────────────────────────────
        if (lay.has_vtable) {
            emit_vtable_init(cls, lay);
        }
    }

    void emit_vtable_init(const ClassDecl& cls, const ClassLayout& lay) {
        // Determine the effective vtable struct (from first base that has virtuals,
        // or from this class itself)
        std::string vt_struct = lay.vtable_struct_name;
        // If this class inherits a vtable but defines no new virtual methods,
        // use the base's vtable struct
        if (lay.virtual_methods.empty() && !lay.bases.empty()) {
            auto it = class_layouts_.find(lay.bases[0]);
            if (it != class_layouts_.end() && it->second.has_vtable) {
                vt_struct = it->second.vtable_struct_name;
            }
        }

        out_ << "static void " << cls.name << "__init_vtable(void) __attribute__((constructor));\n";
        out_ << "static void " << cls.name << "__init_vtable(void) {\n";

        // Fill each virtual slot
        for (auto& m : cls.members) {
            if (!m || m->kind != NodeKind::FunctionDecl) continue;
            auto& fn = *m->as<FunctionDecl>();
            if (!fn.is_virtual && !fn.is_override) continue;
            out_ << "    " << cls.name << "__vtable." << fn.name
                 << " = (" << emit_type_str(fn.return_type.get())
                 << "(*)(" << cls.name << " const*";
            for (auto& p : fn.params) {
                if (!p) continue;
                auto& param = *p->as<ParamDecl>();
                out_ << ", " << emit_type_str(param.type.get());
            }
            out_ << ")) " << cls.name << "_" << fn.name << ";\n";
        }

        // Inherit slots from base classes
        if (!lay.bases.empty()) {
            std::string base = lay.bases[0];
            auto it = class_layouts_.find(base);
            if (it != class_layouts_.end() && it->second.has_vtable) {
                // Copy base vtable entries that are not overridden
                out_ << "    /* inherit from " << base << " */\n";
                // We do this at runtime init by copying the base vtable
                out_ << "    /* (base vtable slots copied if not overridden) */\n";
            }
        }
        out_ << "}\n\n";
    }

    // ── Constructor ──────────────────────────────────────────────
    void emit_constructor_def(const ConstructorDecl& ctor, const ClassDecl& cls) {
        if (!ctor.body && ctor.init_list.empty()) return;

        std::string prev_class = current_emit_class_;
        current_emit_class_ = cls.name;

        std::string c_name = cls.name + "_ctor";
        out_ << cls.name << "* " << c_name << "(" << cls.name << "* self";
        for (auto& p : ctor.params) {
            if (!p) continue;
            auto& param = *p->as<ParamDecl>();
            if (param.is_variadic) { out_ << ", ..."; break; }
            out_ << ", " << emit_type_str(param.type.get());
            if (!param.name.empty()) out_ << " " << param.name;
        }
        out_ << ") {\n";

        // Set vtable pointer
        auto& lay = class_layouts_[cls.name];
        if (lay.has_vtable) {
            out_ << "    self->__vt = &" << cls.name << "__vtable;\n";
        }

        // Call base class constructor(s) via initialiser list
        for (auto& init : ctor.init_list) {
            // Is this a base class initialiser?
            bool is_base = false;
            for (auto& base : cls.bases) {
                if (base.name == init.name) { is_base = true; break; }
            }
            if (is_base) {
                out_ << "    " << init.name << "_ctor(&self->__base_" << init.name;
                for (auto& arg : init.args) out_ << ", " << emit_expr_str(*arg);
                out_ << ");\n";
                // Override vtable (we override the base's vtable with ours)
                if (lay.has_vtable) {
                    out_ << "    self->__base_" << init.name << ".__vt = (";
                    // Use the base's vtable type
                    auto it = class_layouts_.find(init.name);
                    if (it != class_layouts_.end() && it->second.has_vtable) {
                        out_ << it->second.vtable_struct_name << "*";
                    } else {
                        out_ << "void*";
                    }
                    out_ << ")&" << cls.name << "__vtable;\n";
                }
                continue;
            }
            // Field initialiser
            if (init.args.size() == 1) {
                std::string arg_str = emit_expr_str(*init.args[0], "self");
                // Is this a std::string field?
                if (is_std_string_field(cls, init.name)) {
                    // arg might be rexc_str (by value) or rexc_str* - use rexc_str_copy
                    out_ << "    self->" << init.name
                         << " = rexc_str_copy(&(" << arg_str << "));\n";
                } else {
                    out_ << "    self->" << init.name << " = " << arg_str << ";\n";
                }
            } else if (init.args.size() > 1) {
                // Multi-arg initialiser: call constructor
                out_ << "    " << init.name << "_ctor(&self->" << init.name;
                for (auto& arg : init.args) out_ << ", " << emit_expr_str(*arg, "self");
                out_ << ");\n";
            }
        }

        // Body
        if (ctor.body) emit_compound_stmt(*ctor.body->as<CompoundStmt>(), "self");
        out_ << "    return self;\n}\n\n";
        current_emit_class_ = prev_class;
    }

    bool is_std_string_field(const ClassDecl& cls, const std::string& field_name) const {
        for (auto& m : cls.members) {
            if (!m) continue;
            const Node* type_node = nullptr;
            std::string name;
            if (m->kind == NodeKind::FieldDecl) {
                auto& fld = *m->as<FieldDecl>();
                name = fld.name; type_node = fld.type.get();
            } else if (m->kind == NodeKind::VarDecl) {
                auto& vd = *m->as<VarDecl>();
                name = vd.name; type_node = vd.type.get();
            }
            if (name != field_name || !type_node) continue;
            return type_is_std_string(type_node);
        }
        return false;
    }

    // ── Destructor ───────────────────────────────────────────────
    void emit_destructor_def(const DestructorDecl& dtor, const ClassDecl& cls) {
        if (!dtor.body) return;
        out_ << "void " << cls.name << "_dtor(" << cls.name << "* self) {\n";
        emit_compound_stmt(*dtor.body->as<CompoundStmt>(), "self");
        out_ << "}\n\n";
    }

    // ── Function declaration ─────────────────────────────────────
    void emit_function_decl(const FunctionDecl& fn, const std::string& class_name) {
        if (fn.is_deleted || fn.is_pure_virtual) return;
        if (fn.name.empty()) return;

        // Determine C function name
        std::string c_name;
        if (fn.is_operator) {
            c_name = (class_name.empty() ? "" : class_name + "_")
                   + detail::op_mangle(fn.operator_token);
        } else if (!class_name.empty()) {
            // Qualified name like Foo::bar → Foo_bar
            if (!fn.scope.empty())
                c_name = fn.scope[0] + "_" + fn.name;
            else
                c_name = class_name + "_" + fn.name;
        } else {
            if (!fn.scope.empty())
                c_name = fn.scope[0] + "_" + fn.name;
            else
                c_name = fn.name;
        }

        // main() stays main
        if (fn.name == "main") c_name = "main";

        // ── Prototype ─────────────────────────────────────────────
        std::string ret_type_str = fn.return_type ? emit_type_str(fn.return_type.get())
                                                   : "void";

        std::string proto;
        if (fn.is_static && !class_name.empty()) proto += "static ";
        if (fn.is_inline && fn.body) proto += "static inline ";
        proto += ret_type_str + " " + c_name + "(";

        bool first_param = true;

        // Implicit self pointer for non-static member functions
        if (!class_name.empty() && !fn.is_static && fn.name != "main") {
            proto += (fn.is_const ? "const " : "") + class_name + "* self";
            first_param = false;
        }

        for (auto& p : fn.params) {
            if (!p) continue;
            auto& param = *p->as<ParamDecl>();
            if (!first_param) proto += ", ";
            first_param = false;
            if (param.is_variadic) { proto += "..."; break; }
            proto += emit_type_str(param.type.get());
            if (!param.name.empty()) proto += " " + param.name;
        }
        if (first_param) proto += "void";
        proto += ")";

        // Emit prototype
        out_ << proto << ";\n";

        if (!fn.body) return;

        // ── Function body ─────────────────────────────────────────
        out_ << "\n" << proto << " {\n";
        indent_++;
        // Insert runtime init at start of main()
        if (fn.name == "main") {
            out_ << ind() << "rexc_runtime_init();\n";
        }
        // Track current class for member access resolution
        std::string prev_class = current_emit_class_;
        if (!class_name.empty()) current_emit_class_ = class_name;
        emit_compound_body(*fn.body->as<CompoundStmt>(), class_name.empty() ? "" : "self");
        current_emit_class_ = prev_class;
        indent_--;
        out_ << "}\n\n";
    }

    // ─────────────────────────────────────────────────────────────
    //  Statement emission
    // ─────────────────────────────────────────────────────────────
    void emit_compound_stmt(const CompoundStmt& cs, const std::string& self_name) {
        for (auto& s : cs.stmts) if (s) emit_stmt(*s, self_name);
    }

    void emit_compound_body(const CompoundStmt& cs, const std::string& self_name) {
        for (auto& s : cs.stmts) if (s) emit_stmt(*s, self_name);
    }

    void emit_stmt(const Node& n, const std::string& self_name) {
        switch (n.kind) {
            case NodeKind::CompoundStmt:
                out_ << ind() << "{\n";
                indent_++;
                emit_compound_stmt(*n.as<CompoundStmt>(), self_name);
                indent_--;
                out_ << ind() << "}\n";
                break;

            case NodeKind::IfStmt: {
                auto& s = *n.as<IfStmt>();
                if (s.init_stmt) { out_ << ind() << "{\n"; indent_++; emit_stmt(*s.init_stmt, self_name); }
                out_ << ind() << "if (";
                if (s.condition) out_ << emit_expr_str(*s.condition, self_name);
                out_ << ") ";
                if (s.then_branch) emit_stmt_inline(*s.then_branch, self_name);
                if (s.else_branch) {
                    out_ << ind() << "else ";
                    emit_stmt_inline(*s.else_branch, self_name);
                }
                if (s.init_stmt) { indent_--; out_ << ind() << "}\n"; }
                break;
            }

            case NodeKind::ForStmt: {
                auto& s = *n.as<ForStmt>();
                out_ << ind() << "for (";
                // init
                if (s.init) {
                    if (s.init->kind == NodeKind::DeclStmt) {
                        auto& ds = *s.init->as<DeclStmt>();
                        for (size_t i = 0; i < ds.decls.size(); i++) {
                            if (i > 0) out_ << ", ";
                            if (ds.decls[i]) out_ << emit_var_inline(*ds.decls[i]->as<VarDecl>(), self_name);
                        }
                    } else if (s.init->kind == NodeKind::VarDecl) {
                        out_ << emit_var_inline(*s.init->as<VarDecl>(), self_name);
                    } else if (s.init->kind == NodeKind::ExprStmt) {
                        out_ << emit_expr_str(*s.init->as<ExprStmt>()->expr, self_name);
                    } else {
                        out_ << emit_expr_str(*s.init, self_name);
                    }
                }
                out_ << "; ";
                if (s.condition) out_ << emit_expr_str(*s.condition, self_name);
                out_ << "; ";
                if (s.increment) out_ << emit_expr_str(*s.increment, self_name);
                out_ << ") ";
                if (s.body) emit_stmt_inline(*s.body, self_name);
                break;
            }

            case NodeKind::RangeForStmt: {
                auto& s = *n.as<RangeForStmt>();
                // Transform:  for (auto* a : animals)  →  for-loop over rexc_vec
                std::string range_var = next_tmp();

                // Determine element type - normalize auto/auto* to void*
                std::string elem_type = "void*";
                std::string cast_expr = "";
                if (s.range_decl) {
                    auto& vd = *s.range_decl->as<VarDecl>();
                    std::string t = emit_type_str(vd.type.get());
                    // Replace 'auto' with 'void*' for C compatibility
                    if (t == "auto" || t == "auto*") {
                        elem_type = "void*";
                    } else {
                        elem_type = t;
                    }
                    cast_expr = "(" + elem_type + ")";
                }

                std::string range_str = s.range_expr ? emit_expr_str(*s.range_expr, self_name) : "NULL";

                out_ << ind() << "{\n";
                indent_++;
                out_ << ind() << "rexc_vec __range_" << range_var
                     << " = (" << range_str << ");\n";
                out_ << ind() << "for (size_t __i_" << range_var
                     << " = 0; __i_" << range_var
                     << " < __range_" << range_var << ".size; "
                     << "__i_" << range_var << "++) {\n";
                indent_++;

                // Declare loop variable
                if (s.range_decl) {
                    auto& vd = *s.range_decl->as<VarDecl>();
                    // rexc_vec_at returns void*; for non-pointer types use an
                    // intermediate intptr_t cast to silence pointer-to-integer
                    // truncation warnings on 64-bit platforms.
                    // Strip trailing spaces before testing for '*' to handle
                    // any emit_type_str output variations.
                    std::string trimmed_elem = elem_type;
                    while (!trimmed_elem.empty() && trimmed_elem.back() == ' ')
                        trimmed_elem.pop_back();
                    bool is_ptr_type = !trimmed_elem.empty() && trimmed_elem.back() == '*';
                    std::string vec_cast = is_ptr_type
                        ? cast_expr
                        : cast_expr + "(intptr_t)";
                    out_ << ind() << elem_type << " " << vd.name
                         << " = " << vec_cast
                         << "rexc_vec_at(&__range_" << range_var
                         << ", __i_" << range_var << ");\n";
                }

                if (s.body) emit_stmt_inline(*s.body, self_name);

                indent_--;
                out_ << ind() << "}\n";
                indent_--;
                out_ << ind() << "}\n";
                break;
            }

            case NodeKind::WhileStmt: {
                auto& s = *n.as<WhileStmt>();
                out_ << ind() << "while (";
                if (s.condition) out_ << emit_expr_str(*s.condition, self_name);
                out_ << ") ";
                if (s.body) emit_stmt_inline(*s.body, self_name);
                break;
            }

            case NodeKind::DoWhileStmt: {
                auto& s = *n.as<DoWhileStmt>();
                out_ << ind() << "do ";
                if (s.body) emit_stmt_inline(*s.body, self_name);
                out_ << ind() << "while (";
                if (s.condition) out_ << emit_expr_str(*s.condition, self_name);
                out_ << ");\n";
                break;
            }

            case NodeKind::SwitchStmt: {
                auto& s = *n.as<SwitchStmt>();
                out_ << ind() << "switch (";
                if (s.control) out_ << emit_expr_str(*s.control, self_name);
                out_ << ") {\n";
                for (auto& c : s.cases) if (c) emit_stmt(*c, self_name);
                out_ << ind() << "}\n";
                break;
            }

            case NodeKind::CaseStmt: {
                auto& s = *n.as<CaseStmt>();
                out_ << ind() << "case " << emit_expr_str(*s.value, self_name) << ":\n";
                indent_++;
                for (auto& st : s.stmts) if (st) emit_stmt(*st, self_name);
                indent_--;
                break;
            }

            case NodeKind::DefaultStmt: {
                out_ << ind() << "default:\n";
                indent_++;
                for (auto& st : n.as<DefaultStmt>()->stmts) if (st) emit_stmt(*st, self_name);
                indent_--;
                break;
            }

            case NodeKind::ReturnStmt: {
                auto& s = *n.as<ReturnStmt>();
                out_ << ind() << "return";
                if (s.value) out_ << " " << emit_expr_str(*s.value, self_name);
                out_ << ";\n";
                break;
            }

            case NodeKind::BreakStmt:
                out_ << ind() << "break;\n";
                break;

            case NodeKind::ContinueStmt:
                out_ << ind() << "continue;\n";
                break;

            case NodeKind::GotoStmt:
                out_ << ind() << "goto " << n.as<GotoStmt>()->label << ";\n";
                break;

            case NodeKind::LabelStmt: {
                auto& s = *n.as<LabelStmt>();
                out_ << s.label << ":;\n";
                if (s.stmt) emit_stmt(*s.stmt, self_name);
                break;
            }

            case NodeKind::NullStmt:
                out_ << ind() << ";\n";
                break;

            case NodeKind::ExprStmt: {
                auto& s = *n.as<ExprStmt>();
                if (!s.expr) break;
                std::string es = emit_expr_str(*s.expr, self_name);
                if (!es.empty()) out_ << ind() << es << ";\n";
                break;
            }

            case NodeKind::DeclStmt: {
                for (auto& d : n.as<DeclStmt>()->decls) {
                    if (!d) continue;
                    if (d->kind == NodeKind::VarDecl) {
                        emit_var_decl(*d->as<VarDecl>(), out_, self_name);
                        out_ << ";\n";
                    } else {
                        emit_top_level_decl(*d);
                    }
                }
                break;
            }

            case NodeKind::VarDecl:
                emit_var_decl(*n.as<VarDecl>(), out_, self_name);
                out_ << ";\n";
                break;

            case NodeKind::TryStmt: {
                // Simplified: just emit the try body (exceptions become fatal)
                auto& s = *n.as<TryStmt>();
                out_ << ind() << "/* try */ {\n";
                indent_++;
                if (s.try_body) emit_compound_stmt(*s.try_body->as<CompoundStmt>(), self_name);
                indent_--;
                out_ << ind() << "}\n";
                // Skip catch clauses (they're dead code with our stub)
                for (auto& c : s.catches) {
                    out_ << ind() << "/* catch (skipped) */\n";
                }
                break;
            }

            case NodeKind::ThrowStmt: {
                auto& s = *n.as<ThrowStmt>();
                out_ << ind() << "rexc_throw_msg(";
                if (s.expr) out_ << emit_expr_str(*s.expr, self_name);
                else        out_ << "\"rethrow\"";
                out_ << ");\n";
                break;
            }

            default:
                break;
        }
    }

    // Emit a statement that might be a single-stmt or compound
    void emit_stmt_inline(const Node& n, const std::string& self_name) {
        if (n.kind == NodeKind::CompoundStmt) {
            out_ << "{\n";
            indent_++;
            emit_compound_stmt(*n.as<CompoundStmt>(), self_name);
            indent_--;
            out_ << ind() << "}\n";
        } else {
            out_ << "\n";
            indent_++;
            emit_stmt(n, self_name);
            indent_--;
        }
    }

    // ─────────────────────────────────────────────────────────────
    //  Variable declaration emission
    // ─────────────────────────────────────────────────────────────

    // Infer the C type for an 'auto' variable from its initialiser expression
    std::string resolve_auto_from_init(const Node* init) const {
        if (!init) return "auto";
        if (init->kind == NodeKind::CallExpr) {
            auto& ce = *init->as<CallExpr>();
            if (ce.callee) {
                std::string callee = emit_callee_name(*ce.callee, "");
                // roll::roll(f,n) / roll_roll / roll_roll_full → roll_result
                if (callee == "roll_roll_full" || callee == "roll_roll") return "roll_result";
                if (callee == "roll_character_stats")  return "int*";
            }
        }
        return "auto";
    }

    void emit_var_decl(const VarDecl& v, std::ostringstream& s,
                       const std::string& self_name = "") {
        std::string type_str = emit_type_str(v.type.get());
        bool is_vec = type_is_std_vector(v.type.get());

        // Resolve 'auto' to concrete C type when possible
        if (type_str == "auto" && v.init) {
            std::string resolved = resolve_auto_from_init(v.init.get());
            if (resolved != "auto") type_str = resolved;
        }

        // Handle unknown/unresolved type (auto or null→void) with InitListExpr:
        // emit as a C array with element type inferred from the first element.
        // e.g.  auto names = {"a","b"};  →  rexc_str names[] = {...};
        if ((type_str == "auto" || type_str == "void") && v.init &&
            v.init->kind == NodeKind::InitListExpr) {
            auto& il = *v.init->as<InitListExpr>();
            if (!il.elements.empty()) {
                const Node* first = il.elements[0].get();
                std::string elem_type = "int";
                if (first->kind == NodeKind::StringLiteralExpr) elem_type = "rexc_str";
                else if (first->kind == NodeKind::FloatLiteralExpr) elem_type = "double";
                else if (first->kind == NodeKind::CharLiteralExpr)  elem_type = "char";
                cin_var_types_[v.name] = elem_type;
                s << ind() << elem_type << " " << v.name << "[]";
                s << " = " << emit_init_expr(v.init.get(), nullptr, v.name, self_name);
                return;
            }
        }

        // Track variable type for cin >> / method call inference
        cin_var_types_[v.name] = type_str;

        // Check for array type (e.g. int arr[] = {1,2,3})
        bool is_array = (v.type && v.type->kind == NodeKind::ArrayType);

        if (is_vec) {
            // rexc_vec needs separate init
            s << ind() << "rexc_vec " << v.name << "; rexc_vec_init(&" << v.name << ")";
        } else if (is_array) {
            auto& at = *v.type->as<ArrayType>();
            std::string elem_type = emit_type_str(at.element.get());
            s << ind() << elem_type << " " << v.name << "[";
            if (at.size_expr) s << emit_expr_str(*at.size_expr, self_name);
            s << "]";
            if (v.init) {
                s << " = " << emit_init_expr(v.init.get(), at.element.get(), v.name, self_name);
            }
        } else {
            s << ind() << type_str << " " << v.name;
            if (v.init) {
                s << " = " << emit_init_expr(v.init.get(), v.type.get(), v.name, self_name);
            }
        }
    }

    std::string emit_var_inline(const VarDecl& v, const std::string& self_name) {
        std::string type_str = emit_type_str(v.type.get());

        // Resolve 'auto' to concrete C type when possible
        if (type_str == "auto" && v.init) {
            std::string resolved = resolve_auto_from_init(v.init.get());
            if (resolved != "auto") type_str = resolved;
        }

        // Handle unknown/unresolved type (auto or null→void) with InitListExpr:
        // emit as a C array with element type inferred from the first element.
        if ((type_str == "auto" || type_str == "void") && v.init &&
            v.init->kind == NodeKind::InitListExpr) {
            auto& il = *v.init->as<InitListExpr>();
            if (!il.elements.empty()) {
                const Node* first = il.elements[0].get();
                std::string elem_type = "int";
                if (first->kind == NodeKind::StringLiteralExpr) elem_type = "rexc_str";
                else if (first->kind == NodeKind::FloatLiteralExpr) elem_type = "double";
                else if (first->kind == NodeKind::CharLiteralExpr)  elem_type = "char";
                cin_var_types_[v.name] = elem_type;
                std::string result = elem_type + " " + v.name + "[]";
                if (v.init) {
                    result += " = " + emit_init_expr(v.init.get(), nullptr, v.name, self_name);
                }
                return result;
            }
        }

        // Track variable type for cin >> inference
        cin_var_types_[v.name] = type_str;

        // Check for array type
        bool is_array = (v.type && v.type->kind == NodeKind::ArrayType);
        if (is_array) {
            auto& at = *v.type->as<ArrayType>();
            std::string elem_type = emit_type_str(at.element.get());
            std::string result = elem_type + " " + v.name + "[";
            if (at.size_expr) result += emit_expr_str(*at.size_expr, "");
            result += "]";
            if (v.init) {
                result += " = " + emit_init_expr(v.init.get(), at.element.get(), v.name, self_name);
            }
            return result;
        }

        std::string result = type_str + " " + v.name;
        if (v.init) {
            result += " = " + emit_init_expr(v.init.get(), v.type.get(), v.name, self_name);
        }
        return result;
    }

    // Emit initialiser, handling std::string and vector specially
    std::string emit_init_expr(const Node* init, const Node* type_node,
                                const std::string& var_name,
                                const std::string& self_name) {
        if (!init) return "";
        bool is_string_type = type_is_std_string(type_node);
        bool is_vec_type    = type_is_std_vector(type_node);

        if (is_string_type) {
            // string var = "literal" or string var = other_string
            if (init->kind == NodeKind::StringLiteralExpr) {
                return "rexc_str_new(" + emit_string_lit(*init->as<StringLiteralExpr>()) + ")";
            }
            // string var = other_string → copy
            std::string inner = emit_expr_str(*init, self_name);
            // If inner is already rexc_str, just copy it
            return "rexc_str_copy(&(" + inner + "))";
        }

        if (is_vec_type) {
            // vector<T> v; → rexc_vec v; rexc_vec_init(&v);
            // We can't do inline init easily; handle in separate init
            return "(rexc_vec){0,0,0}";
        }

        // Generic initialiser
        if (init->kind == NodeKind::InitListExpr) {
            auto& il = *init->as<InitListExpr>();
            // InitListExpr → function-call style init
            // e.g.  Foo f{1, 2}  → emit as  (Foo){.f1=1, .f2=2} or just skip
            if (il.elements.empty()) return "{0}";
            std::string s = "{";
            for (size_t i = 0; i < il.elements.size(); i++) {
                if (i > 0) s += ", ";
                s += emit_expr_str(*il.elements[i], self_name);
            }
            return s + "}";
        }

        return emit_expr_str(*init, self_name);
    }

    // ─────────────────────────────────────────────────────────────
    //  Enum emission
    // ─────────────────────────────────────────────────────────────
    void emit_enum_decl(const EnumDecl& en, std::ostringstream& s) {
        s << "typedef enum {\n";
        for (size_t i = 0; i < en.enumerators.size(); i++) {
            auto& e = en.enumerators[i];
            s << "    " << en.name << "_" << e.name;
            if (e.value) s << " = " << emit_expr_str(*e.value);
            if (i + 1 < en.enumerators.size()) s << ",";
            s << "\n";
        }
        s << "} " << en.name << ";\n\n";
    }

    // ─────────────────────────────────────────────────────────────
    //  Type string emission
    // ─────────────────────────────────────────────────────────────
    std::string emit_type_str(const Node* n) const {
        if (!n) return "void";
        switch (n->kind) {
            case NodeKind::PrimitiveType: {
                auto& pt = *n->as<PrimitiveType>();
                switch (pt.prim_kind) {
                    case PrimitiveType::Kind::Void:       return "void";
                    case PrimitiveType::Kind::Bool:       return "int";   // C has no _Bool for us
                    case PrimitiveType::Kind::Char:       return "char";
                    case PrimitiveType::Kind::SChar:      return "signed char";
                    case PrimitiveType::Kind::UChar:      return "unsigned char";
                    case PrimitiveType::Kind::WChar:      return "wchar_t";
                    case PrimitiveType::Kind::Short:      return "short";
                    case PrimitiveType::Kind::UShort:     return "unsigned short";
                    case PrimitiveType::Kind::Int:        return "int";
                    case PrimitiveType::Kind::UInt:       return "unsigned int";
                    case PrimitiveType::Kind::Long:       return "long";
                    case PrimitiveType::Kind::ULong:      return "unsigned long";
                    case PrimitiveType::Kind::LongLong:   return "long long";
                    case PrimitiveType::Kind::ULongLong:  return "unsigned long long";
                    case PrimitiveType::Kind::Float:      return "float";
                    case PrimitiveType::Kind::Double:     return "double";
                    case PrimitiveType::Kind::LongDouble: return "long double";
                    case PrimitiveType::Kind::Auto:       return "auto";  // resolved by initialiser
                    case PrimitiveType::Kind::SizeT:      return "size_t";
                    case PrimitiveType::Kind::Nullptr_t:  return "void*";
                    default:                              return "int";
                }
            }
            case NodeKind::NamedType: {
                auto& nt = *n->as<NamedType>();
                return translate_named_type(nt.scope, nt.name);
            }
            case NodeKind::TemplateInstType: {
                auto& ti = *n->as<TemplateInstType>();
                return translate_template_type(ti);
            }
            case NodeKind::QualifiedType: {
                auto& qt = *n->as<QualifiedType>();
                std::string s;
                if (qt.quals.is_const) s += "const ";
                s += emit_type_str(qt.inner.get());
                return s;
            }
            case NodeKind::PointerType: {
                auto& pt = *n->as<PointerType>();
                std::string inner = emit_type_str(pt.pointee.get());
                if (pt.quals.is_const) return inner + "* const";
                return inner + "*";
            }
            case NodeKind::ReferenceType: {
                // const T& → T (pass by value for rexc_str/rexc_vec)
                auto& rt = *n->as<ReferenceType>();
                std::string inner = emit_type_str(rt.referee.get());
                // Strip leading "const " for comparison
                std::string base = inner;
                if (base.substr(0, 6) == "const ") base = base.substr(6);
                if (base == "rexc_str" || base == "rexc_vec")
                    return base;  // pass by value
                return inner + "*";
            }
            case NodeKind::RValueRefType: {
                auto& rr = *n->as<RValueRefType>();
                std::string inner = emit_type_str(rr.referee.get());
                std::string base = inner;
                if (base.substr(0, 6) == "const ") base = base.substr(6);
                if (base == "rexc_str" || base == "rexc_vec")
                    return base;
                return inner + "*";
            }
            case NodeKind::ArrayType: {
                auto& at = *n->as<ArrayType>();
                return emit_type_str(at.element.get());  // size handled at declarator
            }
            default:
                return "int";
        }
    }

    std::string translate_named_type(const std::vector<std::string>& scope,
                                      const std::string& name) const {
        // std::string → rexc_str
        if (!scope.empty() && scope[0] == "std") {
            if (name == "string")  return "rexc_str";
            if (name == "wstring") return "rexc_str";
            if (name == "size_t")  return "size_t";
            if (name == "ptrdiff_t") return "ptrdiff_t";
            // All other std:: types → opaque
            return "void*";
        }
        // size_t, etc.
        if (name == "size_t")    return "size_t";
        if (name == "ptrdiff_t") return "ptrdiff_t";
        if (name == "int8_t")    return "int8_t";
        if (name == "int16_t")   return "int16_t";
        if (name == "int32_t")   return "int32_t";
        if (name == "int64_t")   return "int64_t";
        if (name == "uint8_t")   return "uint8_t";
        if (name == "uint16_t")  return "uint16_t";
        if (name == "uint32_t")  return "uint32_t";
        if (name == "uint64_t")  return "uint64_t";
        // Bare string/wstring (from using namespace std) → rexc_str
        if (name == "string" || name == "wstring") return "rexc_str";
        // User-defined type → use as-is
        return name;
    }

    std::string translate_template_type(const TemplateInstType& ti) const {
        // std::vector<T> → rexc_vec (also handle bare names from using namespace std)
        bool is_std = (!ti.scope.empty() && ti.scope[0] == "std");

        if (is_std || ti.scope.empty()) {
            if (ti.name == "vector" || ti.name == "deque" ||
                ti.name == "list"   || ti.name == "array") return "rexc_vec";
            if (ti.name == "string")  return "rexc_str";
            if (ti.name == "basic_string") return "rexc_str";
            if (ti.name == "map" || ti.name == "unordered_map" ||
                ti.name == "set" || ti.name == "unordered_set") return "rexc_vec";
            if (ti.name == "pair") return "struct { void* first; void* second; }";
            if (ti.name == "optional") {
                // optional<T> → struct with bool and T
                std::string inner = ti.args.empty() ? "void*" : emit_type_str(ti.args[0].get());
                return "struct { int has_value; " + inner + " value; }";
            }
            if (ti.name == "unique_ptr" || ti.name == "shared_ptr" ||
                ti.name == "weak_ptr") {
                // smart ptr → raw pointer
                return ti.args.empty() ? "void*" : emit_type_str(ti.args[0].get()) + "*";
            }
            if (ti.name == "function") return "void*";   // simplified
            // Only treat unknown std:: templates as opaque; bare unknown
            // templates fall through to user-template mangling below.
            if (is_std) return "void*";
        }
        // User template: emit as-is for known types, otherwise mangled name
        std::string r = ti.qualified_name() + "_";
        for (auto& arg : ti.args) r += emit_type_str(arg.get()) + "_";
        return r.empty() ? "void*" : r;
    }

    // ─────────────────────────────────────────────────────────────
    //  Expression emission
    // ─────────────────────────────────────────────────────────────
    std::string emit_expr_str(const Node& n, const std::string& self_name = "") const {
        switch (n.kind) {
            case NodeKind::IntLiteralExpr:
                return n.as<IntLiteralExpr>()->raw;

            case NodeKind::FloatLiteralExpr:
                return n.as<FloatLiteralExpr>()->raw;

            case NodeKind::StringLiteralExpr:
                // std::string contexts handled by caller; here emit rexc_str
                return "rexc_str_from_lit(" + emit_string_lit(*n.as<StringLiteralExpr>()) + ")";

            case NodeKind::CharLiteralExpr:
                return n.as<CharLiteralExpr>()->raw;

            case NodeKind::BoolLiteralExpr:
                return n.as<BoolLiteralExpr>()->value ? "1" : "0";

            case NodeKind::NullptrExpr:
                return "NULL";

            case NodeKind::ThisExpr:
                return self_name.empty() ? "self" : self_name;

            case NodeKind::IdentifierExpr: {
                auto& id = *n.as<IdentifierExpr>();
                // Map standard library identifiers (from using namespace std)
                if (id.name == "cout") return "rexc_stdout_stream";
                if (id.name == "cin")  return "rexc_stdin_stream";
                if (id.name == "cerr") return "rexc_stderr_stream";
                if (id.name == "endl") return "\"\\n\"";
                // If we're in a method body, check if identifier is a field or method
                if (!self_name.empty() && !current_emit_class_.empty()) {
                    auto it = class_layouts_.find(current_emit_class_);
                    if (it != class_layouts_.end()) {
                        // Check fields
                        for (auto& [ft, fn] : it->second.fields) {
                            if (fn == id.name)
                                return self_name + "->" + id.name;
                        }
                        // Check virtual methods
                        for (auto& vm : it->second.virtual_methods) {
                            if (vm == id.name)
                                return self_name + "->" + id.name; // will be a method call
                        }
                    }
                    // Also look in semantic context
                    auto cit = ctx_.classes.find(current_emit_class_);
                    if (cit != ctx_.classes.end()) {
                        // Check field names (vector)
                        auto& flds = cit->second.fields;
                        bool is_field = std::find(flds.begin(), flds.end(), id.name) != flds.end();
                        bool is_method = cit->second.methods.count(id.name) > 0;
                        if (is_field || is_method)
                            return self_name + "->" + id.name;
                    }
                }
                return id.name;
            }

            case NodeKind::ParenExpr:
                return "(" + emit_expr_str(*n.as<ParenExpr>()->inner, self_name) + ")";

            case NodeKind::BinaryExpr:
                return emit_binary_expr(*n.as<BinaryExpr>(), self_name);

            case NodeKind::AssignExpr: {
                auto& e = *n.as<AssignExpr>();
                std::string target = emit_expr_str(*e.target, self_name);
                std::string value  = emit_expr_str(*e.value,  self_name);
                std::string op;
                switch (e.op) {
                    case TokenKind::Assign:        op = "=";   break;
                    case TokenKind::PlusAssign:    op = "+=";  break;
                    case TokenKind::MinusAssign:   op = "-=";  break;
                    case TokenKind::StarAssign:    op = "*=";  break;
                    case TokenKind::SlashAssign:   op = "/=";  break;
                    case TokenKind::PercentAssign: op = "%=";  break;
                    case TokenKind::AmpAssign:     op = "&=";  break;
                    case TokenKind::PipeAssign:    op = "|=";  break;
                    case TokenKind::CaretAssign:   op = "^=";  break;
                    case TokenKind::LShiftAssign:  op = "<<="; break;
                    case TokenKind::RShiftAssign:  op = ">>="; break;
                    default:                       op = "=";   break;
                }
                return target + " " + op + " " + value;
            }

            case NodeKind::UnaryExpr: {
                auto& e = *n.as<UnaryExpr>();
                std::string op_str;
                switch (e.op) {
                    case TokenKind::Plus:       op_str = "+";  break;
                    case TokenKind::Minus:      op_str = "-";  break;
                    case TokenKind::Bang:       op_str = "!";  break;
                    case TokenKind::Tilde:      op_str = "~";  break;
                    case TokenKind::Star:       op_str = "*";  break;
                    case TokenKind::Amp:        op_str = "&";  break;
                    case TokenKind::PlusPlus:   op_str = "++"; break;
                    case TokenKind::MinusMinus: op_str = "--"; break;
                    default:                    op_str = "?";  break;
                }
                std::string operand = emit_expr_str(*e.operand, self_name);
                if (e.is_postfix) return operand + op_str;
                return op_str + operand;
            }

            case NodeKind::TernaryExpr: {
                auto& e = *n.as<TernaryExpr>();
                return "(" + emit_expr_str(*e.condition, self_name) + " ? "
                           + emit_expr_str(*e.then_expr,  self_name) + " : "
                           + emit_expr_str(*e.else_expr,  self_name) + ")";
            }

            case NodeKind::CallExpr:
                return emit_call_expr(*n.as<CallExpr>(), self_name);

            case NodeKind::MemberExpr:
                return emit_member_expr(*n.as<MemberExpr>(), self_name);

            case NodeKind::ScopeExpr:
                return emit_scope_expr(*n.as<ScopeExpr>(), self_name);

            case NodeKind::IndexExpr: {
                auto& e = *n.as<IndexExpr>();
                return emit_expr_str(*e.object, self_name) + "["
                     + emit_expr_str(*e.index,  self_name) + "]";
            }

            case NodeKind::CastExpr: {
                auto& e = *n.as<CastExpr>();
                std::string ty = emit_type_str(e.type.get());
                std::string ex = emit_expr_str(*e.expr, self_name);
                return "((" + ty + ")(" + ex + "))";
            }

            case NodeKind::SizeofExpr: {
                auto& e = *n.as<SizeofExpr>();
                if (e.type) return "sizeof(" + emit_type_str(e.type.get()) + ")";
                if (e.expr) return "sizeof(" + emit_expr_str(*e.expr, self_name) + ")";
                return "sizeof(int)";
            }

            case NodeKind::NewExpr:
                return emit_new_expr(*n.as<NewExpr>(), self_name);

            case NodeKind::DeleteExpr: {
                auto& e = *n.as<DeleteExpr>();
                std::string p = emit_expr_str(*e.expr, self_name);
                return "free(" + p + ")";
            }

            case NodeKind::InitListExpr: {
                auto& il = *n.as<InitListExpr>();
                std::string s = "{";
                for (size_t i = 0; i < il.elements.size(); i++) {
                    if (i > 0) s += ", ";
                    s += emit_expr_str(*il.elements[i], self_name);
                }
                return s + "}";
            }

            case NodeKind::LambdaExpr:
                // Simplified: emit a void* cast to NULL (lambdas need major work)
                return "NULL /* lambda */";

            case NodeKind::CommaExpr: {
                auto& e = *n.as<CommaExpr>();
                return "(" + emit_expr_str(*e.left,  self_name) + ", "
                           + emit_expr_str(*e.right, self_name) + ")";
            }

            default:
                return "/* unknown expr */0";
        }
    }

    // ── Binary expression ────────────────────────────────────────
    std::string emit_binary_expr(const BinaryExpr& e, const std::string& self_name) const {
        // Special: std::cout << chain
        if (e.op == TokenKind::LShift) {
            if (is_cout_node(*e.left) || is_cout_chain(*e.left)) {
                return emit_cout_chain(e, self_name);
            }
        }

        // Special: std::cin >> chain
        if (e.op == TokenKind::RShift) {
            if (is_cin_node(*e.left) || is_cin_chain(*e.left)) {
                return emit_cin_chain(e, self_name);
            }
        }

        std::string lhs = emit_expr_str(*e.left,  self_name);
        std::string rhs = emit_expr_str(*e.right, self_name);

        std::string op;
        switch (e.op) {
            case TokenKind::Plus:      op = "+";   break;
            case TokenKind::Minus:     op = "-";   break;
            case TokenKind::Star:      op = "*";   break;
            case TokenKind::Slash:     op = "/";   break;
            case TokenKind::Percent:   op = "%";   break;
            case TokenKind::Amp:       op = "&";   break;
            case TokenKind::Pipe:      op = "|";   break;
            case TokenKind::Caret:     op = "^";   break;
            case TokenKind::LShift:    op = "<<";  break;
            case TokenKind::RShift:    op = ">>";  break;
            case TokenKind::Eq:        op = "==";  break;
            case TokenKind::NotEq:     op = "!=";  break;
            case TokenKind::Lt:        op = "<";   break;
            case TokenKind::Gt:        op = ">";   break;
            case TokenKind::LtEq:      op = "<=";  break;
            case TokenKind::GtEq:      op = ">=";  break;
            case TokenKind::AmpAmp:    op = "&&";  break;
            case TokenKind::PipePipe:  op = "||";  break;
            case TokenKind::KwAnd:     op = "&&";  break;
            case TokenKind::KwOr:      op = "||";  break;
            case TokenKind::DotStar:   op = ".";   break;
            case TokenKind::ArrowStar: op = "->";  break;
            default:
                op = "/* ? */";
        }
        return lhs + " " + op + " " + rhs;
    }

    // ── std::cout << … emission ──────────────────────────────────
    bool is_cout_node(const Node& n) const {
        if (n.kind == NodeKind::ScopeExpr) {
            auto& se = *n.as<ScopeExpr>();
            return se.name == "cout" || se.name == "cerr";
        }
        if (n.kind == NodeKind::IdentifierExpr) {
            auto& ie = *n.as<IdentifierExpr>();
            return ie.name == "cout" || ie.name == "cerr";
        }
        return false;
    }

    bool is_cout_chain(const Node& n) const {
        if (n.kind != NodeKind::BinaryExpr) return false;
        auto& e = *n.as<BinaryExpr>();
        if (e.op != TokenKind::LShift) return false;
        return is_cout_node(*e.left) || is_cout_chain(*e.left);
    }

    // Collect all cout arguments from a << chain
    void collect_cout_args(const Node& n,
                           std::string& stream_var,
                           std::vector<const Node*>& args) const {
        if (n.kind == NodeKind::ScopeExpr) {
            auto& se = *n.as<ScopeExpr>();
            stream_var = (se.name == "cerr") ? "&rexc_stderr_stream"
                                             : "&rexc_stdout_stream";
            return;
        }
        if (n.kind == NodeKind::IdentifierExpr) {
            auto& ie = *n.as<IdentifierExpr>();
            if (ie.name == "cerr")
                stream_var = "&rexc_stderr_stream";
            else if (ie.name == "cout")
                stream_var = "&rexc_stdout_stream";
            else
                stream_var = "&" + ie.name;
            return;
        }
        if (n.kind == NodeKind::BinaryExpr) {
            auto& be = *n.as<BinaryExpr>();
            if (be.op == TokenKind::LShift) {
                collect_cout_args(*be.left, stream_var, args);
                args.push_back(be.right.get());
                return;
            }
        }
        stream_var = "&rexc_stdout_stream";
    }

    std::string emit_cout_chain(const BinaryExpr& chain, const std::string& self_name) const {
        std::string stream_var;
        std::vector<const Node*> args;
        // collect_cout_args already collects all args including chain.right
        collect_cout_args(chain, stream_var, args);

        if (args.empty()) return stream_var;

        std::string result = stream_var;
        for (const Node* arg : args) {
            if (!arg) continue;
            result = emit_cout_arg(result, *arg, self_name);
        }
        return result;
    }

    std::string emit_cout_arg(const std::string& stream_expr,
                               const Node& arg, const std::string& self_name) const {
        // std::endl
        if (arg.kind == NodeKind::ScopeExpr) {
            auto& se = *arg.as<ScopeExpr>();
            if (se.name == "endl") return "rexc_cout_endl(" + stream_expr + ")";
            if (se.name == "flush") return "rexc_cout_flush(" + stream_expr + ")";
            // ANSI terminal color/style manipulator (e.g. fg::red → fg_red)
            std::string flat;
            for (auto& s : se.scope) flat += s + "_";
            flat += se.name;
            if (detail::is_ansi_identifier(flat))
                return "rexc_cout_cstr(" + stream_expr + ", " + flat + ")";
        }
        if (arg.kind == NodeKind::IdentifierExpr) {
            auto& ie = *arg.as<IdentifierExpr>();
            if (ie.name == "endl") return "rexc_cout_endl(" + stream_expr + ")";
            if (ie.name == "flush") return "rexc_cout_flush(" + stream_expr + ")";
            // ANSI terminal color/style manipulator (e.g. bold, reset)
            if (detail::is_ansi_identifier(ie.name))
                return "rexc_cout_cstr(" + stream_expr + ", " + ie.name + ")";
        }

        // ANSI helper function calls returning const char* (e.g. fg_rgb, bg_rgb)
        if (arg.kind == NodeKind::CallExpr) {
            auto& ce = *arg.as<CallExpr>();
            if (ce.callee) {
                std::string cn = emit_callee_name(*ce.callee, self_name);
                if (detail::is_ansi_cstr_function(cn)) {
                    std::string val = emit_expr_str(arg, self_name);
                    return "rexc_cout_cstr(" + stream_expr + ", " + val + ")";
                }
                // roll_parse() returns int, not rexc_str – emit as integer
                if (cn == "roll_parse") {
                    std::string val = emit_expr_str(arg, self_name);
                    return "rexc_cout_int(" + stream_expr + ", (int64_t)(" + val + "))";
                }
            }
        }

        // Array subscript on a known rexc_str[] array → rexc_cout_str (lvalue)
        if (arg.kind == NodeKind::IndexExpr) {
            auto& ie = *arg.as<IndexExpr>();
            if (ie.object && ie.object->kind == NodeKind::IdentifierExpr) {
                const std::string& arr_name = ie.object->as<IdentifierExpr>()->name;
                auto vit = cin_var_types_.find(arr_name);
                if (vit != cin_var_types_.end() && vit->second == "rexc_str") {
                    std::string val = emit_expr_str(arg, self_name);
                    return "rexc_cout_str(" + stream_expr + ", &(" + val + "))";
                }
            }
        }

        // String literal → rexc_cout_cstr
        if (arg.kind == NodeKind::StringLiteralExpr) {
            std::string lit = emit_string_lit(*arg.as<StringLiteralExpr>());
            return "rexc_cout_cstr(" + stream_expr + ", " + lit + ")";
        }

        // Bool literal
        if (arg.kind == NodeKind::BoolLiteralExpr) {
            return "rexc_cout_bool(" + stream_expr + ", "
                   + (arg.as<BoolLiteralExpr>()->value ? "1" : "0") + ")";
        }

        // Char literal
        if (arg.kind == NodeKind::CharLiteralExpr) {
            return "rexc_cout_char(" + stream_expr + ", "
                   + arg.as<CharLiteralExpr>()->raw + ")";
        }

        // General: emit a generic print.  We use rexc_cout_int for scalars,
        // rexc_cout_str for rexc_str.  Without full type info we default to
        // rexc_cout_int which works for int/pointer casts.
        std::string val = emit_expr_str(arg, self_name);

        // Heuristic: if it looks like a rexc_str value, use str variant
        // - contains "rexc_str" in val (from_lit, function returns, etc.)
        // - accesses .name or ->name (field that is rexc_str)
        // - vtable call ->__vt->method (likely returns rexc_str if it's a speak-like call)
        // - contains "to_string" (e.g. roll_result_to_string → rexc_str)
        bool is_str = val.find("rexc_str")  != std::string::npos ||
                      val.find("->name")    != std::string::npos ||
                      val.find(".name")     != std::string::npos ||
                      val.find("to_string") != std::string::npos;

        // Check known variable types from declarations
        std::string known_type;
        if (arg.kind == NodeKind::IdentifierExpr) {
            auto vit = cin_var_types_.find(arg.as<IdentifierExpr>()->name);
            if (vit != cin_var_types_.end()) {
                known_type = vit->second;
                if (known_type == "rexc_str") is_str = true;
            }
        }

        // Check if this is a call to a known rexc_str-returning function
        if (!is_str && arg.kind == NodeKind::CallExpr) {
            auto& ce = *arg.as<CallExpr>();
            if (ce.callee) {
                std::string callee = emit_expr_str(*ce.callee, self_name);
                // Method call that returns rexc_str based on known functions
                if (callee.find("speak") != std::string::npos ||
                    callee.find("_str")  != std::string::npos ||
                    callee.find("name")  != std::string::npos) {
                    is_str = true;
                }
                // Check vtable calls - all virtual methods returning rexc_str
                if (callee.find("->__vt->") != std::string::npos) {
                    // Extract method name from vtable call
                    auto pos = callee.rfind("->");
                    if (pos != std::string::npos) {
                        std::string mname = callee.substr(pos + 2);
                        // Check if any class has this virtual method returning rexc_str
                        for (auto& [cn, ci] : ctx_.classes) {
                            (void)cn;
                            if (std::find(ci.virtual_methods.begin(),
                                          ci.virtual_methods.end(), mname)
                                != ci.virtual_methods.end()) {
                                is_str = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (is_str) {
            // Use _v variant for function call results and ternary expressions
            // (rvalues) to avoid &(rvalue) error.
            // Walk through any ParenExpr wrappers to find the underlying node kind.
            const Node* inner_arg = &arg;
            while (inner_arg->kind == NodeKind::ParenExpr)
                inner_arg = inner_arg->as<ParenExpr>()->inner.get();
            bool is_rvalue = (inner_arg->kind == NodeKind::CallExpr) ||
                             (inner_arg->kind == NodeKind::TernaryExpr);
            if (is_rvalue)
                return "rexc_cout_str_v(" + stream_expr + ", " + val + ")";
            return "rexc_cout_str(" + stream_expr + ", &(" + val + "))";
        }

        // Use type-specific cout for known types
        if (known_type == "float")
            return "rexc_cout_float(" + stream_expr + ", " + val + ")";
        if (known_type == "double")
            return "rexc_cout_double(" + stream_expr + ", " + val + ")";
        if (known_type == "char")
            return "rexc_cout_char(" + stream_expr + ", " + val + ")";
        if (known_type == "unsigned int" || known_type == "unsigned long" ||
            known_type == "unsigned long long" || known_type == "size_t")
            return "rexc_cout_uint(" + stream_expr + ", (uint64_t)(" + val + "))";

        // Default: int
        return "rexc_cout_int(" + stream_expr + ", (int64_t)(" + val + "))";
    }

    // ── std::cin >> … emission ───────────────────────────────────
    bool is_cin_node(const Node& n) const {
        if (n.kind == NodeKind::ScopeExpr) {
            auto& se = *n.as<ScopeExpr>();
            return se.name == "cin";
        }
        if (n.kind == NodeKind::IdentifierExpr) {
            return n.as<IdentifierExpr>()->name == "cin";
        }
        return false;
    }

    bool is_cin_chain(const Node& n) const {
        if (n.kind != NodeKind::BinaryExpr) return false;
        auto& e = *n.as<BinaryExpr>();
        if (e.op != TokenKind::RShift) return false;
        return is_cin_node(*e.left) || is_cin_chain(*e.left);
    }

    void collect_cin_args(const Node& n,
                          std::string& stream_var,
                          std::vector<const Node*>& args) const {
        if (n.kind == NodeKind::ScopeExpr || n.kind == NodeKind::IdentifierExpr) {
            stream_var = "&rexc_stdin_stream";
            return;
        }
        if (n.kind == NodeKind::BinaryExpr) {
            auto& be = *n.as<BinaryExpr>();
            if (be.op == TokenKind::RShift) {
                collect_cin_args(*be.left, stream_var, args);
                args.push_back(be.right.get());
                return;
            }
        }
        stream_var = "&rexc_stdin_stream";
    }

    std::string emit_cin_chain(const BinaryExpr& chain, const std::string& self_name) const {
        std::string stream_var;
        std::vector<const Node*> args;
        collect_cin_args(chain, stream_var, args);

        if (args.empty()) return stream_var;

        std::string result = stream_var;
        for (const Node* arg : args) {
            if (!arg) continue;
            result = emit_cin_arg(result, *arg, self_name);
        }
        return result;
    }

    std::string emit_cin_arg(const std::string& stream_expr,
                             const Node& arg, const std::string& self_name) const {
        std::string val = emit_expr_str(arg, self_name);

        // Determine the type of the variable being read into
        // Check if this is a known variable with type info from declarations
        std::string var_type = infer_var_type(arg);

        if (var_type == "rexc_str" || var_type == "string" || var_type == "std::string")
            return "rexc_cin_str(" + stream_expr + ", &(" + val + "))";
        if (var_type == "float")
            return "rexc_cin_float(" + stream_expr + ", &(" + val + "))";
        if (var_type == "double")
            return "rexc_cin_double(" + stream_expr + ", &(" + val + "))";
        if (var_type == "char")
            return "rexc_cin_char(" + stream_expr + ", &(" + val + "))";

        // Default: int
        return "rexc_cin_int(" + stream_expr + ", &(" + val + "))";
    }

    // Attempt to infer the C type of a variable from semantic context
    std::string infer_var_type(const Node& n) const {
        if (n.kind == NodeKind::IdentifierExpr) {
            const std::string& name = n.as<IdentifierExpr>()->name;
            auto it = cin_var_types_.find(name);
            if (it != cin_var_types_.end()) return it->second;
        }
        return "";
    }

    // ── Call expression ──────────────────────────────────────────
    std::string emit_call_expr(const CallExpr& call, const std::string& self_name) const {
        if (!call.callee) return "/* null call */";

        // Detect push_back on a vector
        if (call.callee->kind == NodeKind::MemberExpr) {
            auto& me = *call.callee->as<MemberExpr>();
            if (me.member == "push_back" || me.member == "emplace_back") {
                std::string obj = emit_expr_str(*me.object, self_name);
                std::string arg = call.args.empty() ? "NULL"
                                : emit_expr_str(*call.args[0], self_name);
                return "rexc_vec_push_back(&(" + obj + "), (void*)(" + arg + "))";
            }
            if (me.member == "size" || me.member == "length") {
                std::string obj = emit_expr_str(*me.object, self_name);
                // If it's a string: return .len, else .size
                return obj + ".size";
            }
            if (me.member == "empty") {
                std::string obj = emit_expr_str(*me.object, self_name);
                return "(" + obj + ".size == 0)";
            }
            if (me.member == "c_str" || me.member == "data") {
                std::string obj = emit_expr_str(*me.object, self_name);
                return "rexc_cstr(&(" + obj + "))";
            }
            if (me.member == "reserve") {
                std::string obj = emit_expr_str(*me.object, self_name);
                std::string arg = call.args.empty() ? "0"
                                : emit_expr_str(*call.args[0], self_name);
                return "rexc_vec_reserve(&(" + obj + "), " + arg + ")";
            }

            // roll_result method calls: r.to_string(), r.max(), r.min()
            if (me.member == "to_string" || me.member == "max" || me.member == "min") {
                if (me.object && me.object->kind == NodeKind::IdentifierExpr) {
                    std::string var_name = me.object->as<IdentifierExpr>()->name;
                    auto type_it = cin_var_types_.find(var_name);
                    if (type_it != cin_var_types_.end() && type_it->second == "roll_result") {
                        std::string obj = emit_expr_str(*me.object, self_name);
                        return "roll_result_" + me.member + "(&(" + obj + "))";
                    }
                }
            }
            // Virtual method call through pointer: a->print()
            if (me.is_arrow) {
                // Could be virtual dispatch
                std::string obj = emit_expr_str(*me.object, self_name);
                return emit_possibly_virtual_call(obj, me.member, call.args, self_name, true);
            }
            // Regular member call: obj.method(args)
            {
                std::string obj = emit_expr_str(*me.object, self_name);
                std::string meth_obj = me.is_arrow ? obj : obj;
                // We translate to ClassName_method(obj, args)
                // We don't always know the class name here, so try to look it up
                return emit_member_call(meth_obj, me.member, call.args, me.is_arrow, self_name);
            }
        }

        // Direct function call
        // Handle implicit this->method() when in a class context
        if (!current_emit_class_.empty() && !self_name.empty() &&
            call.callee->kind == NodeKind::IdentifierExpr) {
            const std::string& fn = call.callee->as<IdentifierExpr>()->name;
            auto it = ctx_.classes.find(current_emit_class_);
            if (it != ctx_.classes.end() && it->second.methods.count(fn)) {
                // Implicit this→method call
                std::string args_str2;
                for (size_t i = 0; i < call.args.size(); i++) {
                    if (i > 0) args_str2 += ", ";
                    args_str2 += emit_expr_str(*call.args[i], self_name);
                }
                auto lay_it2 = class_layouts_.find(current_emit_class_);
                if (lay_it2 == class_layouts_.end()) goto fallback_call;
                {
                auto& lay = lay_it2->second;
                bool is_virt = std::find(lay.virtual_methods.begin(),
                                          lay.virtual_methods.end(), fn)
                                != lay.virtual_methods.end();
                if (is_virt) {
                    if (args_str2.empty())
                        return self_name + "->__vt->" + fn + "(" + self_name + ")";
                    return self_name + "->__vt->" + fn + "(" + self_name + ", " + args_str2 + ")";
                }
                std::string qfn = current_emit_class_ + "_" + fn;
                if (args_str2.empty()) return qfn + "(" + self_name + ")";
                return qfn + "(" + self_name + ", " + args_str2 + ")";
                }
            }
        }
        fallback_call:

        std::string callee_str = emit_callee_name(*call.callee, self_name);

        // Handle std::getline(cin, str) → rexc_getline(&rexc_stdin_stream, &str)
        if (callee_str == "getline" || callee_str == "std_getline") {
            if (call.args.size() >= 2) {
                std::string stream_arg = emit_expr_str(*call.args[0], self_name);
                std::string str_arg    = emit_expr_str(*call.args[1], self_name);
                // Convert cin/stream identifier to pointer form
                if (stream_arg == "rexc_stdin_stream")
                    stream_arg = "&rexc_stdin_stream";
                return "rexc_getline(" + stream_arg + ", &(" + str_arg + "))";
            }
        }

        // Handle system() with rexc_str argument → extract .data for const char*
        if (callee_str == "system" && call.args.size() == 1) {
            std::string arg = emit_expr_str(*call.args[0], self_name);
            // If argument produces an rexc_str (e.g. rexc_str_from_lit), extract C-string
            if (arg.find("rexc_str") != std::string::npos) {
                return "system((" + arg + ").data)";
            }
            // Check if it's a known rexc_str variable
            if (call.args[0]->kind == NodeKind::IdentifierExpr) {
                auto& id_name = call.args[0]->as<IdentifierExpr>()->name;
                auto vit = cin_var_types_.find(id_name);
                if (vit != cin_var_types_.end() && vit->second == "rexc_str") {
                    return "system((" + arg + ").data)";
                }
            }
        }

        // Handle roll_parse() with rexc_str argument → extract .data for const char*
        if (callee_str == "roll_parse" && call.args.size() == 1) {
            std::string arg = emit_expr_str(*call.args[0], self_name);
            if (arg.find("rexc_str") != std::string::npos) {
                return "roll_parse((" + arg + ").data)";
            }
            if (call.args[0]->kind == NodeKind::IdentifierExpr) {
                auto& id_name = call.args[0]->as<IdentifierExpr>()->name;
                auto vit = cin_var_types_.find(id_name);
                if (vit != cin_var_types_.end() && vit->second == "rexc_str") {
                    return "roll_parse((" + arg + ").data)";
                }
            }
        }

        // Handle colored()/styled() with variable number of color arguments.
        // colored(text, c1)            → colored(...)    (2 args, existing)
        // colored(text, c1, c2)        → colored2(...)   (3 args)
        // colored(text, c1, c2, c3)    → colored3(...)   (4 args, max supported)
        // styled(text, c1, c2)         → styled(...)     (3 args, existing)
        // styled(text, c1, c2, c3)     → styled2(...)    (4 args)
        // styled(text, c1, c2, c3, c4) → styled3(...)    (5 args, max supported)
        if (callee_str == "colored" && call.args.size() > 2 && call.args.size() <= 4) {
            callee_str = "colored" + std::to_string(call.args.size() - 1);
        }
        if (callee_str == "styled" && call.args.size() > 3 && call.args.size() <= 5) {
            callee_str = "styled" + std::to_string(call.args.size() - 2);
        }

        // Handle roll:: die overloads and helpers:
        //   roll_dX()          → roll_dX()           (0 args: single die roll)
        //   roll_dX(n)         → roll_dX_n(n)        (1 arg: roll n dice of type X)
        //   roll_roll(f,n)     → roll_roll_full(f,n)  (full roll with per-die breakdown)
        //   roll_pick(vec)     → roll_pick_ptr(&vec)  (random element from vector)
        //   roll_shuffle(vec)  → roll_shuffle_vec(&vec) (in-place Fisher-Yates shuffle)
        if (call.args.size() == 1) {
            static const char* k_die_funcs[] = {
                "roll_d4","roll_d6","roll_d8","roll_d10",
                "roll_d12","roll_d20","roll_d100", nullptr
            };
            for (const char** p = k_die_funcs; *p; ++p) {
                if (callee_str == *p) {
                    return callee_str + "_n(" + emit_expr_str(*call.args[0], self_name) + ")";
                }
            }
        }
        if (callee_str == "roll_roll") {
            std::string as;
            for (size_t i = 0; i < call.args.size(); i++) {
                if (i) as += ", ";
                as += emit_expr_str(*call.args[i], self_name);
            }
            return "roll_roll_full(" + as + ")";
        }
        if (callee_str == "roll_pick" && !call.args.empty()) {
            return "roll_pick_ptr(&(" + emit_expr_str(*call.args[0], self_name) + "))";
        }
        if (callee_str == "roll_shuffle" && !call.args.empty()) {
            return "roll_shuffle_vec(&(" + emit_expr_str(*call.args[0], self_name) + "))";
        }

        std::string args_str;
        for (size_t i = 0; i < call.args.size(); i++) {
            if (i > 0) args_str += ", ";
            args_str += emit_expr_str(*call.args[i], self_name);
        }
        return callee_str + "(" + args_str + ")";
    }

    std::string emit_callee_name(const Node& callee, const std::string& self_name) const {
        if (callee.kind == NodeKind::IdentifierExpr) {
            return callee.as<IdentifierExpr>()->name;
        }
        if (callee.kind == NodeKind::ScopeExpr) {
            auto& se = *callee.as<ScopeExpr>();
            // std::cout already handled as stream
            if (!se.scope.empty() && se.scope[0] == "std") {
                if (se.name == "to_string") return "rexc_str_from_int"; // rough
                if (se.name == "move")      return ""; // ignore
                if (se.name == "forward")   return "";
                if (se.name == "getline")   return "getline"; // handled in emit_call_expr
            }
            // Qualified function: A::foo → A_foo
            std::string r;
            for (auto& s : se.scope) r += s + "_";
            return r + se.name;
        }
        return emit_expr_str(callee, self_name);
    }

    // Try to figure out if a method is virtual and emit accordingly
    std::string emit_possibly_virtual_call(const std::string& obj_expr,
                                            const std::string& method,
                                            const std::vector<NodePtr>& args,
                                            const std::string& self_name,
                                            bool is_arrow) const {
        // Check if any class has this as a virtual method
        for (auto& [cls_name, lay] : class_layouts_) {
            if (std::find(lay.virtual_methods.begin(), lay.virtual_methods.end(), method)
                != lay.virtual_methods.end()) {
                // Emit: obj->__vt->method(obj, args)
                std::string a;
                for (auto& arg : args) { a += ", "; a += emit_expr_str(*arg, self_name); }
                return obj_expr + "->__vt->" + method + "(" + obj_expr + a + ")";
            }
        }
        // Not virtual – emit as regular member call
        return emit_member_call(obj_expr, method, args, is_arrow, self_name);
    }

    std::string emit_member_call(const std::string& obj_expr,
                                  const std::string& method,
                                  const std::vector<NodePtr>& args,
                                  bool is_arrow,
                                  const std::string& self_name) const {
        std::string args_str;
        for (auto& arg : args) {
            if (!args_str.empty()) args_str += ", ";
            args_str += emit_expr_str(*arg, self_name);
        }

        // Find the declaring class (base-most class that defines the method directly)
        // Use class_order_ (topological order: base classes come before derived)
        for (auto& cn : class_order_) {
            auto lay_it = class_layouts_.find(cn);
            if (lay_it == class_layouts_.end()) continue;
            auto& lay = lay_it->second;
            auto it = ctx_.classes.find(cn);
            if (it == ctx_.classes.end()) continue;
            if (!it->second.methods.count(method)) continue;

            std::string cast_obj = "((" + cn + "*)(" + obj_expr + "))";
            bool is_virt = std::find(lay.virtual_methods.begin(),
                                      lay.virtual_methods.end(), method)
                            != lay.virtual_methods.end();
            if (is_virt) {
                // For virtual: cast to base type and use vtable dispatch
                if (args_str.empty())
                    return cast_obj + "->__vt->" + method + "(" + cast_obj + ")";
                return cast_obj + "->__vt->" + method + "(" + cast_obj + ", " + args_str + ")";
            }
            // Non-virtual: call the C function directly
            std::string fn_name = cn + "_" + method;
            if (args_str.empty())
                return fn_name + "(" + cast_obj + ")";
            return fn_name + "(" + cast_obj + ", " + args_str + ")";
        }

        // Fallback for unknown types
        std::string deref = is_arrow ? obj_expr : "&(" + obj_expr + ")";
        if (args_str.empty()) return method + "(" + deref + ")";
        return method + "(" + deref + ", " + args_str + ")";
    }

    // ── Member expression (field access) ─────────────────────────
    std::string emit_member_expr(const MemberExpr& me, const std::string& self_name) const {
        std::string obj = emit_expr_str(*me.object, self_name);
        std::string op  = me.is_arrow ? "->" : ".";
        return obj + op + me.member;
    }

    // ── Scope expression ─────────────────────────────────────────
    std::string emit_scope_expr(const ScopeExpr& se, const std::string& self_name) const {
        // std::endl → "\n" (in cout context, already handled above)
        if (!se.scope.empty() && se.scope[0] == "std") {
            if (se.name == "endl")   return "\"\\n\"";
            if (se.name == "string") return "rexc_str";
            if (se.name == "vector") return "rexc_vec";
            if (se.name == "cout")   return "rexc_stdout_stream";
            if (se.name == "cerr")   return "rexc_stderr_stream";
            if (se.name == "cin")    return "rexc_stdin_stream";
        }
        // Enum value:  Color::Red → Color_Red
        std::string result;
        for (auto& s : se.scope) result += s + "_";
        return result + se.name;
    }

    // ── new expression ───────────────────────────────────────────
    std::string emit_new_expr(const NewExpr& ne, const std::string& self_name) const {
        if (!ne.type) return "malloc(sizeof(void*))";

        std::string ty = emit_type_str(ne.type.get());

        if (ne.is_array) {
            std::string sz = ne.array_size ? emit_expr_str(*ne.array_size, self_name) : "0";
            return "calloc((" + sz + "), sizeof(" + ty + "))";
        }

        // Find if there's a constructor to call
        std::string ctor_call;
        for (auto& [cn, lay] : class_layouts_) {
            if (cn == ty || lay.c_struct_name == ty) {
                // Has a constructor?
                ctor_call = cn + "_ctor";
                break;
            }
        }

        std::string alloc = "(" + ty + "*)calloc(1, sizeof(" + ty + "))";

        if (!ctor_call.empty()) {
            std::string args_str;
            for (auto& arg : ne.args) {
                args_str += ", ";
                args_str += emit_expr_str(*arg, self_name);
            }
            // If class has a base, return pointer to the base subobject
            // so it can be stored in a vector<BaseClass*>
            std::string base_access;
            auto lay_it = class_layouts_.find(ty);
            if (lay_it != class_layouts_.end() && !lay_it->second.bases.empty()) {
                std::string base = lay_it->second.bases[0];
                base_access = "&__np->__base_" + base;
            } else {
                base_access = "__np";
            }
            return "( __extension__ ({ " + ty + "* __np = " + alloc + "; "
                   + ctor_call + "(__np" + args_str + "); " + base_access + "; }) )";
        }

        return alloc;
    }

    // ── String literal ───────────────────────────────────────────
    std::string emit_string_lit(const StringLiteralExpr& s) const {
        // Strip prefix (L/u/u8/U) and return the raw quoted literal
        const std::string& raw = s.value;
        size_t quote = raw.find('"');
        if (quote == std::string::npos) return "\"\"";
        return raw.substr(quote); // includes closing quote
    }

    // ─────────────────────────────────────────────────────────────
    //  Type predicates
    // ─────────────────────────────────────────────────────────────
    bool type_is_std_string(const Node* n) const {
        if (!n) return false;
        if (n->kind == NodeKind::NamedType) {
            auto& nt = *n->as<NamedType>();
            if (nt.name == "string") return true;
            if (!nt.scope.empty() && nt.scope.back() == "std" && nt.name == "string") return true;
        }
        if (n->kind == NodeKind::TemplateInstType) {
            auto& ti = *n->as<TemplateInstType>();
            if (ti.name == "string" || ti.name == "basic_string") return true;
        }
        if (n->kind == NodeKind::QualifiedType) {
            return type_is_std_string(n->as<QualifiedType>()->inner.get());
        }
        if (n->kind == NodeKind::ReferenceType) {
            return type_is_std_string(n->as<ReferenceType>()->referee.get());
        }
        return false;
    }

    bool type_is_std_vector(const Node* n) const {
        if (!n) return false;
        if (n->kind == NodeKind::TemplateInstType) {
            auto& ti = *n->as<TemplateInstType>();
            return ti.name == "vector" || ti.name == "list" || ti.name == "deque";
        }
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────
//  Convenience entry point
// ─────────────────────────────────────────────────────────────────
inline std::string generate_c_source(const TranslationUnit& tu,
                                      const SemanticContext& ctx,
                                      const std::string& runtime_path = "rexc_runtime.h") {
    CodeGenerator gen(ctx, runtime_path);
    return gen.generate_source(tu);
}

} // namespace rexc
