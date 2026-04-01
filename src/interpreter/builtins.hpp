#pragma once
/*
 * builtins.hpp  –  REX Interpreter Builtin Functions
 *
 * Registers cout, cin, cerr, endl, vector, string methods,
 * math functions, and utility functions into the global environment.
 */

#include "value.hpp"
#include "environment.hpp"

namespace rex::interp {

void register_builtins(EnvPtr env);

ValuePtr make_cout_object();
ValuePtr make_cerr_object();
ValuePtr make_cin_object();
ValuePtr make_endl_value();

ValuePtr make_vector_object();

ValuePtr call_string_method(const std::string& str,
                             const std::string& method,
                             std::vector<ValuePtr> args);

void register_math_builtins(EnvPtr env);
void register_utility_builtins(EnvPtr env);

} // namespace rex::interp
