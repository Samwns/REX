/*
 * interpreter.cpp  –  REX Tree-Walking Interpreter Implementation
 *
 * Walks the AST produced by rexc's Lexer/Parser/Semantic and
 * evaluates each node recursively — no compilation involved.
 */

#include "interpreter.hpp"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <fstream>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rex::interp {

using namespace rexc;

// ─── Constructor ─────────────────────────────────────────────────

Interpreter::Interpreter() {
    global_env_ = std::make_shared<Environment>();
    register_builtins(global_env_);
}

// ─── Helpers ─────────────────────────────────────────────────────

std::string Interpreter::extract_type_name(const rexc::Node* type_node) {
    if (!type_node) return "";
    switch (type_node->kind) {
        case NodeKind::PrimitiveType: {
            auto* p = type_node->as<const PrimitiveType>();
            switch (p->prim_kind) {
                case PrimitiveType::Kind::Void:   return "void";
                case PrimitiveType::Kind::Bool:   return "bool";
                case PrimitiveType::Kind::Int:    return "int";
                case PrimitiveType::Kind::UInt:   return "unsigned int";
                case PrimitiveType::Kind::Long:   return "long";
                case PrimitiveType::Kind::LongLong: return "long long";
                case PrimitiveType::Kind::Float:  return "float";
                case PrimitiveType::Kind::Double: return "double";
                case PrimitiveType::Kind::Char:   return "char";
                case PrimitiveType::Kind::Short:  return "short";
                case PrimitiveType::Kind::Auto:   return "auto";
                case PrimitiveType::Kind::SizeT:  return "size_t";
                default: return "int";
            }
        }
        case NodeKind::NamedType: {
            auto* n = type_node->as<const NamedType>();
            return n->qualified_name();
        }
        case NodeKind::TemplateInstType: {
            auto* t = type_node->as<const TemplateInstType>();
            return t->qualified_name();
        }
        case NodeKind::QualifiedType: {
            auto* q = type_node->as<const QualifiedType>();
            return extract_type_name(q->inner.get());
        }
        case NodeKind::ReferenceType: {
            auto* r = type_node->as<const ReferenceType>();
            return extract_type_name(r->referee.get());
        }
        case NodeKind::RValueRefType: {
            auto* r = type_node->as<const RValueRefType>();
            return extract_type_name(r->referee.get());
        }
        default:
            return "";
    }
}

ValuePtr Interpreter::default_value(const std::string& type_name) {
    if (type_name == "int" || type_name == "long" || type_name == "short" ||
        type_name == "long long" || type_name == "size_t" ||
        type_name == "unsigned int" || type_name == "char")
        return Value::make_int(0);
    if (type_name == "float" || type_name == "double")
        return Value::make_float(0.0);
    if (type_name == "bool")
        return Value::make_bool(false);
    if (type_name == "string" || type_name == "std::string")
        return Value::make_str("");
    return Value::make_null();
}

ValuePtr Interpreter::try_parse_number(const std::string& s) {
    try {
        size_t pos;
        int64_t iv = std::stoll(s, &pos);
        if (pos == s.size()) return Value::make_int(iv);
    } catch (...) {}
    try {
        size_t pos;
        double dv = std::stod(s, &pos);
        if (pos == s.size()) return Value::make_float(dv);
    } catch (...) {}
    return Value::make_str(s);
}

bool Interpreter::is_stdlib_include(const std::string& header) {
    static const std::vector<std::string> known = {
        "iostream", "string", "vector", "map", "set", "algorithm",
        "cmath", "cstdlib", "cstdio", "cstring", "cassert",
        "fstream", "sstream", "iomanip", "memory", "functional",
        "array", "deque", "list", "stack", "queue", "unordered_map",
        "unordered_set", "numeric", "utility", "stdexcept", "exception",
        "climits", "cfloat", "ctime", "chrono", "random",
    };
    for (auto& k : known)
        if (header.find(k) != std::string::npos) return true;
    return false;
}

std::vector<ValuePtr> Interpreter::eval_args(const std::vector<NodePtr>& exprs, EnvPtr env) {
    std::vector<ValuePtr> args;
    args.reserve(exprs.size());
    for (auto& e : exprs)
        args.push_back(eval_expr(*e, env));
    return args;
}

// ─── Source splitter ─────────────────────────────────────────────
// The rexc parser expects valid C++ top-level (declarations only).
// Script-style code needs to be restructured: function/class defs
// stay at the top level, and loose statements go into a wrapper.

struct SplitSource {
    std::string top_level;   // function/class declarations
    std::string statements;  // loose statements for the wrapper
};

static std::string ltrim(const std::string& s) {
    auto pos = s.find_first_not_of(" \t");
    return pos == std::string::npos ? "" : s.substr(pos);
}

// Checks if a trimmed line begins a top-level block (function or class/struct).
// Returns the number of source characters consumed by that block (including
// its closing brace and optional semicolon), or 0 if the position does not
// start a block.
static size_t detect_block(const std::string& src, size_t pos) {
    std::string rest = src.substr(pos);
    std::string trimmed = ltrim(rest);

    // ── class / struct ──────────────────────────────────────
    bool is_class = (trimmed.rfind("class ", 0) == 0 ||
                     trimmed.rfind("struct ", 0) == 0);
    if (is_class) {
        // Must contain a '{' to be a definition (not a forward decl)
        size_t brace = rest.find('{');
        if (brace == std::string::npos) return 0;
        int depth = 0;
        for (size_t i = brace; i < rest.size(); i++) {
            if (rest[i] == '{') depth++;
            else if (rest[i] == '}') { depth--; if (depth == 0) {
                // skip optional ';'
                size_t end = i + 1;
                while (end < rest.size() && (rest[end] == ' ' || rest[end] == '\t' || rest[end] == '\n')) end++;
                if (end < rest.size() && rest[end] == ';') end++;
                return end;
            }}
        }
        return 0;
    }

    // ── function definition ──────────────────────────────────
    // Heuristic: a line that looks like  TYPE NAME ( ... ) { ... }
    // Quick check: find '(' and then '{' before ';'
    size_t paren = rest.find('(');
    if (paren == std::string::npos) return 0;
    // The text before '(' should be a type + name (no '=' or other stmts)
    std::string before_paren = ltrim(rest.substr(0, paren));
    // Must not start with keywords that indicate a statement
    if (before_paren.rfind("if", 0) == 0 || before_paren.rfind("while", 0) == 0 ||
        before_paren.rfind("for", 0) == 0 || before_paren.rfind("switch", 0) == 0 ||
        before_paren.rfind("catch", 0) == 0 || before_paren.rfind("return", 0) == 0 ||
        before_paren.rfind("try", 0) == 0 || before_paren.rfind("throw", 0) == 0)
        return 0;
    // Must contain at least one space (for "TYPE NAME")
    if (before_paren.find(' ') == std::string::npos &&
        before_paren.find('\t') == std::string::npos)
        return 0;
    // Must NOT contain '=', ';', '<<', '>>' or other operators before '('
    // These indicate it's a statement (e.g., "int i=0; while(...")
    if (before_paren.find('=') != std::string::npos ||
        before_paren.find(';') != std::string::npos ||
        before_paren.find('+') != std::string::npos ||
        before_paren.find('-') != std::string::npos)
        return 0;

    // Find matching ')' then look for '{'
    int pdepth = 0;
    size_t rparen = std::string::npos;
    for (size_t i = paren; i < rest.size(); i++) {
        if (rest[i] == '(') pdepth++;
        else if (rest[i] == ')') { pdepth--; if (pdepth == 0) { rparen = i; break; } }
    }
    if (rparen == std::string::npos) return 0;

    // After ')' skip whitespace, optional qualifiers (const, override, noexcept), then look for '{'
    size_t scan = rparen + 1;
    while (scan < rest.size()) {
        while (scan < rest.size() && (rest[scan] == ' ' || rest[scan] == '\t' || rest[scan] == '\n')) scan++;
        // Skip known qualifiers
        bool skipped = false;
        for (const char* kw : {"const", "override", "final", "noexcept"}) {
            size_t kwlen = std::strlen(kw);
            if (rest.substr(scan, kwlen) == kw && (scan+kwlen >= rest.size() || !std::isalnum(rest[scan+kwlen]))) {
                scan += kwlen; skipped = true; break;
            }
        }
        if (!skipped) break;
    }

    // Handle constructor initializer list (: field(val), ...)
    if (scan < rest.size() && rest[scan] == ':') {
        scan++;
        while (scan < rest.size() && rest[scan] != '{') scan++;
    }

    if (scan >= rest.size() || rest[scan] != '{') return 0;
    // If there's a ';' before '{', it's a declaration, not definition
    for (size_t i = rparen + 1; i < scan; i++) {
        if (rest[i] == ';') return 0;
    }

    // Found '{' — match the brace block
    int depth = 0;
    for (size_t i = scan; i < rest.size(); i++) {
        if (rest[i] == '{') depth++;
        else if (rest[i] == '}') { depth--; if (depth == 0) return i + 1; }
    }
    return 0;
}

