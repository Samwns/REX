# REX Interpreter — Instruções completas



---

## CONTEXTO E OBJETIVO

O REX já tem um compilador completo funcionando.
O objetivo agora é adicionar um **interpretador** ao lado dele.

A separação de comandos é simples:

| Comando               | O que faz                                              |
|-----------------------|--------------------------------------------------------|
| `rex run file.cpp`    | **Interpreta** — executa sem compilar, como Python     |
| `rex brun file.cpp`   | **Build + Run** — compila para binário nativo e executa|
| `rex build file.cpp`  | **Build** — só compila, gera o binário (já existe)     |
| `rex repl`            | **REPL** — prompt interativo do interpretador          |

**Não mudar nada no compilador existente.**
Apenas adicionar o interpretador como um módulo novo em `src/interpreter/`.

---

## ARQUITETURA

```
Código-fonte C++
      │
      ├──────────────────────────────────────────────────────┐
      │  rex run (interpreta)                                │  rex brun / rex build (compila)
      ▼                                                      ▼
┌─────────────┐                                     ┌─────────────┐
│    Lexer    │  (reutilizar o existente)            │    Lexer    │
└──────┬──────┘                                     └──────┬──────┘
       ▼                                                   ▼
┌─────────────┐                                     ┌─────────────┐
│   Parser    │  (reutilizar o existente)            │   Parser    │
└──────┬──────┘                                     └──────┬──────┘
       ▼                                                   ▼
┌─────────────┐                                     ┌─────────────┐
│  Semantic   │  (reutilizar o existente)            │  Semantic   │
└──────┬──────┘                                     └──────┬──────┘
       ▼                                                   ▼
┌──────────────────┐                          ┌─────────────────────┐
│  Tree-Walking    │                          │  Native Code Gen    │
│  Interpreter     │  <- NOVO                 │  (já existe)        │
│                  │                          └──────────┬──────────┘
│  Executa a AST   │                                     ▼
│  diretamente     │                          ┌─────────────────────┐
│  sem gerar       │                          │  ELF / PE / Mach-O  │
│  nenhum arquivo  │                          │  Writer (já existe) │
└──────┬───────────┘                          └──────────┬──────────┘
       ▼                                                 ▼
  Saída na tela                                 Binário nativo
  (zero compilação)                              (sem deps externas)
```

O interpretador é um **tree-walking interpreter**: percorre a AST e avalia
cada nó recursivamente. Mesma abordagem do Python original, Ruby 1.x, PHP original.

---

## ESTRUTURA DE ARQUIVOS

Criar tudo dentro de `src/interpreter/`:

```
src/
└── interpreter/
    ├── value.hpp              <- Tipo universal de valor em runtime
    ├── environment.hpp        <- Escopos de variáveis + sinais de controle
    ├── builtins.hpp           <- cout, cin, vector, string, math, etc.
    ├── builtins.cpp           <- Implementação dos builtins
    ├── interpreter.hpp        <- Interpretador principal
    ├── interpreter.cpp        <- Implementação
    ├── repl.hpp               <- REPL interativo
    ├── repl.cpp               <- Implementação do REPL
    └── test_interpreter.cpp   <- Testes
```

---

## FASE 1 — Sistema de Valores (value.hpp)

Tipo que representa qualquer valor que o programa pode produzir em runtime.

