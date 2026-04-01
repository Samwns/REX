#pragma once
/*
 * value.hpp  –  REX Interpreter Runtime Value System
 *
 * Universal value type that represents any value the interpreter
 * can produce at runtime: null, bool, int, float, string,
 * user functions (closures), native functions, objects, arrays.
 */

#include <variant>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace rex::interp {

struct Value;
struct Environment;

using ValuePtr = std::shared_ptr<Value>;
using EnvPtr   = std::shared_ptr<Environment>;

// ─── Value subtypes ──────────────────────────────────────────────

struct NullVal  {};
struct BoolVal  { bool v; };
struct IntVal   { int64_t v; };
struct FloatVal { double v; };
struct StrVal   { std::string v; };

// User-defined function (stores pointer to FunctionDecl AST + closure)
struct FuncVal {
    std::string              name;
    std::vector<std::string> params;
    const void*              body_ast;    // pointer to FunctionDecl in AST
    EnvPtr                   closure_env;
};

// Native function (builtin implemented in C++)
struct NativeFunc {
    std::string name;
    std::function<ValuePtr(std::vector<ValuePtr>)> fn;
};

// Class instance
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

// ─── RuntimeError (for interpreter errors) ───────────────────────

struct RuntimeError : std::runtime_error {
    int line = 0, col = 0;
    explicit RuntimeError(const std::string& msg, int l = 0, int c = 0)
        : std::runtime_error(msg), line(l), col(c) {}
};

// ─── Value ───────────────────────────────────────────────────────

struct Value {
    using Inner = std::variant<
        NullVal, BoolVal, IntVal, FloatVal, StrVal,
        FuncVal, NativeFunc, ObjectVal, ArrayVal
    >;
    Inner inner;

    // ── Factories ────────────────────────────────────────────────
    static ValuePtr make_null()  { auto v = std::make_shared<Value>(); v->inner = NullVal{}; return v; }
    static ValuePtr make_bool(bool b)   { auto v = std::make_shared<Value>(); v->inner = BoolVal{b}; return v; }
    static ValuePtr make_int(int64_t i) { auto v = std::make_shared<Value>(); v->inner = IntVal{i}; return v; }
    static ValuePtr make_float(double d){ auto v = std::make_shared<Value>(); v->inner = FloatVal{d}; return v; }
    static ValuePtr make_str(const std::string& s) { auto v = std::make_shared<Value>(); v->inner = StrVal{s}; return v; }

    static ValuePtr make_func(const std::string& name,
                               std::vector<std::string> params,
                               const void* body_ast, EnvPtr closure) {
        auto v = std::make_shared<Value>();
        v->inner = FuncVal{name, std::move(params), body_ast, std::move(closure)};
        return v;
    }

    static ValuePtr make_native(const std::string& name,
                                 std::function<ValuePtr(std::vector<ValuePtr>)> fn) {
        auto v = std::make_shared<Value>();
        v->inner = NativeFunc{name, std::move(fn)};
        return v;
    }

    static ValuePtr make_object(const std::string& cls,
                                 const void* cls_ast, EnvPtr method_env) {
        auto v = std::make_shared<Value>();
        v->inner = ObjectVal{cls, {}, cls_ast, std::move(method_env)};
        return v;
    }

    static ValuePtr make_array() {
        auto v = std::make_shared<Value>();
        v->inner = ArrayVal{};
        return v;
    }

    // ── Type queries ─────────────────────────────────────────────
    bool is_null()    const { return std::holds_alternative<NullVal>(inner); }
    bool is_bool()    const { return std::holds_alternative<BoolVal>(inner); }
    bool is_int()     const { return std::holds_alternative<IntVal>(inner); }
    bool is_float()   const { return std::holds_alternative<FloatVal>(inner); }
    bool is_str()     const { return std::holds_alternative<StrVal>(inner); }
    bool is_func()    const { return std::holds_alternative<FuncVal>(inner); }
    bool is_native()  const { return std::holds_alternative<NativeFunc>(inner); }
    bool is_object()  const { return std::holds_alternative<ObjectVal>(inner); }
    bool is_array()   const { return std::holds_alternative<ArrayVal>(inner); }
    bool is_numeric() const { return is_int() || is_float(); }

