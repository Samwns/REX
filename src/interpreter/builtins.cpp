/*
 * builtins.cpp  –  REX Interpreter Builtin Implementations
 */

#define _USE_MATH_DEFINES

#include "builtins.hpp"
#include <iostream>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <thread>
#include <chrono>

namespace rex::interp {

// ─── cout / cerr / cin / endl ────────────────────────────────────

ValuePtr make_cout_object() {
    auto obj = Value::make_object("__ostream__", nullptr, nullptr);
    obj->as_object().fields["__is_cout__"] = Value::make_bool(true);
    return obj;
}

ValuePtr make_cerr_object() {
    auto obj = Value::make_object("__ostream__", nullptr, nullptr);
    obj->as_object().fields["__is_cerr__"] = Value::make_bool(true);
    return obj;
}

ValuePtr make_cin_object() {
    auto obj = Value::make_object("__istream__", nullptr, nullptr);
    return obj;
}

ValuePtr make_endl_value() {
    auto obj = Value::make_object("__endl__", nullptr, nullptr);
    return obj;
}

// ─── vector<T> ───────────────────────────────────────────────────

ValuePtr make_vector_object() {
    return Value::make_array();
}

// ─── String methods ──────────────────────────────────────────────

ValuePtr call_string_method(const std::string& str,
                             const std::string& method,
                             std::vector<ValuePtr> args) {
    if (method == "size" || method == "length")
        return Value::make_int((int64_t)str.size());
    if (method == "empty")
        return Value::make_bool(str.empty());
    if (method == "find") {
        if (args.empty()) throw RuntimeError("string::find requires an argument");
        auto pos = str.find(args[0]->to_string());
        return Value::make_int(pos == std::string::npos ? -1 : (int64_t)pos);
    }
    if (method == "substr") {
        if (args.empty()) throw RuntimeError("string::substr requires at least 1 argument");
        size_t start = (size_t)args[0]->as_int();
        size_t len = args.size() > 1 ? (size_t)args[1]->as_int() : std::string::npos;
        return Value::make_str(str.substr(start, len));
    }
    if (method == "at") {
        if (args.empty()) throw RuntimeError("string::at requires an argument");
        size_t idx = (size_t)args[0]->as_int();
        if (idx >= str.size()) throw RuntimeError("string::at index out of range");
        return Value::make_str(std::string(1, str[idx]));
    }
    if (method == "c_str")
        return Value::make_str(str);
    throw RuntimeError("unknown string method: '" + method + "'");
}

// ─── Math builtins ───────────────────────────────────────────────

void register_math_builtins(EnvPtr env) {
    auto reg = [&](const std::string& name, auto fn) {
        env->define(name, Value::make_native(name, fn));
    };

    reg("sqrt", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("sqrt requires 1 argument");
        return Value::make_float(std::sqrt(a[0]->to_number()));
    });
    reg("pow", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.size() < 2) throw RuntimeError("pow requires 2 arguments");
        return Value::make_float(std::pow(a[0]->to_number(), a[1]->to_number()));
    });
    reg("abs", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("abs requires 1 argument");
        if (a[0]->is_int()) return Value::make_int(std::abs(a[0]->as_int()));
        return Value::make_float(std::abs(a[0]->to_number()));
    });
    reg("floor", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("floor requires 1 argument");
        return Value::make_float(std::floor(a[0]->to_number()));
    });
    reg("ceil", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("ceil requires 1 argument");
        return Value::make_float(std::ceil(a[0]->to_number()));
    });
    reg("round", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("round requires 1 argument");
        return Value::make_float(std::round(a[0]->to_number()));
    });
    reg("sin", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("sin requires 1 argument");
        return Value::make_float(std::sin(a[0]->to_number()));
    });
    reg("cos", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("cos requires 1 argument");
        return Value::make_float(std::cos(a[0]->to_number()));
    });
    reg("tan", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("tan requires 1 argument");
        return Value::make_float(std::tan(a[0]->to_number()));
    });
    reg("log", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("log requires 1 argument");
        return Value::make_float(std::log(a[0]->to_number()));
    });
    reg("exp", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("exp requires 1 argument");
        return Value::make_float(std::exp(a[0]->to_number()));
    });

    env->define("M_PI", Value::make_float(M_PI));
    env->define("M_E",  Value::make_float(M_E));
}

