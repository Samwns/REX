#pragma once
/*
 * environment.hpp  –  REX Interpreter Variable Scopes + Control Flow Signals
 *
 * Chained scope system (child → parent → global) with define/get/assign/has.
 * Also defines control flow signals: ReturnSignal, BreakSignal, ContinueSignal, ThrowSignal.
 */

#include "value.hpp"

namespace rex::interp {

// ─── Variable scope (chained) ────────────────────────────────────

struct Environment : std::enable_shared_from_this<Environment> {
    std::unordered_map<std::string, ValuePtr> vars;
    EnvPtr parent;

    explicit Environment(EnvPtr parent_ = nullptr) : parent(std::move(parent_)) {}

    EnvPtr make_child() {
        return std::make_shared<Environment>(shared_from_this());
    }

    void define(const std::string& name, ValuePtr value) {
        vars[name] = std::move(value);
    }

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

    void assign(const std::string& name, ValuePtr value) {
        auto it = vars.find(name);
        if (it != vars.end()) { it->second = std::move(value); return; }
        if (parent) { parent->assign(name, std::move(value)); return; }
        throw RuntimeError("assignment to undefined variable: '" + name + "'");
    }
};

// ─── Control flow signals (not real errors) ──────────────────────

struct ReturnSignal   { ValuePtr value; };
struct BreakSignal    {};
struct ContinueSignal {};
struct ThrowSignal    { ValuePtr value; std::string type_name; };

} // namespace rex::interp