static SplitSource split_source(const std::string& source) {
    SplitSource result;
    size_t pos = 0;
    while (pos < source.size()) {
        // Skip whitespace
        while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n'))
            pos++;
        if (pos >= source.size()) break;

        // Check for preprocessor / using
        std::string trimmed = ltrim(source.substr(pos));
        if (trimmed.rfind("#include", 0) == 0 || trimmed.rfind("#define", 0) == 0 ||
            trimmed.rfind("#pragma", 0) == 0 || trimmed.rfind("#ifdef", 0) == 0 ||
            trimmed.rfind("#ifndef", 0) == 0 || trimmed.rfind("#endif", 0) == 0 ||
            trimmed.rfind("using namespace", 0) == 0 || trimmed.rfind("using ", 0) == 0) {
            // Skip to end of line
            size_t eol = source.find('\n', pos);
            if (eol == std::string::npos) eol = source.size();
            pos = eol + 1;
            continue;
        }

        // Try to detect a top-level block (function or class)
        size_t block_len = detect_block(source, pos);
        if (block_len > 0) {
            result.top_level += source.substr(pos, block_len) + "\n";
            pos += block_len;
            continue;
        }

        // Otherwise, consume until ';' or end of brace block — it's a statement
        int depth = 0;
        int paren_depth = 0;
        size_t start = pos;
        while (pos < source.size()) {
            if (source[pos] == '(') paren_depth++;
            else if (source[pos] == ')') paren_depth--;
            else if (source[pos] == '{') depth++;
            else if (source[pos] == '}') {
                depth--;
                if (depth <= 0) { pos++; break; }
            }
            else if (source[pos] == ';' && depth == 0 && paren_depth == 0) { pos++; break; }
            pos++;
        }
        result.statements += source.substr(start, pos - start) + "\n";
    }
    return result;
}

// ─── run_file ────────────────────────────────────────────────────

// Resolve #include directives for installed libraries.
// Standard library includes (iostream, string, etc.) are skipped by the
// interpreter.  Non-stdlib includes (e.g. #include "termcolor.hpp") are
// searched in: (1) the same directory as the source file, (2) each
// subdirectory under ~/.rex/libs/.  Matched files are inlined.
static std::string resolve_includes(const std::string& source,
                                     const std::string& filepath,
                                     int depth = 0) {
    // Guard against infinite include recursion
    if (depth > 16) return source;

    // Known stdlib headers that the interpreter handles natively
    static const std::vector<std::string> stdlib_headers = {
        "iostream", "string", "vector", "map", "set", "algorithm",
        "cmath", "cstdlib", "cstdio", "cstring", "cassert",
        "fstream", "sstream", "iomanip", "memory", "functional",
        "array", "deque", "list", "stack", "queue", "unordered_map",
        "unordered_set", "numeric", "utility", "stdexcept", "exception",
        "climits", "cfloat", "ctime", "chrono", "random",
        "stdio.h", "stdlib.h", "string.h", "math.h", "time.h",
        "windows.h", "Windows.h", "unistd.h",
        "thread", "mutex", "condition_variable", "atomic",
    };
    auto is_stdlib = [&](const std::string& header) -> bool {
        for (auto& h : stdlib_headers)
            if (header == h || header.find(h) != std::string::npos) return true;
        return false;
    };

    // Source file directory for relative includes
    fs::path source_dir = fs::path(filepath).parent_path();

    // Collect ~/.rex/libs/ subdirectories for library search
    fs::path libs_root;
    {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home) libs_root = fs::path(home) / ".rex" / "libs";
    }

    std::string result;
    std::istringstream stream(source);
    std::string line;

    while (std::getline(stream, line)) {
        std::string trimmed = ltrim(line);
        if (trimmed.rfind("#include", 0) != 0) {
            result += line + "\n";
            continue;
        }

        // Extract header name from #include "header" or #include <header>
        std::string header;
        bool is_quoted = false;
        size_t q1 = trimmed.find('"');
        if (q1 != std::string::npos) {
            size_t q2 = trimmed.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                header = trimmed.substr(q1 + 1, q2 - q1 - 1);
                is_quoted = true;
            }
        }
        if (header.empty()) {
            size_t a1 = trimmed.find('<');
            if (a1 != std::string::npos) {
                size_t a2 = trimmed.find('>', a1 + 1);
                if (a2 != std::string::npos)
                    header = trimmed.substr(a1 + 1, a2 - a1 - 1);
            }
        }

        // If stdlib header, skip (interpreter handles natively)
        if (header.empty() || is_stdlib(header)) {
            result += "// " + line + "\n";  // Comment it out
            continue;
        }

        // Try to find the header file
        fs::path found_path;

        // 1. Check relative to source file
        if (is_quoted) {
            fs::path candidate = source_dir / header;
            if (fs::exists(candidate)) found_path = candidate;
        }

        // 2. Check in ~/.rex/libs/ subdirectories
        if (found_path.empty() && !libs_root.empty() && fs::exists(libs_root)) {
            for (auto& entry : fs::directory_iterator(libs_root)) {
                if (!fs::is_directory(entry)) continue;
                fs::path candidate = entry.path() / header;
                if (fs::exists(candidate)) {
                    found_path = candidate;
                    break;
                }
            }
        }

        if (!found_path.empty()) {
            // Inline the included file
            std::ifstream inc_file(found_path);
            if (inc_file.is_open()) {
                std::string inc_source((std::istreambuf_iterator<char>(inc_file)), {});
                // Recursively resolve includes in the included file
                inc_source = resolve_includes(inc_source, found_path.string(), depth + 1);
                result += "// [rex:include] " + header + "\n";
                result += inc_source + "\n";
                continue;
            }
        }

        // Header not found — comment it out (interpreter will handle
        // missing symbols with clear error messages)
        result += "// " + line + "\n";
    }
    return result;
}

