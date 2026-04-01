#pragma once
/*
 * ir.hpp  –  REXC Intermediate Representation (SSA Form)
 *
 * Defines the IR structures used between the frontend (AST) and
 * the backend (x86/ARM64 emitters).  The IR is in Static Single
 * Assignment form — each value is defined exactly once.
 *
 * Pipeline:  AST → IRGenerator → IRModule → IROptimizer → Emitter
 */

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace rexc {

// ── IR Types ─────────────────────────────────────────────────────

enum class IRType {
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    Float32,
    Float64,
    Ptr
};

inline const char* ir_type_name(IRType t) {
    switch (t) {
        case IRType::Void:    return "void";
        case IRType::Bool:    return "i1";
        case IRType::Int8:    return "i8";
        case IRType::Int16:   return "i16";
        case IRType::Int32:   return "i32";
        case IRType::Int64:   return "i64";
        case IRType::Float32: return "f32";
        case IRType::Float64: return "f64";
        case IRType::Ptr:     return "ptr";
    }
    return "?";
}

/// Returns the size in bytes for a given IRType.
inline std::size_t ir_type_size(IRType t) {
    switch (t) {
        case IRType::Void:    return 0;
        case IRType::Bool:    return 1;
        case IRType::Int8:    return 1;
        case IRType::Int16:   return 2;
        case IRType::Int32:   return 4;
        case IRType::Int64:   return 8;
        case IRType::Float32: return 4;
        case IRType::Float64: return 8;
        case IRType::Ptr:     return 8;
    }
    return 0;
}

// ── SSA Value ────────────────────────────────────────────────────

struct IRValue {
    uint32_t    id   = 0;      // SSA version number (0 = unused)
    IRType      type = IRType::Void;
    std::string name;          // debug / pretty-print name
};

// ── IR Operations ────────────────────────────────────────────────

enum class IROp {
    // ── Arithmetic ───────────────────────────────────────────────
    Add, Sub, Mul, Div, Mod, Neg,

    // ── Logic / Bitwise ──────────────────────────────────────────
    And, Or, Xor, Not, Shl, Shr,

    // ── Comparison ───────────────────────────────────────────────
    CmpEq, CmpNe, CmpLt, CmpLe, CmpGt, CmpGe,

    // ── Memory ───────────────────────────────────────────────────
    Alloca, Load, Store, GetElementPtr,

    // ── Control Flow ─────────────────────────────────────────────
    Br, BrCond, Ret, Call, Phi,

    // ── Conversion / Constants ───────────────────────────────────
    Cast, Const
};

inline const char* ir_op_name(IROp op) {
    switch (op) {
        case IROp::Add:           return "add";
        case IROp::Sub:           return "sub";
        case IROp::Mul:           return "mul";
        case IROp::Div:           return "div";
        case IROp::Mod:           return "mod";
        case IROp::Neg:           return "neg";
        case IROp::And:           return "and";
        case IROp::Or:            return "or";
        case IROp::Xor:           return "xor";
        case IROp::Not:           return "not";
        case IROp::Shl:           return "shl";
        case IROp::Shr:           return "shr";
        case IROp::CmpEq:        return "cmp.eq";
        case IROp::CmpNe:        return "cmp.ne";
        case IROp::CmpLt:        return "cmp.lt";
        case IROp::CmpLe:        return "cmp.le";
        case IROp::CmpGt:        return "cmp.gt";
        case IROp::CmpGe:        return "cmp.ge";
        case IROp::Alloca:       return "alloca";
        case IROp::Load:         return "load";
        case IROp::Store:        return "store";
        case IROp::GetElementPtr:return "gep";
        case IROp::Br:           return "br";
        case IROp::BrCond:       return "br.cond";
        case IROp::Ret:          return "ret";
        case IROp::Call:         return "call";
        case IROp::Phi:          return "phi";
        case IROp::Cast:         return "cast";
        case IROp::Const:        return "const";
    }
    return "?";
}

// ── IR Instruction ───────────────────────────────────────────────

struct IRInstr {
    IROp                    op;
    IRValue                 result;
    std::vector<IRValue>    operands;

    // Const payload
    int64_t                 const_int   = 0;
    double                  const_float = 0.0;

    // Branch targets (Br / BrCond)
    std::string             label_true;
    std::string             label_false;

    // Call target
    std::string             callee;

    // Phi incoming edges: (block_label, value)
    std::vector<std::pair<std::string, IRValue>> phi_incoming;

    // Cast destination type
    IRType                  cast_to = IRType::Void;

    // GetElementPtr indices
    std::vector<IRValue>    gep_indices;
};

// ── Basic Block ──────────────────────────────────────────────────

struct IRBlock {
    std::string             label;
    std::vector<IRInstr>    instrs;
    // Invariant: last instruction must be Br, BrCond, or Ret.
};

// ── Function ─────────────────────────────────────────────────────

struct IRFunction {
    std::string name;
    IRType      return_type = IRType::Void;
    std::vector<std::pair<std::string, IRType>> params;
    std::vector<IRBlock> blocks;

    uint32_t next_value_id = 1;

    /// Allocate a fresh SSA value with a unique id.
    IRValue new_value(IRType t, const std::string& nm = "") {
        IRValue v;
        v.id   = next_value_id++;
        v.type = t;
        v.name = nm.empty() ? ("%" + std::to_string(v.id)) : nm;
        return v;
    }

    /// Return the entry block (first block).
    IRBlock& entry() { return blocks.front(); }
    const IRBlock& entry() const { return blocks.front(); }

    /// Return the current (last) block being built.
    IRBlock& current() { return blocks.back(); }
    const IRBlock& current() const { return blocks.back(); }

    /// Append a new basic block and return a reference to it.
    IRBlock& new_block(const std::string& lbl) {
        blocks.push_back(IRBlock{lbl, {}});
        return blocks.back();
    }
};

// ── Module (translation-unit level) ──────────────────────────────

struct IRModule {
    std::vector<IRFunction> functions;

    // Global read-only data (e.g. string literals): (label, content)
    std::vector<std::pair<std::string, std::string>> globals;
};

} // namespace rexc
