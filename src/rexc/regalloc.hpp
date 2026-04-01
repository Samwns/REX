#pragma once
/*
 * regalloc.hpp  –  Graph Coloring Register Allocator (Chaitin-Briggs)
 *
 * Phase 5 of the REXC compiler pipeline.  Takes SSA-form IR functions
 * and assigns physical registers (or spill slots) to each SSA value
 * using a Chaitin-Briggs–style graph coloring approach:
 *
 *   1. Compute live ranges for every SSA value
 *   2. Build an interference graph
 *   3. Simplify / potential-spill / select  (k-coloring)
 *   4. Assign stack slots for any spilled values
 *
 * Target: x86-64 System V ABI
 *   Caller-saved (0-8): RAX RCX RDX RSI RDI R8 R9 R10 R11
 *   Callee-saved (9-13): RBX R12 R13 R14 R15
 *   Default k = 14
 */

#include "ir.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rexc {

// ── x86-64 physical register descriptors ─────────────────────────

enum class PhysReg : int {
    RAX = 0, RCX = 1, RDX = 2, RSI = 3, RDI = 4,
    R8  = 5, R9  = 6, R10 = 7, R11 = 8,
    RBX = 9, R12 = 10, R13 = 11, R14 = 12, R15 = 13
};

inline constexpr int kCallerSavedCount = 9;
inline constexpr int kCalleeSavedCount = 5;
inline constexpr int kTotalAllocatable = 14;

inline const char* phys_reg_name(int idx) {
    static const char* names[] = {
        "rax", "rcx", "rdx", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11",
        "rbx", "r12", "r13", "r14", "r15"
    };
    if (idx >= 0 && idx < kTotalAllocatable) return names[idx];
    return "?reg";
}

// ── Graph Coloring Allocator ─────────────────────────────────────

class GraphColoringAllocator {
public:
    /// Result of register allocation for a single function.
    struct Allocation {
        // value_id → physical register index (0-based, see PhysReg)
        std::unordered_map<uint32_t, int> reg_map;
        // value_id → stack offset (negative offset from RBP)
        std::unordered_map<uint32_t, int> spill_map;
        // Total bytes consumed by spill slots
        int total_spill_size = 0;
    };

    /// Run the allocator on `fn` using at most `num_regs` colours.
    Allocation allocate(const IRFunction& fn, int num_regs = kTotalAllocatable) {
        // Reset internal state
        live_ranges_.clear();
        interference_.clear();
        coloring_.clear();
        spilled_.clear();

        compute_liveness(fn);
        build_interference_graph();

        if (!color_graph(num_regs)) {
            spill_and_retry(fn, num_regs);
        }

        return build_allocation(fn);
    }

private:
    // ── Liveness ─────────────────────────────────────────────────

    struct LiveRange {
        uint32_t value_id = 0;
        // Each point is (block_idx, instr_idx)
        std::set<std::pair<int, int>> live_points;
    };