```cpp
#pragma once
#include <variant>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace rex::interp {

struct Value;
struct Environment;

using ValuePtr = std::shared_ptr<Value>;
using EnvPtr   = std::shared_ptr<Environment>;

struct NullVal  {};
struct BoolVal  { bool v; };
struct IntVal   { int64_t v; };
struct FloatVal { double v; };
struct StrVal   { std::string v; };

// Funcao definida pelo usuario (guarda ponteiro para o AST + closure)
struct FuncVal {
    std::string              name;
    std::vector<std::string> params;
    const void*              body_ast;    // ponteiro para FunctionDecl na AST
    EnvPtr                   closure_env;
};

// Funcao nativa (builtin implementada em C++)
struct NativeFunc {
    std::string name;
    std::function<ValuePtr(std::vector<ValuePtr>)> fn;
};

// Instancia de classe
struct ObjectVal {
    std::string                               class_name;
    std::unordered_map<std::string, ValuePtr> fields;
    const void*                               class_ast;
    EnvPtr                                    method_env;
};

// std::vector / array
struct ArrayVal {
    std::vector<ValuePtr> elements;
};

struct Value {
    using Inner = std::variant<
        NullVal, BoolVal, IntVal, FloatVal, StrVal,
        FuncVal, NativeFunc, ObjectVal, ArrayVal
    >;
    Inner inner;

    // Factories
    static ValuePtr make_null();
    static ValuePtr make_bool(bool v);
    static ValuePtr make_int(int64_t v);
    static ValuePtr make_float(double v);
    static ValuePtr make_str(const std::string& v);
    static ValuePtr make_func(const std::string& name,
                               std::vector<std::string> params,
                               const void* body_ast, EnvPtr closure);
    static ValuePtr make_native(const std::string& name,
                                 std::function<ValuePtr(std::vector<ValuePtr>)> fn);
    static ValuePtr make_object(const std::string& cls,
                                 const void* cls_ast, EnvPtr method_env);
    static ValuePtr make_array();

    // Consulta de tipo
    bool is_null()    const;
    bool is_bool()    const;
    bool is_int()     const;
    bool is_float()   const;
    bool is_str()     const;
    bool is_func()    const;
    bool is_native()  const;
    bool is_object()  const;
    bool is_array()   const;
    bool is_numeric() const;

    // Getters (lancam RuntimeError se tipo errado)
    bool        as_bool()   const;
    int64_t     as_int()    const;
    double      as_float()  const;
    std::string as_str()    const;
    FuncVal&    as_func();
    NativeFunc& as_native();
    ObjectVal&  as_object();
    ArrayVal&   as_array();

    // Conversao para string (para cout <<)
    std::string to_string() const;

    // Truthiness: null=false, 0=false, ""=false, resto=true
    bool to_bool() const;

    // Operadores aritmeticos
    // int+int=int, float+float=float, str+str=concatenacao, str*int=repeticao
    ValuePtr op_add(const Value& o) const;
    ValuePtr op_sub(const Value& o) const;
    ValuePtr op_mul(const Value& o) const;
    ValuePtr op_div(const Value& o) const;
    ValuePtr op_mod(const Value& o) const;
    ValuePtr op_neg()               const;
    ValuePtr op_eq (const Value& o) const;
    ValuePtr op_ne (const Value& o) const;
    ValuePtr op_lt (const Value& o) const;
    ValuePtr op_le (const Value& o) const;
    ValuePtr op_gt (const Value& o) const;
    ValuePtr op_ge (const Value& o) const;
    ValuePtr op_not()               const;
};

} // namespace rex::interp
```

---

## FASE 2 — Escopos (environment.hpp)

```cpp
#pragma once
#include "value.hpp"
#include <stdexcept>

namespace rex::interp {

// Escopo de variaveis encadeado (filho -> pai -> global)
struct Environment : std::enable_shared_from_this<Environment> {
    std::unordered_map<std::string, ValuePtr> vars;
    EnvPtr parent;

    explicit Environment(EnvPtr parent = nullptr) : parent(std::move(parent)) {}

    EnvPtr make_child() {
        return std::make_shared<Environment>(shared_from_this());
    }

    // Declarar variavel no escopo atual
    void define(const std::string& name, ValuePtr value) {
        vars[name] = std::move(value);
    }

    // Buscar variavel (sobe a cadeia, lanca RuntimeError se nao achar)
    ValuePtr get(const std::string& name) const {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        if (parent) return parent->get(name);
        throw RuntimeError("undefined variable: '" + name + "'");
    }

    bool has(const std::string& name) const {
        if (vars.count(name)) return true;
        if (parent) return parent->has(name);
        return false;
    }

    // Atribuir a variavel existente (sobe a cadeia)
    void assign(const std::string& name, ValuePtr value) {
        auto it = vars.find(name);
        if (it != vars.end()) { it->second = std::move(value); return; }
        if (parent) { parent->assign(name, std::move(value)); return; }
        throw RuntimeError("assignment to undefined variable: '" + name + "'");
    }
};

// Excecoes internas de controle de fluxo (nao sao erros de verdade)
struct RuntimeError : std::runtime_error {
    int line = 0, col = 0;
    explicit RuntimeError(const std::string& msg, int line=0, int col=0)
        : std::runtime_error(msg), line(line), col(col) {}
};
struct ReturnSignal   { ValuePtr value; };
struct BreakSignal    {};
struct ContinueSignal {};
struct ThrowSignal    { ValuePtr value; std::string type_name; };

} // namespace rex::interp
```

