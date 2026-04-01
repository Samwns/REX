// Minimal smoke-test that all rexc headers compile together
#include "rexc.hpp"
#include "../cross_compile.hpp"
#include "../i18n.hpp"
#include "../signal_handler.hpp"
#include "runtime/rexc_alloc.hpp"
#include "runtime/rexc_string.hpp"
#include "runtime/rexc_containers.hpp"
#include "runtime/rexc_io.hpp"
#include "runtime/rexc_except.hpp"
#include "runtime/rexc_startup.hpp"
#include "stdlib/rexc_algorithm.hpp"
#include "stdlib/rexc_memory.hpp"
#include "stdlib/rexc_optional.hpp"
#include "stdlib/rexc_cstring.hpp"
#include "stdlib/rexc_cstdlib.hpp"
#include "stdlib/rexc_functional.hpp"
#include "object_layout.hpp"
#include "mangler.hpp"
#include "regalloc.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <set>

// ---------------------------------------------------------------------------
// Helper functions for setjmp/longjmp exception tests.
//
// On Windows x64 (MinGW-w64), longjmp triggers SEH-based stack unwinding via
// RtlUnwindEx.  The unwinder needs correct .pdata/.xdata metadata for every
// function on the call stack.  In a very large main() (~2500 lines), the
// compiler may generate unreliable unwind tables.  Isolating each
// setjmp/longjmp test in its own small, noinline function guarantees that the
// compiler produces a self-contained, correct unwind scope for the
// setjmp ↔ longjmp pair.
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define REXC_NOINLINE __attribute__((noinline))
#else
#define REXC_NOINLINE
#endif

REXC_NOINLINE static bool test_exception_handling() {
    // Basic throw and catch
    rexc_rt::ExceptionFrame frame;
    rexc_rt::rexc_push_frame(&frame);

    volatile int caught_value = 0;
    int thrown_int = 42;

    if (setjmp(frame.env) == 0) {
        REXC_THROW(&thrown_int, "int");
        return false; // should not reach here
    } else {
        if (!frame.type_name) return false;
        if (std::strcmp(frame.type_name, "int") != 0) return false;
        caught_value = *static_cast<int*>(frame.exception_obj);
    }
    if (caught_value != 42) return false;

    // Nested try/catch
    rexc_rt::ExceptionFrame outer_frame;
    rexc_rt::rexc_push_frame(&outer_frame);

    volatile int nested_caught = 0;
    int thrown_99 = 99;

    if (setjmp(outer_frame.env) == 0) {
        rexc_rt::ExceptionFrame inner_frame;
        rexc_rt::rexc_push_frame(&inner_frame);

        if (setjmp(inner_frame.env) == 0) {
            REXC_THROW(&thrown_99, "int");
            return false;
        } else {
            nested_caught = *static_cast<int*>(inner_frame.exception_obj);
        }
    } else {
        return false; // outer catch should not be reached
    }
    rexc_rt::rexc_pop_frame(); // pop outer
    if (nested_caught != 99) return false;

    return true;
}

REXC_NOINLINE static bool test_exception_recovery() {
    rexc_rt::ExceptionFrame frame;
    rexc_rt::rexc_push_frame(&frame);

    volatile int result = 0;
    const char* error_msg = "division by zero";

    if (setjmp(frame.env) == 0) {
        REXC_THROW(const_cast<char*>(error_msg), "const char*");
        return false;
    } else {
        if (!frame.type_name) return false;
        if (std::strcmp(frame.type_name, "const char*") != 0) return false;
        const char* msg = static_cast<const char*>(frame.exception_obj);
        if (std::strcmp(msg, "division by zero") != 0) return false;
        result = -1;
    }
    if (result != -1) return false;

    // Program continues after catch
    result = 42;
    if (result != 42) return false;

    return true;
}

int main() {
    // Force line-by-line flushing so progress is visible on Windows CI
    std::cout << std::unitbuf;

    // ── Test 1: Lexer ─────────────────────────────────────────────
    {
        auto tokens = rexc::tokenize("int main() { return 42; }", "test.cpp");
        assert(!tokens.empty());
        assert(tokens.back().kind == rexc::TokenKind::Eof);
        std::cout << "Lexer: OK (" << tokens.size() << " tokens)\n";
    }

    // ── Test 2: Parser ────────────────────────────────────────────
    {
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(
            "int foo(int x) { return x + 1; }",
            "test.cpp", diags);
        assert(tu != nullptr);
        assert(!tu->decls.empty());
        std::cout << "Parser: OK (" << tu->decls.size() << " top-level decls)\n";
    }

    // ── Test 3: Semantic + CodeGen on simple program ──────────────
    {
        const char* src = R"(
int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(3, 4);
    return 0;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "simple.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);

        assert(!c_src.empty());
        assert(c_src.find("int add") != std::string::npos
            || c_src.find("add(") != std::string::npos);
        std::cout << "CodeGen (simple): OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 4: Parse the Animal/Dog test program ─────────────────
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>
#include <vector>

class Animal {
public:
    std::string name;
    int age;

    Animal(const std::string& n, int a) : name(n), age(a) {}

    virtual std::string speak() const {
        return "...";
    }

    void print() const {
        std::cout << name << " (age " << age << "): " << speak() << "\n";
    }
};

class Dog : public Animal {
public:
    Dog(const std::string& n, int a) : Animal(n, a) {}

    std::string speak() const override {
        return "Woof!";
    }
};

int main() {
    std::vector<Animal*> animals;
    animals.push_back(new Dog("Rex", 3));
    animals.push_back(new Dog("Buddy", 5));

    for (auto* a : animals) {
        a->print();
    }

    std::cout << "Done!\n";
    return 0;
}
)CPP";

        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "animals.cpp", diags);
        assert(tu != nullptr);

        // Count declarations
        int num_decls = (int)tu->decls.size();
        std::cout << "Parser (animals.cpp): OK (" << num_decls << " top-level decls)\n";

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        auto& ctx = sem.context();
        assert(ctx.classes.count("Animal") > 0);
        assert(ctx.classes.count("Dog")    > 0);
        assert(ctx.classes.at("Animal").has_vtable);
        std::cout << "Semantic: OK (Animal.has_vtable = "
                  << ctx.classes.at("Animal").has_vtable << ")\n";

        rexc::CodeGenerator gen(ctx, "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());
        std::cout << "CodeGen (animals.cpp): OK (" << c_src.size() << " bytes)\n";

        // Print first 40 lines of generated code
        std::cout << "\n--- Generated C (first 40 lines) ---\n";
        std::istringstream ss(c_src);
        std::string line;
        int n = 0;
        while (std::getline(ss, line) && n < 40) {
            std::cout << line << "\n"; n++;
        }
        std::cout << "...\n";
    }

    // ── Test 5: I/O with 'using namespace std' (bare cout/cin/string) ─
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>
using namespace std;