int Interpreter::run_file(const std::string& filepath) {
    std::ifstream f(filepath);
    if (!f) throw RuntimeError("cannot open: " + filepath);
    std::string source((std::istreambuf_iterator<char>(f)), {});

    // Shebang support (#!/usr/bin/env rex)
    if (source.size() >= 2 && source[0] == '#' && source[1] == '!')
        source = source.substr(source.find('\n') + 1);

    // Resolve #include directives for installed libraries
    source = resolve_includes(source, filepath);

    auto result = run_string(source, filepath);
    if (result && result->is_int()) return (int)result->as_int();
    return 0;
}

// ─── run_string ──────────────────────────────────────────────────

ValuePtr Interpreter::run_string(const std::string& source,
                                  const std::string& filename) {
    // Split source into top-level declarations and loose statements.
    // Top-level function/class defs are kept as-is for the parser;
    // loose statements are wrapped in a synthetic function so the
    // parser can handle them.
    auto split = split_source(source);

    std::string combined = split.top_level;
    bool has_stmts = !split.statements.empty() &&
                     split.statements.find_first_not_of(" \t\n") != std::string::npos;
    if (has_stmts)
        combined += "void __rex_stmts__() {\n" + split.statements + "\n}\n";

    Lexer lexer(combined, filename);
    auto tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    auto tu_ptr = parser.parse();
    auto* tu = tu_ptr->as<TranslationUnit>();

    // First pass: register all top-level functions and classes
    exec_decls(tu->decls, global_env_);

    // Second pass: execute __rex_stmts__() if it exists
    if (has_stmts && global_env_->has("__rex_stmts__")) {
        auto stmts_fn = global_env_->get("__rex_stmts__");
        if (stmts_fn->is_func()) {
            try {
                call_func(stmts_fn->as_func(), {});
            } catch (ReturnSignal&) {}
        }
    }

    // If main() was defined, call it
    if (global_env_->has("main")) {
        auto main_fn = global_env_->get("main");
        if (main_fn->is_func()) {
            try {
                return call_func(main_fn->as_func(), {});
            } catch (ReturnSignal& r) {
                return r.value;
            }
        }
    }

    return Value::make_null();
}

// ─── exec_decls ──────────────────────────────────────────────────

void Interpreter::exec_decls(const std::vector<NodePtr>& decls, EnvPtr env) {
    for (auto& d : decls) {
        if (!d) continue;
        switch (d->kind) {
            case NodeKind::FunctionDecl:
            case NodeKind::ConstructorDecl:
            case NodeKind::DestructorDecl:
                exec_function_decl(*d->as<FunctionDecl>(), env);
                break;
            case NodeKind::ClassDecl:
                exec_class_decl(*d->as<ClassDecl>(), env);
                break;
            case NodeKind::VarDecl:
                exec_var_decl(*d->as<VarDecl>(), env);
                break;
            case NodeKind::NamespaceDecl:
                exec_namespace(*d->as<NamespaceDecl>(), env);
                break;
            case NodeKind::DeclStmt: {
                auto* ds = d->as<DeclStmt>();
                exec_decls(ds->decls, env);
                break;
            }
            case NodeKind::ExprStmt: {
                auto* es = d->as<ExprStmt>();
                if (es->expr) eval_expr(*es->expr, env);
                break;
            }
            case NodeKind::PreprocessorDecl:
                // Skip #include, #define, etc
                break;
            case NodeKind::UsingDecl:
                // using namespace std; — already handled by builtins
                break;
            case NodeKind::TemplateDecl:
                // Template declarations: register the inner decl if it's a function or class
                break;
            default:
                exec_stmt(*d, env);
                break;
        }
    }
}

void Interpreter::exec_function_decl(const FunctionDecl& fn, EnvPtr env) {
    if (fn.name == "main") {
        // Register main but don't execute yet
        std::vector<std::string> param_names;
        for (auto& p : fn.params) {
            if (p && p->kind == NodeKind::ParamDecl)
                param_names.push_back(p->as<ParamDecl>()->name);
        }
        env->define("main", Value::make_func("main", std::move(param_names), &fn, env));
        return;
    }

    std::vector<std::string> param_names;
    for (auto& p : fn.params) {
        if (p && p->kind == NodeKind::ParamDecl)
            param_names.push_back(p->as<ParamDecl>()->name);
    }
    env->define(fn.name, Value::make_func(fn.name, std::move(param_names), &fn, env));
}

void Interpreter::exec_class_decl(const ClassDecl& cls, EnvPtr env) {
    class_registry_[cls.name] = &cls;

    auto cls_env = env->make_child();
    class_method_envs_[cls.name] = cls_env;

    // Register base class methods if inheritance
    for (auto& base : cls.bases) {
        auto it = class_registry_.find(base.name);
        if (it != class_registry_.end()) {
            auto base_env = class_method_envs_[base.name];
            if (base_env) {
                for (auto& [name, val] : base_env->vars) {
                    if (!cls_env->vars.count(name))
                        cls_env->define(name, val);
                }
            }
        }
    }

    // Register methods and fields
    for (auto& m : cls.members) {
        if (!m) continue;
        if (m->kind == NodeKind::FunctionDecl ||
            m->kind == NodeKind::ConstructorDecl ||
            m->kind == NodeKind::DestructorDecl) {
            auto* fn = m->as<FunctionDecl>();
            std::vector<std::string> params;
            for (auto& p : fn->params) {
                if (p && p->kind == NodeKind::ParamDecl)
                    params.push_back(p->as<ParamDecl>()->name);
            }
            cls_env->define(fn->name, Value::make_func(fn->name, std::move(params), fn, cls_env));
        }
        // AccessSpecifier and FieldDecl are handled during construction
    }

    // Make the class name callable as a constructor
    env->define(cls.name, Value::make_native(cls.name,
        [this, cls_name = cls.name, env](std::vector<ValuePtr> args) -> ValuePtr {
            return construct_object(cls_name, std::move(args), env);
        }));
}

