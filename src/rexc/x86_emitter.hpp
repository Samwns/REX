#pragma once
/*
 * x86_emitter.hpp  –  x86_64 Machine Code Emitter
 *
 * Encodes x86_64 instructions into a byte buffer.  Used by the rexc
 * native backend to generate executables without any external assembler.
 *
 * Supports the subset of instructions needed for compiled C output:
 *   - Integer arithmetic (add, sub, imul, idiv)
 *   - Move / load / store
 *   - Push / pop / call / ret
 *   - Comparisons and conditional jumps
 *   - Linux syscalls via SYSCALL instruction
 *   - LEA for address computation (RIP-relative)
 */

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <cassert>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Register encoding (System V AMD64 ABI order)
// ─────────────────────────────────────────────────────────────────
enum class Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

// Lower 3 bits of register encoding
inline uint8_t reg_lo(Reg r) { return static_cast<uint8_t>(r) & 7; }

// Does this register need a REX.B or REX.R bit?
inline bool reg_ext(Reg r) { return static_cast<uint8_t>(r) >= 8; }

// Linux x86_64 syscall numbers
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
namespace syscall_nr {
    static constexpr uint64_t SYS_write = 1;
    static constexpr uint64_t SYS_exit  = 60;
    static constexpr uint64_t SYS_brk   = 12;
    static constexpr uint64_t SYS_mmap  = 9;
}

// ─────────────────────────────────────────────────────────────────
//  Label / relocation tracking
// ─────────────────────────────────────────────────────────────────
struct Relocation {
    size_t   patch_offset;  // offset in code buffer where imm32 lives
    uint32_t label_id;      // target label
    bool     relative;      // true = RIP-relative, false = absolute
};

// ─────────────────────────────────────────────────────────────────
//  X86Emitter  –  emits x86_64 machine code
// ─────────────────────────────────────────────────────────────────
class X86Emitter {
public:
    // Access the raw code buffer
    const std::vector<uint8_t>& code() const { return code_; }
    std::vector<uint8_t>& code() { return code_; }
    size_t size() const { return code_.size(); }

    // ── Label management ─────────────────────────────────────────
    uint32_t new_label() { return next_label_++; }

    void bind_label(uint32_t label) {
        labels_[label] = code_.size();
    }

    // Bind a label to an arbitrary offset (e.g. IAT address for Windows imports)
    void bind_label_at(uint32_t label, size_t offset) {
        labels_[label] = offset;
    }

    // Resolve all label references after code generation is complete.
    // code_vaddr is the virtual address of the start of the code buffer.
    void resolve_labels(uint64_t code_vaddr) {
        for (auto& rel : relocs_) {
            auto it = labels_.find(rel.label_id);
            if (it == labels_.end()) continue;
            uint64_t target = it->second;
            if (rel.relative) {
                // RIP-relative: target - (patch_offset + 4)
                int64_t disp = static_cast<int64_t>(target) -
                               static_cast<int64_t>(rel.patch_offset + 4);
                // Bounds check: rel32 displacement must fit in int32_t
                if (disp < INT32_MIN || disp > INT32_MAX) continue;
                write32_at(rel.patch_offset, static_cast<int32_t>(disp));
            } else {
                int64_t disp = static_cast<int64_t>(code_vaddr + target);
                if (disp < INT32_MIN || disp > INT32_MAX) continue;
                write32_at(rel.patch_offset, static_cast<int32_t>(disp));
            }
        }
    }