    // ── Getters (throw RuntimeError on wrong type) ───────────────
    bool        as_bool()   const { if (!is_bool()) throw RuntimeError("expected bool"); return std::get<BoolVal>(inner).v; }
    int64_t     as_int()    const { if (!is_int()) throw RuntimeError("expected int"); return std::get<IntVal>(inner).v; }
    double      as_float()  const { if (!is_float()) throw RuntimeError("expected float"); return std::get<FloatVal>(inner).v; }
    std::string as_str()    const { if (!is_str()) throw RuntimeError("expected string"); return std::get<StrVal>(inner).v; }
    FuncVal&    as_func()         { if (!is_func()) throw RuntimeError("expected function"); return std::get<FuncVal>(inner); }
    const FuncVal& as_func() const { if (!is_func()) throw RuntimeError("expected function"); return std::get<FuncVal>(inner); }
    NativeFunc& as_native()       { if (!is_native()) throw RuntimeError("expected native function"); return std::get<NativeFunc>(inner); }
    ObjectVal&  as_object()       { if (!is_object()) throw RuntimeError("expected object"); return std::get<ObjectVal>(inner); }
    const ObjectVal& as_object() const { if (!is_object()) throw RuntimeError("expected object"); return std::get<ObjectVal>(inner); }
    ArrayVal&   as_array()        { if (!is_array()) throw RuntimeError("expected array"); return std::get<ArrayVal>(inner); }
    const ArrayVal& as_array() const { if (!is_array()) throw RuntimeError("expected array"); return std::get<ArrayVal>(inner); }

    // ── Get numeric value as double ──────────────────────────────
    double to_number() const {
        if (is_int()) return (double)as_int();
        if (is_float()) return as_float();
        throw RuntimeError("expected numeric value");
    }

    // ── Conversion to string (for cout <<) ───────────────────────
    std::string to_string() const {
        if (is_null())  return "null";
        if (is_bool())  return as_bool() ? "true" : "false";
        if (is_int())   return std::to_string(as_int());
        if (is_float()) {
            std::ostringstream oss;
            double d = as_float();
            if (d == (int64_t)d && std::abs(d) < 1e15)
                oss << (int64_t)d;
            else
                oss << d;
            return oss.str();
        }
        if (is_str())   return as_str();
        if (is_func())  return "<function " + std::get<FuncVal>(inner).name + ">";
        if (is_native()) return "<native " + std::get<NativeFunc>(inner).name + ">";
        if (is_object()) return "<" + std::get<ObjectVal>(inner).class_name + " object>";
        if (is_array()) {
            std::string r = "[";
            auto& elems = std::get<ArrayVal>(inner).elements;
            for (size_t i = 0; i < elems.size(); i++) {
                if (i > 0) r += ", ";
                r += elems[i]->to_string();
            }
            return r + "]";
        }
        return "<?>";
    }

    // ── Truthiness: null=false, 0=false, ""=false, rest=true ─────
    bool to_bool() const {
        if (is_null()) return false;
        if (is_bool()) return as_bool();
        if (is_int())  return as_int() != 0;
        if (is_float()) return as_float() != 0.0;
        if (is_str())  return !as_str().empty();
        if (is_array()) return !std::get<ArrayVal>(inner).elements.empty();
        return true;
    }

    // ── Arithmetic operators ─────────────────────────────────────
    ValuePtr op_add(const Value& o) const {
        if (is_int() && o.is_int()) return make_int(as_int() + o.as_int());
        if (is_numeric() && o.is_numeric()) return make_float(to_number() + o.to_number());
        if (is_str() && o.is_str()) return make_str(as_str() + o.as_str());
        if (is_str()) return make_str(as_str() + o.to_string());
        if (o.is_str()) return make_str(to_string() + o.as_str());
        throw RuntimeError("invalid operands for '+'");
    }