void Interpreter::exec_var_decl(const VarDecl& var, EnvPtr env) {
    std::string type_name = extract_type_name(var.type.get());

    // Detect vector/array type (TemplateInstType with "vector" name)
    bool is_vector_type = false;
    if (var.type) {
        if (var.type->kind == NodeKind::TemplateInstType) {
            auto* ti = var.type->as<const TemplateInstType>();
            std::string qn = ti->qualified_name();
            if (qn == "vector" || qn == "std::vector") is_vector_type = true;
        }
        if (type_name == "vector" || type_name == "std::vector") is_vector_type = true;
    }

    ValuePtr val;
    if (var.init) {
        // Constructor-syntax: Type name(args) stores args as InitListExpr
        if (var.init->kind == NodeKind::InitListExpr) {
            auto* il = var.init->as<const InitListExpr>();
            if (class_registry_.count(type_name)) {
                // User-defined class: treat init list as constructor args
                auto args = eval_args(il->elements, env);
                val = construct_object(type_name, std::move(args), env);
            } else if (is_vector_type) {
                // vector<T> v = {1,2,3} or vector<T> v({...})
                val = make_vector_object();
                for (auto& elem : il->elements)
                    val->as_array().elements.push_back(eval_expr(*elem, env));
            } else if (type_name == "string" || type_name == "std::string") {
                if (!il->elements.empty())
                    val = eval_expr(*il->elements[0], env);
                else
                    val = Value::make_str("");
            } else {
                val = eval_expr(*var.init, env);
            }
        } else {
            val = eval_expr(*var.init, env);
        }
    } else {
        // No initializer
        if (is_vector_type)
            val = make_vector_object();
        else if (class_registry_.count(type_name))
            val = construct_object(type_name, {}, env);
        else
            val = default_value(type_name);
    }
    env->define(var.name, val);
}

void Interpreter::exec_namespace(const NamespaceDecl& ns, EnvPtr env) {
    // For simplicity, execute namespace declarations in the current scope
    // (e.g., using namespace std; already covered)
    exec_decls(ns.decls, env);
}

// ─── Statements ──────────────────────────────────────────────────

void Interpreter::exec_stmt(const Node& stmt, EnvPtr env) {
    switch (stmt.kind) {
        case NodeKind::CompoundStmt:
            exec_block(*stmt.as<const CompoundStmt>(), env);
            break;
        case NodeKind::IfStmt:
            exec_if(*stmt.as<const IfStmt>(), env);
            break;
        case NodeKind::WhileStmt:
            exec_while(*stmt.as<const WhileStmt>(), env);
            break;
        case NodeKind::DoWhileStmt: {
            auto* dw = stmt.as<const DoWhileStmt>();
            auto loop_env = env->make_child();
            do {
                try {
                    exec_stmt(*dw->body, loop_env);
                } catch (BreakSignal&) { return; }
                  catch (ContinueSignal&) { /* continue */ }
            } while (eval_expr(*dw->condition, loop_env)->to_bool());
            break;
        }
        case NodeKind::ForStmt:
            exec_for(*stmt.as<const ForStmt>(), env);
            break;
        case NodeKind::RangeForStmt:
            exec_for_range(*stmt.as<const RangeForStmt>(), env);
            break;
        case NodeKind::ReturnStmt:
            exec_return(*stmt.as<const ReturnStmt>(), env);
            break;
        case NodeKind::BreakStmt:
            throw BreakSignal{};
        case NodeKind::ContinueStmt:
            throw ContinueSignal{};
        case NodeKind::ExprStmt: {
            auto* es = stmt.as<const ExprStmt>();
            if (es->expr) eval_expr(*es->expr, env);
            break;
        }
        case NodeKind::DeclStmt: {
            auto* ds = stmt.as<const DeclStmt>();
            exec_decls(ds->decls, env);
            break;
        }
        case NodeKind::VarDecl:
            exec_var_decl(*stmt.as<const VarDecl>(), env);
            break;
        case NodeKind::TryStmt:
            exec_try_catch(*stmt.as<const TryStmt>(), env);
            break;
        case NodeKind::ThrowStmt:
            exec_throw(*stmt.as<const ThrowStmt>(), env);
            break;
        case NodeKind::FunctionDecl:
        case NodeKind::ConstructorDecl:
        case NodeKind::DestructorDecl:
            exec_function_decl(*stmt.as<const FunctionDecl>(), env);
            break;
        case NodeKind::ClassDecl:
            exec_class_decl(*stmt.as<const ClassDecl>(), env);
            break;
        case NodeKind::NullStmt:
            break;
        default:
            break;
    }
}

void Interpreter::exec_block(const CompoundStmt& blk, EnvPtr env) {
    auto block_env = env->make_child();
    for (auto& s : blk.stmts) {
        if (!s) continue;
        exec_stmt(*s, block_env);
    }
}

void Interpreter::exec_if(const IfStmt& s, EnvPtr env) {
    auto if_env = env->make_child();
    bool cond = eval_expr(*s.condition, if_env)->to_bool();
    if (cond) {
        exec_stmt(*s.then_branch, if_env);
    } else if (s.else_branch) {
        exec_stmt(*s.else_branch, if_env);
    }
}

void Interpreter::exec_while(const WhileStmt& s, EnvPtr env) {
    while (eval_expr(*s.condition, env)->to_bool()) {
        try {
            exec_stmt(*s.body, env);
        } catch (BreakSignal&) { break; }
          catch (ContinueSignal&) { continue; }
    }
}

void Interpreter::exec_for(const ForStmt& s, EnvPtr env) {
    auto for_env = env->make_child();
    if (s.init) exec_stmt(*s.init, for_env);
    while (true) {
        if (s.condition) {
            auto cond = eval_expr(*s.condition, for_env);
            if (!cond->to_bool()) break;
        }
        try {
            exec_stmt(*s.body, for_env);
        } catch (BreakSignal&) { break; }
          catch (ContinueSignal&) { /* fall through to increment */ }
        if (s.increment) eval_expr(*s.increment, for_env);
    }
}

void Interpreter::exec_for_range(const RangeForStmt& s, EnvPtr env) {
    auto range_val = eval_expr(*s.range_expr, env);
    if (!range_val->is_array())
        throw RuntimeError("range-for requires an array/vector");

    std::string var_name;
    if (s.range_decl) {
        if (s.range_decl->kind == NodeKind::VarDecl)
            var_name = s.range_decl->as<const VarDecl>()->name;
    }
    if (var_name.empty()) var_name = "__range_var__";

    auto& elems = range_val->as_array().elements;
    for (auto& elem : elems) {
        auto loop_env = env->make_child();
        loop_env->define(var_name, elem);
        try {
            exec_stmt(*s.body, loop_env);
        } catch (BreakSignal&) { break; }
          catch (ContinueSignal&) { continue; }
    }
}

void Interpreter::exec_return(const ReturnStmt& s, EnvPtr env) {
    ValuePtr val = s.value ? eval_expr(*s.value, env) : Value::make_null();
    throw ReturnSignal{val};
}

void Interpreter::exec_try_catch(const TryStmt& s, EnvPtr env) {
    try {
        exec_stmt(*s.try_body, env);
    } catch (ThrowSignal& ts) {
        for (auto& c : s.catches) {
            auto* clause = c->as<CatchClause>();
            auto catch_env = env->make_child();
            if (clause->is_ellipsis) {
                // catch(...) — catch everything
                exec_stmt(*clause->body, catch_env);
                return;
            }
            if (clause->param) {
                auto* param = clause->param->as<ParamDecl>();
                catch_env->define(param->name, ts.value);
            }
            exec_stmt(*clause->body, catch_env);
            return;
        }
        throw; // re-throw if no catch matched
    }
}

void Interpreter::exec_throw(const ThrowStmt& s, EnvPtr env) {
    ValuePtr val = s.expr ? eval_expr(*s.expr, env) : Value::make_null();
    std::string type = val->is_int() ? "int" : val->is_str() ? "string" : "unknown";
    throw ThrowSignal{val, type};
}