// ─── Utility builtins ────────────────────────────────────────────

void register_utility_builtins(EnvPtr env) {
    auto reg = [&](const std::string& name, auto fn) {
        env->define(name, Value::make_native(name, fn));
    };

    reg("to_string", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("to_string requires 1 argument");
        return Value::make_str(a[0]->to_string());
    });
    reg("stoi", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("stoi requires 1 argument");
        return Value::make_int(std::stoll(a[0]->as_str()));
    });
    reg("stof", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("stof requires 1 argument");
        return Value::make_float(std::stod(a[0]->as_str()));
    });
    reg("rand", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::make_int(std::rand());
    });
    reg("srand", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) std::srand((unsigned)std::time(nullptr));
        else std::srand((unsigned)a[0]->as_int());
        return Value::make_null();
    });
    reg("exit", [](std::vector<ValuePtr> a) -> ValuePtr {
        int code = a.empty() ? 0 : (int)a[0]->as_int();
        std::exit(code);
        return Value::make_null(); // unreachable
    });

    // getline(cin, str) — reads a full line from stdin
    reg("getline", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.size() < 2)
            throw RuntimeError("getline requires 2 arguments (stream, string)");
        std::string line;
        std::getline(std::cin, line);
        a[1]->inner = StrVal{line};
        return Value::make_bool(!std::cin.fail());
    });

    // system(cmd) — execute a shell command
    reg("system", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("system requires 1 argument");
        int ret = std::system(a[0]->as_str().c_str());
        return Value::make_int(ret);
    });

    // printf — basic printf support (format string + args)
    reg("printf", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("printf requires at least 1 argument");
        std::string fmt = a[0]->as_str();
        // Simple printf: just print the format string with %d/%s/%f replaced
        size_t arg_idx = 1;
        std::string output;
        for (size_t i = 0; i < fmt.size(); i++) {
            if (fmt[i] == '%' && i + 1 < fmt.size()) {
                char spec = fmt[i + 1];
                if (spec == 'd' || spec == 'i') {
                    if (arg_idx < a.size())
                        output += std::to_string(a[arg_idx++]->as_int());
                    i++;
                } else if (spec == 'f') {
                    if (arg_idx < a.size())
                        output += std::to_string(a[arg_idx++]->to_number());
                    i++;
                } else if (spec == 's') {
                    if (arg_idx < a.size())
                        output += a[arg_idx++]->to_string();
                    i++;
                } else if (spec == '%') {
                    output += '%';
                    i++;
                } else {
                    output += fmt[i];
                }
            } else if (fmt[i] == '\\' && i + 1 < fmt.size()) {
                char esc = fmt[i + 1];
                if (esc == 'n') { output += '\n'; i++; }
                else if (esc == 't') { output += '\t'; i++; }
                else if (esc == '\\') { output += '\\'; i++; }
                else output += fmt[i];
            } else {
                output += fmt[i];
            }
        }
        std::cout << output;
        return Value::make_int((int64_t)output.size());
    });

    // sleep_for — sleep for N milliseconds (useful for animations)
    reg("sleep_for", [](std::vector<ValuePtr> a) -> ValuePtr {
        if (a.empty()) throw RuntimeError("sleep_for requires 1 argument (milliseconds)");
        int ms = (int)a[0]->as_int();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::make_null();
    });
}

// ─── Register all builtins ───────────────────────────────────────

void register_builtins(EnvPtr env) {
    env->define("cout",  make_cout_object());
    env->define("cerr",  make_cerr_object());
    env->define("cin",   make_cin_object());
    env->define("endl",  make_endl_value());
    env->define("true",  Value::make_bool(true));
    env->define("false", Value::make_bool(false));
    env->define("nullptr", Value::make_null());
    env->define("NULL",  Value::make_null());

    register_math_builtins(env);
    register_utility_builtins(env);
}

} // namespace rex::interp