    ValuePtr op_sub(const Value& o) const {
        if (is_int() && o.is_int()) return make_int(as_int() - o.as_int());
        if (is_numeric() && o.is_numeric()) return make_float(to_number() - o.to_number());
        throw RuntimeError("invalid operands for '-'");
    }

    ValuePtr op_mul(const Value& o) const {
        if (is_int() && o.is_int()) return make_int(as_int() * o.as_int());
        if (is_numeric() && o.is_numeric()) return make_float(to_number() * o.to_number());
        if (is_str() && o.is_int()) {
            std::string r; for (int64_t i = 0; i < o.as_int(); i++) r += as_str(); return make_str(r);
        }
        throw RuntimeError("invalid operands for '*'");
    }

    ValuePtr op_div(const Value& o) const {
        if (o.is_int() && o.as_int() == 0) throw RuntimeError("division by zero");
        if (o.is_float() && o.as_float() == 0.0) throw RuntimeError("division by zero");
        if (is_int() && o.is_int()) return make_int(as_int() / o.as_int());
        if (is_numeric() && o.is_numeric()) return make_float(to_number() / o.to_number());
        throw RuntimeError("invalid operands for '/'");
    }

    ValuePtr op_mod(const Value& o) const {
        if (o.is_int() && o.as_int() == 0) throw RuntimeError("modulo by zero");
        if (is_int() && o.is_int()) return make_int(as_int() % o.as_int());
        if (is_numeric() && o.is_numeric()) return make_float(std::fmod(to_number(), o.to_number()));
        throw RuntimeError("invalid operands for '%'");
    }

    ValuePtr op_neg() const {
        if (is_int()) return make_int(-as_int());
        if (is_float()) return make_float(-as_float());
        throw RuntimeError("invalid operand for unary '-'");
    }

    // ── Comparison operators ─────────────────────────────────────
    ValuePtr op_eq(const Value& o) const {
        if (is_int() && o.is_int()) return make_bool(as_int() == o.as_int());
        if (is_numeric() && o.is_numeric()) return make_bool(to_number() == o.to_number());
        if (is_str() && o.is_str()) return make_bool(as_str() == o.as_str());
        if (is_bool() && o.is_bool()) return make_bool(as_bool() == o.as_bool());
        if (is_null() && o.is_null()) return make_bool(true);
        return make_bool(false);
    }

    ValuePtr op_ne(const Value& o) const {
        auto r = op_eq(o);
        return make_bool(!r->as_bool());
    }

    ValuePtr op_lt(const Value& o) const {
        if (is_int() && o.is_int()) return make_bool(as_int() < o.as_int());
        if (is_numeric() && o.is_numeric()) return make_bool(to_number() < o.to_number());
        if (is_str() && o.is_str()) return make_bool(as_str() < o.as_str());
        throw RuntimeError("invalid operands for '<'");
    }

    ValuePtr op_le(const Value& o) const {
        if (is_int() && o.is_int()) return make_bool(as_int() <= o.as_int());
        if (is_numeric() && o.is_numeric()) return make_bool(to_number() <= o.to_number());
        if (is_str() && o.is_str()) return make_bool(as_str() <= o.as_str());
        throw RuntimeError("invalid operands for '<='");
    }

    ValuePtr op_gt(const Value& o) const {
        if (is_int() && o.is_int()) return make_bool(as_int() > o.as_int());
        if (is_numeric() && o.is_numeric()) return make_bool(to_number() > o.to_number());
        if (is_str() && o.is_str()) return make_bool(as_str() > o.as_str());
        throw RuntimeError("invalid operands for '>'");
    }

    ValuePtr op_ge(const Value& o) const {
        if (is_int() && o.is_int()) return make_bool(as_int() >= o.as_int());
        if (is_numeric() && o.is_numeric()) return make_bool(to_number() >= o.to_number());
        if (is_str() && o.is_str()) return make_bool(as_str() >= o.as_str());
        throw RuntimeError("invalid operands for '>='");
    }

    ValuePtr op_not() const {
        return make_bool(!to_bool());
    }
};

} // namespace rex::interp