---

## FASE 3 — Builtins (builtins.hpp / builtins.cpp)

```cpp
#pragma once
#include "value.hpp"
#include "environment.hpp"

namespace rex::interp {

// Registrar TODOS os builtins no env global
void register_builtins(EnvPtr env);

// cout, cerr -> objeto __ostream__
// cin        -> objeto __istream__
// endl       -> objeto __endl__
ValuePtr make_cout_object();
ValuePtr make_cerr_object();
ValuePtr make_cin_object();
ValuePtr make_endl_value();

// vector<T> -> ArrayVal com metodos nativos:
// push_back, pop_back, size, empty, clear, at, front, back, []
ValuePtr make_vector_object();

// Metodos de string acessados via s.method()
// size, length, empty, find, substr, append, push_back, at, c_str
ValuePtr call_string_method(const std::string& str,
                             const std::string& method,
                             std::vector<ValuePtr> args);

// Math: sqrt, pow, abs, floor, ceil, round, sin, cos, tan, log, exp
// Constantes: M_PI, M_E
void register_math_builtins(EnvPtr env);

// Utilitarios: to_string, stoi, stof, rand, srand, assert, exit
void register_utility_builtins(EnvPtr env);

// Arquivos: ifstream, ofstream
ValuePtr make_ifstream_object(const std::string& path);
ValuePtr make_ofstream_object(const std::string& path);

} // namespace rex::interp
```

**Implementacao do operador << no eval_binary:**

```cpp
// Dentro de eval_binary, antes de avaliar os dois lados:
if (e.op == "<<") {
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
        return left; // retorna cout para permitir chaining: cout << a << b
    }
}

// Operador >> para cin:
if (e.op == ">>") {
    auto left = eval_expr(*e.left, env);
    if (left->is_object() && left->as_object().class_name == "__istream__") {
        std::string token;
        std::cin >> token;
        // Tentar parsear como numero, senao manter como string
        ValuePtr val = try_parse_number(token);
        assign_lvalue(*e.right, val, env);
        return left; // retorna cin para chaining
    }
}
```

---

## FASE 4 — Interpretador Principal (interpreter.hpp / interpreter.cpp)