    // ── MOV reg, imm64 ──────────────────────────────────────────
    void mov_reg_imm64(Reg dst, uint64_t imm) {
        // REX.W + B8+rd io
        uint8_t rex = 0x48 | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0xB8 + reg_lo(dst));
        emit64(imm);
    }

    // ── MOV reg, imm32 (sign-extended to 64-bit) ────────────────
    void mov_reg_imm32(Reg dst, int32_t imm) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0xC7);
        emit(0xC0 | reg_lo(dst));
        emit32(imm);
    }

    // ── MOV reg, reg (64-bit) ───────────────────────────────────
    void mov_reg_reg(Reg dst, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(src) ? 4 : 0) | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0x89);
        emit(0xC0 | (reg_lo(src) << 3) | reg_lo(dst));
    }

    // ── MOV [reg+disp], reg (store 64-bit) ─────────────────────
    void mov_mem_reg(Reg base, int32_t disp, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(src) ? 4 : 0) | (reg_ext(base) ? 1 : 0);
        emit(rex);
        emit(0x89);
        emit_modrm_disp(reg_lo(src), base, disp);
    }

    // ── MOV reg, [reg+disp] (load 64-bit) ──────────────────────
    void mov_reg_mem(Reg dst, Reg base, int32_t disp) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 4 : 0) | (reg_ext(base) ? 1 : 0);
        emit(rex);
        emit(0x8B);
        emit_modrm_disp(reg_lo(dst), base, disp);
    }

    // ── XOR reg, reg (zeroing idiom) ────────────────────────────
    void xor_reg_reg(Reg dst, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(src) ? 4 : 0) | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0x31);
        emit(0xC0 | (reg_lo(src) << 3) | reg_lo(dst));
    }

    // ── ADD reg, reg ────────────────────────────────────────────
    void add_reg_reg(Reg dst, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(src) ? 4 : 0) | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0x01);
        emit(0xC0 | (reg_lo(src) << 3) | reg_lo(dst));
    }

    // ── ADD reg, imm32 ─────────────────────────────────────────
    void add_reg_imm32(Reg dst, int32_t imm) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        if (dst == Reg::RAX) {
            emit(0x05);
        } else {
            emit(0x81);
            emit(0xC0 | reg_lo(dst));
        }
        emit32(imm);
    }

    // ── SUB reg, reg ────────────────────────────────────────────
    void sub_reg_reg(Reg dst, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(src) ? 4 : 0) | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        emit(0x29);
        emit(0xC0 | (reg_lo(src) << 3) | reg_lo(dst));
    }

    // ── SUB reg, imm32 ─────────────────────────────────────────
    void sub_reg_imm32(Reg dst, int32_t imm) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 1 : 0);
        emit(rex);
        if (dst == Reg::RAX) {
            emit(0x2D);
        } else {
            emit(0x81);
            emit(0xE8 | reg_lo(dst));
        }
        emit32(imm);
    }

    // ── IMUL reg, reg ───────────────────────────────────────────
    void imul_reg_reg(Reg dst, Reg src) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 4 : 0) | (reg_ext(src) ? 1 : 0);
        emit(rex);
        emit(0x0F);
        emit(0xAF);
        emit(0xC0 | (reg_lo(dst) << 3) | reg_lo(src));
    }

    // ── NEG reg ─────────────────────────────────────────────────
    void neg_reg(Reg r) {
        uint8_t rex = 0x48 | (reg_ext(r) ? 1 : 0);
        emit(rex);
        emit(0xF7);
        emit(0xD8 | reg_lo(r));
    }

    // ── CMP reg, reg ────────────────────────────────────────────
    void cmp_reg_reg(Reg left, Reg right) {
        uint8_t rex = 0x48 | (reg_ext(right) ? 4 : 0) | (reg_ext(left) ? 1 : 0);
        emit(rex);
        emit(0x39);
        emit(0xC0 | (reg_lo(right) << 3) | reg_lo(left));
    }

    // ── CMP reg, imm32 ─────────────────────────────────────────
    void cmp_reg_imm32(Reg r, int32_t imm) {
        uint8_t rex = 0x48 | (reg_ext(r) ? 1 : 0);
        emit(rex);
        emit(0x81);
        emit(0xF8 | reg_lo(r));
        emit32(imm);
    }

    // ── PUSH reg ────────────────────────────────────────────────
    void push(Reg r) {
        if (reg_ext(r)) emit(0x41);
        emit(0x50 + reg_lo(r));
    }

    // ── POP reg ─────────────────────────────────────────────────
    void pop(Reg r) {
        if (reg_ext(r)) emit(0x41);
        emit(0x58 + reg_lo(r));
    }

    // ── CALL rel32 (to label) ───────────────────────────────────
    void call_label(uint32_t label) {
        emit(0xE8);
        relocs_.push_back({code_.size(), label, true});
        emit32(0);  // placeholder
    }

    // ── JMP rel32 (to label) ────────────────────────────────────
    void jmp_label(uint32_t label) {
        emit(0xE9);
        relocs_.push_back({code_.size(), label, true});
        emit32(0);  // placeholder
    }

    // ── Jcc rel32 (conditional jump to label) ───────────────────
    // cc: 0x84=JE, 0x85=JNE, 0x8C=JL, 0x8D=JGE, 0x8E=JLE, 0x8F=JG
    void jcc_label(uint8_t cc, uint32_t label) {
        emit(0x0F);
        emit(cc);
        relocs_.push_back({code_.size(), label, true});
        emit32(0);  // placeholder
    }

    void je_label(uint32_t label)  { jcc_label(0x84, label); }
    void jne_label(uint32_t label) { jcc_label(0x85, label); }
    void jl_label(uint32_t label)  { jcc_label(0x8C, label); }
    void jge_label(uint32_t label) { jcc_label(0x8D, label); }
    void jle_label(uint32_t label) { jcc_label(0x8E, label); }
    void jg_label(uint32_t label)  { jcc_label(0x8F, label); }

    // ── RET ─────────────────────────────────────────────────────
    void ret() { emit(0xC3); }

    // ── SYSCALL ─────────────────────────────────────────────────
    void syscall() { emit(0x0F); emit(0x05); }

    // ── CALL [RIP+disp32]  (indirect call through IAT) ─────────
    // Used on Windows to call imported functions via the Import
    // Address Table.  The displacement is patched after layout.
    void call_indirect_rip(int32_t disp) {
        emit(0xFF);           // opcode FF /2
        emit(0x15);           // ModRM: mod=00, reg=010, rm=101 (RIP-relative)
        emit32(disp);
    }

    // Emit CALL [RIP+label] — indirect call patched via label system.
    void call_indirect_rip_label(uint32_t label) {
        emit(0xFF);
        emit(0x15);
        relocs_.push_back({code_.size(), label, true});
        emit32(0);  // placeholder
    }

    // ── LEA reg, [RIP+disp32] ───────────────────────────────────
    // Used for loading addresses of string constants in rodata.
    // The displacement is relative to the RIP after this instruction.
    void lea_rip_relative(Reg dst, int32_t disp) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 4 : 0);
        emit(rex);
        emit(0x8D);
        emit((reg_lo(dst) << 3) | 0x05);  // ModRM: mod=00, rm=101 (RIP-relative)
        emit32(disp);
    }

    // LEA reg, [RIP+label] — patch later
    void lea_rip_label(Reg dst, uint32_t label) {
        uint8_t rex = 0x48 | (reg_ext(dst) ? 4 : 0);
        emit(rex);
        emit(0x8D);
        emit((reg_lo(dst) << 3) | 0x05);
        relocs_.push_back({code_.size(), label, true});
        emit32(0);  // placeholder
    }

    // ── Emit raw bytes (for instructions not individually encoded) ─
    void emit_raw(std::initializer_list<uint8_t> bytes) {
        for (auto b : bytes) emit(b);
    }

    // ── NOP ─────────────────────────────────────────────────────
    void nop() { emit(0x90); }

    // ── Emit function prologue ──────────────────────────────────
    void function_prologue(int32_t local_size = 0) {
        push(Reg::RBP);
        mov_reg_reg(Reg::RBP, Reg::RSP);
        if (local_size > 0) {
            sub_reg_imm32(Reg::RSP, align_up(local_size, 16));
        }
    }

    // ── Emit function epilogue ──────────────────────────────────
    void function_epilogue() {
        mov_reg_reg(Reg::RSP, Reg::RBP);
        pop(Reg::RBP);
        ret();
    }

    // ── Windows x64 function prologue (with 32-byte shadow space) ─
    void function_prologue_win(int32_t local_size = 0) {
        push(Reg::RBP);
        mov_reg_reg(Reg::RBP, Reg::RSP);
        // Windows x64 ABI: 32-byte shadow space + locals, 16-byte aligned
        int32_t total = 32 + local_size;
        total = align_up(total, 16);
        sub_reg_imm32(Reg::RSP, total);
    }

    // ═══════════════════════════════════════════════════════════════
    //  Platform-specific I/O and exit
    // ═══════════════════════════════════════════════════════════════

    // ── Linux/macOS: exit via syscall ────────────────────────────
    void emit_exit_syscall(Reg exit_code_reg = Reg::RDI) {
        if (exit_code_reg != Reg::RDI)
            mov_reg_reg(Reg::RDI, exit_code_reg);
        mov_reg_imm64(Reg::RAX, syscall_nr::SYS_exit);
        syscall();
    }

    // ── Linux/macOS: write(fd, buf, count) via syscall ──────────
    // RDI=fd, RSI=buf, RDX=count already set by caller
    void emit_write_syscall() {
        mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        syscall();
    }

    // ── Windows: ExitProcess(code) via IAT ──────────────────────
    // exit_code is in RCX (Windows x64 convention).
    // iat_label points to the IAT slot for ExitProcess.
    void emit_exit_win(Reg exit_code_reg, uint32_t iat_label) {
        if (exit_code_reg != Reg::RCX)
            mov_reg_reg(Reg::RCX, exit_code_reg);
        sub_reg_imm32(Reg::RSP, 32);   // shadow space
        call_indirect_rip_label(iat_label);
        // no need to restore — ExitProcess never returns
    }

    // ── Windows: GetStdHandle(nStdHandle) via IAT ───────────────
    // nStdHandle in RCX; result in RAX.
    void emit_get_std_handle_win(int32_t handle_id, uint32_t iat_label) {
        mov_reg_imm32(Reg::RCX, handle_id);
        sub_reg_imm32(Reg::RSP, 32);   // shadow space
        call_indirect_rip_label(iat_label);
        add_reg_imm32(Reg::RSP, 32);
    }

    // ── Windows: WriteFile(hFile, buf, len, &written, NULL) ─────
    // hFile in RCX, buf in RDX, len in R8, &written in R9.
    // Caller must set up these registers before calling.
    void emit_write_file_win(uint32_t iat_label) {
        sub_reg_imm32(Reg::RSP, 32);   // shadow space
        call_indirect_rip_label(iat_label);
        add_reg_imm32(Reg::RSP, 32);
    }

    // ── Legacy wrappers (backwards-compatible) ──────────────────
    void emit_exit(Reg exit_code_reg = Reg::RDI) {
        emit_exit_syscall(exit_code_reg);
    }

    // ── Get current code position ───────────────────────────────
    size_t current_offset() const { return code_.size(); }

    // ═══════════════════════════════════════════════════════════════
    //  SSE2 double-precision floating-point instructions
    // ═══════════════════════════════════════════════════════════════

    // ── MOVSD XMM0, [base+disp] (load double) ──────────────────
    void movsd_xmm0_mem(Reg base, int32_t disp) {
        emit(0xF2); emit(0x0F); emit(0x10);
        emit_modrm_disp(0, base, disp);  // reg=XMM0(0)
    }

    // ── MOVSD [base+disp], XMM0 (store double) ─────────────────
    void movsd_mem_xmm0(Reg base, int32_t disp) {
        emit(0xF2); emit(0x0F); emit(0x11);
        emit_modrm_disp(0, base, disp);  // reg=XMM0(0)
    }

    // ── MOVSD XMM1, [base+disp] (load double into XMM1) ────────
    void movsd_xmm1_mem(Reg base, int32_t disp) {
        emit(0xF2); emit(0x0F); emit(0x10);
        emit_modrm_disp(1, base, disp);  // reg=XMM1(1)
    }

    // ── MOVSD XMM0, XMM1 ───────────────────────────────────────
    void movsd_xmm0_xmm1() {
        emit(0xF2); emit(0x0F); emit(0x10); emit(0xC1);
    }

    // ── MOVSD XMM1, XMM0 ───────────────────────────────────────
    void movsd_xmm1_xmm0() {
        emit(0xF2); emit(0x0F); emit(0x10); emit(0xC8);
    }

    // ── ADDSD XMM0, XMM1 ───────────────────────────────────────
    void addsd_xmm0_xmm1() {
        emit(0xF2); emit(0x0F); emit(0x58); emit(0xC1);
    }

    // ── SUBSD XMM0, XMM1 ───────────────────────────────────────
    void subsd_xmm0_xmm1() {
        emit(0xF2); emit(0x0F); emit(0x5C); emit(0xC1);
    }

    // ── MULSD XMM0, XMM1 ───────────────────────────────────────
    void mulsd_xmm0_xmm1() {
        emit(0xF2); emit(0x0F); emit(0x59); emit(0xC1);
    }

    // ── DIVSD XMM0, XMM1 ───────────────────────────────────────
    void divsd_xmm0_xmm1() {
        emit(0xF2); emit(0x0F); emit(0x5E); emit(0xC1);
    }

    // ── CVTTSD2SI RAX, XMM0 (truncate double to int64) ─────────
    void cvttsd2si_rax_xmm0() {
        emit(0xF2); emit(0x48); emit(0x0F); emit(0x2C); emit(0xC0);
    }

    // ── CVTSI2SD XMM0, RAX (int64 to double) ───────────────────
    void cvtsi2sd_xmm0_rax() {
        emit(0xF2); emit(0x48); emit(0x0F); emit(0x2A); emit(0xC0);
    }

    // ── CVTSI2SD XMM1, RAX (int64 to double into XMM1) ─────────
    void cvtsi2sd_xmm1_rax() {
        emit(0xF2); emit(0x48); emit(0x0F); emit(0x2A); emit(0xC8);
    }

    // ── XORPD XMM0, XMM0 (zero XMM0) ──────────────────────────
    void xorpd_xmm0() {
        emit(0x66); emit(0x0F); emit(0x57); emit(0xC0);
    }

    // ── XORPD XMM1, XMM1 (zero XMM1) ──────────────────────────
    void xorpd_xmm1() {
        emit(0x66); emit(0x0F); emit(0x57); emit(0xC9);
    }

    // ── UCOMISD XMM0, XMM1 (compare doubles, set flags) ────────
    void ucomisd_xmm0_xmm1() {
        emit(0x66); emit(0x0F); emit(0x2E); emit(0xC1);
    }

    // ── MOVQ XMM0, RAX (move int64 bits to XMM0) ───────────────
    void movq_xmm0_rax() {
        emit(0x66); emit(0x48); emit(0x0F); emit(0x6E); emit(0xC0);
    }

