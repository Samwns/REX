#pragma once
/*
 * ir_printer.hpp  –  Human-readable IR printer for REXC
 *
 * Produces textual output of the SSA-based IR for debugging and
 * inspection.  Header-only; depends only on ir.hpp.
 *
 * Example output:
 *
 *   function main() -> i64:
 *     bb0:
 *       %1 = alloca i64
 *       store i64 0, ptr %1
 *       %2 = load i64, ptr %1
 *       %3 = const i64 42
 *       %4 = add i64 %2, %3
 *       ret i64 %4
 */

#include "ir.hpp"
#include <string>
#include <sstream>

namespace rexc {

class IRPrinter {
public:
    static std::string print(const IRModule& mod) {
        std::ostringstream os;
        for (std::size_t i = 0; i < mod.functions.size(); ++i) {
            if (i > 0) os << '\n';
            os << print_function(mod.functions[i]);
        }
        if (!mod.globals.empty()) {
            if (!mod.functions.empty()) os << '\n';
            for (const auto& g : mod.globals) {
                os << '@' << g.first << " = \"" << escape(g.second) << "\"\n";
            }
        }
        return os.str();
    }

    static std::string print_function(const IRFunction& fn) {
        std::ostringstream os;
        os << "function " << fn.name << '(';
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i > 0) os << ", ";
            os << ir_type_name(fn.params[i].second) << ' ' << fn.params[i].first;
        }
        os << ") -> " << ir_type_name(fn.return_type) << ":\n";
        for (const auto& blk : fn.blocks) {
            os << print_block(blk);
        }
        return os.str();
    }

    static std::string print_block(const IRBlock& blk) {
        std::ostringstream os;
        os << "  " << blk.label << ":\n";
        for (const auto& instr : blk.instrs) {
            os << "    " << print_instr(instr) << '\n';
        }
        return os.str();
    }

    static std::string print_instr(const IRInstr& instr) {
        std::ostringstream os;

        switch (instr.op) {
        case IROp::Const:
            os << fmt_def(instr.result) << " = const "
               << ir_type_name(instr.result.type) << ' ';
            if (instr.result.type == IRType::Float32 ||
                instr.result.type == IRType::Float64) {
                os << instr.const_float;
            } else {
                os << instr.const_int;
            }
            os << fmt_comment(instr.result);
            break;

        case IROp::Alloca:
            os << fmt_def(instr.result) << " = alloca "
               << ir_type_name(instr.result.type)
               << fmt_comment(instr.result);
            break;

        case IROp::Load:
            os << fmt_def(instr.result) << " = load "
               << ir_type_name(instr.result.type) << ", ptr "
               << fmt_ref(instr.operands[0])
               << fmt_comment(instr.result);
            break;

        case IROp::Store:
            os << "store " << ir_type_name(instr.operands[0].type) << ' '
               << fmt_ref(instr.operands[0]) << ", ptr "
               << fmt_ref(instr.operands[1]);
            break;

        case IROp::Add: case IROp::Sub: case IROp::Mul:
        case IROp::Div: case IROp::Mod:
        case IROp::And: case IROp::Or:  case IROp::Xor:
        case IROp::Shl: case IROp::Shr:
            os << fmt_def(instr.result) << " = " << ir_op_name(instr.op) << ' '
               << ir_type_name(instr.result.type) << ' '
               << fmt_ref(instr.operands[0]) << ", "
               << fmt_ref(instr.operands[1])
               << fmt_comment(instr.result);
            break;

        case IROp::CmpEq: case IROp::CmpNe:
        case IROp::CmpLt: case IROp::CmpLe:
        case IROp::CmpGt: case IROp::CmpGe:
            os << fmt_def(instr.result) << " = " << ir_op_name(instr.op) << ' '
               << ir_type_name(instr.operands[0].type) << ' '
               << fmt_ref(instr.operands[0]) << ", "
               << fmt_ref(instr.operands[1])
               << fmt_comment(instr.result);
            break;

        case IROp::Neg: case IROp::Not:
            os << fmt_def(instr.result) << " = " << ir_op_name(instr.op) << ' '
               << ir_type_name(instr.result.type) << ' '
               << fmt_ref(instr.operands[0])
               << fmt_comment(instr.result);
            break;

        case IROp::Br:
            os << "br " << instr.label_true;
            break;

        case IROp::BrCond:
            os << "br.cond " << fmt_ref(instr.operands[0]) << ", "
               << instr.label_true << ", " << instr.label_false;
            break;

        case IROp::Ret:
            if (instr.operands.empty()) {
                os << "ret void";
            } else {
                os << "ret " << ir_type_name(instr.operands[0].type) << ' '
                   << fmt_ref(instr.operands[0]);
            }
            break;

        case IROp::Call:
            if (instr.result.id != 0) {
                os << fmt_def(instr.result) << " = ";
            }
            os << "call " << instr.callee << '(';
            for (std::size_t i = 0; i < instr.operands.size(); ++i) {
                if (i > 0) os << ", ";
                os << fmt_ref(instr.operands[i]);
            }
            os << ')';
            if (instr.result.id != 0) os << fmt_comment(instr.result);
            break;

        case IROp::Phi:
            os << fmt_def(instr.result) << " = phi "
               << ir_type_name(instr.result.type);
            for (std::size_t i = 0; i < instr.phi_incoming.size(); ++i) {
                if (i > 0) os << ',';
                os << " [" << fmt_ref(instr.phi_incoming[i].second) << ", "
                   << instr.phi_incoming[i].first << ']';
            }
            os << fmt_comment(instr.result);
            break;

        case IROp::Cast:
            os << fmt_def(instr.result) << " = cast "
               << ir_type_name(instr.operands[0].type) << ' '
               << fmt_ref(instr.operands[0]) << " to "
               << ir_type_name(instr.cast_to)
               << fmt_comment(instr.result);
            break;

        case IROp::GetElementPtr:
            os << fmt_def(instr.result) << " = gep "
               << ir_type_name(instr.result.type) << ", ptr "
               << fmt_ref(instr.operands[0]);
            for (const auto& idx : instr.gep_indices) {
                os << ", " << fmt_ref(idx);
            }
            os << fmt_comment(instr.result);
            break;
        }

        return os.str();
    }

private:
    /// Format a value as `%id` for definitions.
    static std::string fmt_def(const IRValue& v) {
        return "%" + std::to_string(v.id);
    }

    /// Format a value reference as `%id`.
    static std::string fmt_ref(const IRValue& v) {
        return "%" + std::to_string(v.id);
    }

    /// Return a trailing comment with the debug name if it differs
    /// from the canonical `%id` form.
    static std::string fmt_comment(const IRValue& v) {
        std::string canon = "%" + std::to_string(v.id);
        if (!v.name.empty() && v.name != canon) {
            return "  ; " + v.name;
        }
        return "";
    }

    /// Escape special characters in string data.
    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\0': out += "\\0";  break;
                case '"':  out += "\\\""; break;
                default:   out += c;      break;
            }
        }
        return out;
    }
};

} // namespace rexc