    /// Compute the set of program points where each SSA value is live.
    void compute_liveness(const IRFunction& fn) {
        live_ranges_.clear();

        // Map value_id → index into live_ranges_
        std::unordered_map<uint32_t, size_t> id_to_lr;

        auto ensure_range = [&](uint32_t vid) -> size_t {
            auto it = id_to_lr.find(vid);
            if (it != id_to_lr.end()) return it->second;
            size_t idx = live_ranges_.size();
            live_ranges_.push_back(LiveRange{vid, {}});
            id_to_lr[vid] = idx;
            return idx;
        };

        // Pass 1 – record defs and uses for every value.
        // def_point[vid] = first (block, instr) where value is defined
        // use_points[vid] = set of (block, instr) where value is used
        struct DefUse {
            std::pair<int, int> def{-1, -1};
            std::set<std::pair<int, int>> uses;
        };
        std::unordered_map<uint32_t, DefUse> du;

        for (int bi = 0; bi < static_cast<int>(fn.blocks.size()); ++bi) {
            const IRBlock& blk = fn.blocks[bi];
            for (int ii = 0; ii < static_cast<int>(blk.instrs.size()); ++ii) {
                const IRInstr& instr = blk.instrs[ii];

                // Record definition
                if (instr.result.id != 0) {
                    du[instr.result.id].def = {bi, ii};
                }

                // Record operand uses
                for (const auto& op : instr.operands) {
                    if (op.id != 0) {
                        du[op.id].uses.insert({bi, ii});
                    }
                }

                // Phi incoming values count as uses
                for (const auto& [label, val] : instr.phi_incoming) {
                    if (val.id != 0) {
                        du[val.id].uses.insert({bi, ii});
                    }
                }

                // GEP indices are also uses
                for (const auto& idx : instr.gep_indices) {
                    if (idx.id != 0) {
                        du[idx.id].uses.insert({bi, ii});
                    }
                }
            }
        }

        // Pass 2 – for each value, the live range spans from its def to
        // every use, covering all points in between within the same block
        // and across blocks for cross-block references.
        for (auto& [vid, info] : du) {
            size_t lr_idx = ensure_range(vid);
            auto& lr = live_ranges_[lr_idx];

            auto def_pt = info.def;

            for (const auto& use_pt : info.uses) {
                if (def_pt.first == -1) {
                    // No visible def (e.g. function parameter) – just mark
                    // the value live at the use.
                    lr.live_points.insert(use_pt);
                    continue;
                }

                if (def_pt.first == use_pt.first) {
                    // Same block: mark every instr from def to use
                    int lo = std::min(def_pt.second, use_pt.second);
                    int hi = std::max(def_pt.second, use_pt.second);
                    for (int i = lo; i <= hi; ++i) {
                        lr.live_points.insert({def_pt.first, i});
                    }
                } else {
                    // Cross-block: mark from def to end of def-block …
                    int def_blk_size = static_cast<int>(
                        fn.blocks[def_pt.first].instrs.size());
                    for (int i = def_pt.second; i < def_blk_size; ++i) {
                        lr.live_points.insert({def_pt.first, i});
                    }
                    // … every instr of blocks between def and use …
                    int lo_blk = std::min(def_pt.first, use_pt.first);
                    int hi_blk = std::max(def_pt.first, use_pt.first);
                    for (int b = lo_blk + 1; b < hi_blk; ++b) {
                        int sz = static_cast<int>(fn.blocks[b].instrs.size());
                        for (int i = 0; i < sz; ++i) {
                            lr.live_points.insert({b, i});
                        }
                    }
                    // … and from start of use-block to the use point.
                    for (int i = 0; i <= use_pt.second; ++i) {
                        lr.live_points.insert({use_pt.first, i});
                    }
                }
            }

            // If the value has a def but no uses, it's still live at def.
            if (info.uses.empty() && def_pt.first != -1) {
                lr.live_points.insert(def_pt);
            }
        }
    }

    // ── Interference Graph ───────────────────────────────────────

    void build_interference_graph() {
        interference_.clear();

        // Ensure every value has an entry (even if no neighbours).
        for (const auto& lr : live_ranges_) {
            interference_[lr.value_id]; // default-construct empty set
        }

        // Collect all live values at each program point, then add edges.
        std::map<std::pair<int, int>, std::vector<uint32_t>> point_to_vals;

        for (const auto& lr : live_ranges_) {
            for (const auto& pt : lr.live_points) {
                point_to_vals[pt].push_back(lr.value_id);
            }
        }

        for (const auto& [pt, vals] : point_to_vals) {
            for (size_t i = 0; i < vals.size(); ++i) {
                for (size_t j = i + 1; j < vals.size(); ++j) {
                    interference_[vals[i]].insert(vals[j]);
                    interference_[vals[j]].insert(vals[i]);
                }
            }
        }
    }

    // ── Graph Coloring (simplify → select) ───────────────────────