// ─── Expressions ─────────────────────────────────────────────────

ValuePtr Interpreter::eval_expr(const Node& e, EnvPtr env) {
    switch (e.kind) {
        case NodeKind::IntLiteralExpr:
            return Value::make_int((int64_t)e.as<const IntLiteralExpr>()->value);
        case NodeKind::FloatLiteralExpr:
            return Value::make_float(e.as<const FloatLiteralExpr>()->value);
        case NodeKind::StringLiteralExpr:
            return Value::make_str(e.as<const StringLiteralExpr>()->cooked);
        case NodeKind::CharLiteralExpr:
            return Value::make_int((int64_t)e.as<const CharLiteralExpr>()->char_val);
        case NodeKind::BoolLiteralExpr:
            return Value::make_bool(e.as<const BoolLiteralExpr>()->value);
        case NodeKind::NullptrExpr:
            return Value::make_null();
        case NodeKind::ThisExpr:
            return env->get("this");
        case NodeKind::IdentifierExpr: {
            auto& name = e.as<const IdentifierExpr>()->name;
            if (env->has(name)) return env->get(name);
            // Check if it's a class name for construction
            if (class_registry_.count(name)) {
                return Value::make_native(name,
                    [this, name, env](std::vector<ValuePtr> args) -> ValuePtr {
                        return construct_object(name, std::move(args), env);
                    });
            }
            throw RuntimeError("undefined variable: '" + name + "'",
                               e.loc.line, e.loc.column);
        }
        case NodeKind::BinaryExpr:
            return eval_binary(*e.as<const BinaryExpr>(), env);
        case NodeKind::UnaryExpr:
            return eval_unary(*e.as<const UnaryExpr>(), env);
        case NodeKind::AssignExpr:
            return eval_assign(*e.as<const AssignExpr>(), env);
        case NodeKind::CallExpr:
            return eval_call(*e.as<const CallExpr>(), env);
        case NodeKind::MemberExpr:
            return eval_member(*e.as<const MemberExpr>(), env);
        case NodeKind::IndexExpr:
            return eval_index(*e.as<const IndexExpr>(), env);
        case NodeKind::NewExpr:
            return eval_new(*e.as<const NewExpr>(), env);
        case NodeKind::LambdaExpr:
            return eval_lambda(*e.as<const LambdaExpr>(), env);
        case NodeKind::TernaryExpr:
            return eval_ternary(*e.as<const TernaryExpr>(), env);
        case NodeKind::InitListExpr:
            return eval_init_list(*e.as<const InitListExpr>(), env);
        case NodeKind::ParenExpr:
            return eval_expr(*e.as<const ParenExpr>()->inner, env);
        case NodeKind::ScopeExpr: {
            auto* se = e.as<const ScopeExpr>();
            // Handle std::cout, std::cerr, std::cin, std::endl, std::string, std::vector
            std::string full = se->qualified();
            if (full == "std::cout") return env->get("cout");
            if (full == "std::cerr") return env->get("cerr");
            if (full == "std::cin")  return env->get("cin");
            if (full == "std::endl") return env->get("endl");
            if (full == "std::string" || full == "std::vector") {
                return Value::make_native(full,
                    [this, full, env](std::vector<ValuePtr> args) -> ValuePtr {
                        if (full == "std::string")
                            return Value::make_str(args.empty() ? "" : args[0]->as_str());
                        return make_vector_object();
                    });
            }
            // std::getline(cin, variable) — reads a full line from stdin
            if (full == "std::getline") {
                return Value::make_native("std::getline",
                    [](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.size() < 2)
                            throw RuntimeError("std::getline requires 2 arguments (stream, string)");
                        std::string line;
                        std::getline(std::cin, line);
                        // Mutate the second argument (string variable) in-place
                        args[1]->inner = StrVal{line};
                        return Value::make_bool(!std::cin.fail());
                    });
            }
            // std::to_string, std::stoi, std::stof via scope resolution
            if (full == "std::to_string") {
                return Value::make_native("std::to_string",
                    [](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) throw RuntimeError("std::to_string requires 1 argument");
                        return Value::make_str(args[0]->to_string());
                    });
            }
            if (full == "std::stoi") {
                return Value::make_native("std::stoi",
                    [](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) throw RuntimeError("std::stoi requires 1 argument");
                        return Value::make_int(std::stoll(args[0]->as_str()));
                    });
            }
            if (full == "std::stof" || full == "std::stod") {
                std::string fn_name = full;
                return Value::make_native(fn_name,
                    [fn_name](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) throw RuntimeError(fn_name + " requires 1 argument");
                        return Value::make_float(std::stod(args[0]->as_str()));
                    });
            }
            if (env->has(se->name)) return env->get(se->name);
            // Try resolving just the name part (e.g. a using-namespace brought it in)
            if (env->has(full)) return env->get(full);
            throw RuntimeError("undefined: '" + full + "'");
        }
        case NodeKind::CastExpr: {
            auto* ce = e.as<const CastExpr>();
            auto val = eval_expr(*ce->expr, env);
            std::string target_type = extract_type_name(ce->type.get());
            if (target_type == "int" || target_type == "long" || target_type == "long long") {
                if (val->is_float()) return Value::make_int((int64_t)val->as_float());
                if (val->is_int()) return val;
                if (val->is_bool()) return Value::make_int(val->as_bool() ? 1 : 0);
            }
            if (target_type == "double" || target_type == "float") {
                if (val->is_int()) return Value::make_float((double)val->as_int());
                if (val->is_float()) return val;
            }
            if (target_type == "bool") return Value::make_bool(val->to_bool());
            if (target_type == "string" || target_type == "std::string")
                return Value::make_str(val->to_string());
            return val;
        }
        case NodeKind::SizeofExpr: {
            auto* se = e.as<const SizeofExpr>();
            if (se->type) {
                std::string tn = extract_type_name(se->type.get());
                if (tn == "int" || tn == "float") return Value::make_int(4);
                if (tn == "double" || tn == "long" || tn == "long long") return Value::make_int(8);
                if (tn == "char" || tn == "bool") return Value::make_int(1);
                if (tn == "short") return Value::make_int(2);
                return Value::make_int(8);
            }
            return Value::make_int(8);
        }
        case NodeKind::CommaExpr: {
            auto* ce = e.as<const CommaExpr>();
            eval_expr(*ce->left, env);
            return eval_expr(*ce->right, env);
        }
        default:
            return Value::make_null();
    }
}

// ─── Binary ──────────────────────────────────────────────────────

