/*
 * test_interpreter.cpp  –  REX Interpreter Tests (20 comprehensive tests)
 */

#include "interpreter.hpp"
#include <cassert>
#include <sstream>
#include <iostream>

using namespace rex::interp;

static int tests_passed = 0;
static int tests_failed = 0;

void assert_output(const std::string& code, const std::string& expected,
                   const std::string& name) {
    Interpreter interp;
    std::ostringstream buf;
    auto* old = std::cout.rdbuf(buf.rdbuf());
    try {
        interp.run_string(code);
    } catch (const std::exception& e) {
        std::cout.rdbuf(old);
        std::cerr << "FAILED: " << name << "\n"
                  << "  exception: " << e.what() << "\n";
        tests_failed++;
        return;
    }
    std::cout.rdbuf(old);
    if (buf.str() != expected) {
        std::cerr << "FAILED: " << name << "\n"
                  << "  expected: [" << expected << "]\n"
                  << "  got:      [" << buf.str() << "]\n";
        tests_failed++;
        return;
    }
    std::cout << "OK: " << name << "\n";
    tests_passed++;
}

void assert_throws(const std::string& code, const std::string& msg_contains,
                   const std::string& name) {
    Interpreter interp;
    std::ostringstream buf;
    auto* old = std::cout.rdbuf(buf.rdbuf());
    try {
        interp.run_string(code);
        std::cout.rdbuf(old);
        std::cerr << "FAILED: " << name << " (should have thrown)\n";
        tests_failed++;
        return;
    } catch (const RuntimeError& e) {
        std::cout.rdbuf(old);
        if (std::string(e.what()).find(msg_contains) != std::string::npos) {
            std::cout << "OK (expected error): " << name << "\n";
            tests_passed++;
        } else {
            std::cerr << "FAILED: " << name << "\n"
                      << "  expected error containing: [" << msg_contains << "]\n"
                      << "  got: [" << e.what() << "]\n";
            tests_failed++;
        }
    } catch (const ThrowSignal&) {
        std::cout.rdbuf(old);
        std::cerr << "FAILED: " << name << " (unhandled ThrowSignal)\n";
        tests_failed++;
    } catch (const std::exception& e) {
        std::cout.rdbuf(old);
        if (std::string(e.what()).find(msg_contains) != std::string::npos) {
            std::cout << "OK (expected error): " << name << "\n";
            tests_passed++;
        } else {
            std::cerr << "FAILED: " << name << "\n"
                      << "  expected error containing: [" << msg_contains << "]\n"
                      << "  got: [" << e.what() << "]\n";
            tests_failed++;
        }
    }
}

