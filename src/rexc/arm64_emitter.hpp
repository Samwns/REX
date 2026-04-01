#pragma once
/*
 * arm64_emitter.hpp  –  ARM64 (AArch64) Machine Code Emitter
 *
 * Encodes AArch64 instructions into a byte buffer.  Used by the rexc
 * native backend to generate executables without any external assembler
 * on ARM64 platforms (Apple Silicon, Linux ARM64, etc.).
 *
 * Supports the subset of instructions needed for compiled C output:
 *   - Integer arithmetic (add, sub, mul, sdiv)
 *   - Move / load / store
 *   - Branch and link (call) / return
 *   - Comparisons and conditional branches
 *   - Linux/macOS syscalls via SVC #0x80 (macOS) or SVC #0 (Linux)
 *   - ADR/ADRP for address computation
 *
 * Calling convention: AAPCS64 (ARM Architecture Procedure Call Standard)
 *   - Args:    X0-X7
 *   - Return:  X0
 *   - Callee-saved: X19-X28, X29(FP), X30(LR), SP
 *   - Scratch: X9-X15
 */

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <cassert>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  ARM64 Register encoding
// ─────────────────────────────────────────────────────────────────
enum class ARM64Reg : uint8_t {
    X0  = 0,  X1  = 1,  X2  = 2,  X3  = 3,
    X4  = 4,  X5  = 5,  X6  = 6,  X7  = 7,
    X8  = 8,  X9  = 9,  X10 = 10, X11 = 11,
    X12 = 12, X13 = 13, X14 = 14, X15 = 15,
    X16 = 16, X17 = 17, X18 = 18, X19 = 19,
    X20 = 20, X21 = 21, X22 = 22, X23 = 23,
    X24 = 24, X25 = 25, X26 = 26, X27 = 27,
    X28 = 28,
    FP  = 29,  // Frame pointer (X29)
    LR  = 30,  // Link register (X30)
    SP  = 31,  // Stack pointer (also encodes XZR in some contexts)
    XZR = 31,  // Zero register
};

inline uint8_t arm64_reg(ARM64Reg r) { return static_cast<uint8_t>(r) & 0x1F; }

// Syscall numbers for Linux AArch64
// Undefine system macros from <sys/syscall.h> that may conflict
#ifdef SYS_write
#undef SYS_write
#endif
#ifdef SYS_exit
#undef SYS_exit
#endif
#ifdef SYS_brk
#undef SYS_brk
#endif
#ifdef SYS_mmap
#undef SYS_mmap
#endif
namespace arm64_syscall_nr {
    static constexpr uint64_t SYS_write = 64;
    static constexpr uint64_t SYS_exit  = 93;
    static constexpr uint64_t SYS_brk   = 214;
    static constexpr uint64_t SYS_mmap  = 222;
}

// Syscall numbers for macOS AArch64 (BSD layer, add 0x2000000)
namespace arm64_macos_syscall_nr {
    static constexpr uint64_t SYS_write = 0x2000004;
    static constexpr uint64_t SYS_exit  = 0x2000001;
}

// ─────────────────────────────────────────────────────────────────
//  Label / relocation tracking
// ─────────────────────────────────────────────────────────────────
struct ARM64Relocation {
    size_t   patch_offset;  // byte offset in code buffer
    uint32_t label_id;      // target label
    enum Type { BranchRel26, Adrp, AddLo12 } type;
};

// ─────────────────────────────────────────────────────────────────
//  ARM64Emitter  –  emits AArch64 machine code
// ─────────────────────────────────────────────────────────────────
class ARM64Emitter {
public:
    const std::vector<uint8_t>& code() const { return code_; }
    std::vector<uint8_t>& code() { return code_; }
    size_t size() const { return code_.size(); }

    // ── Label management ─────────────────────────────────────────
    uint32_t new_label() { return next_label_++; }

    void bind_label(uint32_t label) {
        labels_[label] = code_.size();
    }