```cpp
#pragma once
#include "value.hpp"
#include "environment.hpp"
#include "builtins.hpp"
#include "../rexc/rexc.hpp"
#include <string>

namespace rex::interp {

class Interpreter {
public:
    Interpreter();

    // Executar arquivo .cpp sem compilar -- retorna exit code
    int run_file(const std::string& filepath);

    // Executar string de codigo (usado nos testes e no REPL)
    ValuePtr run_string(const std::string& source,
                        const std::string& filename = "<string>");

    // Executar uma linha no REPL (preserva estado entre chamadas)
    std::string run_repl_line(const std::string& line);

    // Iniciar REPL interativo (loop completo)
    void run_repl();

    void set_verbose(bool v) { verbose_ = v; }

private:
    // Declarations
    void exec_decls(const std::vector<DeclPtr>& decls, EnvPtr env);
    void exec_function_decl(const FunctionDecl& fn, EnvPtr env);
    void exec_class_decl(const ClassDecl& cls, EnvPtr env);
    void exec_var_decl(const VarDecl& var, EnvPtr env);
    void exec_namespace(const NamespaceDecl& ns, EnvPtr env);

    // Statements
    void exec_stmt(const Stmt& stmt, EnvPtr env);
    void exec_block(const BlockStmt& blk, EnvPtr env);
    void exec_if(const IfStmt& s, EnvPtr env);
    void exec_while(const WhileStmt& s, EnvPtr env);
    void exec_for(const ForStmt& s, EnvPtr env);
    void exec_for_range(const ForRangeStmt& s, EnvPtr env);
    void exec_return(const ReturnStmt& s, EnvPtr env);
    void exec_try_catch(const TryCatchStmt& s, EnvPtr env);
    void exec_throw(const ThrowStmt& s, EnvPtr env);

    // Expressions
    ValuePtr eval_expr(const Expr& e, EnvPtr env);
    ValuePtr eval_binary(const BinaryExpr& e, EnvPtr env);
    ValuePtr eval_unary(const UnaryExpr& e, EnvPtr env);
    ValuePtr eval_assign(const AssignExpr& e, EnvPtr env);
    ValuePtr eval_call(const CallExpr& e, EnvPtr env);
    ValuePtr eval_member(const MemberAccessExpr& e, EnvPtr env);
    ValuePtr eval_index(const IndexExpr& e, EnvPtr env);
    ValuePtr eval_new(const NewExpr& e, EnvPtr env);
    ValuePtr eval_lambda(const LambdaExpr& e, EnvPtr env);
    ValuePtr eval_ternary(const TernaryExpr& e, EnvPtr env);
    ValuePtr eval_init_list(const InitListExpr& e, EnvPtr env);

    // Chamadas
    ValuePtr call_func(const FuncVal& fn, std::vector<ValuePtr> args);
    ValuePtr call_method(ObjectVal& obj, const std::string& method,
                         std::vector<ValuePtr> args);
    ValuePtr construct_object(const std::string& cls_name,
                               std::vector<ValuePtr> args, EnvPtr env);

    // Utilitarios
    std::vector<ValuePtr> eval_args(const std::vector<ExprPtr>& exprs, EnvPtr env);
    void     assign_lvalue(const Expr& target, ValuePtr val, EnvPtr env);
    bool     is_stdlib_include(const std::string& header);
    ValuePtr default_value(const std::string& type_name);
    ValuePtr try_parse_number(const std::string& s);

    // Estado
    EnvPtr  global_env_;
    bool    verbose_ = false;
    std::unordered_map<std::string, const ClassDecl*> class_registry_;
    std::unordered_map<std::string, EnvPtr>           class_method_envs_;
    std::string repl_source_acc_; // estado acumulado do REPL
};

} // namespace rex::interp
```

**run_file:**

```cpp
int Interpreter::run_file(const std::string& filepath) {
    // Ler arquivo
    std::ifstream f(filepath);
    if (!f) throw RuntimeError("cannot open: " + filepath);
    std::string source((std::istreambuf_iterator<char>(f)), {});

    // Suporte a shebang (#!/usr/bin/env rex)
    if (source.size() >= 2 && source[0] == '#' && source[1] == '!')
        source = source.substr(source.find('\n') + 1);

    // Lexer + Parser + Semantic (reutilizar pipeline existente do rexc)
    Lexer  lexer(source, filepath);
    Parser parser(lexer.tokenize());
    auto   ast = parser.parse();
    SemanticAnalyzer sem;
    sem.analyze(ast);

    // Registrar builtins
    register_builtins(global_env_);

    // Registrar funcoes e classes (sem executar ainda)
    exec_decls(ast, global_env_);

    // Se existe main(), chamar ela
    if (global_env_->has("main")) {
        auto main_fn = global_env_->get("main");
        try {
            auto result = call_func(main_fn->as_func(), {});
            return result->is_int() ? (int)result->as_int() : 0;
        } catch (ReturnSignal& r) {
            return r.value->is_int() ? (int)r.value->as_int() : 0;
        }
    }
    // Se nao tem main(), ja foi executado tudo no top-level (modo script)
    return 0;
}
```