int main() {
    std::cout << "=== REX Interpreter Tests ===\n\n";

    // 1. Arithmetic
    assert_output("cout << 2+3*4 << endl;", "14\n", "1. arithmetic");

    // 2. Parentheses
    assert_output("cout << (2+3)*4 << endl;", "20\n", "2. parentheses");

    // 3. Integer division
    assert_output("cout << 10/3 << endl;", "3\n", "3. integer division");

    // 4. Integer variables
    assert_output("int x=42; cout<<x<<endl;", "42\n", "4. int variable");

    // 5. String variables
    assert_output("string s=\"oi\"; cout<<s<<endl;", "oi\n", "5. string variable");

    // 6. if (true)
    assert_output("if(5>3) cout<<\"sim\"<<endl;", "sim\n", "6. if true");

    // 7. if/else
    assert_output("if(1>2) cout<<\"x\"<<endl; else cout<<\"ok\"<<endl;", "ok\n", "7. if else");

    // 8. while loop
    assert_output("int i=0; while(i<3){cout<<i<<endl;i++;}", "0\n1\n2\n", "8. while");

    // 9. for loop
    assert_output("for(int i=0;i<3;i++) cout<<i<<endl;", "0\n1\n2\n", "9. for");

    // 10. User function
    assert_output(R"(
        int dobro(int n) { return n*2; }
        cout << dobro(7) << endl;
    )", "14\n", "10. function");

    // 11. Recursion (Fibonacci)
    assert_output(R"(
        int fib(int n){ if(n<=1) return n; return fib(n-1)+fib(n-2); }
        cout << fib(10) << endl;
    )", "55\n", "11. fibonacci");

    // 12. Basic class
    assert_output(R"(
        class Ponto { public:
            int x; int y;
            Ponto(int a, int b) : x(a), y(b) {}
            int soma() { return x+y; }
        };
        Ponto p(3,4); cout << p.soma() << endl;
    )", "7\n", "12. class");

    // 13. Inheritance
    assert_output(R"(
        class Animal { public: string fala(){ return "..."; } };
        class Gato : public Animal { public: string fala(){ return "Miau!"; } };
        Gato g; cout << g.fala() << endl;
    )", "Miau!\n", "13. inheritance");

    // 14. vector
    assert_output(R"(
        vector<int> v;
        v.push_back(10); v.push_back(20); v.push_back(30);
        cout << v.size() << endl;
        for(int i=0;i<v.size();i++) cout<<v[i]<<endl;
    )", "3\n10\n20\n30\n", "14. vector");

    // 15. Lambda
    assert_output(R"(
        auto mult = [](int a, int b){ return a*b; };
        cout << mult(6,7) << endl;
    )", "42\n", "15. lambda");

    // 16. Lambda with capture
    assert_output(R"(
        int base=100;
        auto add=[base](int n){ return base+n; };
        cout << add(5) << endl;
    )", "105\n", "16. lambda capture");

    // 17. try/catch
    assert_output(R"(
        try { throw 42; }
        catch(int e){ cout << "capturado: " << e << endl; }
    )", "capturado: 42\n", "17. try catch");

    // 18. range-for
    assert_output(R"(
        vector<int> v={1,2,3,4,5};
        int soma=0; for(auto x:v) soma+=x;
        cout << soma << endl;
    )", "15\n", "18. range-for");

    // 19. main() auto-execution
    assert_output(R"(
        int main(){ cout << "Hello!" << endl; return 0; }
    )", "Hello!\n", "19. main auto");

    // 20. FizzBuzz
    assert_output(R"(
        for(int i=1;i<=5;i++){
            if(i%15==0) cout<<"FizzBuzz"<<endl;
            else if(i%3==0) cout<<"Fizz"<<endl;
            else if(i%5==0) cout<<"Buzz"<<endl;
            else cout<<i<<endl;
        }
    )", "1\n2\nFizz\n4\nBuzz\n", "20. FizzBuzz");

    // 21. to_string builtin
    assert_output(R"(
        string s = to_string(42);
        cout << s << endl;
    )", "42\n", "21. to_string int");

    // 22. to_string float
    assert_output(R"(
        string s = to_string(3.14);
        cout << s << endl;
    )", "3.14\n", "22. to_string float");

    // 23. stoi builtin
    assert_output(R"(
        int n = stoi("123");
        cout << n + 1 << endl;
    )", "124\n", "23. stoi");

    // 24. stof builtin
    assert_output(R"(
        double x = stof("2.5");
        cout << x + 0.5 << endl;
    )", "3\n", "24. stof");

    // 25. printf with %d and %s
    assert_output(R"(
        printf("hello %s, you are %d years old\n", "world", 7);
    )", "hello world, you are 7 years old\n", "25. printf %s %d");

    // 26. printf with %%
    assert_output(R"(
        printf("100%%\n");
    )", "100%\n", "26. printf %%");

    // 27. sleep_for 0ms (no-op, just verify it does not throw)
    assert_output(R"(
        sleep_for(0);
        cout << "ok" << endl;
    )", "ok\n", "27. sleep_for 0ms");

    std::cout << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    if (tests_failed > 0) {
        std::cout << "SOME TESTS FAILED!\n";
        return 1;
    }
    std::cout << "All tests passed!\n";
    return 0;
}