    // Resolve all label references after code generation is complete.
    void resolve_labels(uint64_t code_vaddr) {
        for (auto& rel : relocs_) {
            auto it = labels_.find(rel.label_id);
            if (it == labels_.end()) continue;
            int64_t target_offset = static_cast<int64_t>(it->second);

            switch (rel.type) {
                case ARM64Relocation::BranchRel26: {
                    // B/BL: imm26 * 4 byte offset from instruction
                    int64_t disp = target_offset - static_cast<int64_t>(rel.patch_offset);
                    int32_t imm26 = static_cast<int32_t>(disp / 4);
                    uint32_t insn = read32_at(rel.patch_offset);
                    insn = (insn & 0xFC000000) | (imm26 & 0x03FFFFFF);
                    write32_at(rel.patch_offset, insn);
                    break;
                }
                case ARM64Relocation::Adrp: {
                    uint64_t target_addr = code_vaddr + target_offset;
                    uint64_t page_target = target_addr & ~0xFFFULL;
                    uint64_t page_insn = (code_vaddr + rel.patch_offset) & ~0xFFFULL;
                    int64_t page_diff = static_cast<int64_t>(page_target - page_insn);
                    int32_t immhi = static_cast<int32_t>((page_diff >> 14) & 0x7FFFF);
                    int32_t immlo = static_cast<int32_t>((page_diff >> 12) & 0x3);
                    uint32_t insn = read32_at(rel.patch_offset);
                    insn = (insn & 0x9F00001F) | (immhi << 5) | (immlo << 29);
                    write32_at(rel.patch_offset, insn);
                    break;
                }
                case ARM64Relocation::AddLo12: {
                    uint64_t target_addr = code_vaddr + target_offset;
                    uint32_t lo12 = static_cast<uint32_t>(target_addr & 0xFFF);
                    uint32_t insn = read32_at(rel.patch_offset);
                    insn = (insn & 0xFFC003FF) | (lo12 << 10);
                    write32_at(rel.patch_offset, insn);
                    break;
                }
            }
        }
    }

