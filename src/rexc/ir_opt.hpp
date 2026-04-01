#pragma once
/*
 * ir_opt.hpp  –  REXC IR Optimizer
 *
 * Implements optimization passes on the SSA IR:
 *   1. mem2reg        – promote stack allocas to SSA registers
 *   2. constant_fold  – evaluate constant expressions at compile time
 *   3. dce            – eliminate dead (unused) instructions
 *   4. cfg_simplify   – simplify the control-flow graph
 *   5. inline         – inline small single-call functions
 *
 * Execution order (per the Phase 1 / v0.5 roadmap):
 *   mem2reg → constant_fold → dce → cfg_simplify →
 *   inline  → constant_fold → dce
 *
 * Pipeline:  AST → IRGenerator → IRModule → IROptimizer → Emitter
 *
 * Phase 1 (v0.5): straightforward single-pass implementations.
 * No dominator trees, no advanced alias analysis.
 */

#include "ir.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rexc {

class IROptimizer {
public:
    /// Run all optimization passes on the module in the prescribed order.
    void run(IRModule& mod) {
        for (auto& fn : mod.functions) {
            pass_mem2reg(fn);
            pass_constant_fold(fn);
            pass_dce(fn);
            pass_cfg_simplify(fn);
        }
        pass_inline(mod);
        for (auto& fn : mod.functions) {
            pass_constant_fold(fn);
            pass_dce(fn);
        }
    }

private:
    // ── Helpers ──────────────────────────────────────────────────

    /// Returns true if the operation has observable side effects.
    static bool has_side_effects(IROp op) {
        switch (op) {
            case IROp::Store: case IROp::Call:
            case IROp::Ret:   case IROp::Br: case IROp::BrCond:
                return true;
            default:
                return false;
        }
    }

    /// Replace every use of @p old_id with @p new_val across @p fn.
    static void replace_value(IRFunction& fn, uint32_t old_id,
                              const IRValue& new_val) {
        if (old_id == 0) return;
        for (auto& block : fn.blocks) {
            for (auto& ins : block.instrs) {
                for (auto& op : ins.operands)
                    if (op.id == old_id) op = new_val;
                for (auto& [lbl, val] : ins.phi_incoming)
                    if (val.id == old_id) val = new_val;
                for (auto& idx : ins.gep_indices)
                    if (idx.id == old_id) idx = new_val;
            }
        }
    }

    // ── Pass 1: Constant Folding ────────────────────────────────

    static bool try_fold_binary(IROp op, int64_t a, int64_t b,
                                int64_t& out) {
        switch (op) {
            case IROp::Add:   out = a + b; return true;
            case IROp::Sub:   out = a - b; return true;
            case IROp::Mul:   out = a * b; return true;
            case IROp::Div:   if (b == 0) return false; out = a / b; return true;
            case IROp::Mod:   if (b == 0) return false; out = a % b; return true;
            case IROp::And:   out = a & b; return true;
            case IROp::Or:    out = a | b; return true;
            case IROp::Xor:   out = a ^ b; return true;
            case IROp::Shl:
                if (b < 0 || b >= 64) return false;
                out = a << static_cast<unsigned>(b); return true;
            case IROp::Shr:
                if (b < 0 || b >= 64) return false;
                out = a >> static_cast<unsigned>(b); return true;
            case IROp::CmpEq: out = (a == b) ? 1 : 0; return true;
            case IROp::CmpNe: out = (a != b) ? 1 : 0; return true;
            case IROp::CmpLt: out = (a <  b) ? 1 : 0; return true;
            case IROp::CmpLe: out = (a <= b) ? 1 : 0; return true;
            case IROp::CmpGt: out = (a >  b) ? 1 : 0; return true;
            case IROp::CmpGe: out = (a >= b) ? 1 : 0; return true;
            default: return false;
        }
    }

    static bool try_fold_unary(IROp op, int64_t a, int64_t& out) {
        switch (op) {
            case IROp::Neg: out = -a; return true;
            case IROp::Not: out = ~a; return true;
            default: return false;
        }
    }