ValuePtr Interpreter::eval_binary(const BinaryExpr& e, EnvPtr env) {
    // Special handling for << (cout) and >> (cin) operators
    if (e.op == TokenKind::LShift) {
        auto left = eval_expr(*e.left, env);
        if (left->is_object() && left->as_object().class_name == "__ostream__") {
            auto right = eval_expr(*e.right, env);
            bool is_cerr = left->as_object().fields.count("__is_cerr__");
            if (right->is_object() && right->as_object().class_name == "__endl__") {
                (is_cerr ? std::cerr : std::cout) << '\n';
                (is_cerr ? std::cerr : std::cout).flush();
            } else {
                (is_cerr ? std::cerr : std::cout) << right->to_string();
            }
            return left; // for chaining: cout << a << b
        }
    }

    if (e.op == TokenKind::RShift) {
        auto left = eval_expr(*e.left, env);
        if (left->is_object() && left->as_object().class_name == "__istream__") {
            std::string token;
            std::cin >> token;
            ValuePtr val = try_parse_number(token);
            assign_lvalue(*e.right, val, env);
            return left; // for chaining
        }
    }

    // Short-circuit logical operators
    if (e.op == TokenKind::AmpAmp) {
        auto left = eval_expr(*e.left, env);
        if (!left->to_bool()) return Value::make_bool(false);
        return Value::make_bool(eval_expr(*e.right, env)->to_bool());
    }
    if (e.op == TokenKind::PipePipe) {
        auto left = eval_expr(*e.left, env);
        if (left->to_bool()) return Value::make_bool(true);
        return Value::make_bool(eval_expr(*e.right, env)->to_bool());
    }

    auto left  = eval_expr(*e.left, env);
    auto right = eval_expr(*e.right, env);

    switch (e.op) {
        case TokenKind::Plus:    return left->op_add(*right);
        case TokenKind::Minus:   return left->op_sub(*right);
        case TokenKind::Star:    return left->op_mul(*right);
        case TokenKind::Slash:   return left->op_div(*right);
        case TokenKind::Percent: return left->op_mod(*right);
        case TokenKind::Eq:      return left->op_eq(*right);
        case TokenKind::NotEq:   return left->op_ne(*right);
        case TokenKind::Lt:      return left->op_lt(*right);
        case TokenKind::LtEq:    return left->op_le(*right);
        case TokenKind::Gt:      return left->op_gt(*right);
        case TokenKind::GtEq:    return left->op_ge(*right);
        case TokenKind::LShift: {
            if (left->is_int() && right->is_int())
                return Value::make_int(left->as_int() << right->as_int());
            return Value::make_null();
        }
        case TokenKind::RShift: {
            if (left->is_int() && right->is_int())
                return Value::make_int(left->as_int() >> right->as_int());
            return Value::make_null();
        }
        case TokenKind::Amp:
            if (left->is_int() && right->is_int())
                return Value::make_int(left->as_int() & right->as_int());
            return Value::make_null();
        case TokenKind::Pipe:
            if (left->is_int() && right->is_int())
                return Value::make_int(left->as_int() | right->as_int());
            return Value::make_null();
        case TokenKind::Caret:
            if (left->is_int() && right->is_int())
                return Value::make_int(left->as_int() ^ right->as_int());
            return Value::make_null();
        default:
            return Value::make_null();
    }
}

// ─── Unary ───────────────────────────────────────────────────────

ValuePtr Interpreter::eval_unary(const UnaryExpr& e, EnvPtr env) {
    if (e.is_postfix) {
        auto val = eval_expr(*e.operand, env);
        ValuePtr result;
        if (e.op == TokenKind::PlusPlus) {
            result = Value::make_int(val->as_int());
            assign_lvalue(*e.operand, Value::make_int(val->as_int() + 1), env);
        } else if (e.op == TokenKind::MinusMinus) {
            result = Value::make_int(val->as_int());
            assign_lvalue(*e.operand, Value::make_int(val->as_int() - 1), env);
        } else {
            result = val;
        }
        return result;
    }

    switch (e.op) {
        case TokenKind::Minus:
            return eval_expr(*e.operand, env)->op_neg();
        case TokenKind::Bang:
            return eval_expr(*e.operand, env)->op_not();
        case TokenKind::Tilde:
            return Value::make_int(~eval_expr(*e.operand, env)->as_int());
        case TokenKind::PlusPlus: {
            auto val = eval_expr(*e.operand, env);
            auto new_val = Value::make_int(val->as_int() + 1);
            assign_lvalue(*e.operand, new_val, env);
            return new_val;
        }
        case TokenKind::MinusMinus: {
            auto val = eval_expr(*e.operand, env);
            auto new_val = Value::make_int(val->as_int() - 1);
            assign_lvalue(*e.operand, new_val, env);
            return new_val;
        }
        case TokenKind::Plus:
            return eval_expr(*e.operand, env);
        default:
            return eval_expr(*e.operand, env);
    }
}

// ─── Assign ──────────────────────────────────────────────────────

ValuePtr Interpreter::eval_assign(const AssignExpr& e, EnvPtr env) {
    auto val = eval_expr(*e.value, env);

    if (e.op != TokenKind::Assign) {
        auto cur = eval_expr(*e.target, env);
        switch (e.op) {
            case TokenKind::PlusAssign:    val = cur->op_add(*val); break;
            case TokenKind::MinusAssign:   val = cur->op_sub(*val); break;
            case TokenKind::StarAssign:    val = cur->op_mul(*val); break;
            case TokenKind::SlashAssign:   val = cur->op_div(*val); break;
            case TokenKind::PercentAssign: val = cur->op_mod(*val); break;
            default: break;
        }
    }

    assign_lvalue(*e.target, val, env);
    return val;
}

// ─── assign_lvalue ───────────────────────────────────────────────

void Interpreter::assign_lvalue(const Node& target, ValuePtr val, EnvPtr env) {
    switch (target.kind) {
        case NodeKind::IdentifierExpr: {
            auto& name = target.as<const IdentifierExpr>()->name;
            if (env->has(name))
                env->assign(name, val);
            else
                env->define(name, val);
            break;
        }
        case NodeKind::MemberExpr: {
            auto* me = target.as<const MemberExpr>();
            auto obj = eval_expr(*me->object, env);
            if (obj->is_object())
                obj->as_object().fields[me->member] = val;
            break;
        }
        case NodeKind::IndexExpr: {
            auto* ie = target.as<const IndexExpr>();
            auto obj = eval_expr(*ie->object, env);
            auto idx = eval_expr(*ie->index, env);
            if (obj->is_array()) {
                auto& elems = obj->as_array().elements;
                size_t i = (size_t)idx->as_int();
                if (i < elems.size()) elems[i] = val;
            } else if (obj->is_str()) {
                // String subscript assignment not typically supported
            }
            break;
        }
        default:
            break;
    }
}

// ─── Call ────────────────────────────────────────────────────────

ValuePtr Interpreter::eval_call(const CallExpr& e, EnvPtr env) {
    // Check if callee is a type constructor (e.g., vector<int>())
    if (e.callee->kind == NodeKind::IdentifierExpr) {
        auto& name = e.callee->as<const IdentifierExpr>()->name;
        // Check for builtin type constructors
        if (name == "vector" || name == "string") {
            auto args = eval_args(e.args, env);
            return construct_object(name, std::move(args), env);
        }
    }

    // Handle template instantiation calls like vector<int>()
    if (e.callee->kind == NodeKind::TemplateInstType) {
        auto* ti = e.callee->as<const TemplateInstType>();
        std::string name = ti->qualified_name();
        if (name == "vector" || name == "std::vector")
            return make_vector_object();
    }

    auto callee = eval_expr(*e.callee, env);
    auto args = eval_args(e.args, env);

    if (callee->is_func()) {
        return call_func(callee->as_func(), std::move(args));
    }
    if (callee->is_native()) {
        return callee->as_native().fn(std::move(args));
    }

    throw RuntimeError("not callable");
}