**call_func (closures funcionam corretamente):**

```cpp
ValuePtr Interpreter::call_func(const FuncVal& fn, std::vector<ValuePtr> args) {
    // Escopo a partir do closure, nao do call site
    auto fn_env = fn.closure_env
                    ? fn.closure_env->make_child()
                    : global_env_->make_child();

    if (args.size() != fn.params.size())
        throw RuntimeError("'" + fn.name + "' expects " +
                           std::to_string(fn.params.size()) + " args, got " +
                           std::to_string(args.size()));

    for (size_t i = 0; i < fn.params.size(); ++i)
        fn_env->define(fn.params[i], args[i]);

    try {
        const auto* decl = static_cast<const FunctionDecl*>(fn.body_ast);
        exec_block(*decl->body, fn_env);
        return Value::make_null(); // void
    } catch (ReturnSignal& r) {
        return r.value;
    }
}
```

**construct_object:**

```cpp
ValuePtr Interpreter::construct_object(const std::string& cls_name,
                                        std::vector<ValuePtr> args,
                                        EnvPtr env) {
    // Tipos builtin
    if (cls_name == "vector" || cls_name == "std::vector")
        return make_vector_object();
    if (cls_name == "string" || cls_name == "std::string")
        return Value::make_str(args.empty() ? "" : args[0]->as_str());

    // Classe definida pelo usuario
    auto it = class_registry_.find(cls_name);
    if (it == class_registry_.end())
        throw RuntimeError("unknown class: '" + cls_name + "'");

    const ClassDecl* cls     = it->second;
    auto             cls_env = class_method_envs_[cls_name];
    auto             obj     = Value::make_object(cls_name, cls, cls_env);

    // Inicializar campos
    for (const auto& field : cls->fields) {
        auto fenv = cls_env->make_child();
        fenv->define("this", obj);
        obj->as_object().fields[field.name] = field.initializer
            ? eval_expr(*field.initializer, fenv)
            : default_value(field.type);
    }

    // Chamar construtor se existir
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
                exec_block(*d->body, ctor_env);
            } catch (ReturnSignal&) {}
        }
    }
    return obj;
}
```

---

## FASE 5 — REPL (repl.cpp)

```
$ rex repl
REX 1.0 -- interpretador interativo  (Ctrl+D ou 'exit' para sair)
>>> int x = 10;
>>> cout << x * 2 << endl;
20
>>> auto f = [](int n){ return n*n; };
>>> cout << f(7) << endl;
49
>>> class Dog {
...     string name;
...     Dog(string n) : name(n) {}
...     void bark() { cout << name << ": Au!" << endl; }
... };
>>> Dog d("Rex");
>>> d.bark();
Rex: Au!
>>> exit
```

```cpp
void Interpreter::run_repl() {
    std::cout << "REX " << REX_VERSION
              << " -- interpretador interativo  (Ctrl+D ou 'exit' para sair)\n";

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

        // Aguardar bloco multi-linha completar
        if (brace_depth > 0) continue;

        try {
            auto result = run_string(pending, "<repl>");
            // Expressao simples sem ; -> imprimir resultado
            if (result && !result->is_null())
                std::cout << result->to_string() << "\n";
        } catch (const RuntimeError& e) {
            std::cerr << "erro: " << e.what();
            if (e.line > 0) std::cerr << " (linha " << e.line << ")";
            std::cerr << "\n";
        } catch (const std::exception& e) {
            std::cerr << "erro: " << e.what() << "\n";
        }

        pending.clear();
        brace_depth = 0;
    }
    std::cout << "\n";
}
```

---

## FASE 6 — Integracao no src/main.cpp

**Adicionar apenas estes 2 casos** ao switch de comandos existente.
Nao mudar nada em `build` e `brun`:

