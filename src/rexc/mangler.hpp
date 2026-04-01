#pragma once
/*
 * mangler.hpp  –  Itanium ABI C++ Name Mangling
 *
 * Implements name mangling following the Itanium C++ ABI for use
 * in the REXC backend.  Supports methods, constructors, destructors,
 * vtables, and free functions.
 *
 * Reference:  https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling
 */

#include "ir.hpp"
#include <string>
#include <vector>

namespace rexc {

class Mangler {
public:
    /// Mangle a class method:  _ZN<class><method>E<params>
    std::string mangle_method(const std::string& class_name,
                              const std::string& method_name,
                              const std::vector<IRType>& param_types) const {
        return "_ZN" + encode_name(class_name)
                     + encode_name(method_name)
                     + "E" + encode_params(param_types);
    }

    /// Mangle a constructor (C1 = complete object ctor):  _ZN<class>C1E<params>
    std::string mangle_ctor(const std::string& class_name,
                            const std::vector<IRType>& param_types) const {
        return "_ZN" + encode_name(class_name)
                     + "C1"
                     + "E" + encode_params(param_types);
    }

    /// Mangle a destructor (D1 = complete object dtor):  _ZN<class>D1Ev
    std::string mangle_dtor(const std::string& class_name) const {
        return "_ZN" + encode_name(class_name) + "D1" + "Ev";
    }

    /// Mangle a vtable symbol:  _ZTV<class>
    std::string mangle_vtable(const std::string& class_name) const {
        return "_ZTV" + encode_name(class_name);
    }

    /// Mangle a free (non-member) function:  _Z<name><params>
    std::string mangle_function(const std::string& name,
                                const std::vector<IRType>& param_types) const {
        return "_Z" + encode_name(name) + encode_params(param_types);
    }

private:
    /// Encode an identifier as <length><name>  (e.g. "Foo" → "3Foo").
    std::string encode_name(const std::string& name) const {
        return std::to_string(name.size()) + name;
    }

    /// Map an IRType to its Itanium ABI single-character encoding.
    std::string encode_type(IRType t) const {
        switch (t) {
            case IRType::Void:    return "v";
            case IRType::Bool:    return "b";
            case IRType::Int8:    return "a";   // signed char
            case IRType::Int16:   return "s";
            case IRType::Int32:   return "i";
            case IRType::Int64:   return "l";
            case IRType::Float32: return "f";
            case IRType::Float64: return "d";
            case IRType::Ptr:     return "Pv";  // pointer to void
        }
        return "v";
    }

    /// Encode a parameter list.  Empty lists produce "v" (void).
    std::string encode_params(const std::vector<IRType>& types) const {
        if (types.empty()) return "v";
        std::string out;
        for (auto t : types) out += encode_type(t);
        return out;
    }
};

} // namespace rexc
