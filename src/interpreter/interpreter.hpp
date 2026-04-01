#pragma once
/*
 * interpreter.hpp  –  REX Tree-Walking Interpreter
 *
 * Reuses the existing Lexer, Parser, and Semantic Analyzer from rexc.
 * Walks the AST and evaluates each node recursively.
 */

#include "value.hpp"
#include "environment.hpp"
#include "builtins.hpp"
#include "../rexc/ast.hpp"
#include "../rexc/lexer.hpp"
#include "../rexc/parser.hpp"
#include "../rexc/semantic.hpp"
#include <string>
#include <fstream>
#include <iostream>

namespace rex::interp {

class Interpreter {
public:
    Interpreter();

    // Execute a .cpp file without compiling — returns exit code
    int run_file(const std::string& filepath);

    // Execute a source string (used in tests and REPL)
    ValuePtr run_string(const std::string& source,
                        const std::string& filename = "<string>");

    // Execute a single REPL line (preserves state between calls)
    std::string run_repl_line(const std::string& line);

    // Start interactive REPL (full loop)
    void run_repl();

    void set_verbose(bool v) { verbose_ = v; }

private:
    // Declarations
    void exec_decls(const std::vector<rexc::NodePtr>& decls, EnvPtr env);
    void exec_function_decl(const rexc::FunctionDecl& fn, EnvPtr env);
    void exec_class_decl(const rexc::ClassDecl& cls, EnvPtr env);
    void exec_var_decl(const rexc::VarDecl& var, EnvPtr env);
    void exec_namespace(const rexc::NamespaceDecl& ns, EnvPtr env);

    // Statements
    void exec_stmt(const rexc::Node& stmt, EnvPtr env);
    void exec_block(const rexc::CompoundStmt& blk, EnvPtr env);
    void exec_if(const rexc::IfStmt& s, EnvPtr env);
    void exec_while(const rexc::WhileStmt& s, EnvPtr env);
    void exec_for(const rexc::ForStmt& s, EnvPtr env);
    void exec_for_range(const rexc::RangeForStmt& s, EnvPtr env);
    void exec_return(const rexc::ReturnStmt& s, EnvPtr env);
    void exec_try_catch(const rexc::TryStmt& s, EnvPtr env);
    void exec_throw(const rexc::ThrowStmt& s, EnvPtr env);

    // Expressions
    ValuePtr eval_expr(const rexc::Node& e, EnvPtr env);
    ValuePtr eval_binary(const rexc::BinaryExpr& e, EnvPtr env);
    ValuePtr eval_unary(const rexc::UnaryExpr& e, EnvPtr env);
    ValuePtr eval_assign(const rexc::AssignExpr& e, EnvPtr env);
    ValuePtr eval_call(const rexc::CallExpr& e, EnvPtr env);
    ValuePtr eval_member(const rexc::MemberExpr& e, EnvPtr env);
    ValuePtr eval_index(const rexc::IndexExpr& e, EnvPtr env);
    ValuePtr eval_new(const rexc::NewExpr& e, EnvPtr env);
    ValuePtr eval_lambda(const rexc::LambdaExpr& e, EnvPtr env);
    ValuePtr eval_ternary(const rexc::TernaryExpr& e, EnvPtr env);
    ValuePtr eval_init_list(const rexc::InitListExpr& e, EnvPtr env);

    // Calls
    ValuePtr call_func(const FuncVal& fn, std::vector<ValuePtr> args);
    ValuePtr call_method(ObjectVal& obj, const std::string& method,
                         std::vector<ValuePtr> args);
    ValuePtr construct_object(const std::string& cls_name,
                               std::vector<ValuePtr> args, EnvPtr env);

    // Utilities
    std::vector<ValuePtr> eval_args(const std::vector<rexc::NodePtr>& exprs, EnvPtr env);
    void     assign_lvalue(const rexc::Node& target, ValuePtr val, EnvPtr env);
    bool     is_stdlib_include(const std::string& header);
    ValuePtr default_value(const std::string& type_name);
    ValuePtr try_parse_number(const std::string& s);
    std::string extract_type_name(const rexc::Node* type_node);

    // State
    EnvPtr  global_env_;
    bool    verbose_ = false;
    std::unordered_map<std::string, const rexc::ClassDecl*> class_registry_;
    std::unordered_map<std::string, EnvPtr>                 class_method_envs_;
    std::string repl_source_acc_;
};

} // namespace rex::interp