```cpp
#include "interpreter/interpreter.hpp"

// rex run file.cpp  ->  INTERPRETAR (sem compilar)
if (cmd == "run") {
    if (args.empty()) { std::cerr << "uso: rex run <arquivo.cpp>\n"; return 1; }
    rex::interp::Interpreter interp;
    if (has_flag(flags, "--verbose")) interp.set_verbose(true);
    return interp.run_file(args[0]);
}

// rex repl  ->  REPL interativo
if (cmd == "repl") {
    rex::interp::Interpreter interp;
    interp.run_repl();
    return 0;
}

// brun e build continuam EXATAMENTE como estao -- nao tocar
```

**Atualizar o texto do `rex help`:**

```
Uso:
  rex run   <arquivo.cpp>   Interpreta diretamente, sem compilar
  rex brun  <arquivo.cpp>   Compila e executa o binario nativo
  rex build <arquivo.cpp>   Compila e gera o binario
  rex repl                  Abre o interpretador interativo
```

---

## FASE 7 — Testes (src/interpreter/test_interpreter.cpp)

```cpp
#include "interpreter.hpp"
#include <cassert>
#include <sstream>

using namespace rex::interp;

// Helper: redireciona stdout e verifica saida
void assert_output(const std::string& code, const std::string& expected,
                   const std::string& name) {
    Interpreter interp;
    std::ostringstream buf;
    auto* old = std::cout.rdbuf(buf.rdbuf());
    interp.run_string(code);
    std::cout.rdbuf(old);
    if (buf.str() != expected) {
        std::cerr << "FALHOU: " << name << "\n"
                  << "  esperado: [" << expected << "]\n"
                  << "  obtido:   [" << buf.str() << "]\n";
        assert(false);
    }
    std::cout << "OK: " << name << "\n";
}

void assert_throws(const std::string& code, const std::string& msg_contains) {
    Interpreter interp;
    try {
        interp.run_string(code);
        assert(false && "deveria ter lancado RuntimeError");
    } catch (const RuntimeError& e) {
        assert(std::string(e.what()).find(msg_contains) != std::string::npos);
        std::cout << "OK (erro esperado): " << msg_contains << "\n";
    }
}

int main() {
    // 1. Aritmetica
    assert_output("cout << 2+3*4 << endl;",  "14\n",  "aritmetica");
    assert_output("cout << (2+3)*4 << endl;", "20\n",  "parenteses");
    assert_output("cout << 10/3 << endl;",    "3\n",   "divisao inteira");

    // 2. Variaveis
    assert_output("int x=42; cout<<x<<endl;",           "42\n", "var int");
    assert_output("string s=\"oi\"; cout<<s<<endl;",    "oi\n", "var string");

    // 3. if/else
    assert_output("if(5>3) cout<<\"sim\"<<endl;",                         "sim\n", "if true");
    assert_output("if(1>2) cout<<\"x\"<<endl; else cout<<\"ok\"<<endl;", "ok\n",  "if else");

    // 4. while
    assert_output("int i=0; while(i<3){cout<<i<<endl;i++;}", "0\n1\n2\n", "while");

    // 5. for
    assert_output("for(int i=0;i<3;i++) cout<<i<<endl;", "0\n1\n2\n", "for");

    // 6. Funcao
    assert_output(R"(
        int dobro(int n) { return n*2; }
        cout << dobro(7) << endl;
    )", "14\n", "funcao");

    // 7. Recursao
    assert_output(R"(
        int fib(int n){ if(n<=1) return n; return fib(n-1)+fib(n-2); }
        cout << fib(10) << endl;
    )", "55\n", "fibonacci");

    // 8. Classe basica
    assert_output(R"(
        class Ponto { public:
            int x, y;
            Ponto(int a, int b) : x(a), y(b) {}
            int soma() { return x+y; }
        };
        Ponto p(3,4); cout << p.soma() << endl;
    )", "7\n", "classe");

    // 9. Heranca
    assert_output(R"(
        class Animal { public: string fala(){ return "..."; } };
        class Gato : public Animal { public: string fala(){ return "Miau!"; } };
        Gato g; cout << g.fala() << endl;
    )", "Miau!\n", "heranca");

    // 10. vector
    assert_output(R"(
        vector<int> v;
        v.push_back(10); v.push_back(20); v.push_back(30);
        cout << v.size() << endl;
        for(int i=0;i<v.size();i++) cout<<v[i]<<endl;
    )", "3\n10\n20\n30\n", "vector");

    // 11. Lambda
    assert_output(R"(
        auto mult = [](int a, int b){ return a*b; };
        cout << mult(6,7) << endl;
    )", "42\n", "lambda");

    // 12. Lambda com capture
    assert_output(R"(
        int base=100;
        auto add=[base](int n){ return base+n; };
        cout << add(5) << endl;
    )", "105\n", "lambda capture");

    // 13. try/catch
    assert_output(R"(
        try { throw 42; }
        catch(int e){ cout << "capturado: " << e << endl; }
    )", "capturado: 42\n", "try catch");

    // 14. range-for
    assert_output(R"(
        vector<int> v={1,2,3,4,5};
        int soma=0; for(auto x:v) soma+=x;
        cout << soma << endl;
    )", "15\n", "range-for");

    // 15. main() chamado automaticamente
    assert_output(R"(
        int main(){ cout << "Hello!" << endl; return 0; }
    )", "Hello!\n", "main auto");

    // 16. Divisao por zero
    assert_throws("cout << 1/0 << endl;", "division by zero");

    // 17. Variavel indefinida
    assert_throws("cout << xyz << endl;", "undefined variable");

    // 18. Concatenacao de strings
    assert_output(R"(
        string a="Ola"; string b=" Mundo";
        cout << a+b << endl;
    )", "Ola Mundo\n", "string concat");

    // 19. FizzBuzz
    assert_output(R"(
        for(int i=1;i<=5;i++){
            if(i%15==0) cout<<"FizzBuzz"<<endl;
            else if(i%3==0) cout<<"Fizz"<<endl;
            else if(i%5==0) cout<<"Buzz"<<endl;
            else cout<<i<<endl;
        }
    )", "1\n2\nFizz\n4\nBuzz\n", "FizzBuzz");

    // 20. Script sem main() (estilo Python)
    assert_output(R"(
        int x=10; int y=32;
        cout << x+y << endl;
    )", "42\n", "script sem main");

    std::cout << "\nTodos os testes passaram!\n";
    return 0;
}
```

