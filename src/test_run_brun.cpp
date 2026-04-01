/*
 * test_run_brun.cpp  –  REX end-to-end tests: interpreter (run) vs compiler (brun)
 *
 * Runs the same C++ programs through both:
 *   - Interpreter  : rex::interp::Interpreter::run_string()   (rex run)
 *   - Compiler     : compile_file() → execute binary          (rex brun)
 *
 * Each test case specifies the expected output (standard C++ behaviour).
 * Both paths are checked independently and any mismatch is reported.
 *
 * Build:
 *   g++ src/test_run_brun.cpp \
 *       src/interpreter/interpreter.cpp \
 *       src/interpreter/builtins.cpp \
 *       -I src -std=c++20 -O2 \
 *       -o build/test_run_brun \
 *       -Wno-reorder -Wno-unused-variable \
 *       -Wno-unused-but-set-variable -Wno-misleading-indentation
 *
 * Run:
 *   ./build/test_run_brun
 */

#include "interpreter/interpreter.hpp"
#include "compiler.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

#ifdef _WIN32
#  include <io.h>
#  define POPEN  _popen
#  define PCLOSE _pclose
#else
#  define POPEN  popen
#  define PCLOSE pclose
#endif

// ─── ANSI helpers (mirrors utils.hpp) ────────────────────────────
#define T_GREEN  "\033[32m"
#define T_RED    "\033[31m"
#define T_YELLOW "\033[33m"
#define T_CYAN   "\033[36m"
#define T_BOLD   "\033[1m"
#define T_RESET  "\033[0m"

// ─── Global counters ──────────────────────────────────────────────
static int run_passed  = 0, run_failed  = 0;
static int brun_passed = 0, brun_failed = 0;

// ═══════════════════════════════════════════════════════════════════
//  Helper: run code through the interpreter, capture stdout
// ═══════════════════════════════════════════════════════════════════
static std::string interp_run(const std::string& code) {
    rex::interp::Interpreter interp;
    std::ostringstream buf;
    auto* old_cout = std::cout.rdbuf(buf.rdbuf());
    try {
        interp.run_string(code);
    } catch (const std::exception&) {
        // interpreter error – leave buffer as-is
    } catch (...) {}
    std::cout.rdbuf(old_cout);
    return buf.str();
}

// ═══════════════════════════════════════════════════════════════════
//  Helper: compile + run a program, capture stdout
//
//  Returns special sentinel strings on failure:
//    "COMPILE_ERROR"  – compile_file() returned success=false
//    "TIMEOUT"        – binary ran for more than timeout_secs
//    "CRASH:<signal>" – binary terminated by a signal
// ═══════════════════════════════════════════════════════════════════
static std::string compiler_run(const std::string& code,
                                 int timeout_secs = 5) {
    // Write source to a temporary file
    static int tmp_counter = 0;
    fs::path tmp_dir = fs::temp_directory_path();
    std::string src_path = (tmp_dir / ("rex_test_" + std::to_string(++tmp_counter) + ".cpp")).string();
    std::string bin_path = (tmp_dir / ("rex_test_bin_" + std::to_string(tmp_counter))).string();

    {
        std::ofstream f(src_path);
        if (!f) return "COMPILE_ERROR";
        f << code;
    }

    // Compile (suppress all [REX] banner output during compilation)
    BuildResult result;
    {
        std::ostringstream null_buf;
        auto* old_cout = std::cout.rdbuf(null_buf.rdbuf());
        auto* old_cerr = std::cerr.rdbuf(null_buf.rdbuf());
        result = compile_file(src_path, bin_path);
        std::cout.rdbuf(old_cout);
        std::cerr.rdbuf(old_cerr);
    }

    if (!result.success) return "COMPILE_ERROR";

    // Run with a timeout and capture stdout
#ifdef _WIN32
    std::string cmd = "\"\"" + result.binary + "\"\" 2>NUL";
#else
    std::string cmd = "timeout " + std::to_string(timeout_secs)
                    + " \"" + result.binary + "\" 2>/dev/null";
#endif

    FILE* pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) return "COMPILE_ERROR";

    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe))
        output += buf;

    int rc = PCLOSE(pipe);