private:
    std::vector<uint8_t> code_;
    std::vector<Relocation> relocs_;
    std::unordered_map<uint32_t, size_t> labels_;
    uint32_t next_label_ = 1;

    void emit(uint8_t byte) { code_.push_back(byte); }

    void emit32(int32_t val) {
        emit(val & 0xFF);
        emit((val >> 8) & 0xFF);
        emit((val >> 16) & 0xFF);
        emit((val >> 24) & 0xFF);
    }

    void emit64(uint64_t val) {
        for (int i = 0; i < 8; i++)
            emit((val >> (i * 8)) & 0xFF);
    }

    void write32_at(size_t offset, int32_t val) {
        code_[offset]     = val & 0xFF;
        code_[offset + 1] = (val >> 8) & 0xFF;
        code_[offset + 2] = (val >> 16) & 0xFF;
        code_[offset + 3] = (val >> 24) & 0xFF;
    }

    // Emit ModR/M + optional SIB + displacement for [base+disp]
    void emit_modrm_disp(uint8_t reg_field, Reg base, int32_t disp) {
        uint8_t base_lo = reg_lo(base);
        if (base_lo == 4) {
            // RSP/R12 needs SIB byte
            if (disp == 0 && base_lo != 5) {
                emit((reg_field << 3) | 0x04);
                emit(0x24);  // SIB: base=RSP, index=none
            } else if (disp >= -128 && disp <= 127) {
                emit(0x44 | (reg_field << 3));
                emit(0x24);
                emit(static_cast<uint8_t>(disp));
            } else {
                emit(0x84 | (reg_field << 3));
                emit(0x24);
                emit32(disp);
            }
        } else if (disp == 0 && base_lo != 5) {
            // [base] — no displacement (RBP/R13 always needs disp8)
            emit((reg_field << 3) | base_lo);
        } else if (disp >= -128 && disp <= 127) {
            // [base+disp8]
            emit(0x40 | (reg_field << 3) | base_lo);
            emit(static_cast<uint8_t>(disp));
        } else {
            // [base+disp32]
            emit(0x80 | (reg_field << 3) | base_lo);
            emit32(disp);
        }
    }

    static int32_t align_up(int32_t val, int32_t align) {
        return (val + align - 1) & ~(align - 1);
    }
};

} // namespace rexc