---

## COMANDOS DE BUILD

```bash
# Compilar o REX com o interpretador incluido
g++ src/main.cpp src/interpreter/interpreter.cpp src/interpreter/builtins.cpp \
    -I src -o build/rex -std=c++20 -O2 -Wall

# Rodar testes do interpretador
g++ src/interpreter/test_interpreter.cpp \
    src/interpreter/interpreter.cpp \
    src/interpreter/builtins.cpp \
    -I src -o build/test_interp -std=c++20 -O2 && ./build/test_interp

# Testar rex run
echo 'int main(){ cout << "Hello!" << endl; }' > /tmp/t.cpp
./build/rex run /tmp/t.cpp

# Testar REPL
./build/rex repl
```

---

## ORDEM DE IMPLEMENTACAO

Execute nesta ordem, rodando os testes apos cada etapa:

1. value.hpp com todos os tipos e operadores
2. environment.hpp com todos os sinais de controle
3. builtins.cpp -- comecar por cout/cerr/endl, depois vector e math
4. interpreter.cpp -- comecar por literais, variaveis, aritmetica
5. Adicionar if / while / for / break / continue
6. Adicionar funcoes e return
7. Adicionar classes, metodos, new, this
8. Adicionar lambda com e sem capture
9. Adicionar try/catch/throw
10. run_repl() no interpreter.cpp
11. Integrar no src/main.cpp (apenas os 2 casos: run e repl, + atualizar help)
12. Rodar todos os 20 testes

**Regra absoluta:** os testes do compilador existentes (testes 1-30) devem
continuar passando apos cada etapa. O interpretador e um modulo paralelo --
nao toca no compilador.