    // ── MOV reg, imm16 (MOVZ) ───────────────────────────────────
    void movz(ARM64Reg dst, uint16_t imm, uint8_t shift = 0) {
        // MOVZ Xd, #imm16, LSL #shift
        uint32_t hw = shift / 16;
        uint32_t insn = 0xD2800000 | (hw << 21) | (static_cast<uint32_t>(imm) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── MOVK reg, imm16, LSL #shift ────────────────────────────
    void movk(ARM64Reg dst, uint16_t imm, uint8_t shift = 0) {
        uint32_t hw = shift / 16;
        uint32_t insn = 0xF2800000 | (hw << 21) | (static_cast<uint32_t>(imm) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── Load 64-bit immediate into register ─────────────────────
    void mov_reg_imm64(ARM64Reg dst, uint64_t imm) {
        movz(dst, static_cast<uint16_t>(imm & 0xFFFF), 0);
        if (imm > 0xFFFF)
            movk(dst, static_cast<uint16_t>((imm >> 16) & 0xFFFF), 16);
        if (imm > 0xFFFFFFFF)
            movk(dst, static_cast<uint16_t>((imm >> 32) & 0xFFFF), 32);
        if (imm > 0xFFFFFFFFFFFF)
            movk(dst, static_cast<uint16_t>((imm >> 48) & 0xFFFF), 48);
    }

    // ── MOV reg, imm32 (sign-extended) ──────────────────────────
    void mov_reg_imm32(ARM64Reg dst, int32_t imm) {
        if (imm >= 0) {
            mov_reg_imm64(dst, static_cast<uint64_t>(imm));
        } else {
            // MOVN Xd, #~(imm & 0xFFFF)
            uint32_t insn = 0x92800000 | (static_cast<uint32_t>(~imm & 0xFFFF) << 5) | arm64_reg(dst);
            emit32(insn);
            if ((static_cast<uint64_t>(imm) >> 16) != 0xFFFFFFFFFFFF) {
                movk(dst, static_cast<uint16_t>((static_cast<uint64_t>(imm) >> 16) & 0xFFFF), 16);
            }
        }
    }

    // ── MOV reg, reg ────────────────────────────────────────────
    void mov_reg_reg(ARM64Reg dst, ARM64Reg src) {
        // ORR Xd, XZR, Xsrc
        uint32_t insn = 0xAA000000 | (arm64_reg(src) << 16) | (arm64_reg(ARM64Reg::XZR) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── STR Xsrc, [Xbase, #offset] (store 64-bit) ──────────────
    void str_reg_base_offset(ARM64Reg src, ARM64Reg base, int32_t offset) {
        if (offset >= 0 && (offset % 8) == 0 && offset < 32768) {
            // STR Xsrc, [Xbase, #uimm12*8]
            uint32_t uimm12 = static_cast<uint32_t>(offset / 8);
            uint32_t insn = 0xF9000000 | (uimm12 << 10) | (arm64_reg(base) << 5) | arm64_reg(src);
            emit32(insn);
        } else {
            // STUR Xsrc, [Xbase, #simm9]
            uint32_t simm9 = static_cast<uint32_t>(offset) & 0x1FF;
            uint32_t insn = 0xF8000000 | (simm9 << 12) | (arm64_reg(base) << 5) | arm64_reg(src);
            emit32(insn);
        }
    }

    // ── LDR Xdst, [Xbase, #offset] (load 64-bit) ───────────────
    void ldr_reg_base_offset(ARM64Reg dst, ARM64Reg base, int32_t offset) {
        if (offset >= 0 && (offset % 8) == 0 && offset < 32768) {
            uint32_t uimm12 = static_cast<uint32_t>(offset / 8);
            uint32_t insn = 0xF9400000 | (uimm12 << 10) | (arm64_reg(base) << 5) | arm64_reg(dst);
            emit32(insn);
        } else {
            uint32_t simm9 = static_cast<uint32_t>(offset) & 0x1FF;
            uint32_t insn = 0xF8400000 | (simm9 << 12) | (arm64_reg(base) << 5) | arm64_reg(dst);
            emit32(insn);
        }
    }

    // ── ADD reg, reg, reg ───────────────────────────────────────
    void add_reg_reg(ARM64Reg dst, ARM64Reg lhs, ARM64Reg rhs) {
        uint32_t insn = 0x8B000000 | (arm64_reg(rhs) << 16) | (arm64_reg(lhs) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── ADD reg, reg, #imm12 ────────────────────────────────────
    void add_reg_imm12(ARM64Reg dst, ARM64Reg src, uint32_t imm12) {
        uint32_t insn = 0x91000000 | ((imm12 & 0xFFF) << 10) | (arm64_reg(src) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── SUB reg, reg, reg ───────────────────────────────────────
    void sub_reg_reg(ARM64Reg dst, ARM64Reg lhs, ARM64Reg rhs) {
        uint32_t insn = 0xCB000000 | (arm64_reg(rhs) << 16) | (arm64_reg(lhs) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── SUB reg, reg, #imm12 ────────────────────────────────────
    void sub_reg_imm12(ARM64Reg dst, ARM64Reg src, uint32_t imm12) {
        uint32_t insn = 0xD1000000 | ((imm12 & 0xFFF) << 10) | (arm64_reg(src) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── MUL reg, reg, reg ───────────────────────────────────────
    void mul_reg_reg(ARM64Reg dst, ARM64Reg lhs, ARM64Reg rhs) {
        // MADD Xd, Xn, Xm, XZR
        uint32_t insn = 0x9B007C00 | (arm64_reg(rhs) << 16) | (arm64_reg(lhs) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── SDIV reg, reg, reg ──────────────────────────────────────
    void sdiv_reg_reg(ARM64Reg dst, ARM64Reg lhs, ARM64Reg rhs) {
        uint32_t insn = 0x9AC00C00 | (arm64_reg(rhs) << 16) | (arm64_reg(lhs) << 5) | arm64_reg(dst);
        emit32(insn);
    }

    // ── NEG reg, reg ────────────────────────────────────────────
    void neg_reg(ARM64Reg dst, ARM64Reg src) {
        // SUB Xd, XZR, Xsrc
        sub_reg_reg(dst, ARM64Reg::XZR, src);
    }

    // ── CMP reg, reg (SUBS XZR, Xn, Xm) ────────────────────────
    void cmp_reg_reg(ARM64Reg lhs, ARM64Reg rhs) {
        uint32_t insn = 0xEB000000 | (arm64_reg(rhs) << 16) | (arm64_reg(lhs) << 5) | arm64_reg(ARM64Reg::XZR);
        emit32(insn);
    }

    // ── CMP reg, #imm12 (SUBS XZR, Xn, #imm12) ────────────────
    void cmp_reg_imm12(ARM64Reg r, uint32_t imm12) {
        uint32_t insn = 0xF1000000 | ((imm12 & 0xFFF) << 10) | (arm64_reg(r) << 5) | arm64_reg(ARM64Reg::XZR);
        emit32(insn);
    }

    // ── STP X29, X30, [SP, #offset]! (pre-index) ────────────────
    void stp_pre(ARM64Reg rt1, ARM64Reg rt2, ARM64Reg base, int32_t offset) {
        uint32_t simm7 = static_cast<uint32_t>(offset / 8) & 0x7F;
        uint32_t insn = 0xA9800000 | (simm7 << 15) | (arm64_reg(rt2) << 10) | (arm64_reg(base) << 5) | arm64_reg(rt1);
        emit32(insn);
    }

    // ── LDP X29, X30, [SP], #offset (post-index) ────────────────
    void ldp_post(ARM64Reg rt1, ARM64Reg rt2, ARM64Reg base, int32_t offset) {
        uint32_t simm7 = static_cast<uint32_t>(offset / 8) & 0x7F;
        uint32_t insn = 0xA8C00000 | (simm7 << 15) | (arm64_reg(rt2) << 10) | (arm64_reg(base) << 5) | arm64_reg(rt1);
        emit32(insn);
    }

    // ── BL (branch with link, i.e., call) to label ──────────────
    void bl_label(uint32_t label) {
        relocs_.push_back({code_.size(), label, ARM64Relocation::BranchRel26});
        emit32(0x94000000);  // BL + placeholder
    }

    // ── B (unconditional branch) to label ───────────────────────
    void b_label(uint32_t label) {
        relocs_.push_back({code_.size(), label, ARM64Relocation::BranchRel26});
        emit32(0x14000000);  // B + placeholder
    }

    // ── B.cond (conditional branch) to label ────────────────────
    // cond: 0=EQ, 1=NE, 10=GE, 11=LT, 12=GT, 13=LE
    void bcond_label(uint8_t cond, uint32_t label) {
        relocs_.push_back({code_.size(), label, ARM64Relocation::BranchRel26});
        // B.cond uses imm19 at bits [23:5]
        uint32_t insn = 0x54000000 | cond;
        emit32(insn);
    }

    void beq_label(uint32_t label)  { bcond_label(0x0, label); }
    void bne_label(uint32_t label)  { bcond_label(0x1, label); }
    void blt_label(uint32_t label)  { bcond_label(0xB, label); }
    void bge_label(uint32_t label)  { bcond_label(0xA, label); }
    void ble_label(uint32_t label)  { bcond_label(0xD, label); }
    void bgt_label(uint32_t label)  { bcond_label(0xC, label); }

    // ── CBZ (compare and branch if zero) ────────────────────────
    void cbz_label(ARM64Reg rt, uint32_t label) {
        relocs_.push_back({code_.size(), label, ARM64Relocation::BranchRel26});
        uint32_t insn = 0xB4000000 | arm64_reg(rt);
        emit32(insn);
    }

    // ── CBNZ (compare and branch if not zero) ───────────────────
    void cbnz_label(ARM64Reg rt, uint32_t label) {
        relocs_.push_back({code_.size(), label, ARM64Relocation::BranchRel26});
        uint32_t insn = 0xB5000000 | arm64_reg(rt);
        emit32(insn);
    }

    // ── RET ─────────────────────────────────────────────────────
    void ret() {
        emit32(0xD65F03C0);  // RET X30
    }

    // ── SVC #0 (syscall on Linux) ───────────────────────────────
    void svc(uint16_t imm = 0) {
        uint32_t insn = 0xD4000001 | (static_cast<uint32_t>(imm) << 5);
        emit32(insn);
    }

    // ── NOP ─────────────────────────────────────────────────────
    void nop() { emit32(0xD503201F); }

    // ── CSET (conditional set) ──────────────────────────────────
    // cond: EQ=0, NE=1, GE=10, LT=11, GT=12, LE=13
    void cset(ARM64Reg dst, uint8_t cond) {
        // CSINC Xd, XZR, XZR, invert(cond)
        uint8_t inv_cond = cond ^ 1;
        uint32_t insn = 0x9A9F0400 | (inv_cond << 12) | arm64_reg(dst);
        emit32(insn);
    }

    // ── Function prologue ───────────────────────────────────────
    void function_prologue(int32_t local_size = 0) {
        int32_t frame_size = align_up(16 + local_size, 16); // 16 for FP+LR
        stp_pre(ARM64Reg::FP, ARM64Reg::LR, ARM64Reg::SP, -frame_size);
        mov_reg_reg(ARM64Reg::FP, ARM64Reg::SP);
    }

    // ── Function epilogue ───────────────────────────────────────
    void function_epilogue(int32_t local_size = 0) {
        int32_t frame_size = align_up(16 + local_size, 16);
        mov_reg_reg(ARM64Reg::SP, ARM64Reg::FP);
        ldp_post(ARM64Reg::FP, ARM64Reg::LR, ARM64Reg::SP, frame_size);
        ret();
    }

    // ── Emit exit syscall (Linux) ───────────────────────────────
    void emit_exit_linux(ARM64Reg exit_code_reg = ARM64Reg::X0) {
        if (exit_code_reg != ARM64Reg::X0)
            mov_reg_reg(ARM64Reg::X0, exit_code_reg);
        mov_reg_imm64(ARM64Reg::X8, arm64_syscall_nr::SYS_exit);
        svc(0);
    }

    // ── Emit exit syscall (macOS) ───────────────────────────────
    void emit_exit_macos(ARM64Reg exit_code_reg = ARM64Reg::X0) {
        if (exit_code_reg != ARM64Reg::X0)
            mov_reg_reg(ARM64Reg::X0, exit_code_reg);
        mov_reg_imm64(ARM64Reg::X16, 1);  // SYS_exit (BSD)
        svc(0x80);
    }

    // ── Emit write syscall (Linux) ──────────────────────────────
    void emit_write_syscall_linux() {
        mov_reg_imm64(ARM64Reg::X8, arm64_syscall_nr::SYS_write);
        svc(0);
    }

    // ── Emit write syscall (macOS) ──────────────────────────────
    void emit_write_syscall_macos() {
        mov_reg_imm64(ARM64Reg::X16, 4);  // SYS_write (BSD)
        svc(0x80);
    }

    // ── Get current code position ───────────────────────────────
    size_t current_offset() const { return code_.size(); }

    // ── Emit raw 32-bit word ────────────────────────────────────
    void emit_raw32(uint32_t word) { emit32(word); }

private:
    std::vector<uint8_t> code_;
    std::vector<ARM64Relocation> relocs_;
    std::unordered_map<uint32_t, size_t> labels_;
    uint32_t next_label_ = 1;

    void emit32(uint32_t val) {
        code_.push_back(val & 0xFF);
        code_.push_back((val >> 8) & 0xFF);
        code_.push_back((val >> 16) & 0xFF);
        code_.push_back((val >> 24) & 0xFF);
    }

    uint32_t read32_at(size_t offset) const {
        return code_[offset] | (code_[offset+1] << 8) |
               (code_[offset+2] << 16) | (code_[offset+3] << 24);
    }

    void write32_at(size_t offset, uint32_t val) {
        code_[offset]     = val & 0xFF;
        code_[offset + 1] = (val >> 8) & 0xFF;
        code_[offset + 2] = (val >> 16) & 0xFF;
        code_[offset + 3] = (val >> 24) & 0xFF;
    }

    static int32_t align_up(int32_t val, int32_t align) {
        return (val + align - 1) & ~(align - 1);
    }
};

} // namespace rexc