int main() {
    string nome;
    int idade;
    float peso;
    float altura;

    cout << "Nome: ";
    getline(cin, nome);

    cout << "Idade: ";
    cin >> idade;

    cout << "Peso: ";
    cin >> peso;

    cout << "Altura: ";
    cin >> altura;

    cout << "Voce se chama: " << nome << "\n";
    cout << "Sua idade: " << idade << "\n";
    cout << "Voce Pesa: " << peso << "\n";
    cout << "Voce Mede: " << altura << "\n";

    system("pause");
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "io_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // Bare 'string' must be translated to rexc_str
        assert(c_src.find("rexc_str nome") != std::string::npos);
        // Bare 'cout <<' must use rexc_cout_* functions
        assert(c_src.find("rexc_cout_cstr") != std::string::npos);
        // Bare 'getline(cin, nome)' must use rexc_getline with rexc_stdin_stream
        assert(c_src.find("rexc_getline(&rexc_stdin_stream") != std::string::npos);
        assert(c_src.find("std_getline") == std::string::npos);
        // Bare 'cin >>' must use rexc_cin_* with rexc_stdin_stream
        assert(c_src.find("rexc_cin_int(&rexc_stdin_stream") != std::string::npos);
        assert(c_src.find("rexc_cin_float(&rexc_stdin_stream") != std::string::npos);
        // String variable must be output with rexc_cout_str, not rexc_cout_int
        assert(c_src.find("rexc_cout_str") != std::string::npos);
        assert(c_src.find("(int64_t)(nome)") == std::string::npos);
        // system("pause") must extract .data for const char*
        assert(c_src.find("system(") != std::string::npos);
        assert(c_src.find(".data)") != std::string::npos);

        std::cout << "I/O (using namespace std): OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 6: Terminal colors (termcolor-style using namespace) ──
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>
using namespace termcolor;

int main() {
    std::cout << fg::red << "Vermelho" << reset << "\n";
    std::cout << bg::blue << fg::white << bold << "Destaque" << reset << "\n";
    std::cout << fg_rgb(255, 100, 50) << "RGB" << reset << "\n";
    std::cout << colored("Magenta", fg::magenta) << "\n";
    std::cout << styled("Ciano+Bold", fg::cyan, bold) << "\n";
    print_error("Algo deu errado!");
    print_success("OK!");
    print_warning("Cuidado!");
    print_info("Info");
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "color_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // fg::red must become rexc_cout_cstr(stream, fg_red), NOT rexc_cout_int
        assert(c_src.find("rexc_cout_cstr") != std::string::npos);
        assert(c_src.find("fg_red") != std::string::npos);
        assert(c_src.find("(int64_t)(fg_red)") == std::string::npos);
        assert(c_src.find("(int64_t)(reset)") == std::string::npos);
        assert(c_src.find("(int64_t)(bold)") == std::string::npos);
        // bg::blue → bg_blue, must be cstr not int
        assert(c_src.find("bg_blue") != std::string::npos);
        assert(c_src.find("(int64_t)(bg_blue)") == std::string::npos);
        // fg_rgb call must use rexc_cout_cstr
        assert(c_src.find("(int64_t)(fg_rgb(") == std::string::npos);
        // colored/styled/print_error must appear as function calls
        assert(c_src.find("colored(") != std::string::npos);
        assert(c_src.find("styled(") != std::string::npos);
        assert(c_src.find("print_error(") != std::string::npos);
        assert(c_src.find("print_success(") != std::string::npos);

        std::cout << "Termcolor codegen: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 7: Multi-arg colored() and styled() ──────────────────
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>
using namespace termcolor;

int main() {
    std::cout << colored("White on red", fg::white, bg::red) << "\n";
    std::cout << colored("Triple", fg::cyan, bg::blue, bold) << "\n";
    std::cout << styled("Styled4", fg::green, bold, underline) << "\n";
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "multi_color.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // colored("text", fg::white, bg::red) has 3 args → must emit colored2(...)
        assert(c_src.find("colored2(") != std::string::npos);
        // colored("text", c1, c2, c3) has 4 args → must emit colored3(...)
        assert(c_src.find("colored3(") != std::string::npos);
        // styled("text", c1, c2, c3) has 4 args → must emit styled2(...)
        assert(c_src.find("styled2(") != std::string::npos);

        std::cout << "Multi-arg colored/styled: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 8: roll:: dice library codegen ──────────────────────────
    {
        const char* src = R"CPP(
#include <iostream>

int main() {
    int a = roll::d6();
    int b = roll::d20(2);
    auto r = roll::roll(6, 3);
    std::cout << r.to_string() << "\n";
    int adv = roll::advantage();
    int dis = roll::disadvantage();
    int c   = roll::chance(75);
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "roll_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // roll::d6() with 0 args → roll_d6()
        assert(c_src.find("roll_d6()") != std::string::npos);
        // roll::d20(2) with 1 arg → roll_d20_n(2)
        assert(c_src.find("roll_d20_n(") != std::string::npos);
        // roll::roll(6,3) → roll_roll_full(6, 3)
        assert(c_src.find("roll_roll_full(") != std::string::npos);
        // r.to_string() → roll_result_to_string(&(r))
        assert(c_src.find("roll_result_to_string(") != std::string::npos);
        // to_string result must be printed with rexc_cout_str_v, not rexc_cout_int
        assert(c_src.find("rexc_cout_str_v(") != std::string::npos);
        // roll::advantage() and roll::disadvantage()
        assert(c_src.find("roll_advantage(") != std::string::npos);
        assert(c_src.find("roll_disadvantage(") != std::string::npos);
        // roll::chance(75)
        assert(c_src.find("roll_chance(") != std::string::npos);

        std::cout << "Roll codegen: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 9: auto/null type + InitListExpr → C array ──────────
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>

int main() {
    auto nomes = {"FOR", "DES", "CON"};
    auto nums  = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        std::cout << i << "\n";
    }
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "array_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // auto nomes = {"a","b","c"} → rexc_str nomes[]
        assert(c_src.find("rexc_str nomes[]") != std::string::npos);
        // auto nums = {1,2,3} → int nums[]
        assert(c_src.find("int nums[]") != std::string::npos);
        // must NOT emit 'void nomes'
        assert(c_src.find("void nomes") == std::string::npos);

        std::cout << "Array from InitList codegen: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 10: ternary with rexc_str result in cout → rexc_cout_str_v ──
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>

int main() {
    bool flag = true;
    std::cout << (flag ? "SIM!" : "nao") << "\n";
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "ternary_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // ternary with string branches → must use _v variant, NOT &(ternary)
        assert(c_src.find("rexc_cout_str_v(") != std::string::npos);
        assert(c_src.find("&(((flag") == std::string::npos);

        std::cout << "Ternary str in cout codegen: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 11: roll_parse() with string literal → extract .data ──
    {
        const char* src = R"CPP(
#include <iostream>

int main() {
    int r = roll::parse("2d6");
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "rollparse_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // roll_parse("2d6") → roll_parse((rexc_str_from_lit("2d6")).data)
        assert(c_src.find("roll_parse(") != std::string::npos);
        assert(c_src.find(".data)") != std::string::npos);
        // must NOT pass rexc_str struct directly to roll_parse
        assert(c_src.find("roll_parse(rexc_str_from_lit(") == std::string::npos);

        std::cout << "roll_parse codegen: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 12: roll_parse() in cout → must use rexc_cout_int, not rexc_cout_str_v ──
    {
        const char* src = R"CPP(
#include <iostream>

int main() {
    std::cout << "\"2d6\": " << roll::parse("2d6") << "\n";
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "rollparse_cout_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // roll::parse returns int → must use rexc_cout_int, NOT rexc_cout_str_v
        assert(c_src.find("rexc_cout_int(") != std::string::npos);
        assert(c_src.find("rexc_cout_str_v(") == std::string::npos);

        std::cout << "roll_parse in cout (int, not str): OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 13: rexc_str[] array subscript in cout → rexc_cout_str, not rexc_cout_int ──
    {
        const char* src = R"CPP(
#include <iostream>
#include <string>

int main() {
    auto nomes = {"FOR", "DES", "CON"};
    auto stats = {"10", "14", "12"};
    for (int i = 0; i < 3; i++) {
        std::cout << nomes[i] << ": " << stats[i] << "\n";
    }
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "str_array_index_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // nomes[i] and stats[i] are rexc_str → must use rexc_cout_str, NOT rexc_cout_int
        assert(c_src.find("rexc_cout_str(") != std::string::npos);
        // must NOT cast rexc_str struct to int64_t
        assert(c_src.find("(int64_t)(nomes[") == std::string::npos);
        assert(c_src.find("(int64_t)(stats[") == std::string::npos);

        std::cout << "rexc_str[] subscript in cout: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 14: range-for over rexc_vec with int element → intptr_t intermediate cast ──
    {
        const char* src = R"CPP(
#include <iostream>
#include <vector>

int main() {
    std::vector<int> codes = {65, 66, 67};
    for (int c : codes) {
        std::cout << c << "\n";
    }
    return 0;
}
)CPP";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "rangefor_int_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::CodeGenerator gen(sem.context(), "runtime.h");
        std::string c_src = gen.generate_source(*tu);
        assert(!c_src.empty());

        // range-for over rexc_vec with int element must use intptr_t intermediate cast
        assert(c_src.find("(int)(intptr_t)rexc_vec_at(") != std::string::npos);
        // must be in a variable assignment context (not a bare cast elsewhere)
        assert(c_src.find("= (int)(intptr_t)rexc_vec_at(") != std::string::npos);
        // must NOT have a direct (int)rexc_vec_at(...) cast (pointer-to-int warning)
        assert(c_src.find("= (int)rexc_vec_at(") == std::string::npos);

        std::cout << "Range-for int element intptr_t cast: OK (" << c_src.size() << " bytes)\n";
    }

    // ── Test 15: Cross-compilation target triple parsing ─────────────
    {
        // Basic triple: arch-os-abi
        auto t1 = parse_target_triple("x86_64-linux-gnu");
        assert(t1.arch == "x86_64");
        assert(t1.os   == "linux");
        assert(t1.abi  == "gnu");
        assert(t1.str() == "x86_64-linux-gnu");

        // ARM64 normalisation
        auto t2 = parse_target_triple("aarch64-linux-gnu");
        assert(t2.arch == "aarch64");
        assert(t2.os   == "linux");
        assert(t2.abi  == "gnu");

        // arm64 → aarch64 alias
        auto t3 = parse_target_triple("arm64-apple-macos");
        assert(t3.arch == "aarch64");
        assert(t3.os   == "macos");
        assert(t3.abi  == "unknown");  // 'apple' is vendor, not abi

        // Windows MinGW triple
        auto t4 = parse_target_triple("x86_64-w64-mingw32");
        assert(t4.arch == "x86_64");
        assert(t4.os   == "windows");
        assert(t4.abi  == "mingw32");

        // ARM with hard float ABI
        auto t5 = parse_target_triple("arm-linux-gnueabihf");
        assert(t5.arch == "arm");
        assert(t5.os   == "linux");
        assert(t5.abi  == "gnueabihf");

        // WebAssembly triple
        auto t6 = parse_target_triple("wasm32-unknown-unknown");
        assert(t6.arch == "wasm32");

        // Empty triple
        auto t7 = parse_target_triple("");
        assert(t7.empty());
        assert(t7.str() == "");

        // i686 32-bit Windows
        auto t8 = parse_target_triple("i686-w64-mingw32");
        assert(t8.arch == "i686");
        assert(t8.os   == "windows");
        assert(t8.abi  == "mingw32");

        // musl variant
        auto t9 = parse_target_triple("x86_64-linux-musl");
        assert(t9.arch == "x86_64");
        assert(t9.os   == "linux");
        assert(t9.abi  == "musl");

        std::cout << "Cross-compile target parsing: OK\n";
    }

    // ── Test 16: Host target detection ───────────────────────────────
    {
        auto host = get_host_target();
        assert(!host.empty());
        assert(!host.arch.empty());
        assert(!host.os.empty());
        assert(!host.str().empty());

        // Host target should be native
        assert(is_native_target(host));

        // A different arch should not be native
        TargetTriple foreign;
        foreign.arch = (host.arch == "x86_64") ? "aarch64" : "x86_64";
        foreign.os   = host.os;
        foreign.abi  = host.abi;
        assert(!is_native_target(foreign));

        std::cout << "Host target detection: OK (host=" << host.str() << ")\n";
    }

    // ── Test 17: Cross-compilation flag generation ───────────────────
    {
        // Linux target with Clang should get --target flag
        auto linux_target = parse_target_triple("aarch64-linux-gnu");
        CrossCompiler cc_clang;
        cc_clang.compiler = "clang";
        cc_clang.uses_target_flag = true;
        auto flags1 = get_cross_compile_flags(linux_target, cc_clang);
        bool has_target_flag = false;
        bool has_static_libgcc = false;
        for (auto& f : flags1) {
            if (f.find("--target=") == 0) has_target_flag = true;
            if (f == "-static-libgcc") has_static_libgcc = true;
        }
        assert(has_target_flag);
        assert(has_static_libgcc);

        // Windows target should get -static
        auto win_target = parse_target_triple("x86_64-w64-mingw32");
        CrossCompiler cc_gcc;
        cc_gcc.compiler = "x86_64-w64-mingw32-gcc";
        cc_gcc.uses_target_flag = false;
        auto flags2 = get_cross_compile_flags(win_target, cc_gcc);
        bool has_static = false;
        bool has_no_target = true;
        for (auto& f : flags2) {
            if (f == "-static") has_static = true;
            if (f.find("--target=") == 0) has_no_target = false;
        }
        assert(has_static);
        assert(has_no_target);  // GCC doesn't use --target

        // musl target should get full -static
        auto musl_target = parse_target_triple("x86_64-linux-musl");
        auto flags3 = get_cross_compile_flags(musl_target, cc_clang);
        bool has_full_static = false;
        for (auto& f : flags3) {
            if (f == "-static") has_full_static = true;
        }
        assert(has_full_static);

        std::cout << "Cross-compile flag generation: OK\n";
    }

    // ── Test 18: Target binary extension ─────────────────────────────
    {
        auto win = parse_target_triple("x86_64-w64-mingw32");
        assert(target_binary_extension(win) == ".exe");

        auto linux_t = parse_target_triple("aarch64-linux-gnu");
        assert(target_binary_extension(linux_t) == "");

        auto macos_t = parse_target_triple("arm64-apple-macos");
        assert(target_binary_extension(macos_t) == "");

        auto wasm_t = parse_target_triple("wasm32-unknown-unknown");
        assert(target_binary_extension(wasm_t) == ".wasm");

        std::cout << "Target binary extension: OK\n";
    }

    // ── Test 19: Known targets list ──────────────────────────────────
    {
        auto targets = get_known_targets();
        assert(targets.size() >= 10);  // At least 10 known targets

        // Check that x86_64-linux-gnu is in the list
        bool found_linux_x86 = false;
        bool found_win_x86 = false;
        bool found_macos_arm = false;
        for (auto& t : targets) {
            if (t.triple == "x86_64-linux-gnu") found_linux_x86 = true;
            if (t.triple == "x86_64-w64-mingw32") found_win_x86 = true;
            if (t.triple == "arm64-apple-macos") found_macos_arm = true;
        }
        assert(found_linux_x86);
        assert(found_win_x86);
        assert(found_macos_arm);

        std::cout << "Known targets list: OK (" << targets.size() << " targets)\n";
    }

    // ── Test 20: Native backend availability ─────────────────────────
    {
        // Native backend should be available on all supported platforms
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__linux__)
        assert(rexc::has_native_backend() == true);
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(_WIN32)
        assert(rexc::has_native_backend() == true);
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__APPLE__)
        assert(rexc::has_native_backend() == true);
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__linux__)
        assert(rexc::has_native_backend() == true);
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__APPLE__)
        assert(rexc::has_native_backend() == true);
#endif
        std::cout << "Native backend availability: OK (available="
                  << (rexc::has_native_backend() ? "true" : "false") << ")\n";
    }

    // ── Test 21: Native backend — simple program ─────────────────────
    {
        const char* src = R"(
int main() {
    return 0;
}
)";
        // Parse through the full pipeline
        auto tokens = rexc::tokenize(src, "test_native.cpp");
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "test_native.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Check native compatibility
        std::string compat = rexc::NativeCodeGenerator::can_compile_natively(*tu);
        assert(compat.empty());  // Should be compatible

        std::cout << "Native backend simple program check: OK\n";
    }

    // ── Test 22: Native backend — class acceptance (v0.6+) ─────────
    {
        const char* src = R"(
class Foo { int x; };
int main() { return 0; }
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "test_class.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Classes are now supported by the native backend (v0.6)
        std::string compat = rexc::NativeCodeGenerator::can_compile_natively(*tu);
        assert(compat.empty());

        std::cout << "Native backend class acceptance: OK\n";
    }

    // ── Test 23: ELF writer structure ────────────────────────────────
    {
        // Verify code_vaddr is consistent for 1 and 2 segment layouts
        uint64_t vaddr_1seg = rexc::ElfWriter::code_vaddr(false);
        uint64_t vaddr_2seg = rexc::ElfWriter::code_vaddr(true);

        // 1-segment layout should have smaller header overhead
        assert(vaddr_1seg < vaddr_2seg);

        // Both should be above the base address
        assert(vaddr_1seg >= 0x400000);
        assert(vaddr_2seg >= 0x400000);

        std::cout << "ELF writer structure: OK (1seg=" << std::hex
                  << vaddr_1seg << ", 2seg=" << vaddr_2seg
                  << std::dec << ")\n";
    }

    // ── Test 24: x86 emitter label resolution ────────────────────────
    {
        rexc::X86Emitter emit;

        // Create a label, emit a jump to it, bind it, resolve
        uint32_t lbl = emit.new_label();
        emit.jmp_label(lbl);  // 5 bytes: E9 + imm32
        emit.nop();            // 1 byte: 90
        emit.bind_label(lbl);  // label points to offset 6
        emit.nop();            // 1 byte

        emit.resolve_labels(0x400080);

        // After resolution, the jmp target should be offset 6
        // JMP encoding: E9 01 00 00 00 (jump +1 byte ahead, over the nop)
        assert(emit.code()[0] == 0xE9);
        int32_t disp = *reinterpret_cast<const int32_t*>(&emit.code()[1]);
        // disp = target_offset - (patch_offset + 4) = 6 - (1 + 4) = 1
        assert(disp == 1);

        std::cout << "x86 emitter label resolution: OK\n";
    }

    // ── Test 25: Native compile_native() end-to-end ─────────────────
#if defined(__x86_64__) && defined(__linux__)
    {
        // Write a small test program
        std::string test_dir = "/tmp/rex_native_test";
        std::string test_src = test_dir + "/test.cpp";
        std::string test_bin = test_dir + "/test_bin";

        // Create test directory
        std::filesystem::create_directories(test_dir);

        // Write test source
        std::ofstream f(test_src);
        f << "int main() { return 0; }\n";
        f.close();

        // Compile natively
        auto result = rexc::compile_native(test_src, test_bin, false);
        assert(result.success);
        assert(std::filesystem::exists(test_bin));

        // Verify it's a valid ELF
        std::ifstream elf(test_bin, std::ios::binary);
        char magic[4];
        elf.read(magic, 4);
        assert(magic[0] == 0x7f);
        assert(magic[1] == 'E');
        assert(magic[2] == 'L');
        assert(magic[3] == 'F');

        // Clean up
        std::filesystem::remove_all(test_dir);

        std::cout << "Native compile_native() end-to-end: OK\n";
    }
#else
    std::cout << "Native compile_native() end-to-end: SKIPPED (not x86_64 Linux)\n";
#endif

    // ── Test 26: PE writer structure ─────────────────────────────────
    {
        // Verify code_vaddr is consistent
        uint64_t pe_vaddr = rexc::PeWriter::code_vaddr();

        // Should be above image base + section alignment
        assert(pe_vaddr >= 0x400000 + 0x1000);

        // Test PE writing to a temporary file
        std::vector<uint8_t> dummy_code = {0x90, 0x90, 0xC3};  // NOP NOP RET
        std::vector<uint8_t> dummy_rodata = {'H', 'i', 0};
        std::string pe_path = (std::filesystem::temp_directory_path() / "rex_test_pe.exe").string();
        bool pe_ok = rexc::PeWriter::write(pe_path, dummy_code, dummy_rodata);
        assert(pe_ok);

        // Verify MZ magic
        {
            std::ifstream pe_f(pe_path, std::ios::binary);
            char pe_magic[2];
            pe_f.read(pe_magic, 2);
            assert(pe_magic[0] == 'M');
            assert(pe_magic[1] == 'Z');

            // Verify PE signature at e_lfanew offset
            pe_f.seekg(60, std::ios::beg);  // e_lfanew is at offset 60
            uint32_t pe_offset;
            pe_f.read(reinterpret_cast<char*>(&pe_offset), 4);
            pe_f.seekg(pe_offset, std::ios::beg);
            char pe_sig[4];
            pe_f.read(pe_sig, 4);
            assert(pe_sig[0] == 'P');
            assert(pe_sig[1] == 'E');
            assert(pe_sig[2] == 0);
            assert(pe_sig[3] == 0);
        } // close pe_f before removal

        std::error_code ec;
        std::filesystem::remove(pe_path, ec);
        std::cout << "PE writer structure: OK (code_vaddr=0x" << std::hex << pe_vaddr << std::dec << ")\n";
    }

    // ── Test 27: Mach-O writer structure ─────────────────────────────
    {
        // Verify code_vaddr is consistent
        uint64_t macho_vaddr = rexc::MachOWriter::code_vaddr(rexc::MachOWriter::Arch::X86_64);

        // Should be above TEXT_BASE
        assert(macho_vaddr >= 0x100000000ULL);

        // Test x86_64 Mach-O writing
        std::vector<uint8_t> dummy_code = {0x90, 0x90, 0xC3};
        std::vector<uint8_t> dummy_rodata = {'H', 'i', 0};
        std::string macho_path = (std::filesystem::temp_directory_path() / "rex_test_macho").string();
        bool macho_ok = rexc::MachOWriter::write(macho_path, rexc::MachOWriter::Arch::X86_64,
                                                   dummy_code, dummy_rodata);
        assert(macho_ok);

        // Verify Mach-O magic
        uint32_t macho_magic;
        uint32_t cpu_type;
        {
            std::ifstream macho_f(macho_path, std::ios::binary);
            macho_f.read(reinterpret_cast<char*>(&macho_magic), 4);
            assert(macho_magic == 0xFEEDFACF);  // MH_MAGIC_64

            // Verify CPU type is x86_64
            macho_f.read(reinterpret_cast<char*>(&cpu_type), 4);
            assert(cpu_type == 0x01000007);  // CPU_TYPE_X86_64
        } // close macho_f before removal

        std::error_code macho_ec;
        std::filesystem::remove(macho_path, macho_ec); // non-throwing on Windows

        // Test ARM64 Mach-O writing
        std::string macho_arm_path = (std::filesystem::temp_directory_path() / "rex_test_macho_arm").string();
        bool macho_arm_ok = rexc::MachOWriter::write(macho_arm_path, rexc::MachOWriter::Arch::ARM64,
                                                       dummy_code, dummy_rodata);
        assert(macho_arm_ok);

        // Verify ARM64 CPU type
        {
            std::ifstream macho_arm_f(macho_arm_path, std::ios::binary);
            macho_arm_f.read(reinterpret_cast<char*>(&macho_magic), 4);
            assert(macho_magic == 0xFEEDFACF);
            macho_arm_f.read(reinterpret_cast<char*>(&cpu_type), 4);
            assert(cpu_type == 0x0100000C);  // CPU_TYPE_ARM64
        } // close macho_arm_f before removal

        std::error_code macho_arm_ec;
        std::filesystem::remove(macho_arm_path, macho_arm_ec); // non-throwing on Windows
        std::cout << "Mach-O writer structure: OK (code_vaddr=0x" << std::hex << macho_vaddr << std::dec << ")\n";
    }

    // ── Test 28: ARM64 emitter basic instructions ────────────────────
    {
        rexc::ARM64Emitter arm;

        // Test label resolution for branch
        uint32_t lbl = arm.new_label();
        arm.b_label(lbl);    // 4 bytes: B + placeholder
        arm.nop();            // 4 bytes: NOP
        arm.bind_label(lbl);  // label at offset 8
        arm.nop();

        arm.resolve_labels(0x100000);

        // Verify NOP encoding (0xD503201F)
        assert(arm.code()[4] == 0x1F);
        assert(arm.code()[5] == 0x20);
        assert(arm.code()[6] == 0x03);
        assert(arm.code()[7] == 0xD5);

        std::cout << "ARM64 emitter basic instructions: OK\n";
    }

    // ── Test 29: ARM64 emitter function prologue/epilogue ────────────
    {
        rexc::ARM64Emitter arm;

        arm.function_prologue(32);
        arm.mov_reg_imm64(rexc::ARM64Reg::X0, 42);
        arm.function_epilogue(32);

        // Should have generated valid instructions (all 4-byte aligned)
        assert(arm.size() % 4 == 0);
        assert(arm.size() > 0);

        std::cout << "ARM64 emitter prologue/epilogue: OK (" << arm.size() << " bytes)\n";
    }

    // ── Test 30: ARM64 emitter arithmetic ────────────────────────────
    {
        rexc::ARM64Emitter arm;

        arm.movz(rexc::ARM64Reg::X0, 10);
        arm.movz(rexc::ARM64Reg::X1, 20);
        arm.add_reg_reg(rexc::ARM64Reg::X2, rexc::ARM64Reg::X0, rexc::ARM64Reg::X1);
        arm.sub_reg_reg(rexc::ARM64Reg::X3, rexc::ARM64Reg::X2, rexc::ARM64Reg::X0);
        arm.mul_reg_reg(rexc::ARM64Reg::X4, rexc::ARM64Reg::X0, rexc::ARM64Reg::X1);

        assert(arm.size() == 20);  // 5 instructions * 4 bytes
        assert(arm.size() % 4 == 0);  // All ARM64 instructions are 4 bytes

        std::cout << "ARM64 emitter arithmetic: OK (" << arm.size() << " bytes)\n";
    }

    // ── Test 31: IRGenerator produces valid module for return 42 ──────
    {
        const char* src = R"(
int main() {
    return 42;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "ir_test1.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::IRGenerator gen(sem.context());
        rexc::IRModule mod = gen.generate(tu->decls);

        // Should have at least 1 function (main)
        assert(!mod.functions.empty());
        bool found_main = false;
        for (auto& fn : mod.functions) {
            if (fn.name == "main") {
                found_main = true;
                // main should have at least one block
                assert(!fn.blocks.empty());
                // return type should be Int32
                assert(fn.return_type == rexc::IRType::Int32);
                // Should have a Ret instruction somewhere
                bool has_ret = false;
                for (auto& blk : fn.blocks)
                    for (auto& instr : blk.instrs)
                        if (instr.op == rexc::IROp::Ret) has_ret = true;
                assert(has_ret);
            }
        }
        assert(found_main);
        std::cout << "IR generator (return 42): OK (" << mod.functions.size() << " functions)\n";
    }

    // ── Test 32: IRGenerator produces Load/Store for local variables ──
    {
        const char* src = R"(
int main() {
    int x = 10;
    int y = x + 20;
    return y;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "ir_test2.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::IRGenerator gen(sem.context());
        rexc::IRModule mod = gen.generate(tu->decls);

        assert(!mod.functions.empty());
        auto& main_fn = mod.functions[0];
        assert(main_fn.name == "main");

        // Count Alloca, Load, Store instructions
        int n_alloca = 0, n_load = 0, n_store = 0;
        for (auto& blk : main_fn.blocks) {
            for (auto& instr : blk.instrs) {
                if (instr.op == rexc::IROp::Alloca) n_alloca++;
                if (instr.op == rexc::IROp::Load)   n_load++;
                if (instr.op == rexc::IROp::Store)  n_store++;
            }
        }
        // Should have at least 2 allocas (x and y)
        assert(n_alloca >= 2);
        // Should have stores for initialisation
        assert(n_store >= 2);
        // Should have loads for reading x and y
        assert(n_load >= 1);

        std::cout << "IR generator (local vars): OK (alloca=" << n_alloca
                  << " load=" << n_load << " store=" << n_store << ")\n";
    }

    // ── Test 33: IROptimizer constant folding: 2 + 3 → Const(5) ──────
    {
        // Build a small module manually
        rexc::IRModule mod;
        rexc::IRFunction fn;
        fn.name = "test_fold";
        fn.return_type = rexc::IRType::Int32;
        fn.new_block("entry");

        // %1 = const i32 2
        rexc::IRInstr c1;
        c1.op = rexc::IROp::Const;
        c1.result = fn.new_value(rexc::IRType::Int32);
        c1.const_int = 2;
        fn.current().instrs.push_back(c1);

        // %2 = const i32 3
        rexc::IRInstr c2;
        c2.op = rexc::IROp::Const;
        c2.result = fn.new_value(rexc::IRType::Int32);
        c2.const_int = 3;
        fn.current().instrs.push_back(c2);

        // %3 = add i32 %1, %2
        rexc::IRInstr add;
        add.op = rexc::IROp::Add;
        add.result = fn.new_value(rexc::IRType::Int32);
        add.operands = {c1.result, c2.result};
        fn.current().instrs.push_back(add);

        // ret i32 %3
        rexc::IRInstr ret;
        ret.op = rexc::IROp::Ret;
        ret.operands = {add.result};
        fn.current().instrs.push_back(ret);

        mod.functions.push_back(std::move(fn));

        rexc::IROptimizer opt;
        opt.run(mod);

        // After constant folding, the add should become a Const(5)
        // or the ret should use a Const(5) value.
        auto& opt_fn = mod.functions[0];
        bool found_const_5 = false;
        for (auto& blk : opt_fn.blocks) {
            for (auto& instr : blk.instrs) {
                if (instr.op == rexc::IROp::Const && instr.const_int == 5)
                    found_const_5 = true;
            }
        }
        assert(found_const_5);
        std::cout << "IR optimizer constant fold (2+3=5): OK\n";
    }

    // ── Test 34: IROptimizer DCE: unused Add is removed ──────────────
    {
        rexc::IRModule mod;
        rexc::IRFunction fn;
        fn.name = "test_dce";
        fn.return_type = rexc::IRType::Int32;
        fn.new_block("entry");

        // %1 = const i32 10
        rexc::IRInstr c1;
        c1.op = rexc::IROp::Const;
        c1.result = fn.new_value(rexc::IRType::Int32);
        c1.const_int = 10;
        fn.current().instrs.push_back(c1);

        // %2 = const i32 20
        rexc::IRInstr c2;
        c2.op = rexc::IROp::Const;
        c2.result = fn.new_value(rexc::IRType::Int32);
        c2.const_int = 20;
        fn.current().instrs.push_back(c2);

        // %3 = add i32 %1, %2  (result is NEVER used)
        rexc::IRInstr dead_add;
        dead_add.op = rexc::IROp::Add;
        dead_add.result = fn.new_value(rexc::IRType::Int32);
        dead_add.operands = {c1.result, c2.result};
        fn.current().instrs.push_back(dead_add);

        // %4 = const i32 0
        rexc::IRInstr c_zero;
        c_zero.op = rexc::IROp::Const;
        c_zero.result = fn.new_value(rexc::IRType::Int32);
        c_zero.const_int = 0;
        fn.current().instrs.push_back(c_zero);

        // ret i32 %4  (only uses c_zero, not dead_add)
        rexc::IRInstr ret;
        ret.op = rexc::IROp::Ret;
        ret.operands = {c_zero.result};
        fn.current().instrs.push_back(ret);

        mod.functions.push_back(std::move(fn));

        rexc::IROptimizer opt;
        opt.run(mod);

        // After DCE, the dead add (and unused consts 10, 20) should be removed
        auto& opt_fn = mod.functions[0];
        bool has_add = false;
        for (auto& blk : opt_fn.blocks)
            for (auto& instr : blk.instrs)
                if (instr.op == rexc::IROp::Add) has_add = true;
        assert(!has_add);
        std::cout << "IR optimizer DCE (dead add removed): OK\n";
    }

    // ── Test 35: IROptimizer mem2reg: alloca promoted to SSA value ────
    {
        rexc::IRModule mod;
        rexc::IRFunction fn;
        fn.name = "test_mem2reg";
        fn.return_type = rexc::IRType::Int32;
        fn.new_block("entry");

        // %1 = alloca i32
        rexc::IRInstr alloca_instr;
        alloca_instr.op = rexc::IROp::Alloca;
        alloca_instr.result = fn.new_value(rexc::IRType::Ptr);
        fn.current().instrs.push_back(alloca_instr);

        // %2 = const i32 42
        rexc::IRInstr c42;
        c42.op = rexc::IROp::Const;
        c42.result = fn.new_value(rexc::IRType::Int32);
        c42.const_int = 42;
        fn.current().instrs.push_back(c42);

        // store i32 %2, ptr %1
        rexc::IRInstr store;
        store.op = rexc::IROp::Store;
        store.operands = {c42.result, alloca_instr.result};
        fn.current().instrs.push_back(store);

        // %3 = load i32, ptr %1
        rexc::IRInstr load;
        load.op = rexc::IROp::Load;
        load.result = fn.new_value(rexc::IRType::Int32);
        load.operands = {alloca_instr.result};
        fn.current().instrs.push_back(load);

        // ret i32 %3
        rexc::IRInstr ret;
        ret.op = rexc::IROp::Ret;
        ret.operands = {load.result};
        fn.current().instrs.push_back(ret);

        mod.functions.push_back(std::move(fn));

        rexc::IROptimizer opt;
        opt.run(mod);

        // After mem2reg, the alloca should be promoted: no more Alloca/Load/Store
        auto& opt_fn = mod.functions[0];
        bool has_alloca = false;
        for (auto& blk : opt_fn.blocks)
            for (auto& instr : blk.instrs)
                if (instr.op == rexc::IROp::Alloca) has_alloca = true;
        assert(!has_alloca);
        std::cout << "IR optimizer mem2reg (alloca promoted): OK\n";
    }

    // ── Test 36: IRPrinter produces non-empty readable output ─────────
    {
        const char* src = R"(
int main() {
    int x = 5;
    return x;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "ir_print_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::IRGenerator gen(sem.context());
        rexc::IRModule mod = gen.generate(tu->decls);

        std::string ir_text = rexc::IRPrinter::print(mod);
        assert(!ir_text.empty());
        // Should contain function name
        assert(ir_text.find("function main") != std::string::npos);
        // Should contain a block label
        assert(ir_text.find("entry") != std::string::npos || ir_text.find("bb") != std::string::npos);
        // Should contain ret instruction
        assert(ir_text.find("ret") != std::string::npos);

        std::cout << "IR printer output: OK (" << ir_text.size() << " chars)\n";
    }

    // ── Test 37: Full pipeline AST → IR → optimised IR for arithmetic ─
    {
        const char* src = R"(
int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(3, 4);
    return result;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "ir_pipeline_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Step 1: unoptimised IR — verify structure
        rexc::IRModule raw = rexc::NativeCodeGenerator::generate_ir(*tu, sem.context(), false);
        assert(raw.functions.size() >= 2);

        bool found_add = false, found_main = false;
        for (auto& fn : raw.functions) {
            if (fn.name == "add") {
                found_add = true;
                assert(fn.return_type == rexc::IRType::Int32);
                assert(fn.params.size() == 2);
            }
            if (fn.name == "main") {
                found_main = true;
                bool has_call = false;
                for (auto& blk : fn.blocks)
                    for (auto& instr : blk.instrs)
                        if (instr.op == rexc::IROp::Call && instr.callee == "add")
                            has_call = true;
                assert(has_call);
            }
        }
        assert(found_add);
        assert(found_main);

        // Step 2: optimised IR — verify it is smaller / constant-folded
        rexc::IRModule opt = rexc::NativeCodeGenerator::generate_ir(*tu, sem.context(), true);
        assert(!opt.functions.empty());

        std::string ir_text = rexc::IRPrinter::print(opt);
        assert(!ir_text.empty());
        assert(ir_text.find("function add") != std::string::npos
            || ir_text.find("function main") != std::string::npos);

        std::cout << "Full IR pipeline (AST->IR->opt): OK (" << raw.functions.size()
                  << " functions, " << ir_text.size() << " chars IR)\n";
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 2 tests (v0.6): Classes, Vtable, Name Mangling
    // ══════════════════════════════════════════════════════════════

    // ── Test 38: ClassLayout — field offsets for simple class ─────────
    {
        // Build a class: class Foo { int x; double y; };
        // Parse and semantically analyze so ObjectLayoutBuilder can work
        const char* src = R"(
class Foo {
public:
    int x;
    double y;
};

int main() { return 0; }
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "layout_test1.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Find the ClassDecl for "Foo"
        const rexc::ClassDecl* foo_cls = nullptr;
        for (auto& decl : tu->decls) {
            if (decl->kind == rexc::NodeKind::ClassDecl) {
                auto* cd = decl->as<const rexc::ClassDecl>();
                if (cd->name == "Foo") { foo_cls = cd; break; }
            }
        }
        assert(foo_cls != nullptr);

        rexc::ObjectLayoutBuilder builder;
        rexc::ClassLayout layout = builder.build(*foo_cls, sem.context());

        assert(layout.class_name == "Foo");
        assert(!layout.has_vtable);   // no virtual methods
        assert(layout.fields.size() == 2);

        // int x: size=4, offset=0
        assert(layout.fields[0].name == "x");
        assert(layout.fields[0].size == 4);
        assert(layout.fields[0].offset == 0);

        // double y: size=8, aligned to 8
        assert(layout.fields[1].name == "y");
        assert(layout.fields[1].size == 8);
        assert(layout.fields[1].offset == 8);  // 0+4 → pad to 8

        // total_size should be 16 (aligned to max alignment 8)
        assert(layout.total_size == 16);
        assert(layout.align == 8);

        std::cout << "ClassLayout (Foo: int,double): OK (size="
                  << layout.total_size << " align=" << layout.align << ")\n";
    }

    // ── Test 39: ClassLayout with vtable — __vptr at offset 0 ────────
    {
        const char* src = R"(
class Animal {
public:
    int age;
    virtual void speak() {}
};

int main() { return 0; }
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "layout_test2.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        const rexc::ClassDecl* anim_cls = nullptr;
        for (auto& decl : tu->decls) {
            if (decl->kind == rexc::NodeKind::ClassDecl) {
                auto* cd = decl->as<const rexc::ClassDecl>();
                if (cd->name == "Animal") { anim_cls = cd; break; }
            }
        }
        assert(anim_cls != nullptr);

        rexc::ObjectLayoutBuilder builder;
        rexc::ClassLayout layout = builder.build(*anim_cls, sem.context());

        assert(layout.class_name == "Animal");
        assert(layout.has_vtable);
        assert(layout.vtable_ptr_offset == 0);  // __vptr at offset 0

        // Should have at least one vtable entry (speak)
        assert(!layout.vtable.empty());
        bool found_speak = false;
        for (auto& e : layout.vtable) {
            if (e.method_name == "speak") { found_speak = true; break; }
        }
        assert(found_speak);

        // age field should come after __vptr (8 bytes)
        bool found_age = false;
        for (auto& f : layout.fields) {
            if (f.name == "age") {
                assert(f.offset >= 8);
                found_age = true;
            }
        }
        assert(found_age);

        std::cout << "ClassLayout (Animal, vtable): OK (vptr_off=0, slots="
                  << layout.vtable.size() << ")\n";
    }

    // ── Test 40: Mangler — Foo::bar(int) → _ZN3Foo3barEi ─────────────
    {
        rexc::Mangler mangler;

        // Method: Foo::bar(int)
        std::string m1 = mangler.mangle_method("Foo", "bar", {rexc::IRType::Int32});
        assert(m1 == "_ZN3Foo3barEi");

        // Constructor: Foo::Foo()
        std::string ctor = mangler.mangle_ctor("Foo", {});
        assert(ctor == "_ZN3FooC1Ev");

        // Destructor: Foo::~Foo()
        std::string dtor = mangler.mangle_dtor("Foo");
        assert(dtor == "_ZN3FooD1Ev");

        // Vtable: Foo
        std::string vtbl = mangler.mangle_vtable("Foo");
        assert(vtbl == "_ZTV3Foo");

        // Free function: add(int, int)
        std::string free_fn = mangler.mangle_function("add",
            {rexc::IRType::Int32, rexc::IRType::Int32});
        assert(free_fn == "_Z3addii");

        // Method with double param: MyClass::compute(double)
        std::string m2 = mangler.mangle_method("MyClass", "compute",
            {rexc::IRType::Float64});
        assert(m2 == "_ZN7MyClass7computeEd");

        std::cout << "Mangler (Itanium ABI): OK\n";
    }

    // ── Test 41: IR generation for class with no inheritance ──────────
    {
        const char* src = R"(
class Point {
public:
    int x;
    int y;
    int sum() { return x + y; }
};

int main() {
    return 0;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "class_ir_test1.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Verify the class is accepted by semantic analysis
        auto it = sem.context().classes.find("Point");
        assert(it != sem.context().classes.end());
        assert(it->second.fields.size() >= 2);

        std::cout << "Class IR (no inheritance): OK (fields="
                  << it->second.fields.size() << ")\n";
    }

    // ── Test 42: IR generation with single inheritance ───────────────
    {
        const char* src = R"(
class Base {
public:
    int a;
    virtual void doSomething() {}
};

class Derived : public Base {
public:
    int b;
    void doSomething() {}
};

int main() { return 0; }
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "class_ir_test2.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Verify both classes recognized
        assert(sem.context().classes.count("Base"));
        assert(sem.context().classes.count("Derived"));

        // Derived should have Base in its bases
        auto& derived = sem.context().classes.at("Derived");
        bool base_found = false;
        for (auto& b : derived.bases) {
            if (b == "Base") { base_found = true; break; }
        }
        assert(base_found);

        std::cout << "Class IR (single inheritance): OK\n";
    }

    // ── Test 43: Virtual dispatch — vtable slots correct ─────────────
    {
        const char* src = R"(
class Shape {
public:
    virtual int area() { return 0; }
    virtual int perimeter() { return 0; }
};

class Square : public Shape {
public:
    int side;
    int area() { return side * side; }
};

int main() { return 0; }
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "vtable_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        // Find Square class decl
        const rexc::ClassDecl* sq_cls = nullptr;
        for (auto& decl : tu->decls) {
            if (decl->kind == rexc::NodeKind::ClassDecl) {
                auto* cd = decl->as<const rexc::ClassDecl>();
                if (cd->name == "Square") { sq_cls = cd; break; }
            }
        }
        assert(sq_cls != nullptr);

        rexc::ObjectLayoutBuilder builder;
        rexc::ClassLayout layout = builder.build(*sq_cls, sem.context());

        // Square inherits virtual methods, so it should have vtable
        assert(layout.has_vtable);
        // Should have at least 2 vtable slots (area, perimeter)
        assert(layout.vtable.size() >= 2);

        // area should be overridden (mangled with Square)
        bool area_overridden = false;
        for (auto& e : layout.vtable) {
            if (e.method_name == "area") {
                assert(e.mangled_name.find("Square") != std::string::npos);
                area_overridden = true;
            }
        }
        assert(area_overridden);

        std::cout << "Virtual dispatch (vtable slots): OK (slots="
                  << layout.vtable.size() << ")\n";
    }

    // ── Test 44: new/delete — native compile acceptance ──────────────
    {
        // Verify that a program using new/delete is accepted for parsing
        const char* src = R"(
class Node {
public:
    int value;
};

int main() {
    Node* p = new Node();
    p->value = 42;
    delete p;
    return 0;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "new_delete_test.cpp", diags);
        assert(tu != nullptr);
        assert(!tu->decls.empty());

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);
        assert(sem.context().classes.count("Node"));

        std::cout << "new/delete acceptance: OK\n";
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 3 tests (v0.7): Runtime — allocator, string, containers
    // ══════════════════════════════════════════════════════════════

    // ── Test 45: rexc_malloc + rexc_free — 1000 allocations ──────────
    {
        // Ensure heap is initialized
        rexc_rt::rexc_heap_init();

        // Allocate 1000 blocks of varying sizes
        const int N = 1000;
        void* ptrs[N];
        for (int i = 0; i < N; ++i) {
            size_t sz = 16 + (i % 256) * 16;
            ptrs[i] = rexc_rt::rexc_malloc(sz);
            assert(ptrs[i] != nullptr);
            // Write a canary byte
            static_cast<char*>(ptrs[i])[0] = static_cast<char>(i & 0xFF);
        }

        // Free all
        for (int i = 0; i < N; ++i) {
            rexc_rt::rexc_free(ptrs[i]);
        }

        // Allocate again to check reuse works
        void* p = rexc_rt::rexc_malloc(64);
        assert(p != nullptr);
        rexc_rt::rexc_free(p);

        // Test calloc
        void* cz = rexc_rt::rexc_calloc(10, 8);
        assert(cz != nullptr);
        for (int i = 0; i < 80; ++i)
            assert(static_cast<char*>(cz)[i] == 0);
        rexc_rt::rexc_free(cz);

        // Test realloc
        void* ra = rexc_rt::rexc_malloc(32);
        assert(ra != nullptr);
        static_cast<char*>(ra)[0] = 'X';
        void* rb = rexc_rt::rexc_realloc(ra, 256);
        assert(rb != nullptr);
        assert(static_cast<char*>(rb)[0] == 'X');
        rexc_rt::rexc_free(rb);

        std::cout << "rexc_malloc/free (1000 allocs): OK\n";
    }

    // ── Test 46: rexc_string — construction, concat, comparison ──────
    {
        rexc_rt::rexc_string s1;
        assert(s1.empty());
        assert(s1.size() == 0);

        rexc_rt::rexc_string s2("Hello");
        assert(s2.size() == 5);
        assert(s2 == rexc_rt::rexc_string("Hello"));
        assert(!(s2 == rexc_rt::rexc_string("World")));

        // Concatenation
        rexc_rt::rexc_string s3 = s2 + rexc_rt::rexc_string(" World");
        assert(s3.size() == 11);
        assert(s3 == rexc_rt::rexc_string("Hello World"));

        // operator+=
        rexc_rt::rexc_string s4("foo");
        s4 += "bar";
        assert(s4 == rexc_rt::rexc_string("foobar"));

        // Comparison
        rexc_rt::rexc_string a("apple"), b("banana");
        assert(a < b);
        assert(!(b < a));
        assert(a != b);

        // Copy
        rexc_rt::rexc_string s5(s3);
        assert(s5 == s3);

        // Move
        rexc_rt::rexc_string s6(static_cast<rexc_rt::rexc_string&&>(s5));
        assert(s6 == rexc_rt::rexc_string("Hello World"));

        // Large string (beyond SSO)
        rexc_rt::rexc_string big(100, 'X');
        assert(big.size() == 100);
        assert(big[0] == 'X');
        assert(big[99] == 'X');

        std::cout << "rexc_string (ctor, concat, cmp): OK\n";
    }

    // ── Test 47: rexc_vector<int> — push_back, operator[], clear ─────
    {
        rexc_rt::rexc_vector<int> v;
        assert(v.empty());
        assert(v.size() == 0);

        for (int i = 0; i < 100; ++i)
            v.push_back(i * 2);

        assert(v.size() == 100);
        assert(v[0] == 0);
        assert(v[50] == 100);
        assert(v[99] == 198);
        assert(v.front() == 0);
        assert(v.back() == 198);

        // Pop
        v.pop_back();
        assert(v.size() == 99);
        assert(v.back() == 196);

        // Copy
        rexc_rt::rexc_vector<int> v2(v);
        assert(v2.size() == v.size());
        assert(v2[0] == v[0]);

        // Clear
        v.clear();
        assert(v.empty());
        assert(v.size() == 0);

        // Resize
        rexc_rt::rexc_vector<int> v3;
        v3.resize(10);
        assert(v3.size() == 10);
        for (size_t i = 0; i < 10; ++i)
            assert(v3[i] == 0);

        std::cout << "rexc_vector<int>: OK\n";
    }

    // ── Test 48: rexc_map<rexc_string,int> — insert, lookup, erase ───
    {
        rexc_rt::rexc_map<rexc_rt::rexc_string, int> m;
        assert(m.empty());

        m[rexc_rt::rexc_string("alpha")]   = 1;
        m[rexc_rt::rexc_string("beta")]    = 2;
        m[rexc_rt::rexc_string("gamma")]   = 3;

        assert(m.size() == 3);
        assert(m.count(rexc_rt::rexc_string("alpha")) == 1);
        assert(m.count(rexc_rt::rexc_string("delta")) == 0);

        assert(m[rexc_rt::rexc_string("beta")] == 2);

        // Update
        m[rexc_rt::rexc_string("alpha")] = 10;
        assert(m[rexc_rt::rexc_string("alpha")] == 10);

        // Erase
        m.erase(rexc_rt::rexc_string("beta"));
        assert(m.size() == 2);
        assert(m.count(rexc_rt::rexc_string("beta")) == 0);

        std::cout << "rexc_map<string,int>: OK\n";
    }

    // ── Test 49: rexc_rt OStream formatting ──────────────────────────
    {
        // Verify OStream object can be constructed and used without crash
        // (actual output goes to stdout; we just verify no segfault)
        rexc_rt::OStream test_out(1);
        // The OStream operators return OStream& for chaining
        rexc_rt::OStream& ref = (test_out << "rexc_rt::cout test: ");
        assert(&ref == &test_out);
        test_out << static_cast<int64_t>(42) << " " << 3.14 << rexc_rt::endl;

        std::cout << "rexc_rt OStream formatting: OK\n";
    }

    // ── Test 50: rexc_string ops — substr, find, conversions ─────────
    {
        rexc_rt::rexc_string s("Hello, World!");

        // find
        assert(s.find('W') == 7);
        assert(s.find('Z') == rexc_rt::rexc_string::npos);
        assert(s.find("World") == 7);
        assert(s.find("xyz") == rexc_rt::rexc_string::npos);

        // substr
        rexc_rt::rexc_string sub = s.substr(7, 5);
        assert(sub == rexc_rt::rexc_string("World"));

        rexc_rt::rexc_string sub2 = s.substr(0, 5);
        assert(sub2 == rexc_rt::rexc_string("Hello"));

        // from_int / to_int
        rexc_rt::rexc_string num = rexc_rt::rexc_string::from_int(12345);
        assert(num == rexc_rt::rexc_string("12345"));
        assert(num.to_int() == 12345);

        rexc_rt::rexc_string neg = rexc_rt::rexc_string::from_int(-42);
        assert(neg.to_int() == -42);

        // from_float / to_float
        rexc_rt::rexc_string fp = rexc_rt::rexc_string::from_float(3.14, 2);
        assert(fp.to_float() > 3.1 && fp.to_float() < 3.2);

        // push_back
        rexc_rt::rexc_string pb;
        pb.push_back('A');
        pb.push_back('B');
        pb.push_back('C');
        assert(pb == rexc_rt::rexc_string("ABC"));

        // clear
        rexc_rt::rexc_string cl("test");
        cl.clear();
        assert(cl.empty());

        std::cout << "rexc_string ops (substr, find, conv): OK\n";
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 4 tests (v0.8): Templates, Stdlib, Exceptions
    // ══════════════════════════════════════════════════════════════

    // ── Test 51: rexc_rt min/max/swap (template functions) ───────────
    {
        int a = 5, b = 10;
        assert(rexc_rt::min(a, b) == 5);
        assert(rexc_rt::max(a, b) == 10);

        rexc_rt::swap(a, b);
        assert(a == 10 && b == 5);

        double x = 1.5, y = 2.5;
        assert(rexc_rt::min(x, y) == 1.5);
        assert(rexc_rt::max(x, y) == 2.5);

        std::cout << "rexc_rt min/max/swap (templates): OK\n";
    }

    // ── Test 52: rexc_vector<rexc_string> — push_back and iteration ──
    {
        rexc_rt::rexc_vector<rexc_rt::rexc_string> vs;
        vs.push_back(rexc_rt::rexc_string("hello"));
        vs.push_back(rexc_rt::rexc_string("world"));
        vs.push_back(rexc_rt::rexc_string("rexc"));

        assert(vs.size() == 3);
        assert(vs[0] == rexc_rt::rexc_string("hello"));
        assert(vs[1] == rexc_rt::rexc_string("world"));
        assert(vs[2] == rexc_rt::rexc_string("rexc"));

        // Iterate
        int count = 0;
        for (auto* it = vs.begin(); it != vs.end(); ++it) {
            assert(!it->empty());
            ++count;
        }
        assert(count == 3);

        std::cout << "rexc_vector<rexc_string>: OK\n";
    }

    // ── Test 53: rexc_rt::sort with vector of ints ───────────────────
    {
        rexc_rt::rexc_vector<int> v;
        v.push_back(42); v.push_back(3); v.push_back(17);
        v.push_back(1);  v.push_back(99); v.push_back(8);

        rexc_rt::sort(v.begin(), v.end());

        assert(v[0] == 1);
        assert(v[1] == 3);
        assert(v[2] == 8);
        assert(v[3] == 17);
        assert(v[4] == 42);
        assert(v[5] == 99);

        // Sort larger dataset
        rexc_rt::rexc_vector<int> big;
        for (int i = 100; i > 0; --i) big.push_back(i);
        rexc_rt::sort(big.begin(), big.end());
        for (size_t i = 0; i < big.size(); ++i)
            assert(big[i] == static_cast<int>(i + 1));

        std::cout << "rexc_rt::sort (introsort): OK\n";
    }

    // ── Test 54: rexc_unique_ptr — construction, auto-destruction ────
    {
        // Use a simple struct to test
        struct Counter {
            int* destroyed;
            Counter(int* d) : destroyed(d) {}
            ~Counter() { if (destroyed) *destroyed += 1; }
        };

        int destroy_count = 0;
        {
            void* mem = rexc_rt::rexc_malloc(sizeof(Counter));
            Counter* raw = new (mem) Counter(&destroy_count);
            rexc_rt::rexc_unique_ptr<Counter> ptr(raw);
            assert(ptr.get() != nullptr);
            assert(ptr->destroyed == &destroy_count);
        }
        assert(destroy_count == 1);

        // Test release
        destroy_count = 0;
        Counter* released = nullptr;
        {
            void* mem = rexc_rt::rexc_malloc(sizeof(Counter));
            Counter* raw = new (mem) Counter(&destroy_count);
            rexc_rt::rexc_unique_ptr<Counter> ptr(raw);
            released = ptr.release();
        }
        assert(destroy_count == 0);  // not destroyed — we released it
        released->~Counter();
        rexc_rt::rexc_free(released);

        // Test shared_ptr basic ref counting
        {
            void* mem = rexc_rt::rexc_malloc(sizeof(int));
            int* raw = new (mem) int(42);

            rexc_rt::rexc_shared_ptr<int> sp1(raw);
            assert(sp1.use_count() == 1);
            assert(*sp1 == 42);

            {
                rexc_rt::rexc_shared_ptr<int> sp2(sp1);
                assert(sp2.use_count() == 2);
                assert(sp1.use_count() == 2);
            }
            assert(sp1.use_count() == 1);
        }

        std::cout << "rexc_unique_ptr / rexc_shared_ptr: OK\n";
    }

    // ── Test 55: rexc_cstring / rexc_cstdlib functions ───────────────
    {
        // strlen
        assert(rexc_rt::rexc_strlen("hello") == 5);
        assert(rexc_rt::rexc_strlen("") == 0);

        // strcmp
        assert(rexc_rt::rexc_strcmp("abc", "abc") == 0);
        assert(rexc_rt::rexc_strcmp("abc", "abd") < 0);
        assert(rexc_rt::rexc_strcmp("abd", "abc") > 0);

        // strncmp
        assert(rexc_rt::rexc_strncmp("abcdef", "abcxyz", 3) == 0);
        assert(rexc_rt::rexc_strncmp("abcdef", "abcxyz", 4) != 0);

        // memcpy / memset
        char buf[16] = {};
        rexc_rt::rexc_memset(buf, 'A', 8);
        assert(buf[0] == 'A' && buf[7] == 'A' && buf[8] == '\0');

        char dst[16] = {};
        rexc_rt::rexc_memcpy(dst, "Hello", 5);
        assert(dst[0] == 'H' && dst[4] == 'o');

        // atoi / atof
        assert(rexc_rt::rexc_atoi("42") == 42);
        assert(rexc_rt::rexc_atoi("-7") == -7);
        assert(rexc_rt::rexc_atoi("  100") == 100);

        double f = rexc_rt::rexc_atof("3.14");
        assert(f > 3.13 && f < 3.15);

        // rand / srand
        rexc_rt::rexc_srand(12345);
        int r1 = rexc_rt::rexc_rand();
        int r2 = rexc_rt::rexc_rand();
        assert(r1 != r2);  // very unlikely to be equal
        assert(r1 >= 0);
        assert(r2 >= 0);

        // Reproducibility
        rexc_rt::rexc_srand(12345);
        assert(rexc_rt::rexc_rand() == r1);

        std::cout << "rexc_cstring / rexc_cstdlib: OK\n";
    }

    // ── Test 56: Exception handling (setjmp/longjmp) ─────────────────
    // Isolated in a separate function so that the compiler generates
    // proper SEH unwind metadata on Windows x64 (MinGW-w64).  In a
    // very large main(), the unwind tables can become unreliable; a
    // dedicated function keeps setjmp/longjmp in a small, well-formed
    // unwind scope.
    {
        auto ok = test_exception_handling();
        assert(ok);
        std::cout << "Exception handling (setjmp/longjmp): OK\n";
    }

    // ── Test 57: Complete program — string, vector, optional (rexc_rt)
    {
        // Simulate a complete program using rexc_rt types
        rexc_rt::rexc_vector<rexc_rt::rexc_string> names;
        names.push_back(rexc_rt::rexc_string("Alice"));
        names.push_back(rexc_rt::rexc_string("Bob"));
        names.push_back(rexc_rt::rexc_string("Charlie"));

        // Optional holding a value
        rexc_rt::rexc_optional<int> opt_val(42);
        assert(opt_val.has_value());
        assert(opt_val.value() == 42);

        // Optional empty
        rexc_rt::rexc_optional<int> opt_empty;
        assert(!opt_empty.has_value());
        assert(opt_empty.value_or(-1) == -1);

        // Use find algorithm
        auto* found = rexc_rt::find(names.begin(), names.end(),
                                     rexc_rt::rexc_string("Bob"));
        assert(found != names.end());
        assert(*found == rexc_rt::rexc_string("Bob"));

        auto* not_found = rexc_rt::find(names.begin(), names.end(),
                                         rexc_rt::rexc_string("Dave"));
        assert(not_found == names.end());

        // Pair
        auto p = rexc_rt::make_pair(rexc_rt::rexc_string("key"), 100);
        assert(p.first == rexc_rt::rexc_string("key"));
        assert(p.second == 100);

        std::cout << "Complete program (string, vector, optional): OK\n";
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 5 tests (v1.0): Integration, Register Allocator, Full
    // ══════════════════════════════════════════════════════════════

    // ── Test 58: Loop with conditionals — native IR pipeline ───────
    {
        const char* src = R"(
int main() {
    int sum = 0;
    int i = 1;
    while (i <= 10) {
        if (i > 5) {
            sum = sum + i;
        }
        i = i + 1;
    }
    return sum;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "loop_cond.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        rexc::IRModule mod = rexc::NativeCodeGenerator::generate_ir(*tu, sem.context(), true);
        assert(!mod.functions.empty());

        // Should have main function with multiple blocks (while + if create branches)
        bool found_main = false;
        for (auto& fn : mod.functions) {
            if (fn.name == "main") {
                found_main = true;
                assert(fn.blocks.size() > 1);  // branches create multiple blocks
            }
        }
        assert(found_main);

        std::cout << "Loop+conditional IR pipeline: OK\n";
    }

    // ── Test 59: Classes + inheritance in semantic pipeline ───────────
    {
        const char* src = R"(
class Vehicle {
public:
    int speed;
    virtual int maxSpeed() { return 100; }
};

class Car : public Vehicle {
public:
    int doors;
    int maxSpeed() { return 200; }
};

class Truck : public Vehicle {
public:
    int payload;
    int maxSpeed() { return 120; }
};

int main() {
    return 0;
}
)";
        std::vector<rexc::Diagnostic> diags;
        auto tu = rexc::parse_translation_unit(src, "vehicle_test.cpp", diags);
        assert(tu != nullptr);

        rexc::SemanticAnalyzer sem;
        sem.analyze(*tu);

        assert(sem.context().classes.count("Vehicle"));
        assert(sem.context().classes.count("Car"));
        assert(sem.context().classes.count("Truck"));

        // Build layouts for all
        rexc::ObjectLayoutBuilder builder;
        for (auto& decl : tu->decls) {
            if (decl->kind == rexc::NodeKind::ClassDecl) {
                auto* cd = decl->as<const rexc::ClassDecl>();
                rexc::ClassLayout layout = builder.build(*cd, sem.context());
                if (cd->name == "Vehicle" || cd->name == "Car" || cd->name == "Truck") {
                    assert(layout.has_vtable);
                    // All should have maxSpeed in vtable
                    bool has_max = false;
                    for (auto& e : layout.vtable) {
                        if (e.method_name == "maxSpeed") has_max = true;
                    }
                    assert(has_max);
                }
            }
        }

        std::cout << "Classes + inheritance + virtual: OK\n";
    }

    // ── Test 60: rexc_vector<rexc_string> + sort ─────────────────────
    {
        rexc_rt::rexc_vector<rexc_rt::rexc_string> words;
        words.push_back(rexc_rt::rexc_string("banana"));
        words.push_back(rexc_rt::rexc_string("apple"));
        words.push_back(rexc_rt::rexc_string("cherry"));
        words.push_back(rexc_rt::rexc_string("date"));
        words.push_back(rexc_rt::rexc_string("elderberry"));

        rexc_rt::sort(words.begin(), words.end());

        assert(words[0] == rexc_rt::rexc_string("apple"));
        assert(words[1] == rexc_rt::rexc_string("banana"));
        assert(words[2] == rexc_rt::rexc_string("cherry"));
        assert(words[3] == rexc_rt::rexc_string("date"));
        assert(words[4] == rexc_rt::rexc_string("elderberry"));

        std::cout << "rexc_vector<rexc_string> + sort: OK\n";
    }

    // ── Test 61: rexc_function — type-erased callable ────────────────
    {
        // Lambda-like: function pointer
        rexc_rt::rexc_function<int(int, int)> add_fn([](int a, int b) { return a + b; });
        assert(add_fn(3, 4) == 7);
        assert(add_fn(10, 20) == 30);

        // Stateful callable (closure-like)
        int offset = 100;
        rexc_rt::rexc_function<int(int)> add_offset([offset](int x) { return x + offset; });
        assert(add_offset(5) == 105);
        assert(add_offset(0) == 100);

        // Copy
        rexc_rt::rexc_function<int(int)> copy_fn(add_offset);
        assert(copy_fn(10) == 110);

        // Null function
        rexc_rt::rexc_function<void()> null_fn;
        assert(!null_fn);

        // Assign
        rexc_rt::rexc_function<int(int)> reassign;
        reassign = [](int x) { return x * 2; };
        assert(reassign(5) == 10);

        std::cout << "rexc_function (type-erased callable): OK\n";
    }

    // ── Test 62: rexc_optional — template class ──────────────────────
    {
        // int optional
        rexc_rt::rexc_optional<int> opt1(42);
        assert(opt1.has_value());
        assert(*opt1 == 42);

        rexc_rt::rexc_optional<int> opt2;
        assert(!opt2.has_value());
        assert(opt2.value_or(0) == 0);

        opt2 = 99;
        assert(opt2.has_value());
        assert(*opt2 == 99);

        // rexc_string optional
        rexc_rt::rexc_optional<rexc_rt::rexc_string> opt_str(rexc_rt::rexc_string("hello"));
        assert(opt_str.has_value());
        assert(opt_str->size() == 5);

        rexc_rt::rexc_optional<rexc_rt::rexc_string> opt_str2;
        assert(!opt_str2.has_value());

        // Reset
        opt_str.reset();
        assert(!opt_str.has_value());

        // Copy
        rexc_rt::rexc_optional<int> opt3(opt1);
        assert(opt3.has_value());
        assert(*opt3 == 42);

        // Move
        rexc_rt::rexc_optional<int> opt4(static_cast<rexc_rt::rexc_optional<int>&&>(opt3));
        assert(opt4.has_value());
        assert(*opt4 == 42);

        // nullopt
        rexc_rt::rexc_optional<int> opt5(rexc_rt::nullopt);
        assert(!opt5.has_value());

        std::cout << "rexc_optional<T> (template class): OK\n";
    }

    // ── Test 63: Lambda-like callables via rexc_function ─────────────
    {
        // Simulate a lambda: [capture](params) -> ret { body }
        // Using rexc_function as the storage mechanism

        // "Lambda" that captures a vector and returns its size
        rexc_rt::rexc_vector<int> data;
        data.push_back(1); data.push_back(2); data.push_back(3);

        int data_size = static_cast<int>(data.size());
        rexc_rt::rexc_function<int()> get_size([data_size]() { return data_size; });
        assert(get_size() == 3);

        // Higher-order: pass function as argument
        auto apply = [](rexc_rt::rexc_function<int(int)>& f, int val) -> int {
            return f(val);
        };

        rexc_rt::rexc_function<int(int)> double_it([](int x) { return x * 2; });
        assert(apply(double_it, 21) == 42);

        rexc_rt::rexc_function<int(int)> negate_it([](int x) { return -x; });
        assert(apply(negate_it, 5) == -5);

        std::cout << "Lambda-like callables: OK\n";
    }

    // ── Test 64: Exception with recovery ─────────────────────────────
    {
        auto ok = test_exception_recovery();
        assert(ok);
        std::cout << "Exception with recovery: OK\n";
    }

    // ── Test 65: has_native_backend() on current platform ────────────
    {
        // On x86_64 Linux (CI environment), native backend should be available
        bool available = rexc::has_native_backend();
#if defined(__x86_64__) && defined(__linux__)
        assert(available == true);
#endif
        // On any platform, the function should not crash
        std::cout << "has_native_backend(): OK (available="
                  << (available ? "true" : "false") << ")\n";
    }

    // ── Test 66: Graph coloring register allocator ───────────────────
    {
        // Build a small IR function and run register allocation
        rexc::IRFunction fn;
        fn.name = "test_regalloc";
        fn.return_type = rexc::IRType::Int32;
        fn.new_block("entry");

        // %1 = const i32 10
        rexc::IRInstr c1;
        c1.op = rexc::IROp::Const;
        c1.result = fn.new_value(rexc::IRType::Int32, "a");
        c1.const_int = 10;
        fn.current().instrs.push_back(c1);

        // %2 = const i32 20
        rexc::IRInstr c2;
        c2.op = rexc::IROp::Const;
        c2.result = fn.new_value(rexc::IRType::Int32, "b");
        c2.const_int = 20;
        fn.current().instrs.push_back(c2);

        // %3 = add i32 %1, %2
        rexc::IRInstr add;
        add.op = rexc::IROp::Add;
        add.result = fn.new_value(rexc::IRType::Int32, "sum");
        add.operands = {c1.result, c2.result};
        fn.current().instrs.push_back(add);

        // %4 = const i32 5
        rexc::IRInstr c3;
        c3.op = rexc::IROp::Const;
        c3.result = fn.new_value(rexc::IRType::Int32, "c");
        c3.const_int = 5;
        fn.current().instrs.push_back(c3);

        // %5 = mul i32 %3, %4
        rexc::IRInstr mul;
        mul.op = rexc::IROp::Mul;
        mul.result = fn.new_value(rexc::IRType::Int32, "prod");
        mul.operands = {add.result, c3.result};
        fn.current().instrs.push_back(mul);

        // ret i32 %5
        rexc::IRInstr ret;
        ret.op = rexc::IROp::Ret;
        ret.operands = {mul.result};
        fn.current().instrs.push_back(ret);

        // Run register allocation with 14 registers (x86-64 default)
        rexc::GraphColoringAllocator alloc;
        auto result = alloc.allocate(fn, rexc::kTotalAllocatable);

        // All 5 values should be allocated (either reg or spill)
        size_t total_allocated = result.reg_map.size() + result.spill_map.size();
        assert(total_allocated >= 5);

        // With 14 registers, no spills should be needed for 5 values
        assert(result.spill_map.empty());
        assert(result.total_spill_size == 0);

        // Each allocated register should be in [0, 14)
        for (auto& [vid, reg] : result.reg_map) {
            assert(reg >= 0 && reg < rexc::kTotalAllocatable);
        }

        // Interfering values should have different registers
        // %1 and %2 are both live at the add instruction
        if (result.reg_map.count(c1.result.id) && result.reg_map.count(c2.result.id)) {
            assert(result.reg_map[c1.result.id] != result.reg_map[c2.result.id]);
        }

        // Test with very few registers to force spilling
        auto spill_result = alloc.allocate(fn, 2);
        // With only 2 registers, some values must be spilled
        assert(spill_result.spill_map.size() > 0 || spill_result.reg_map.size() <= 2);

        std::cout << "Graph coloring register allocator: OK (regs="
                  << result.reg_map.size() << " spills="
                  << result.spill_map.size() << ")\n";
    }

    // ── i18n and signal tests ──────────────────────────────────────
    {
        // ── i18n: English messages ─────────────────────────────────
        rex_i18n::current_lang() = rex_i18n::Lang::EN;
        assert(rex_i18n::msg("run_compilation_failed") == "Compilation failed.");
        assert(rex_i18n::msg("run_exit_code") == "Exit code: ");
        assert(rex_i18n::msg("signal_interrupted") == "Process interrupted by user (Ctrl+C).");
        assert(rex_i18n::msg("banner_tagline") == " Compile. Run. Execute.");
        std::cout << "i18n English messages: OK\n";

        // ── i18n: Portuguese messages ──────────────────────────────
        rex_i18n::current_lang() = rex_i18n::Lang::PT;
        assert(rex_i18n::msg("run_compilation_failed") == "Compila\u00e7\u00e3o falhou.");
        assert(rex_i18n::msg("run_exit_code") == "C\u00f3digo de sa\u00edda: ");
        assert(rex_i18n::msg("signal_interrupted") == "Processo interrompido pelo usu\u00e1rio (Ctrl+C).");
        assert(rex_i18n::msg("banner_tagline") == " Compilar. Executar. Rodar.");
        std::cout << "i18n Portuguese messages: OK\n";

        // ── i18n: Fallback for missing keys ────────────────────────
        rex_i18n::current_lang() = rex_i18n::Lang::EN;
        assert(rex_i18n::msg("nonexistent_key_xyz") == "nonexistent_key_xyz");
        std::cout << "i18n fallback for missing keys: OK\n";

        // ── i18n: set_language / current_lang_name ─────────────────
        rex_i18n::set_language("pt");
        assert(rex_i18n::current_lang_name() == "pt");
        rex_i18n::set_language("en");
        assert(rex_i18n::current_lang_name() == "en");
        rex_i18n::set_language("pt-BR");
        assert(rex_i18n::current_lang_name() == "pt");
        rex_i18n::set_language("en-US");
        assert(rex_i18n::current_lang_name() == "en");
        std::cout << "i18n set_language: OK\n";

        // ── signal: exit code decoding ─────────────────────────────
        // Normal exit code 0
        assert(rex_signal::decode_exit_code(0) == 0);
#ifdef _WIN32
        // On Windows, system() returns the exit code directly (no encoding)
        assert(rex_signal::decode_exit_code(42) == 42);
        // On Windows, Ctrl+C yields STATUS_CONTROL_C_EXIT (0xC000013A)
        assert(rex_signal::was_signaled(static_cast<int>(0xC000013A)));
        assert(rex_signal::signal_number(static_cast<int>(0xC000013A)) == 2); // SIGINT
#else
        // Simulated exit code 42: system() encodes as 42 << 8 on POSIX
        assert(rex_signal::decode_exit_code(42 << 8) == 42);
        // Signal exit code 130 (128+2 = SIGINT via shell) encoded by system()
        int raw130 = 130 << 8;
        assert(rex_signal::was_signaled(raw130));
        assert(rex_signal::signal_number(raw130) == 2); // SIGINT
#endif
        std::cout << "signal exit code decoding: OK\n";

        // Reset to English
        rex_i18n::current_lang() = rex_i18n::Lang::EN;
    }

    std::cout << "\nAll tests passed!\n";
    return 0;
}