// ─── call_func ───────────────────────────────────────────────────

ValuePtr Interpreter::call_func(const FuncVal& fn, std::vector<ValuePtr> args) {
    auto fn_env = fn.closure_env
                    ? fn.closure_env->make_child()
                    : global_env_->make_child();

    // Allow calling with fewer args (default to null)
    for (size_t i = 0; i < fn.params.size(); ++i) {
        fn_env->define(fn.params[i], i < args.size() ? args[i] : Value::make_null());
    }

    try {
        // body_ast may be FunctionDecl or LambdaExpr — check Node::kind
        const auto* node = static_cast<const Node*>(fn.body_ast);
        if (node->kind == NodeKind::LambdaExpr) {
            const auto* lambda = node->as<const LambdaExpr>();
            if (lambda->body) {
                if (lambda->body->kind == NodeKind::CompoundStmt)
                    exec_block(*lambda->body->as<CompoundStmt>(), fn_env);
                else
                    exec_stmt(*lambda->body, fn_env);
            }
        } else {
            const auto* decl = static_cast<const FunctionDecl*>(fn.body_ast);
            if (!decl->body) return Value::make_null();

        // Handle constructor initializer list
        for (auto& init : decl->init_list) {
            if (!init.args.empty()) {
                auto val = eval_expr(*init.args[0], fn_env);
                if (fn_env->has("this")) {
                    auto this_obj = fn_env->get("this");
                    if (this_obj->is_object()) {
                        this_obj->as_object().fields[init.name] = val;
                    }
                }
            }
        }

        if (decl->body->kind == NodeKind::CompoundStmt)
            exec_block(*decl->body->as<CompoundStmt>(), fn_env);
        else
            exec_stmt(*decl->body, fn_env);
        }

        return Value::make_null(); // void
    } catch (ReturnSignal& r) {
        return r.value;
    }
}

// ─── Member access ───────────────────────────────────────────────

ValuePtr Interpreter::eval_member(const MemberExpr& e, EnvPtr env) {
    auto obj = eval_expr(*e.object, env);

    // String methods
    if (obj->is_str()) {
        auto str = obj->as_str(); // get the string value (copy)
        // Return a native function that captures the string
        return Value::make_native(e.member,
            [this, str, member = e.member](std::vector<ValuePtr> args) -> ValuePtr {
                return call_string_method(str, member, std::move(args));
            });
    }

    // Array methods
    if (obj->is_array()) {
        auto& arr = obj->as_array();
        if (e.member == "size")
            return Value::make_native("size",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    return Value::make_int((int64_t)obj->as_array().elements.size());
                });
        if (e.member == "empty")
            return Value::make_native("empty",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    return Value::make_bool(obj->as_array().elements.empty());
                });
        if (e.member == "push_back")
            return Value::make_native("push_back",
                [obj](std::vector<ValuePtr> args) -> ValuePtr {
                    if (!args.empty()) obj->as_array().elements.push_back(args[0]);
                    return Value::make_null();
                });
        if (e.member == "pop_back")
            return Value::make_native("pop_back",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    if (!obj->as_array().elements.empty()) obj->as_array().elements.pop_back();
                    return Value::make_null();
                });
        if (e.member == "clear")
            return Value::make_native("clear",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    obj->as_array().elements.clear();
                    return Value::make_null();
                });
        if (e.member == "front")
            return Value::make_native("front",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    if (obj->as_array().elements.empty()) throw RuntimeError("front on empty vector");
                    return obj->as_array().elements.front();
                });
        if (e.member == "back")
            return Value::make_native("back",
                [obj](std::vector<ValuePtr>) -> ValuePtr {
                    if (obj->as_array().elements.empty()) throw RuntimeError("back on empty vector");
                    return obj->as_array().elements.back();
                });
        if (e.member == "at")
            return Value::make_native("at",
                [obj](std::vector<ValuePtr> args) -> ValuePtr {
                    if (args.empty()) throw RuntimeError("at requires index");
                    size_t idx = (size_t)args[0]->as_int();
                    if (idx >= obj->as_array().elements.size())
                        throw RuntimeError("vector::at index out of range");
                    return obj->as_array().elements[idx];
                });
    }

    // Object field/method access
    if (obj->is_object()) {
        auto& o = obj->as_object();

        // Check fields first
        auto fit = o.fields.find(e.member);
        if (fit != o.fields.end()) return fit->second;

        // Check method environment
        if (o.method_env && o.method_env->has(e.member)) {
            auto method = o.method_env->get(e.member);
            if (method->is_func()) {
                // Return a bound method (captures this)
                auto captured_obj = obj;
                return Value::make_native(e.member,
                    [this, captured_obj, method](std::vector<ValuePtr> args) mutable -> ValuePtr {
                        return call_method(captured_obj->as_object(), method->as_func().name, std::move(args));
                    });
            }
            return method;
        }
    }

    throw RuntimeError("no member '" + e.member + "' in value");
}

// ─── call_method ─────────────────────────────────────────────────

static void sync_method_fields(ObjectVal& obj, EnvPtr method_env) {
    for (auto& [fname, fval] : obj.fields) {
        if (method_env->has(fname))
            obj.fields[fname] = method_env->get(fname);
    }
    auto updated = method_env->get("this");
    if (updated->is_object()) {
        for (auto& [fname, fval] : updated->as_object().fields)
            obj.fields[fname] = fval;
    }
}

ValuePtr Interpreter::call_method(ObjectVal& obj, const std::string& method,
                                   std::vector<ValuePtr> args) {
    auto it = class_method_envs_.find(obj.class_name);
    if (it == class_method_envs_.end())
        throw RuntimeError("unknown class for method call: '" + obj.class_name + "'");

    auto cls_env = it->second;
    if (!cls_env->has(method))
        throw RuntimeError("no method '" + method + "' in class '" + obj.class_name + "'");

    auto fn_val = cls_env->get(method);
    if (!fn_val->is_func())
        throw RuntimeError("'" + method + "' is not a function");

    auto& fn = fn_val->as_func();
    auto method_env = cls_env->make_child();

    // Create a temporary object value for 'this'
    auto this_obj = std::make_shared<Value>();
    this_obj->inner = obj;
    method_env->define("this", this_obj);

    // Expose object fields as local variables (implicit this->field)
    for (auto& [fname, fval] : obj.fields)
        method_env->define(fname, fval);

    for (size_t i = 0; i < fn.params.size(); ++i)
        method_env->define(fn.params[i], i < args.size() ? args[i] : Value::make_null());

    try {
        const auto* decl = static_cast<const FunctionDecl*>(fn.body_ast);
        if (decl->body) {
            if (decl->body->kind == NodeKind::CompoundStmt)
                exec_block(*decl->body->as<CompoundStmt>(), method_env);
            else
                exec_stmt(*decl->body, method_env);
        }

        sync_method_fields(obj, method_env);
        return Value::make_null();
    } catch (ReturnSignal& r) {
        sync_method_fields(obj, method_env);
        return r.value;
    }
}

// ─── Index ───────────────────────────────────────────────────────