    /// Single-pass constant propagation.  Tracks which value IDs
    /// are compile-time constants and folds binary/unary operations
    /// whose operands are all known.  Division by zero and
    /// out-of-range shifts are left un-folded.
    void pass_constant_fold(IRFunction& fn) {
        std::unordered_map<uint32_t, int64_t> cmap;

        for (auto& block : fn.blocks) {
            for (auto& ins : block.instrs) {
                if (ins.op == IROp::Const) {
                    cmap[ins.result.id] = ins.const_int;
                    continue;
                }

                int64_t result = 0;
                bool folded = false;

                if (ins.operands.size() >= 2) {
                    auto ia = cmap.find(ins.operands[0].id);
                    auto ib = cmap.find(ins.operands[1].id);
                    if (ia != cmap.end() && ib != cmap.end())
                        folded = try_fold_binary(ins.op, ia->second,
                                                 ib->second, result);
                }
                if (!folded && !ins.operands.empty()) {
                    auto ia = cmap.find(ins.operands[0].id);
                    if (ia != cmap.end())
                        folded = try_fold_unary(ins.op, ia->second, result);
                }

                if (folded) {
                    ins.op        = IROp::Const;
                    ins.const_int = result;
                    ins.operands.clear();
                    cmap[ins.result.id] = result;
                }
            }
        }
    }

    // ── Pass 2: Dead Code Elimination ───────────────────────────

    /// Single-pass dead code elimination.  Collects every value ID
    /// referenced as an operand, then removes instructions whose
    /// result is never used.  Side-effecting ops (Store, Call, Ret,
    /// Br, BrCond) are always preserved.
    void pass_dce(IRFunction& fn) {
        std::unordered_set<uint32_t> used;

        for (auto& block : fn.blocks) {
            for (auto& ins : block.instrs) {
                for (auto& op : ins.operands)
                    if (op.id != 0) used.insert(op.id);
                for (auto& [lbl, val] : ins.phi_incoming)
                    if (val.id != 0) used.insert(val.id);
                for (auto& idx : ins.gep_indices)
                    if (idx.id != 0) used.insert(idx.id);
            }
        }

        for (auto& block : fn.blocks) {
            block.instrs.erase(
                std::remove_if(block.instrs.begin(), block.instrs.end(),
                    [&](const IRInstr& ins) {
                        if (has_side_effects(ins.op)) return false;
                        if (ins.result.id == 0) return false;
                        return used.find(ins.result.id) == used.end();
                    }),
                block.instrs.end());
        }
    }

    // ── Pass 3: mem2reg ─────────────────────────────────────────

    /// Promotes simple allocas to SSA registers.  Only handles
    /// allocas whose sole uses are Load(ptr) and Store(_, ptr)
    /// within the same basic block.  Cross-block allocas and complex
    /// addressing (GEP, Call arguments) are left untouched.
    void pass_mem2reg(IRFunction& fn) {
        struct AllocaInfo {
            uint32_t id;
            size_t   block_idx;
        };
        std::vector<AllocaInfo> allocas;

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi)
            for (auto& ins : fn.blocks[bi].instrs)
                if (ins.op == IROp::Alloca)
                    allocas.push_back({ins.result.id, bi});