#ifndef _WIN32
    if (WIFEXITED(rc)) {
        int code_exit = WEXITSTATUS(rc);
        if (code_exit == 124) return "TIMEOUT";
    } else if (WIFSIGNALED(rc)) {
        return "CRASH(signal " + std::to_string(WTERMSIG(rc)) + ")";
    }
#endif

    return output;
}

// ─── Max display length for output values in the report ──────────
static const size_t MAX_OUTPUT_DISPLAY_LEN = 80;

// ═══════════════════════════════════════════════════════════════════
//  Helper: pretty-print a string for the output report
// ═══════════════════════════════════════════════════════════════════
static std::string show(const std::string& s) {
    if (s.empty()) return "(empty)";
    // Replace newlines with ↵ for compact display
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else           out += c;
    }
    // Truncate very long outputs
    if (out.size() > MAX_OUTPUT_DISPLAY_LEN)
        out = out.substr(0, MAX_OUTPUT_DISPLAY_LEN - 3) + "...";
    return "\"" + out + "\"";
}

// ═══════════════════════════════════════════════════════════════════
//  Single test: run + brun, compare against expected, print result
// ═══════════════════════════════════════════════════════════════════
static void run_test(const std::string& name,
                     const std::string& code,
                     const std::string& expected) {
    std::cout << T_BOLD << "Test: " << name << T_RESET << "\n";

    // ── interpreter (run) ─────────────────────────────────────────
    std::string interp_out = interp_run(code);
    bool interp_ok = (interp_out == expected);
    if (interp_ok) {
        std::cout << "  [run]  " << T_GREEN << "PASS" << T_RESET
                  << " → " << show(interp_out) << "\n";
        run_passed++;
    } else {
        std::cout << "  [run]  " << T_RED << "FAIL" << T_RESET
                  << "\n    expected: " << show(expected)
                  << "\n    got:      " << show(interp_out) << "\n";
        run_failed++;
    }

    // ── compiler (brun) ───────────────────────────────────────────
    std::string comp_out = compiler_run(code);
    bool comp_ok = (comp_out == expected);
    if (comp_ok) {
        std::cout << "  [brun] " << T_GREEN << "PASS" << T_RESET
                  << " → " << show(comp_out) << "\n";
        brun_passed++;
    } else {
        std::string tag;
        if (comp_out == "COMPILE_ERROR") tag = T_YELLOW " [COMPILE_ERROR]" T_RESET;
        else if (comp_out == "TIMEOUT")  tag = T_YELLOW " [TIMEOUT]" T_RESET;
        else if (comp_out.rfind("CRASH", 0) == 0) tag = T_YELLOW " [" + comp_out + "]" T_RESET;

        std::cout << "  [brun] " << T_RED << "FAIL" << T_RESET << tag
                  << "\n    expected: " << show(expected)
                  << "\n    got:      " << show(comp_out) << "\n";
        brun_failed++;
    }
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════
int main() {
    std::cout << T_BOLD T_CYAN
              << "=== REX run (interpreter) vs brun (compiler) Tests ===\n"
              << T_RESET << "\n";

    // ── 1. Hello World ───────────────────────────────────────────
    run_test("01. Hello World",
R"(
#include <iostream>
int main() {
    std::cout << "Hello World" << "\n";
    return 0;
}
)", "Hello World\n");

    // ── 2. Integer arithmetic (+, -, *) ──────────────────────────
    run_test("02. Integer arithmetic (+, -, *)",
R"(
#include <iostream>
int main() {
    int a = 10, b = 3;
    std::cout << a + b << "\n";
    std::cout << a - b << "\n";
    std::cout << a * b << "\n";
    return 0;
}
)", "13\n7\n30\n");

    // ── 3. Integer division and modulo ───────────────────────────
    run_test("03. Integer division and modulo",
R"(
#include <iostream>
int main() {
    int a = 10, b = 3;
    std::cout << a / b << "\n";
    std::cout << a % b << "\n";
    return 0;
}
)", "3\n1\n");

    // ── 4. Floating point ─────────────────────────────────────────
    run_test("04. Floating point arithmetic",
R"(
#include <iostream>
int main() {
    double a = 3.14, b = 2.0;
    std::cout << a * b << "\n";
    return 0;
}
)", "6.28\n");

    // ── 5. String variable ───────────────────────────────────────
    run_test("05. String variable and output",
R"(
#include <iostream>
#include <string>
int main() {
    std::string s = "Ola Mundo";
    std::cout << s << "\n";
    return 0;
}
)", "Ola Mundo\n");

    // ── 6. String concatenation ───────────────────────────────────
    run_test("06. String concatenation",
R"(
#include <iostream>
#include <string>
int main() {
    std::string a = "Ola";
    std::string b = " Mundo";
    std::string c = a + b;
    std::cout << c << "\n";
    return 0;
}
)", "Ola Mundo\n");

    // ── 7. if / else ──────────────────────────────────────────────
    run_test("07. if/else branching",
R"(
#include <iostream>
int main() {
    int x = 5;
    if (x > 3)
        std::cout << "maior\n";
    else
        std::cout << "menor\n";
    if (x < 3)
        std::cout << "menor\n";
    else
        std::cout << "nao menor\n";
    return 0;
}
)", "maior\nnao menor\n");

    // ── 8. while loop ─────────────────────────────────────────────
    run_test("08. while loop",
R"(
#include <iostream>
int main() {
    int i = 0;
    while (i < 5) {
        std::cout << i << "\n";
        i++;
    }
    return 0;
}
)", "0\n1\n2\n3\n4\n");

    // ── 9. for loop ───────────────────────────────────────────────
    run_test("09. for loop",
R"(
#include <iostream>
int main() {
    for (int i = 0; i < 5; i++) {
        std::cout << i * i << "\n";
    }
    return 0;
}
)", "0\n1\n4\n9\n16\n");

    // ── 10. Functions ─────────────────────────────────────────────
    run_test("10. User-defined functions",
R"(
#include <iostream>
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int main() {
    std::cout << add(3, 4) << "\n";
    std::cout << mul(3, 4) << "\n";
    return 0;
}
)", "7\n12\n");

    // ── 11. Recursion – factorial ─────────────────────────────────
    run_test("11. Recursion (factorial)",
R"(
#include <iostream>
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
int main() {
    std::cout << factorial(5)  << "\n";
    std::cout << factorial(10) << "\n";
    return 0;
}
)", "120\n3628800\n");

    // ── 12. Recursion – Fibonacci ─────────────────────────────────
    run_test("12. Recursion (fibonacci)",
R"(
#include <iostream>
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
int main() {
    std::cout << fib(10) << "\n";
    return 0;
}
)", "55\n");

    // ── 13. FizzBuzz (loops + conditionals + modulo) ─────────────
    run_test("13. FizzBuzz",
R"(
#include <iostream>
int main() {
    for (int i = 1; i <= 15; i++) {
        if (i % 15 == 0)      std::cout << "FizzBuzz\n";
        else if (i % 3 == 0)  std::cout << "Fizz\n";
        else if (i % 5 == 0)  std::cout << "Buzz\n";
        else                  std::cout << i << "\n";
    }
    return 0;
}
)", "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n");

    // ── 14. Vector push_back and iteration ───────────────────────
    run_test("14. std::vector push_back and loop",
R"(
#include <iostream>
#include <vector>
int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    for (int i = 0; i < (int)v.size(); i++)
        std::cout << v[i] << "\n";
    return 0;
}
)", "10\n20\n30\n");

    // ── 15. Range-based for ───────────────────────────────────────
    run_test("15. Range-based for loop",
R"(
#include <iostream>
#include <vector>
int main() {
    std::vector<int> v = {1, 2, 3, 4, 5};
    int sum = 0;
    for (int x : v) sum += x;
    std::cout << sum << "\n";
    return 0;
}
)", "15\n");

    // ── 16. Class with constructor and method ─────────────────────
    run_test("16. Class with constructor and method",
R"(
#include <iostream>
class Ponto {
public:
    int x, y;
    Ponto(int a, int b) : x(a), y(b) {}
    int soma() { return x + y; }
};
int main() {
    Ponto p(3, 4);
    std::cout << p.soma() << "\n";
    return 0;
}
)", "7\n");

    // ── 17. Inheritance and virtual method ────────────────────────
    run_test("17. Inheritance",
R"(
#include <iostream>
#include <string>
class Animal {
public:
    std::string fala() { return "..."; }
};
class Gato : public Animal {
public:
    std::string fala() { return "Miau!"; }
};
int main() {
    Gato g;
    std::cout << g.fala() << "\n";
    return 0;
}
)", "Miau!\n");

    // ── 18. Multiple output types (int + string in one line) ──────
    run_test("18. Mixed cout (int and string)",
R"(
#include <iostream>
#include <string>
int main() {
    int age = 30;
    std::string name = "sam";
    std::cout << "Nome: " << name << "\n";
    std::cout << "Idade: " << age << "\n";
    return 0;
}
)", "Nome: sam\nIdade: 30\n");

    // ── 19. Nested loops ──────────────────────────────────────────
    run_test("19. Nested loops (multiplication table 3x3)",
R"(
#include <iostream>
int main() {
    for (int i = 1; i <= 3; i++)
        for (int j = 1; j <= 3; j++)
            std::cout << i * j << "\n";
    return 0;
}
)", "1\n2\n3\n2\n4\n6\n3\n6\n9\n");

    // ── 20. Early return from function ────────────────────────────
    run_test("20. Early return / guard clause",
R"(
#include <iostream>
int abs_val(int x) {
    if (x < 0) return -x;
    return x;
}
int main() {
    std::cout << abs_val(-7)  << "\n";
    std::cout << abs_val(5)   << "\n";
    std::cout << abs_val(0)   << "\n";
    return 0;
}
)", "7\n5\n0\n");

    // ═══════════════════════════════════════════════════════════════
    //  Summary
    // ═══════════════════════════════════════════════════════════════
    int run_total  = run_passed  + run_failed;
    int brun_total = brun_passed + brun_failed;

    std::cout << T_BOLD T_CYAN
              << "=== Summary ===\n"
              << T_RESET;
    std::cout << "  run  (interpreter) : "
              << run_passed << "/" << run_total << " passed";
    if (run_failed == 0)
        std::cout << "  " << T_GREEN << "ALL PASS" << T_RESET;
    std::cout << "\n";

    std::cout << "  brun (compiler)    : "
              << brun_passed << "/" << brun_total << " passed";
    if (brun_failed == 0)
        std::cout << "  " << T_GREEN << "ALL PASS" << T_RESET;
    else
        std::cout << "  " << T_YELLOW
                  << "(" << brun_failed << " failed – compiler bugs)" << T_RESET;
    std::cout << "\n\n";

    if (run_failed > 0) {
        std::cout << T_RED << "INTERPRETER FAILURES DETECTED." << T_RESET << "\n";
        return 1;
    }
    if (brun_failed > 0) {
        std::cout << T_YELLOW
                  << "Compiler (brun) has failures – see report above.\n"
                  << T_RESET;
        return 2;
    }
    std::cout << T_GREEN << "All tests passed!\n" << T_RESET;
    return 0;
}