ValuePtr Interpreter::eval_index(const IndexExpr& e, EnvPtr env) {
    auto obj = eval_expr(*e.object, env);
    auto idx = eval_expr(*e.index, env);

    if (obj->is_array()) {
        auto& elems = obj->as_array().elements;
        size_t i = (size_t)idx->as_int();
        if (i >= elems.size()) throw RuntimeError("index out of range");
        return elems[i];
    }
    if (obj->is_str()) {
        size_t i = (size_t)idx->as_int();
        auto str = obj->as_str();
        if (i >= str.size()) throw RuntimeError("string index out of range");
        return Value::make_str(std::string(1, str[i]));
    }

    throw RuntimeError("indexing non-indexable value");
}

// ─── New ─────────────────────────────────────────────────────────

ValuePtr Interpreter::eval_new(const NewExpr& e, EnvPtr env) {
    std::string type_name = extract_type_name(e.type.get());
    auto args = eval_args(e.args, env);
    return construct_object(type_name, std::move(args), env);
}

// ─── Lambda ──────────────────────────────────────────────────────

ValuePtr Interpreter::eval_lambda(const LambdaExpr& e, EnvPtr env) {
    std::vector<std::string> param_names;
    for (auto& p : e.params) {
        if (p && p->kind == NodeKind::ParamDecl)
            param_names.push_back(p->as<ParamDecl>()->name);
    }

    // Create closure environment with captured variables
    auto closure = env->make_child();
    for (auto& cap : e.captures) {
        if (env->has(cap.name))
            closure->define(cap.name, env->get(cap.name));
    }

    // Also handle default capture modes
    if (e.capture_default == LambdaExpr::CaptureDefault::ByCopy ||
        e.capture_default == LambdaExpr::CaptureDefault::ByRef) {
        // Copy all variables from parent scope
        for (auto& [name, val] : env->vars) {
            if (!closure->vars.count(name))
                closure->define(name, val);
        }
    }

    return Value::make_func("<lambda>", std::move(param_names), &e, closure);
}

// ─── Ternary ─────────────────────────────────────────────────────

ValuePtr Interpreter::eval_ternary(const TernaryExpr& e, EnvPtr env) {
    if (eval_expr(*e.condition, env)->to_bool())
        return eval_expr(*e.then_expr, env);
    return eval_expr(*e.else_expr, env);
}

// ─── InitList ────────────────────────────────────────────────────

ValuePtr Interpreter::eval_init_list(const InitListExpr& e, EnvPtr env) {
    auto arr = Value::make_array();
    for (auto& elem : e.elements)
        arr->as_array().elements.push_back(eval_expr(*elem, env));
    return arr;
}

// ─── construct_object ────────────────────────────────────────────

ValuePtr Interpreter::construct_object(const std::string& cls_name,
                                        std::vector<ValuePtr> args,
                                        EnvPtr env) {
    // Builtin types
    if (cls_name == "vector" || cls_name == "std::vector")
        return make_vector_object();
    if (cls_name == "string" || cls_name == "std::string")
        return Value::make_str(args.empty() ? "" : args[0]->as_str());

    // User-defined class
    auto it = class_registry_.find(cls_name);
    if (it == class_registry_.end())
        throw RuntimeError("unknown class: '" + cls_name + "'");

    const ClassDecl* cls = it->second;
    auto cls_env = class_method_envs_[cls_name];
    auto obj = Value::make_object(cls_name, cls, cls_env);

    // Initialize fields from base classes
    for (auto& base : cls->bases) {
        auto base_it = class_registry_.find(base.name);
        if (base_it != class_registry_.end()) {
            for (auto& m : base_it->second->members) {
                if (!m) continue;
                if (m->kind == NodeKind::FieldDecl) {
                    auto* field = m->as<FieldDecl>();
                    auto fenv = cls_env->make_child();
                    fenv->define("this", obj);
                    obj->as_object().fields[field->name] = field->init
                        ? eval_expr(*field->init, fenv)
                        : default_value(extract_type_name(field->type.get()));
                }
            }
        }
    }

    // Initialize own fields
    for (auto& m : cls->members) {
        if (!m) continue;
        if (m->kind == NodeKind::FieldDecl) {
            auto* field = m->as<FieldDecl>();
            auto fenv = cls_env->make_child();
            fenv->define("this", obj);
            obj->as_object().fields[field->name] = field->init
                ? eval_expr(*field->init, fenv)
                : default_value(extract_type_name(field->type.get()));
        } else if (m->kind == NodeKind::VarDecl) {
            auto* var = m->as<VarDecl>();
            auto fenv = cls_env->make_child();
            fenv->define("this", obj);
            obj->as_object().fields[var->name] = var->init
                ? eval_expr(*var->init, fenv)
                : default_value(extract_type_name(var->type.get()));
        }
    }

    // Call constructor if exists
    if (cls_env->has(cls_name)) {
        auto ctor = cls_env->get(cls_name);
        if (ctor->is_func()) {
            auto ctor_env = cls_env->make_child();
            ctor_env->define("this", obj);

            auto& fn = ctor->as_func();
            for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i)
                ctor_env->define(fn.params[i], args[i]);
            try {
                const auto* d = static_cast<const FunctionDecl*>(fn.body_ast);

                // Handle constructor initializer list before exposing fields
                for (auto& init : d->init_list) {
                    if (!init.args.empty()) {
                        auto val = eval_expr(*init.args[0], ctor_env);
                        obj->as_object().fields[init.name] = val;
                    }
                }

                // Expose object fields as local variables (implicit this->field)
                // Done after init list so sync_method_fields won't overwrite init list values
                for (auto& [fname, fval] : obj->as_object().fields)
                    ctor_env->define(fname, fval);

                if (d->body) {
                    if (d->body->kind == NodeKind::CompoundStmt)
                        exec_block(*d->body->as<CompoundStmt>(), ctor_env);
                    else
                        exec_stmt(*d->body, ctor_env);
                }

                sync_method_fields(obj->as_object(), ctor_env);
            } catch (ReturnSignal&) {
                sync_method_fields(obj->as_object(), ctor_env);
            }
        }
    }

    return obj;
}

// ─── REPL ────────────────────────────────────────────────────────

std::string Interpreter::run_repl_line(const std::string& line) {
    try {
        auto result = run_string(line, "<repl>");
        if (result && !result->is_null())
            return result->to_string();
    } catch (const RuntimeError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
    return "";
}

void Interpreter::run_repl() {
    std::cout << "REX -- interactive interpreter  (Ctrl+D or 'exit' to quit)\n";

    std::string pending;
    int brace_depth = 0;

    while (true) {
        std::cout << (pending.empty() ? ">>> " : "... ") << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break; // Ctrl+D

        if (pending.empty() && (line == "exit" || line == "quit")) break;

        pending += line + "\n";
        for (char c : line) {
            if (c == '{') ++brace_depth;
            if (c == '}') --brace_depth;
        }

        if (brace_depth > 0) continue;

        try {
            auto result = run_string(pending, "<repl>");
            if (result && !result->is_null())
                std::cout << result->to_string() << "\n";
        } catch (const RuntimeError& e) {
            std::cerr << "error: " << e.what();
            if (e.line > 0) std::cerr << " (line " << e.line << ")";
            std::cerr << "\n";
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
        }

        pending.clear();
        brace_depth = 0;
    }
    std::cout << "\n";
}

} // namespace rex::interp