        for (auto& ai : allocas) {
            uint32_t aid = ai.id;

            // Verify every use is Load(ptr) or Store(_, ptr) in the
            // same block as the alloca.
            bool simple     = true;
            bool same_block = true;

            for (size_t bi = 0; bi < fn.blocks.size() && simple; ++bi) {
                for (auto& ins : fn.blocks[bi].instrs) {
                    if (ins.op == IROp::Alloca && ins.result.id == aid)
                        continue;

                    for (size_t oi = 0; oi < ins.operands.size(); ++oi) {
                        if (ins.operands[oi].id != aid) continue;
                        bool ok = (ins.op == IROp::Load  && oi == 0) ||
                                  (ins.op == IROp::Store && oi == 1);
                        if (!ok)  { simple = false; break; }
                        if (bi != ai.block_idx) same_block = false;
                    }
                    if (!simple) break;

                    for (auto& idx : ins.gep_indices)
                        if (idx.id == aid) { simple = false; break; }
                    if (!simple) break;
                    for (auto& [lbl, val] : ins.phi_incoming)
                        if (val.id == aid) { simple = false; break; }
                }
            }

            if (!simple || !same_block) continue;

            // Walk the block; track the most-recent stored value.
            auto& block = fn.blocks[ai.block_idx];
            std::unordered_map<uint32_t, IRValue> replacements;
            std::vector<size_t> to_remove;
            IRValue  cur_val{};
            bool     has_val   = false;
            bool     promotable = true;

            for (size_t ii = 0; ii < block.instrs.size(); ++ii) {
                auto& ins = block.instrs[ii];

                if (ins.op == IROp::Alloca && ins.result.id == aid) {
                    to_remove.push_back(ii);
                } else if (ins.op == IROp::Store &&
                           ins.operands.size() >= 2 &&
                           ins.operands[1].id == aid) {
                    cur_val = ins.operands[0];
                    has_val = true;
                    to_remove.push_back(ii);
                } else if (ins.op == IROp::Load &&
                           !ins.operands.empty() &&
                           ins.operands[0].id == aid) {
                    if (!has_val) { promotable = false; break; }
                    replacements[ins.result.id] = cur_val;
                    to_remove.push_back(ii);
                }
            }

            if (!promotable) continue;

            // Erase marked instructions (reverse order preserves indices).
            for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
                block.instrs.erase(
                    block.instrs.begin() +
                    static_cast<std::ptrdiff_t>(*it));

            for (auto& [old_id, new_val] : replacements)
                replace_value(fn, old_id, new_val);
        }
    }

    // ── Pass 4: Basic Inlining ──────────────────────────────────

    /// Inlines small, single-call functions.  Candidates must have
    /// exactly one basic block, ≤ 10 instructions, and be called
    /// exactly once (excluding self-recursion).  Value IDs in the
    /// inlined body are renumbered; implicit (undefined) IDs are
    /// mapped to the call-site arguments in ascending-ID order.
    void pass_inline(IRModule& mod) {
        std::unordered_map<std::string, int> call_count;
        for (auto& fn : mod.functions)
            for (auto& block : fn.blocks)
                for (auto& ins : block.instrs)
                    if (ins.op == IROp::Call)
                        ++call_count[ins.callee];

        std::vector<std::string> candidates;
        for (auto& fn : mod.functions) {
            if (fn.blocks.size() != 1) continue;
            if (call_count[fn.name] != 1) continue;
            size_t n = 0;
            for (auto& b : fn.blocks) n += b.instrs.size();
            if (n > 10) continue;
            candidates.push_back(fn.name);
        }

        for (auto& cname : candidates) {
            IRFunction* callee = nullptr;
            for (auto& fn : mod.functions)
                if (fn.name == cname) { callee = &fn; break; }
            if (!callee) continue;

            bool done = false;
            for (auto& caller : mod.functions) {
                if (done) break;
                if (&caller == callee) continue;
                for (auto& block : caller.blocks) {
                    if (done) break;
                    for (size_t ii = 0; ii < block.instrs.size(); ++ii) {
                        if (block.instrs[ii].op == IROp::Call &&
                            block.instrs[ii].callee == cname) {
                            inline_call(caller, block, ii, *callee);
                            done = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    /// Replace a Call instruction with the callee's body.
    static void inline_call(IRFunction& caller, IRBlock& block,
                            size_t call_idx,
                            const IRFunction& callee) {
        auto& call_ins = block.instrs[call_idx];

        // Collect value IDs defined by callee instructions.
        std::unordered_set<uint32_t> defined;
        for (auto& cb : callee.blocks)
            for (auto& ci : cb.instrs)
                if (ci.result.id != 0) defined.insert(ci.result.id);

        // Implicit (undefined) value IDs are function parameters.
        std::unordered_set<uint32_t> undef_set;
        auto collect_undef = [&](uint32_t id) {
            if (id != 0 && !defined.count(id)) undef_set.insert(id);
        };
        for (auto& cb : callee.blocks)
            for (auto& ci : cb.instrs) {
                for (auto& op : ci.operands)      collect_undef(op.id);
                for (auto& [l, v] : ci.phi_incoming) collect_undef(v.id);
                for (auto& idx : ci.gep_indices)  collect_undef(idx.id);
            }

        std::vector<uint32_t> implicit_params(undef_set.begin(),
                                               undef_set.end());
        std::sort(implicit_params.begin(), implicit_params.end());

        // Build value map: callee-id → caller IRValue.
        std::unordered_map<uint32_t, IRValue> vmap;

        for (size_t i = 0;
             i < implicit_params.size() && i < call_ins.operands.size();
             ++i)
            vmap[implicit_params[i]] = call_ins.operands[i];

        // Assign fresh caller-side IDs for each defined value.
        for (auto& cb : callee.blocks)
            for (auto& ci : cb.instrs)
                if (ci.result.id != 0 && !vmap.count(ci.result.id)) {
                    IRValue nv;
                    nv.id   = caller.next_value_id++;
                    nv.type = ci.result.type;
                    nv.name = ci.result.name;
                    vmap[ci.result.id] = nv;
                }

        auto remap = [&](const IRValue& v) -> IRValue {
            if (v.id == 0) return v;
            auto it = vmap.find(v.id);
            return (it != vmap.end()) ? it->second : v;
        };

        // Copy callee body, recording the return value.
        std::vector<IRInstr> new_instrs;
        IRValue ret_val{};

        for (auto& cb : callee.blocks) {
            for (auto& ci : cb.instrs) {
                if (ci.op == IROp::Ret) {
                    if (!ci.operands.empty())
                        ret_val = remap(ci.operands[0]);
                    continue;
                }
                IRInstr ni = ci;
                if (ni.result.id != 0) ni.result = remap(ni.result);
                for (auto& op : ni.operands)         op  = remap(op);
                for (auto& [l, v] : ni.phi_incoming) v   = remap(v);
                for (auto& idx : ni.gep_indices)     idx = remap(idx);
                new_instrs.push_back(std::move(ni));
            }
        }

        // Splice: replace the Call with the copied body.
        uint32_t call_result = call_ins.result.id;
        auto pos = block.instrs.begin() +
                   static_cast<std::ptrdiff_t>(call_idx);
        pos = block.instrs.erase(pos);
        block.instrs.insert(pos, new_instrs.begin(), new_instrs.end());

        if (call_result != 0 && ret_val.id != 0)
            replace_value(caller, call_result, ret_val);
    }

    // ── Pass 5: CFG Simplification ──────────────────────────────

    /// Simplifies the control-flow graph with two transformations:
    ///  A. Remove empty non-entry blocks that contain only an
    ///     unconditional Br, redirecting all incoming edges.
    ///  B. Merge a block into its sole predecessor when the
    ///     predecessor ends with an unconditional Br to it,
    ///     resolving any trivial Phi nodes first.
    void pass_cfg_simplify(IRFunction& fn) {
        if (fn.blocks.size() <= 1) return;

        bool changed = true;
        while (changed) {
            changed = false;

            // A. Remove empty blocks (single unconditional Br, not entry).
            for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
                auto& blk = fn.blocks[bi];
                if (blk.instrs.size() != 1) continue;
                if (blk.instrs[0].op != IROp::Br) continue;

                std::string target  = blk.instrs[0].label_true;
                std::string removed = blk.label;
                if (target == removed) continue; // self-loop guard

                for (auto& b : fn.blocks) {
                    for (auto& ins : b.instrs) {
                        if (ins.label_true  == removed) ins.label_true  = target;
                        if (ins.label_false == removed) ins.label_false = target;
                        for (auto& [lbl, val] : ins.phi_incoming)
                            if (lbl == removed) lbl = target;
                    }
                }

                fn.blocks.erase(fn.blocks.begin() +
                                static_cast<std::ptrdiff_t>(bi));
                --bi;
                changed = true;
            }

            // B. Merge B1 → B2 where B1 is B2's sole predecessor.
            for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
                auto& b1 = fn.blocks[bi];
                if (b1.instrs.empty()) continue;
                if (b1.instrs.back().op != IROp::Br) continue;

                std::string succ = b1.instrs.back().label_true;

                size_t b2i = fn.blocks.size();
                for (size_t j = 0; j < fn.blocks.size(); ++j)
                    if (fn.blocks[j].label == succ) { b2i = j; break; }
                if (b2i >= fn.blocks.size() || b2i == bi) continue;

                // Count distinct predecessors of B2.
                std::unordered_set<std::string> preds;
                for (auto& b : fn.blocks)
                    for (auto& ins : b.instrs)
                        if (ins.label_true == succ || ins.label_false == succ)
                            preds.insert(b.label);
                if (preds.size() != 1) continue;

                auto& b2 = fn.blocks[b2i];

                // Resolve trivial Phi nodes (single predecessor).
                size_t phi_end = 0;
                for (size_t pi = 0; pi < b2.instrs.size(); ++pi) {
                    if (b2.instrs[pi].op != IROp::Phi) break;
                    ++phi_end;
                    if (!b2.instrs[pi].phi_incoming.empty())
                        replace_value(fn, b2.instrs[pi].result.id,
                                      b2.instrs[pi].phi_incoming[0].second);
                }

                // Blocks that B2 branches to may carry phi_incoming
                // referencing B2 – update those to reference B1.
                std::string old_label = b2.label;
                for (auto& b : fn.blocks)
                    for (auto& ins : b.instrs)
                        for (auto& [lbl, val] : ins.phi_incoming)
                            if (lbl == old_label) lbl = b1.label;

                // Append B2's (non-Phi) instructions to B1.
                b1.instrs.pop_back(); // remove the unconditional Br
                b1.instrs.insert(
                    b1.instrs.end(),
                    b2.instrs.begin() +
                        static_cast<std::ptrdiff_t>(phi_end),
                    b2.instrs.end());

                fn.blocks.erase(fn.blocks.begin() +
                                static_cast<std::ptrdiff_t>(b2i));
                changed = true;
                break; // restart outer loop after structural change
            }
        }
    }
};

} // namespace rexc