    /// Attempt to k-colour the interference graph.
    /// Returns true if every node received a colour, false if spills needed.
    bool color_graph(int k) {
        coloring_.clear();
        spilled_.clear();

        // Working copy of adjacency (we will mutate during simplify)
        auto adj = interference_;

        // Degree cache
        std::unordered_map<uint32_t, int> degree;
        for (const auto& [v, nbrs] : adj) {
            degree[v] = static_cast<int>(nbrs.size());
        }

        std::unordered_set<uint32_t> remaining;
        for (const auto& [v, _] : adj) {
            remaining.insert(v);
        }

        // Stack for the select phase
        std::stack<uint32_t> select_stack;

        // Simplify + potential spill
        while (!remaining.empty()) {
            // Try to find a node with degree < k
            uint32_t candidate = 0;
            bool found_low = false;

            for (uint32_t v : remaining) {
                if (degree[v] < k) {
                    candidate = v;
                    found_low = true;
                    break;
                }
            }

            if (!found_low) {
                // Potential spill: pick the node with highest degree
                int best_deg = -1;
                for (uint32_t v : remaining) {
                    if (degree[v] > best_deg) {
                        best_deg = degree[v];
                        candidate = v;
                    }
                }
            }

            // Remove candidate from the graph
            remaining.erase(candidate);
            select_stack.push(candidate);

            for (uint32_t nb : adj[candidate]) {
                if (remaining.count(nb)) {
                    --degree[nb];
                }
            }
        }

        // Select phase – pop nodes and assign colours
        bool all_colored = true;

        while (!select_stack.empty()) {
            uint32_t v = select_stack.top();
            select_stack.pop();

            // Colours already used by neighbours
            std::unordered_set<int> used_colors;
            for (uint32_t nb : interference_[v]) {
                auto it = coloring_.find(nb);
                if (it != coloring_.end()) {
                    used_colors.insert(it->second);
                }
            }

            // Find the smallest available colour in [0, k)
            int color = -1;
            for (int c = 0; c < k; ++c) {
                if (used_colors.find(c) == used_colors.end()) {
                    color = c;
                    break;
                }
            }

            if (color >= 0) {
                coloring_[v] = color;
            } else {
                spilled_.insert(v);
                all_colored = false;
            }
        }

        return all_colored;
    }

    // ── Spill Handling ───────────────────────────────────────────

    void spill_and_retry(const IRFunction& fn, int k) {
        // For values that failed to colour, leave them in the spilled_
        // set.  We could iteratively rebuild the graph with spilled
        // values removed and re-run, but for v1.0 we treat any node
        // that didn't receive a colour as a spill.
        //
        // Remove spilled nodes from interference and re-colour the rest.
        for (uint32_t s : spilled_) {
            interference_.erase(s);
            for (auto& [v, nbrs] : interference_) {
                nbrs.erase(s);
            }
        }

        // Preserve the current spill set, then re-colour.
        auto saved_spills = spilled_;
        color_graph(k);

        // Merge back any additional spills from the retry.
        for (uint32_t s : saved_spills) {
            spilled_.insert(s);
        }

        (void)fn; // fn reserved for future iterative-spill refinement
    }

    // ── Build final allocation result ────────────────────────────

    Allocation build_allocation(const IRFunction& fn) const {
        Allocation alloc;

        for (const auto& [vid, color] : coloring_) {
            alloc.reg_map[vid] = color;
        }

        // Assign spill slots: each spilled value gets 8 bytes on the
        // stack, at increasing negative offsets from RBP.
        int offset = -8;
        for (uint32_t vid : spilled_) {
            alloc.spill_map[vid] = offset;
            offset -= 8;
        }
        alloc.total_spill_size = -offset - 8;
        if (alloc.total_spill_size < 0) alloc.total_spill_size = 0;

        (void)fn; // fn reserved for future per-type sizing

        return alloc;
    }

    // ── Internal state ───────────────────────────────────────────

    std::vector<LiveRange> live_ranges_;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> interference_;
    std::unordered_map<uint32_t, int> coloring_;
    std::unordered_set<uint32_t> spilled_;
};

} // namespace rexc
