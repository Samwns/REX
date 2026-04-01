#pragma once
/*
 * native_codegen.hpp  –  REXC Native Code Generator
 *
 * Generates machine code directly from the C++ AST, producing
 * a static executable *without any external compiler or linker*.
 *
 * This makes REX completely independent from GCC, Clang, and friends
 * on ALL supported platforms.
 *
 * Pipeline (v0.5):
 *   AST → IRGenerator → IRModule → IROptimizer → IRModule (optimised)
 *       → x86/ARM64 Emitter → ELF/PE/Mach-O writer → executable
 *
 * The generate_ir() method exposes the IR pipeline for inspection
 * and testing.  The main generate() method still uses the direct
 * AST-to-machine-code path; IR-based code emission will replace
 * it incrementally.
 *
 * Supported features (v0.5):
 *   - String literal output (cout << "...")
 *   - Integer output (cout << int)
 *   - endl / newline
 *   - Integer variables and arithmetic
 *   - Function declarations and calls
 *   - Return statements
 *   - If/else branching
 *   - While/for loops
 *   - Basic expressions (+, -, *, /, comparisons)
 *
 * Supported platforms:
 *   - x86_64 Linux (ELF64, System V ABI, Linux syscalls)
 *   - x86_64 Windows (PE32+, Windows x64 ABI)
 *   - x86_64 macOS (Mach-O, System V ABI, BSD syscalls)
 *   - ARM64 Linux (ELF64, AAPCS64, Linux syscalls)
 *   - ARM64 macOS (Mach-O, AAPCS64, BSD syscalls)
 */

#include "ast.hpp"
#include "semantic.hpp"
#include "ir.hpp"
#include "ir_gen.hpp"
#include "ir_opt.hpp"
#include "ir_printer.hpp"
#include "object_layout.hpp"
#include "mangler.hpp"
#include "regalloc.hpp"
#include "elf_writer.hpp"
#include "pe_writer.hpp"
#include "macho_writer.hpp"
#include "x86_emitter.hpp"
#include "arm64_emitter.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <iostream>
#include <cstring>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  String constant pool for .rodata
// ─────────────────────────────────────────────────────────────────
struct StringConstant {
    uint32_t label_id;
    std::string data;         // cooked (unescaped) string
    size_t rodata_offset;
};

// ─────────────────────────────────────────────────────────────────
//  Local variable tracking
// ─────────────────────────────────────────────────────────────────
struct LocalVar {
    int32_t rbp_offset;
    std::string type_hint;      // "int", "string", "double"
    std::string string_data;    // For string variables: the literal content
};

// ─────────────────────────────────────────────────────────────────
//  Function info
// ─────────────────────────────────────────────────────────────────
struct NativeFuncInfo {
    uint32_t label_id;
    std::string name;
    std::vector<std::string> param_names;
};

// ─────────────────────────────────────────────────────────────────
//  NativeCodeGenerator
// ─────────────────────────────────────────────────────────────────
class NativeCodeGenerator {
public:
    NativeCodeGenerator(const SemanticContext& ctx)
        : ctx_(ctx) {}

    bool generate(const TranslationUnit& tu, const std::string& output_path) {
        emit_ = X86Emitter();
        strings_.clear();
        str_pool_.clear();
        functions_.clear();
        locals_.clear();
        local_offset_ = 0;
        rodata_refs_.clear();

        // Allocate IAT labels for Windows (they'll be bound after layout)
        iat_ExitProcess_label_  = emit_.new_label();
        iat_GetStdHandle_label_ = emit_.new_label();
        iat_WriteFile_label_    = emit_.new_label();

        collect_info(tu);
        build_rodata();

        uint64_t entry_offset = emit_.current_offset();
        emit_start();
        emit_builtin_write_str();
        emit_builtin_write_int();
        emit_builtin_write_double();
        emit_builtin_write_newline();

        for (auto& d : tu.decls) {
            if (d && d->kind == NodeKind::FunctionDecl)
                emit_function(*d->as<FunctionDecl>());
            else if (d && d->kind == NodeKind::NamespaceDecl)
                emit_namespace(*d->as<NamespaceDecl>());
        }

        // Select the appropriate executable writer based on the host platform
#if defined(__APPLE__)
    #if defined(__aarch64__) || defined(_M_ARM64)
        uint64_t code_vaddr = MachOWriter::code_vaddr(MachOWriter::Arch::ARM64);
    #else
        uint64_t code_vaddr = MachOWriter::code_vaddr(MachOWriter::Arch::X86_64);
    #endif
#elif defined(_WIN32)
        uint64_t code_vaddr = PeWriter::code_vaddr();
#else
        uint64_t code_vaddr = ElfWriter::code_vaddr();
#endif

#if defined(_WIN32)
        // On Windows, we need to know IAT RVAs before resolving labels.
        // First, do a "dry run" write to get the import info, then bind
        // IAT labels and resolve all references.
        PeImportInfo imp_info;
        // Write with a temporary placeholder — we need the layout to
        // determine IAT RVAs.  PeWriter::write fills imp_info for us.
        // Bind IAT labels to their absolute virtual addresses so that
        // CALL [RIP+disp] relocations resolve correctly.
        //
        // The IAT lives in .idata at a known RVA.  We must convert
        // that RVA to an offset within the code's virtual address space.
        // Since labels are resolved as: target_offset - (patch_offset + 4),
        // and the call instruction uses RIP-relative addressing,
        // we bind each IAT label to: (IAT_RVA + IMAGE_BASE) - code_vaddr.
        // This way, resolve_labels computes the correct RIP-relative disp.

        // We need the PE layout to know IAT RVAs.  PeWriter calculates
        // them deterministically from code/rodata/data sizes.
        // Do a temp write to get the info, then re-write with patched code.
        {
            // First pass: write PE to get IAT layout
            PeWriter::write(output_path, emit_.code(), str_pool_,
                            {}, 0, entry_offset, &imp_info);

            // Bind IAT labels: convert RVA to label "offset" that the
            // resolve_labels system can use.  The label system resolves
            // RIP-relative as: (code_vaddr + label_offset) - (code_vaddr + patch_offset + 4)
            // We want the result to be: (IMAGE_BASE + iat_rva) - (code_vaddr + patch_offset + 4)
            // So label_offset = IMAGE_BASE + iat_rva - code_vaddr
            uint64_t iat_exit_abs  = PeWriter::IMAGE_BASE + imp_info.iat_ExitProcess;
            uint64_t iat_gsh_abs   = PeWriter::IMAGE_BASE + imp_info.iat_GetStdHandle;
            uint64_t iat_wf_abs    = PeWriter::IMAGE_BASE + imp_info.iat_WriteFile;

            // label_offset such that code_vaddr + label_offset == iat_abs
            // => label_offset = iat_abs - code_vaddr
            emit_.bind_label_at(iat_ExitProcess_label_,
                                static_cast<size_t>(iat_exit_abs - code_vaddr));
            emit_.bind_label_at(iat_GetStdHandle_label_,
                                static_cast<size_t>(iat_gsh_abs - code_vaddr));
            emit_.bind_label_at(iat_WriteFile_label_,
                                static_cast<size_t>(iat_wf_abs - code_vaddr));
        }

        // Bind string labels to rodata offsets so LEA [RIP+label] can resolve.
        // label_offset is relative to code start; rodata begins after code bytes.
        for (auto& sc : strings_) {
            emit_.bind_label_at(sc.label_id, emit_.code().size() + sc.rodata_offset);
        }

        emit_.resolve_labels(code_vaddr);
        patch_rodata_refs(code_vaddr);

        // Final write with patched code
        return PeWriter::write(output_path, emit_.code(), str_pool_,
                               {}, 0, entry_offset, &imp_info);

#else  // Linux / macOS
    // Bind string labels to rodata offsets so LEA [RIP+label] can resolve.
    // label_offset is relative to code start; rodata begins after code bytes.
    for (auto& sc : strings_) {
        emit_.bind_label_at(sc.label_id, emit_.code().size() + sc.rodata_offset);
    }

    emit_.resolve_labels(code_vaddr);
        patch_rodata_refs(code_vaddr);

    #if defined(__APPLE__)
        #if defined(__aarch64__) || defined(_M_ARM64)
        return MachOWriter::write(output_path, MachOWriter::Arch::ARM64,
                                  emit_.code(), str_pool_, {}, 0, entry_offset);
        #else
        return MachOWriter::write(output_path, MachOWriter::Arch::X86_64,
                                  emit_.code(), str_pool_, {}, 0, entry_offset);
        #endif
    #else
        return ElfWriter::write(output_path, emit_.code(), str_pool_,
                                {}, 0, entry_offset);
    #endif
#endif
    }

    static std::string can_compile_natively(const TranslationUnit& tu) {
        // Native stdin support is not fully implemented yet.
        // Refuse native codegen when input operators/functions are present
        // so the caller can fall back to the C backend.
        std::function<bool(const Node*)> uses_input = [&](const Node* node) -> bool {
            if (!node) return false;

            if (node->kind == NodeKind::BinaryExpr) {
                auto* be = node->as<BinaryExpr>();
                if (be->op == TokenKind::RShift) return true; // cin >> ...
            }

            if (node->kind == NodeKind::CallExpr) {
                auto* ce = node->as<CallExpr>();
                if (ce->callee) {
                    if (ce->callee->kind == NodeKind::IdentifierExpr) {
                        auto name = ce->callee->as<IdentifierExpr>()->name;
                        if (name == "getline") return true;
                    } else if (ce->callee->kind == NodeKind::ScopeExpr) {
                        auto* se = ce->callee->as<ScopeExpr>();
                        if (se->name == "getline") return true;
                    }
                }
            }

            bool found = false;
            visit_children(node, [&](const Node* child) {
                if (!found && uses_input(child)) found = true;
            });
            return found;
        };

        for (auto& d : tu.decls) {
            if (!d) continue;
            if (uses_input(d.get())) {
                return "stdin input (cin/getline) not supported yet by native backend";
            }
            switch (d->kind) {
                case NodeKind::FunctionDecl:
                case NodeKind::PreprocessorDecl:
                case NodeKind::UsingDecl:
                case NodeKind::NamespaceDecl:
                case NodeKind::VarDecl:
                case NodeKind::ClassDecl:
                case NodeKind::StructDecl:
                case NodeKind::ConstructorDecl:
                case NodeKind::DestructorDecl:
                case NodeKind::EnumDecl:
                case NodeKind::TypedefDecl:
                    break;
                case NodeKind::TemplateDecl:
                    // Templates are supported since v0.8 for basic cases.
                    break;
                default:
                    break;
            }
        }
        return "";
    }

    /// Generate the SSA IR for a translation unit (without writing an executable).
    /// Useful for testing the IR pipeline independently.
    ///   1. AST → IRGenerator → IRModule
    ///   2. IROptimizer → optimised IRModule
    /// Returns the (optionally optimised) IRModule.
    static IRModule generate_ir(const TranslationUnit& tu,
                                const SemanticContext& ctx,
                                bool optimise = true) {
        IRGenerator gen(ctx);
        IRModule mod = gen.generate(tu.decls);
        if (optimise) {
            IROptimizer opt;
            opt.run(mod);
        }
        return mod;
    }

private:
    const SemanticContext& ctx_;
    X86Emitter emit_;
    std::vector<StringConstant> strings_;
    std::vector<uint8_t> str_pool_;
    std::unordered_map<std::string, NativeFuncInfo> functions_;
    std::unordered_map<std::string, LocalVar> locals_;
    int32_t local_offset_ = 0;

    struct RodataRef {
        size_t instruction_offset;
        size_t rodata_offset;
    };
    std::vector<RodataRef> rodata_refs_;

    uint32_t main_label_ = 0;
    uint32_t write_str_label_ = 0;
    uint32_t write_int_label_ = 0;
    uint32_t write_double_label_ = 0;
    uint32_t write_newline_label_ = 0;
    size_t newline_rodata_offset_ = 0;
    size_t dot_rodata_offset_ = 0;
    size_t double_10_rodata_offset_ = 0;

    // Windows IAT labels — only used when targeting Windows
    uint32_t iat_ExitProcess_label_  = 0;
    uint32_t iat_GetStdHandle_label_ = 0;
    uint32_t iat_WriteFile_label_    = 0;

    // ═══════════════════════════════════════════════════════════════
    //  Phase 1: Collect information
    // ═══════════════════════════════════════════════════════════════

    void collect_info(const TranslationUnit& tu) {
        for (auto& d : tu.decls) {
            if (!d) continue;
            if (d->kind == NodeKind::FunctionDecl) {
                auto* fn = d->as<FunctionDecl>();
                NativeFuncInfo fi;
                fi.name = fn->name;
                fi.label_id = emit_.new_label();
                for (auto& p : fn->params)
                    if (p) fi.param_names.push_back(p->as<ParamDecl>()->name);
                functions_[fn->name] = fi;
                if (fn->name == "main") main_label_ = fi.label_id;
            } else if (d->kind == NodeKind::NamespaceDecl) {
                collect_namespace_info(*d->as<NamespaceDecl>());
            }
            collect_strings(d.get());
        }
    }

    void collect_namespace_info(const NamespaceDecl& ns) {
        for (auto& d : ns.decls) {
            if (!d) continue;
            if (d->kind == NodeKind::FunctionDecl) {
                auto* fn = d->as<FunctionDecl>();
                std::string full_name = ns.name + "_" + fn->name;
                NativeFuncInfo fi;
                fi.name = full_name;
                fi.label_id = emit_.new_label();
                for (auto& p : fn->params)
                    if (p) fi.param_names.push_back(p->as<ParamDecl>()->name);
                functions_[full_name] = fi;
            }
        }
    }

    void collect_strings(const Node* node) {
        if (!node) return;
        if (node->kind == NodeKind::StringLiteralExpr) {
            auto* s = node->as<StringLiteralExpr>();
            add_string(s->cooked);
        }
        visit_children(node, [this](const Node* child) {
            collect_strings(child);
        });
    }

    uint32_t add_string(const std::string& s) {
        for (auto& sc : strings_)
            if (sc.data == s) return sc.label_id;
        StringConstant sc;
        sc.label_id = emit_.new_label();
        sc.data = s;
        sc.rodata_offset = 0;
        strings_.push_back(sc);
        return sc.label_id;
    }

    /// Append a string to the rodata pool if not already present.
    /// Unlike add_string + build_rodata, this preserves existing offsets.
    void ensure_string_in_pool(const std::string& s) {
        for (auto& sc : strings_)
            if (sc.data == s) return;  // already in pool
        StringConstant sc;
        sc.label_id = emit_.new_label();
        sc.data = s;
        sc.rodata_offset = str_pool_.size();  // append at current end
        for (char c : s)
            str_pool_.push_back(static_cast<uint8_t>(c));
        str_pool_.push_back(0);
        strings_.push_back(sc);
    }

    // ═══════════════════════════════════════════════════════════════
    //  Phase 2: Build rodata
    // ═══════════════════════════════════════════════════════════════

    void build_rodata() {
        str_pool_.clear();
        for (auto& sc : strings_) {
            sc.rodata_offset = str_pool_.size();
            for (char c : sc.data)
                str_pool_.push_back(static_cast<uint8_t>(c));
            str_pool_.push_back(0);
        }
        newline_rodata_offset_ = str_pool_.size();
        str_pool_.push_back('\n');
        str_pool_.push_back(0);
        dot_rodata_offset_ = str_pool_.size();
        str_pool_.push_back('.');
        str_pool_.push_back(0);
        // 8-byte aligned constant 10.0 for double-to-string conversion
        while (str_pool_.size() % 8 != 0)
            str_pool_.push_back(0);
        double_10_rodata_offset_ = str_pool_.size();
        double val_10 = 10.0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&val_10);
        for (int i = 0; i < 8; i++)
            str_pool_.push_back(bytes[i]);
    }

    // ═══════════════════════════════════════════════════════════════
    //  Phase 3: Code generation  (platform-specific)
    // ═══════════════════════════════════════════════════════════════

#ifdef _WIN32
    // ── Windows: uses kernel32.dll via IAT ──────────────────────
    // Windows x64 calling convention: RCX, RDX, R8, R9 + 32-byte shadow

    // Helper: emit WriteFile(hStdout, buf, len, &written, NULL)
    // Caller puts buf address in R12, length in R13 before calling.
    // Uses stack for &written.
    void emit_win_write_call(Reg buf_reg, Reg len_reg) {
        // GetStdHandle(STD_OUTPUT_HANDLE = -11)
        emit_.mov_reg_imm32(Reg::RCX, -11);
        emit_.sub_reg_imm32(Reg::RSP, 32);
        emit_.call_indirect_rip_label(iat_GetStdHandle_label_);
        emit_.add_reg_imm32(Reg::RSP, 32);
        // RAX = hStdout
        // WriteFile(hFile=RAX, lpBuffer=buf, nBytes=len, &written, NULL)
        emit_.mov_reg_reg(Reg::RCX, Reg::RAX);    // hFile
        emit_.mov_reg_reg(Reg::RDX, buf_reg);      // lpBuffer
        emit_.mov_reg_reg(Reg::R8, len_reg);        // nNumberOfBytesToWrite
        emit_.mov_reg_reg(Reg::R9, Reg::RBP);       // lpNumberOfBytesWritten (use stack space)
        emit_.sub_reg_imm32(Reg::R9, 8);
        // 5th param (lpOverlapped = NULL) on stack
        // sub 48: 32 shadow + 8 param5 slot + 8 alignment pad → RSP stays 16-byte aligned
        emit_.sub_reg_imm32(Reg::RSP, 48);
        emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
        emit_.mov_mem_reg(Reg::RSP, 32, Reg::RAX);  // [RSP+32] = NULL (lpOverlapped)
        emit_.call_indirect_rip_label(iat_WriteFile_label_);
        emit_.add_reg_imm32(Reg::RSP, 48);
    }

    void emit_start() {
        // Windows entry: sub RSP for alignment + shadow space
        emit_.sub_reg_imm32(Reg::RSP, 40); // 32 shadow + 8 for alignment
        if (main_label_ != 0) {
            emit_.call_label(main_label_);
        } else {
            emit_.mov_reg_imm32(Reg::RCX, 1);
            emit_.sub_reg_imm32(Reg::RSP, 32);
            emit_.call_indirect_rip_label(iat_ExitProcess_label_);
            return;
        }
        // ExitProcess(main return value)
        emit_.mov_reg_reg(Reg::RCX, Reg::RAX);
        emit_.sub_reg_imm32(Reg::RSP, 32);
        emit_.call_indirect_rip_label(iat_ExitProcess_label_);
    }

    void emit_builtin_write_str() {
        // write_str(RSI=buf, RDX=len) — uses System V regs as internal ABI
        // (our internal functions use these regs, callers set them up)
        write_str_label_ = emit_.new_label();
        emit_.bind_label(write_str_label_);
        emit_.function_prologue_win(64);
        // Save args (RSI, RDX are our internal convention)
        emit_.mov_mem_reg(Reg::RBP, -40, Reg::RSI);  // save buf
        emit_.mov_mem_reg(Reg::RBP, -48, Reg::RDX);  // save len
        // GetStdHandle(-11)
        emit_.mov_reg_imm32(Reg::RCX, -11);
        emit_.sub_reg_imm32(Reg::RSP, 32);
        emit_.call_indirect_rip_label(iat_GetStdHandle_label_);
        emit_.add_reg_imm32(Reg::RSP, 32);
        // WriteFile(hFile, buf, len, &written, NULL)
        emit_.mov_reg_reg(Reg::RCX, Reg::RAX);                    // hFile
        emit_.mov_reg_mem(Reg::RDX, Reg::RBP, -40);               // lpBuffer
        emit_.mov_reg_mem(Reg::R8, Reg::RBP, -48);                // nBytes
        emit_.mov_reg_reg(Reg::R9, Reg::RBP);                      // &written
        emit_.sub_reg_imm32(Reg::R9, 56);
        // Allocate 48 bytes: 32 shadow + 8 param5 slot + 8 alignment pad.
        // 48 is a multiple of 16, so RSP stays 16-byte aligned before the CALL.
        // (Windows x64 ABI requires RSP % 16 == 0 before every CALL.)
        emit_.sub_reg_imm32(Reg::RSP, 48);
        emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
        emit_.mov_mem_reg(Reg::RSP, 32, Reg::RAX);                 // [RSP+32] = NULL (lpOverlapped)
        emit_.call_indirect_rip_label(iat_WriteFile_label_);
        emit_.add_reg_imm32(Reg::RSP, 48);
        emit_.function_epilogue();
    }

    void emit_builtin_write_int() {
        // write_int(RDI=value) — converts int to string and writes
        write_int_label_ = emit_.new_label();
        emit_.bind_label(write_int_label_);
        emit_.function_prologue_win(128);

        uint32_t positive_label = emit_.new_label();
        uint32_t print_loop = emit_.new_label();
        uint32_t print_done = emit_.new_label();
        uint32_t not_zero = emit_.new_label();

        emit_.mov_reg_reg(Reg::RAX, Reg::RDI);
        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jge_label(positive_label);

        // Negative: write '-' and negate
        emit_.push(Reg::RAX);
        emit_.mov_reg_imm32(Reg::RAX, 0x2D); // '-'
        emit_.mov_mem_reg(Reg::RBP, -80, Reg::RAX);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 80);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.sub_reg_imm32(Reg::RSP, 8);  // align: push above made RSP 8 mod 16
        emit_.call_label(write_str_label_);
        emit_.add_reg_imm32(Reg::RSP, 8);  // restore alignment
        emit_.pop(Reg::RAX);
        emit_.neg_reg(Reg::RAX);

        emit_.bind_label(positive_label);

        emit_.mov_reg_reg(Reg::R8, Reg::RBP);
        emit_.sub_reg_imm32(Reg::R8, 65);   // digit buffer area
        emit_.xor_reg_reg(Reg::R9, Reg::R9); // digit count

        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jne_label(not_zero);

        // Special case: value is 0
        emit_.mov_reg_imm32(Reg::RCX, 0x30); // '0'
        emit_.mov_mem_reg(Reg::RBP, -90, Reg::RCX);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 90);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.call_label(write_str_label_);
        emit_.jmp_label(print_done);

        emit_.bind_label(not_zero);
        emit_.bind_label(print_loop);

        emit_.xor_reg_reg(Reg::RDX, Reg::RDX);
        emit_.mov_reg_imm32(Reg::RCX, 10);
        emit_.emit_raw({0x48, 0x99});        // CQO
        emit_.emit_raw({0x48, 0xF7, 0xF9});  // IDIV RCX

        emit_.add_reg_imm32(Reg::RDX, 0x30);
        emit_.emit_raw({0x41, 0x88, 0x10});  // mov [r8], dl
        emit_.sub_reg_imm32(Reg::R8, 1);
        emit_.add_reg_imm32(Reg::R9, 1);

        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jne_label(print_loop);

        // Write the digits
        emit_.mov_reg_reg(Reg::RSI, Reg::R8);
        emit_.add_reg_imm32(Reg::RSI, 1);
        emit_.mov_reg_reg(Reg::RDX, Reg::R9);
        emit_.call_label(write_str_label_);

        emit_.bind_label(print_done);
        emit_.function_epilogue();
    }

    void emit_builtin_write_double() {
        // write_double (Windows): value in XMM0
        write_double_label_ = emit_.new_label();
        emit_.bind_label(write_double_label_);
        emit_.function_prologue_win(192);

        uint32_t positive_lbl = emit_.new_label();
        uint32_t frac_loop    = emit_.new_label();
        uint32_t frac_done    = emit_.new_label();
        uint32_t done_lbl     = emit_.new_label();

        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        emit_.jge_label(positive_lbl);

        // Print '-'
        emit_.mov_reg_imm32(Reg::RAX, 0x2D);
        emit_.mov_mem_reg(Reg::RBP, -170, Reg::RAX);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 170);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.call_label(write_str_label_);

        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.movsd_xmm1_xmm0();
        emit_.xorpd_xmm0();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        emit_.bind_label(positive_lbl);

        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.cvttsd2si_rax_xmm0();
        emit_.mov_mem_reg(Reg::RBP, -168, Reg::RAX);

        emit_.mov_reg_reg(Reg::RDI, Reg::RAX);
        emit_.call_label(write_int_label_);

        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.mov_reg_mem(Reg::RAX, Reg::RBP, -168);
        emit_.cvtsi2sd_xmm1_rax();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        emit_.je_label(done_lbl);

        {
            size_t dot_instr_start = emit_.current_offset();
            emit_.mov_reg_imm64(Reg::RSI, 0);
            rodata_refs_.push_back({dot_instr_start + 2, dot_rodata_offset_});
            emit_.mov_reg_imm32(Reg::RDX, 1);
            emit_.call_label(write_str_label_);
        }

        emit_.xor_reg_reg(Reg::R9, Reg::R9);
        emit_.xor_reg_reg(Reg::R10, Reg::R10);

        emit_.bind_label(frac_loop);

        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        {
            size_t ten_instr_start = emit_.current_offset();
            emit_.mov_reg_imm64(Reg::RAX, 0);
            rodata_refs_.push_back({ten_instr_start + 2, double_10_rodata_offset_});
        }
        emit_.movsd_xmm1_mem(Reg::RAX, 0);
        emit_.mulsd_xmm0_xmm1();

        emit_.cvttsd2si_rax_xmm0();
        emit_.cvtsi2sd_xmm1_rax();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        emit_.add_reg_imm32(Reg::RAX, 0x30);
        emit_.mov_reg_reg(Reg::RCX, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RCX, 130);
        emit_.add_reg_reg(Reg::RCX, Reg::R9);
        emit_.emit_raw({0x88, 0x01});

        emit_.cmp_reg_imm32(Reg::RAX, 0x30);
        uint32_t skip_update = emit_.new_label();
        emit_.je_label(skip_update);
        emit_.mov_reg_reg(Reg::R10, Reg::R9);
        emit_.add_reg_imm32(Reg::R10, 1);
        emit_.bind_label(skip_update);

        emit_.add_reg_imm32(Reg::R9, 1);
        emit_.cmp_reg_imm32(Reg::R9, 15);
        emit_.jge_label(frac_done);

        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        uint32_t not_done = emit_.new_label();
        emit_.jne_label(not_done);
        emit_.jmp_label(frac_done);
        emit_.bind_label(not_done);
        emit_.jmp_label(frac_loop);

        emit_.bind_label(frac_done);

        emit_.cmp_reg_imm32(Reg::R10, 0);
        uint32_t has_digits = emit_.new_label();
        emit_.jne_label(has_digits);
        emit_.mov_reg_imm32(Reg::R10, 1);
        emit_.bind_label(has_digits);

        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 130);
        emit_.mov_reg_reg(Reg::RDX, Reg::R10);
        emit_.call_label(write_str_label_);

        emit_.bind_label(done_lbl);
        emit_.function_epilogue();
    }

    void emit_builtin_write_newline() {
        write_newline_label_ = emit_.new_label();
        emit_.bind_label(write_newline_label_);
        emit_.function_prologue_win(64);
        size_t nl_instr_start = emit_.current_offset();
        emit_.mov_reg_imm64(Reg::RSI, 0);
        rodata_refs_.push_back({nl_instr_start + 2, newline_rodata_offset_});
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.call_label(write_str_label_);
        emit_.function_epilogue();
    }

    void emit_function(const FunctionDecl& fn) {
        auto it = functions_.find(fn.name);
        if (it == functions_.end()) return;

        emit_.bind_label(it->second.label_id);
        locals_.clear();
        local_offset_ = 0;

        int32_t local_space = count_locals(fn.body.get()) * 8;
        local_space += static_cast<int32_t>(fn.params.size()) * 8;
        if (local_space < 64) local_space = 64;

        // Windows x64: 32-byte shadow + locals
        emit_.function_prologue_win(local_space);

        // Windows x64 ABI: RCX, RDX, R8, R9
        static const Reg param_regs[] = {Reg::RCX, Reg::RDX, Reg::R8, Reg::R9};
        for (size_t i = 0; i < fn.params.size() && i < 4; i++) {
            auto* p = fn.params[i]->as<ParamDecl>();
            local_offset_ -= 8;
            locals_[p->name] = {local_offset_, "int"};
            emit_.mov_mem_reg(Reg::RBP, local_offset_, param_regs[i]);
        }

        if (fn.body) emit_stmt(*fn.body);

        if (fn.name == "main")
            emit_.xor_reg_reg(Reg::RAX, Reg::RAX);

        emit_.function_epilogue();
    }

    void emit_namespace(const NamespaceDecl& ns) {
        for (auto& d : ns.decls) {
            if (!d) continue;
            if (d->kind == NodeKind::FunctionDecl) {
                auto* fn = d->as<FunctionDecl>();
                std::string full_name = ns.name + "_" + fn->name;
                auto it = functions_.find(full_name);
                if (it == functions_.end()) continue;

                emit_.bind_label(it->second.label_id);
                locals_.clear();
                local_offset_ = 0;
                int32_t ls = count_locals(fn->body.get()) * 8
                           + static_cast<int32_t>(fn->params.size()) * 8;
                if (ls < 64) ls = 64;
                emit_.function_prologue_win(ls);

                static const Reg pr[] = {Reg::RCX, Reg::RDX, Reg::R8, Reg::R9};
                for (size_t i = 0; i < fn->params.size() && i < 4; i++) {
                    auto* p = fn->params[i]->as<ParamDecl>();
                    local_offset_ -= 8;
                    locals_[p->name] = {local_offset_, "int"};
                    emit_.mov_mem_reg(Reg::RBP, local_offset_, pr[i]);
                }
                if (fn->body) emit_stmt(*fn->body);
                emit_.function_epilogue();
            }
        }
    }

#else
    // ── Linux / macOS: uses syscalls ────────────────────────────
    // System V AMD64 ABI: RDI, RSI, RDX, RCX, R8, R9

    void emit_start() {
        if (main_label_ != 0) {
            emit_.call_label(main_label_);
        } else {
            emit_.mov_reg_imm32(Reg::RDI, 1);
            emit_.emit_exit_syscall(Reg::RDI);
            return;
        }
        emit_.mov_reg_reg(Reg::RDI, Reg::RAX);
        emit_.emit_exit_syscall(Reg::RDI);
    }

    void emit_builtin_write_str() {
        write_str_label_ = emit_.new_label();
        emit_.bind_label(write_str_label_);
        emit_.function_prologue();
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.syscall();
        emit_.function_epilogue();
    }

    void emit_builtin_write_int() {
        write_int_label_ = emit_.new_label();
        emit_.bind_label(write_int_label_);
        emit_.function_prologue(64);

        uint32_t positive_label = emit_.new_label();
        uint32_t print_loop = emit_.new_label();
        uint32_t print_done = emit_.new_label();
        uint32_t not_zero = emit_.new_label();

        emit_.mov_reg_reg(Reg::RAX, Reg::RDI);
        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jge_label(positive_label);

        // Negative: write '-' and negate
        emit_.push(Reg::RAX);
        emit_.mov_reg_imm32(Reg::RAX, 0x2D);
        emit_.mov_mem_reg(Reg::RBP, -64, Reg::RAX);
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 64);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.syscall();
        emit_.pop(Reg::RAX);
        emit_.neg_reg(Reg::RAX);

        emit_.bind_label(positive_label);

        emit_.mov_reg_reg(Reg::R8, Reg::RBP);
        emit_.sub_reg_imm32(Reg::R8, 1);
        emit_.xor_reg_reg(Reg::R9, Reg::R9);

        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jne_label(not_zero);

        // Write '0'
        emit_.mov_reg_imm32(Reg::RCX, 0x30);
        emit_.mov_mem_reg(Reg::RBP, -48, Reg::RCX);
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 48);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.syscall();
        emit_.jmp_label(print_done);

        emit_.bind_label(not_zero);
        emit_.bind_label(print_loop);

        emit_.xor_reg_reg(Reg::RDX, Reg::RDX);
        emit_.mov_reg_imm32(Reg::RCX, 10);
        emit_.emit_raw({0x48, 0x99});        // CQO
        emit_.emit_raw({0x48, 0xF7, 0xF9});  // IDIV RCX

        emit_.add_reg_imm32(Reg::RDX, 0x30);
        emit_.emit_raw({0x41, 0x88, 0x10});  // mov [r8], dl
        emit_.sub_reg_imm32(Reg::R8, 1);
        emit_.add_reg_imm32(Reg::R9, 1);

        emit_.cmp_reg_imm32(Reg::RAX, 0);
        emit_.jne_label(print_loop);

        emit_.mov_reg_reg(Reg::RSI, Reg::R8);
        emit_.add_reg_imm32(Reg::RSI, 1);
        emit_.mov_reg_reg(Reg::RDX, Reg::R9);
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.syscall();

        emit_.bind_label(print_done);
        emit_.function_epilogue();
    }

    void emit_builtin_write_double() {
        // write_double: value in XMM0
        // Prints integer part, '.', fractional digits (trimming trailing zeros)
        write_double_label_ = emit_.new_label();
        emit_.bind_label(write_double_label_);
        emit_.function_prologue(192);

        uint32_t positive_lbl = emit_.new_label();
        uint32_t frac_loop    = emit_.new_label();
        uint32_t frac_done    = emit_.new_label();
        uint32_t done_lbl     = emit_.new_label();

        // Save XMM0 to stack
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        // Check negative: compare with 0.0
        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        emit_.jge_label(positive_lbl);

        // Print '-' sign
        emit_.mov_reg_imm32(Reg::RAX, 0x2D);
        emit_.mov_mem_reg(Reg::RBP, -170, Reg::RAX);
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 170);
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.syscall();

        // Negate: XMM0 = 0 - XMM0
        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.movsd_xmm1_xmm0();
        emit_.xorpd_xmm0();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        emit_.bind_label(positive_lbl);

        // Extract integer part: RAX = (int64_t)XMM0
        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.cvttsd2si_rax_xmm0();
        emit_.mov_mem_reg(Reg::RBP, -168, Reg::RAX);

        // Print integer part via write_int
        emit_.mov_reg_reg(Reg::RDI, Reg::RAX);
        emit_.call_label(write_int_label_);

        // Compute fractional: XMM0 = original - integer_part
        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.mov_reg_mem(Reg::RAX, Reg::RBP, -168);
        emit_.cvtsi2sd_xmm1_rax();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        // Check if fractional is zero
        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        emit_.je_label(done_lbl);

        // Print '.'
        {
            size_t dot_instr_start = emit_.current_offset();
            emit_.mov_reg_imm64(Reg::RSI, 0);
            rodata_refs_.push_back({dot_instr_start + 2, dot_rodata_offset_});
            emit_.mov_reg_imm32(Reg::RDX, 1);
            emit_.call_label(write_str_label_);
        }

        // Generate fractional digits into buffer [RBP-130..RBP-115]
        emit_.xor_reg_reg(Reg::R9, Reg::R9);   // digit count
        emit_.xor_reg_reg(Reg::R10, Reg::R10);  // last non-zero count

        emit_.bind_label(frac_loop);

        // Load 10.0 from rodata via absolute address
        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        {
            size_t ten_instr_start = emit_.current_offset();
            emit_.mov_reg_imm64(Reg::RAX, 0);
            rodata_refs_.push_back({ten_instr_start + 2, double_10_rodata_offset_});
        }
        emit_.movsd_xmm1_mem(Reg::RAX, 0);
        emit_.mulsd_xmm0_xmm1();

        // digit = (int)XMM0
        emit_.cvttsd2si_rax_xmm0();
        // fractional = XMM0 - digit
        emit_.cvtsi2sd_xmm1_rax();
        emit_.subsd_xmm0_xmm1();
        emit_.movsd_mem_xmm0(Reg::RBP, -160);

        // Store ASCII digit in buffer [RBP - 130 + R9]
        emit_.add_reg_imm32(Reg::RAX, 0x30);
        emit_.mov_reg_reg(Reg::RCX, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RCX, 130);
        emit_.add_reg_reg(Reg::RCX, Reg::R9);
        emit_.emit_raw({0x88, 0x01});  // mov [RCX], AL

        // If digit != '0', update last non-zero position
        emit_.cmp_reg_imm32(Reg::RAX, 0x30);
        uint32_t skip_update = emit_.new_label();
        emit_.je_label(skip_update);
        emit_.mov_reg_reg(Reg::R10, Reg::R9);
        emit_.add_reg_imm32(Reg::R10, 1);
        emit_.bind_label(skip_update);

        emit_.add_reg_imm32(Reg::R9, 1);
        emit_.cmp_reg_imm32(Reg::R9, 15);
        emit_.jge_label(frac_done);

        // Check if remaining fractional is approximately zero
        emit_.movsd_xmm0_mem(Reg::RBP, -160);
        emit_.xorpd_xmm1();
        emit_.ucomisd_xmm0_xmm1();
        uint32_t not_done = emit_.new_label();
        emit_.jne_label(not_done);
        emit_.jmp_label(frac_done);
        emit_.bind_label(not_done);
        emit_.jmp_label(frac_loop);

        emit_.bind_label(frac_done);

        // Print digits up to last non-zero (at least 1)
        emit_.cmp_reg_imm32(Reg::R10, 0);
        uint32_t has_digits = emit_.new_label();
        emit_.jne_label(has_digits);
        emit_.mov_reg_imm32(Reg::R10, 1);
        emit_.bind_label(has_digits);

        emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
        emit_.sub_reg_imm32(Reg::RSI, 130);
        emit_.mov_reg_reg(Reg::RDX, Reg::R10);
        emit_.call_label(write_str_label_);

        emit_.bind_label(done_lbl);
        emit_.function_epilogue();
    }

    void emit_builtin_write_newline() {
        write_newline_label_ = emit_.new_label();
        emit_.bind_label(write_newline_label_);
        emit_.function_prologue();
        size_t nl_instr_start = emit_.current_offset();
        emit_.mov_reg_imm64(Reg::RSI, 0);
        rodata_refs_.push_back({nl_instr_start + 2, newline_rodata_offset_});
        emit_.mov_reg_imm32(Reg::RDX, 1);
        emit_.mov_reg_imm64(Reg::RAX, syscall_nr::SYS_write);
        emit_.mov_reg_imm32(Reg::RDI, 1);
        emit_.syscall();
        emit_.function_epilogue();
    }

    void emit_function(const FunctionDecl& fn) {
        auto it = functions_.find(fn.name);
        if (it == functions_.end()) return;

        emit_.bind_label(it->second.label_id);
        locals_.clear();
        local_offset_ = 0;

        int32_t local_space = count_locals(fn.body.get()) * 8;
        local_space += static_cast<int32_t>(fn.params.size()) * 8;
        if (local_space < 64) local_space = 64;

        emit_.function_prologue(local_space);

        static const Reg param_regs[] = {Reg::RDI, Reg::RSI, Reg::RDX,
                                          Reg::RCX, Reg::R8,  Reg::R9};
        for (size_t i = 0; i < fn.params.size() && i < 6; i++) {
            auto* p = fn.params[i]->as<ParamDecl>();
            local_offset_ -= 8;
            locals_[p->name] = {local_offset_, "int"};
            emit_.mov_mem_reg(Reg::RBP, local_offset_, param_regs[i]);
        }

        if (fn.body) emit_stmt(*fn.body);

        if (fn.name == "main")
            emit_.xor_reg_reg(Reg::RAX, Reg::RAX);

        emit_.function_epilogue();
    }

    void emit_namespace(const NamespaceDecl& ns) {
        for (auto& d : ns.decls) {
            if (!d) continue;
            if (d->kind == NodeKind::FunctionDecl) {
                auto* fn = d->as<FunctionDecl>();
                std::string full_name = ns.name + "_" + fn->name;
                auto it = functions_.find(full_name);
                if (it == functions_.end()) continue;

                emit_.bind_label(it->second.label_id);
                locals_.clear();
                local_offset_ = 0;
                int32_t ls = count_locals(fn->body.get()) * 8
                           + static_cast<int32_t>(fn->params.size()) * 8;
                if (ls < 64) ls = 64;
                emit_.function_prologue(ls);

                static const Reg pr[] = {Reg::RDI, Reg::RSI, Reg::RDX,
                                          Reg::RCX, Reg::R8,  Reg::R9};
                for (size_t i = 0; i < fn->params.size() && i < 6; i++) {
                    auto* p = fn->params[i]->as<ParamDecl>();
                    local_offset_ -= 8;
                    locals_[p->name] = {local_offset_, "int"};
                    emit_.mov_mem_reg(Reg::RBP, local_offset_, pr[i]);
                }
                if (fn->body) emit_stmt(*fn->body);
                emit_.function_epilogue();
            }
        }
    }
#endif  // _WIN32

    // ═══════════════════════════════════════════════════════════════
    //  Type detection helpers
    // ═══════════════════════════════════════════════════════════════

    static bool is_string_type(const Node* type) {
        if (!type) return false;
        if (type->kind == NodeKind::NamedType) {
            auto* nt = type->as<NamedType>();
            return nt->name == "string" || nt->name == "wstring";
        }
        return false;
    }

    static bool is_double_type(const Node* type) {
        if (!type) return false;
        if (type->kind == NodeKind::PrimitiveType) {
            auto* pt = type->as<PrimitiveType>();
            return pt->prim_kind == PrimitiveType::Kind::Double ||
                   pt->prim_kind == PrimitiveType::Kind::Float ||
                   pt->prim_kind == PrimitiveType::Kind::LongDouble;
        }
        return false;
    }

    /// Check if an expression involves doubles (for cout type dispatch)
    bool is_double_expr(const Node& node) const {
        if (node.kind == NodeKind::FloatLiteralExpr) return true;
        if (node.kind == NodeKind::IdentifierExpr) {
            auto it = locals_.find(node.as<IdentifierExpr>()->name);
            return it != locals_.end() && it->second.type_hint == "double";
        }
        if (node.kind == NodeKind::BinaryExpr) {
            auto& be = *node.as<BinaryExpr>();
            return (be.left && is_double_expr(*be.left)) ||
                   (be.right && is_double_expr(*be.right));
        }
        if (node.kind == NodeKind::UnaryExpr) {
            auto& ue = *node.as<UnaryExpr>();
            return ue.operand && is_double_expr(*ue.operand);
        }
        if (node.kind == NodeKind::ParenExpr) {
            auto& pe = *node.as<ParenExpr>();
            return pe.inner && is_double_expr(*pe.inner);
        }
        return false;
    }

    /// Try to resolve a string expression at compile time.
    /// Returns {true, content} if resolvable, {false, ""} otherwise.
    std::pair<bool, std::string> try_resolve_string(const Node& expr) const {
        if (expr.kind == NodeKind::StringLiteralExpr)
            return {true, expr.as<StringLiteralExpr>()->cooked};
        if (expr.kind == NodeKind::IdentifierExpr) {
            auto it = locals_.find(expr.as<IdentifierExpr>()->name);
            if (it != locals_.end() && it->second.type_hint == "string")
                return {true, it->second.string_data};
        }
        if (expr.kind == NodeKind::BinaryExpr) {
            auto& be = *expr.as<BinaryExpr>();
            if (be.op == TokenKind::Plus && be.left && be.right) {
                auto [left_ok, left_val] = try_resolve_string(*be.left);
                auto [right_ok, right_val] = try_resolve_string(*be.right);
                if (left_ok && right_ok) return {true, left_val + right_val};
            }
        }
        return {false, ""};
    }

    // ═══════════════════════════════════════════════════════════════
    //  Double-precision expression emission (result → XMM0)
    // ═══════════════════════════════════════════════════════════════

    void emit_expr_double(const Node& node) {
        switch (node.kind) {
            case NodeKind::FloatLiteralExpr: {
                double val = node.as<FloatLiteralExpr>()->value;
                uint64_t bits;
                std::memcpy(&bits, &val, 8);
                emit_.mov_reg_imm64(Reg::RAX, bits);
                emit_.movq_xmm0_rax();
                break;
            }
            case NodeKind::IntLiteralExpr: {
                emit_.mov_reg_imm64(Reg::RAX, node.as<IntLiteralExpr>()->value);
                emit_.cvtsi2sd_xmm0_rax();
                break;
            }
            case NodeKind::IdentifierExpr: {
                auto it = locals_.find(node.as<IdentifierExpr>()->name);
                if (it != locals_.end()) {
                    if (it->second.type_hint == "double")
                        emit_.movsd_xmm0_mem(Reg::RBP, it->second.rbp_offset);
                    else {
                        // int variable → convert to double
                        emit_.mov_reg_mem(Reg::RAX, Reg::RBP, it->second.rbp_offset);
                        emit_.cvtsi2sd_xmm0_rax();
                    }
                }
                break;
            }
            case NodeKind::BinaryExpr: {
                auto& be = *node.as<BinaryExpr>();
                if (be.left) emit_expr_double(*be.left);    // XMM0 = left
                emit_.movsd_mem_xmm0(Reg::RBP, -120);       // save to stack
                if (be.right) emit_expr_double(*be.right);   // XMM0 = right
                emit_.movsd_xmm1_xmm0();                    // XMM1 = right
                emit_.movsd_xmm0_mem(Reg::RBP, -120);       // XMM0 = left
                switch (be.op) {
                    case TokenKind::Plus:  emit_.addsd_xmm0_xmm1(); break;
                    case TokenKind::Minus: emit_.subsd_xmm0_xmm1(); break;
                    case TokenKind::Star:  emit_.mulsd_xmm0_xmm1(); break;
                    case TokenKind::Slash: emit_.divsd_xmm0_xmm1(); break;
                    default: break;
                }
                break;
            }
            case NodeKind::UnaryExpr: {
                auto& ue = *node.as<UnaryExpr>();
                if (ue.operand) emit_expr_double(*ue.operand);
                if (ue.op == TokenKind::Minus) {
                    // Negate: 0.0 - XMM0
                    emit_.movsd_xmm1_xmm0();
                    emit_.xorpd_xmm0();
                    emit_.subsd_xmm0_xmm1();
                }
                break;
            }
            case NodeKind::ParenExpr:
                if (node.as<ParenExpr>()->inner)
                    emit_expr_double(*node.as<ParenExpr>()->inner);
                break;
            case NodeKind::CallExpr: {
                emit_call_expr(*node.as<CallExpr>(), Reg::RAX);
                emit_.cvtsi2sd_xmm0_rax();
                break;
            }
            default:
                emit_.xorpd_xmm0();
                break;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //  Statement emission
    // ═══════════════════════════════════════════════════════════════

    void emit_stmt(const Node& node) {
        switch (node.kind) {
            case NodeKind::CompoundStmt:
                for (auto& s : node.as<CompoundStmt>()->stmts)
                    if (s) emit_stmt(*s);
                break;
            case NodeKind::ExprStmt:
                if (node.as<ExprStmt>()->expr) emit_expr(*node.as<ExprStmt>()->expr, Reg::RAX);
                break;
            case NodeKind::ReturnStmt: {
                auto& rs = *node.as<ReturnStmt>();
                if (rs.value) emit_expr(*rs.value, Reg::RAX);
                else emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
                emit_.function_epilogue();
                break;
            }
            case NodeKind::VarDecl:
                emit_var_decl_node(*node.as<VarDecl>());
                break;
            case NodeKind::DeclStmt:
                for (auto& decl : node.as<DeclStmt>()->decls)
                    if (decl && decl->kind == NodeKind::VarDecl)
                        emit_var_decl_node(*decl->as<VarDecl>());
                break;
            case NodeKind::IfStmt: {
                auto& ifs = *node.as<IfStmt>();
                uint32_t else_lbl = emit_.new_label();
                uint32_t end_lbl = emit_.new_label();
                if (ifs.condition) emit_expr(*ifs.condition, Reg::RAX);
                emit_.cmp_reg_imm32(Reg::RAX, 0);
                emit_.je_label(ifs.else_branch ? else_lbl : end_lbl);
                if (ifs.then_branch) emit_stmt(*ifs.then_branch);
                if (ifs.else_branch) {
                    emit_.jmp_label(end_lbl);
                    emit_.bind_label(else_lbl);
                    emit_stmt(*ifs.else_branch);
                }
                emit_.bind_label(end_lbl);
                break;
            }
            case NodeKind::WhileStmt: {
                auto& ws = *node.as<WhileStmt>();
                uint32_t loop_lbl = emit_.new_label();
                uint32_t end_lbl = emit_.new_label();
                emit_.bind_label(loop_lbl);
                if (ws.condition) emit_expr(*ws.condition, Reg::RAX);
                emit_.cmp_reg_imm32(Reg::RAX, 0);
                emit_.je_label(end_lbl);
                if (ws.body) emit_stmt(*ws.body);
                emit_.jmp_label(loop_lbl);
                emit_.bind_label(end_lbl);
                break;
            }
            case NodeKind::ForStmt: {
                auto& fs = *node.as<ForStmt>();
                uint32_t cond_lbl = emit_.new_label();
                uint32_t end_lbl = emit_.new_label();
                if (fs.init) emit_stmt(*fs.init);
                emit_.bind_label(cond_lbl);
                if (fs.condition) {
                    emit_expr(*fs.condition, Reg::RAX);
                    emit_.cmp_reg_imm32(Reg::RAX, 0);
                    emit_.je_label(end_lbl);
                }
                if (fs.body) emit_stmt(*fs.body);
                if (fs.increment) emit_expr(*fs.increment, Reg::RAX);
                emit_.jmp_label(cond_lbl);
                emit_.bind_label(end_lbl);
                break;
            }
            default: break;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //  Expression emission (result → dest register)
    // ═══════════════════════════════════════════════════════════════

    void emit_expr(const Node& node, Reg dest) {
        switch (node.kind) {
            case NodeKind::IntLiteralExpr:
                emit_.mov_reg_imm64(dest, node.as<IntLiteralExpr>()->value);
                break;
            case NodeKind::StringLiteralExpr:
                emit_load_string_addr(dest, node.as<StringLiteralExpr>()->cooked);
                break;
            case NodeKind::BoolLiteralExpr:
                emit_.mov_reg_imm32(dest, node.as<BoolLiteralExpr>()->value ? 1 : 0);
                break;
            case NodeKind::CharLiteralExpr:
                emit_.mov_reg_imm32(dest, node.as<CharLiteralExpr>()->char_val);
                break;
            case NodeKind::IdentifierExpr: {
                auto it = locals_.find(node.as<IdentifierExpr>()->name);
                if (it != locals_.end())
                    emit_.mov_reg_mem(dest, Reg::RBP, it->second.rbp_offset);
                else
                    emit_.xor_reg_reg(dest, dest);
                break;
            }
            case NodeKind::BinaryExpr:
                emit_binary_expr(*node.as<BinaryExpr>(), dest);
                break;
            case NodeKind::UnaryExpr: {
                auto& ue = *node.as<UnaryExpr>();
                if ((ue.op == TokenKind::PlusPlus || ue.op == TokenKind::MinusMinus) &&
                    ue.operand && ue.operand->kind == NodeKind::IdentifierExpr) {
                    // ++ / -- on a local variable
                    auto& var_name = ue.operand->as<IdentifierExpr>()->name;
                    auto local_it = locals_.find(var_name);
                    if (local_it != locals_.end()) {
                        emit_.mov_reg_mem(Reg::RAX, Reg::RBP, local_it->second.rbp_offset);
                        if (ue.is_postfix) emit_.push(Reg::RAX); // save old value for return
                        if (ue.op == TokenKind::PlusPlus) emit_.add_reg_imm32(Reg::RAX, 1);
                        else                              emit_.sub_reg_imm32(Reg::RAX, 1);
                        emit_.mov_mem_reg(Reg::RBP, local_it->second.rbp_offset, Reg::RAX);
                        if (ue.is_postfix) emit_.pop(dest); // return old value
                        else if (dest != Reg::RAX) emit_.mov_reg_reg(dest, Reg::RAX);
                    }
                } else {
                    if (ue.operand) emit_expr(*ue.operand, dest);
                    if (ue.op == TokenKind::Minus) emit_.neg_reg(dest);
                }
                break;
            }
            case NodeKind::CallExpr:
                emit_call_expr(*node.as<CallExpr>(), dest);
                break;
            case NodeKind::AssignExpr: {
                auto& ae = *node.as<AssignExpr>();
                if (ae.value) emit_expr(*ae.value, Reg::RAX);
                if (ae.target && ae.target->kind == NodeKind::IdentifierExpr) {
                    auto& target_name = ae.target->as<IdentifierExpr>()->name;
                    auto target_it = locals_.find(target_name);
                    if (target_it != locals_.end()) {
                        if (ae.op == TokenKind::Assign) {
                            emit_.mov_mem_reg(Reg::RBP, target_it->second.rbp_offset, Reg::RAX);
                        } else {
                            // Compound assignment: lhs op= rhs
                            emit_.push(Reg::RAX);  // save rhs
                            emit_.mov_reg_mem(Reg::RAX, Reg::RBP, target_it->second.rbp_offset);
                            emit_.pop(Reg::RCX);   // rhs → RCX
                            if      (ae.op == TokenKind::PlusAssign)  emit_.add_reg_reg(Reg::RAX, Reg::RCX);
                            else if (ae.op == TokenKind::MinusAssign) emit_.sub_reg_reg(Reg::RAX, Reg::RCX);
                            else if (ae.op == TokenKind::StarAssign)  emit_.imul_reg_reg(Reg::RAX, Reg::RCX);
                            emit_.mov_mem_reg(Reg::RBP, target_it->second.rbp_offset, Reg::RAX);
                        }
                    }
                }
                if (dest != Reg::RAX) emit_.mov_reg_reg(dest, Reg::RAX);
                break;
            }
            case NodeKind::ParenExpr:
                if (node.as<ParenExpr>()->inner)
                    emit_expr(*node.as<ParenExpr>()->inner, dest);
                break;
            case NodeKind::ScopeExpr:
                break;
            default: break;
        }
    }

    // ── Binary expressions ──────────────────────────────────────
    void emit_binary_expr(const BinaryExpr& be, Reg dest) {
        if (be.op == TokenKind::LShift) {
            emit_cout_chain(be);
            return;
        }
        if (be.left) emit_expr(*be.left, Reg::RAX);
        emit_.push(Reg::RAX);
        if (be.right) emit_expr(*be.right, Reg::RCX);
        emit_.pop(Reg::RAX);

        switch (be.op) {
            case TokenKind::Plus:  emit_.add_reg_reg(Reg::RAX, Reg::RCX); break;
            case TokenKind::Minus: emit_.sub_reg_reg(Reg::RAX, Reg::RCX); break;
            case TokenKind::Star:  emit_.imul_reg_reg(Reg::RAX, Reg::RCX); break;
            case TokenKind::Slash:
                emit_.emit_raw({0x48, 0x99});
                emit_.emit_raw({0x48, 0xF7, 0xF9});
                break;
            case TokenKind::Percent:
                emit_.emit_raw({0x48, 0x99});
                emit_.emit_raw({0x48, 0xF7, 0xF9});
                emit_.mov_reg_reg(Reg::RAX, Reg::RDX);
                break;
            case TokenKind::Eq: case TokenKind::NotEq:
            case TokenKind::Lt: case TokenKind::Gt:
            case TokenKind::LtEq: case TokenKind::GtEq: {
                emit_.cmp_reg_reg(Reg::RAX, Reg::RCX);
                uint8_t setcc = 0x94;
                if (be.op == TokenKind::NotEq) setcc = 0x95;
                if (be.op == TokenKind::Lt)    setcc = 0x9C;
                if (be.op == TokenKind::GtEq)  setcc = 0x9D;
                if (be.op == TokenKind::LtEq)  setcc = 0x9E;
                if (be.op == TokenKind::Gt)    setcc = 0x9F;
                emit_.emit_raw({0x0F, setcc, 0xC0}); // SETcc AL
                // MOVZX RAX, AL — zero-extend without clobbering flags
                emit_.emit_raw({0x48, 0x0F, 0xB6, 0xC0});
                break;
            }
            default: break;
        }
        if (dest != Reg::RAX) emit_.mov_reg_reg(dest, Reg::RAX);
    }

    // ── cout << chain ───────────────────────────────────────────
    void emit_cout_chain(const BinaryExpr& be) {
        if (be.left && be.left->kind == NodeKind::BinaryExpr &&
            be.left->as<BinaryExpr>()->op == TokenKind::LShift)
            emit_cout_chain(*be.left->as<BinaryExpr>());
        if (be.right) emit_cout_operand(*be.right);
    }

    void emit_cout_operand(const Node& node) {
        if (node.kind == NodeKind::StringLiteralExpr) {
            auto& s = *node.as<StringLiteralExpr>();
            emit_load_string_addr(Reg::RSI, s.cooked);
            emit_.mov_reg_imm32(Reg::RDX, static_cast<int32_t>(s.cooked.size()));
            emit_.call_label(write_str_label_);
        } else if (node.kind == NodeKind::FloatLiteralExpr) {
            emit_expr_double(node);  // XMM0 = value
            emit_.call_label(write_double_label_);
        } else if (node.kind == NodeKind::IdentifierExpr) {
            auto& id = *node.as<IdentifierExpr>();
            if (id.name == "endl") {
                emit_.call_label(write_newline_label_);
            } else {
                auto it = locals_.find(id.name);
                if (it != locals_.end()) {
                    if (it->second.type_hint == "string") {
                        // String variable: load address from stack, use stored length
                        emit_.mov_reg_mem(Reg::RSI, Reg::RBP, it->second.rbp_offset);
                        emit_.mov_reg_imm32(Reg::RDX,
                            static_cast<int32_t>(it->second.string_data.size()));
                        emit_.call_label(write_str_label_);
                    } else if (it->second.type_hint == "double") {
                        emit_.movsd_xmm0_mem(Reg::RBP, it->second.rbp_offset);
                        emit_.call_label(write_double_label_);
                    } else {
                        emit_.mov_reg_mem(Reg::RDI, Reg::RBP, it->second.rbp_offset);
                        emit_.call_label(write_int_label_);
                    }
                }
            }
        } else if (node.kind == NodeKind::IntLiteralExpr) {
            emit_.mov_reg_imm64(Reg::RDI, node.as<IntLiteralExpr>()->value);
            emit_.call_label(write_int_label_);
        } else if (node.kind == NodeKind::ScopeExpr) {
            auto& se = *node.as<ScopeExpr>();
            if (se.name == "endl")
                emit_.call_label(write_newline_label_);
        } else if (node.kind == NodeKind::CallExpr) {
            emit_call_expr(*node.as<CallExpr>(), Reg::RAX);
            emit_.mov_reg_reg(Reg::RDI, Reg::RAX);
            emit_.call_label(write_int_label_);
        } else if (node.kind == NodeKind::BinaryExpr) {
            if (is_double_expr(node)) {
                emit_expr_double(node);
                emit_.call_label(write_double_label_);
            } else {
                emit_expr(node, Reg::RAX);
                emit_.mov_reg_reg(Reg::RDI, Reg::RAX);
                emit_.call_label(write_int_label_);
            }
        } else if (node.kind == NodeKind::CharLiteralExpr) {
            emit_.mov_reg_imm32(Reg::RAX, node.as<CharLiteralExpr>()->char_val);
            emit_.mov_mem_reg(Reg::RBP, -56, Reg::RAX);
            emit_.mov_reg_reg(Reg::RSI, Reg::RBP);
            emit_.sub_reg_imm32(Reg::RSI, 56);
            emit_.mov_reg_imm32(Reg::RDX, 1);
            emit_.call_label(write_str_label_);
        }
    }

    // ── Function call emission ──────────────────────────────────
    void emit_call_expr(const CallExpr& ce, Reg dest) {
        std::string fname;
        if (ce.callee) {
            if (ce.callee->kind == NodeKind::IdentifierExpr)
                fname = ce.callee->as<IdentifierExpr>()->name;
            else if (ce.callee->kind == NodeKind::ScopeExpr) {
                auto& se = *ce.callee->as<ScopeExpr>();
                if (!se.scope.empty())
                    fname = se.scope.back() + "_" + se.name;
                else
                    fname = se.name;
            }
        }

#ifdef _WIN32
        // Windows x64 ABI: RCX, RDX, R8, R9
        static const Reg param_regs[] = {Reg::RCX, Reg::RDX, Reg::R8, Reg::R9};
        size_t nargs = std::min(ce.args.size(), (size_t)4);
#else
        // System V AMD64 ABI: RDI, RSI, RDX, RCX, R8, R9
        static const Reg param_regs[] = {Reg::RDI, Reg::RSI, Reg::RDX,
                                          Reg::RCX, Reg::R8,  Reg::R9};
        size_t nargs = std::min(ce.args.size(), (size_t)6);
#endif

        // Evaluate each argument; push completed ones to preserve them
        // while subsequent arguments are being evaluated
        for (size_t i = 0; i < nargs; i++) {
            if (ce.args[i])
                emit_expr(*ce.args[i], param_regs[i]);
            if (i + 1 < nargs)
                emit_.push(param_regs[i]);
        }
        // Restore registers in reverse order (last pushed = first popped)
        for (int i = static_cast<int>(nargs) - 2; i >= 0; i--)
            emit_.pop(param_regs[i]);

        auto it = functions_.find(fname);
        if (it != functions_.end())
            emit_.call_label(it->second.label_id);

        if (dest != Reg::RAX) emit_.mov_reg_reg(dest, Reg::RAX);
    }

    // ── Variable declaration ────────────────────────────────────
    void emit_var_decl_node(const VarDecl& vd) {
        local_offset_ -= 8;

        // Determine type from the AST type node
        std::string type_hint = "int";
        if (is_string_type(vd.type.get())) type_hint = "string";
        else if (is_double_type(vd.type.get())) type_hint = "double";

        if (type_hint == "string") {
            std::string str_data;
            if (vd.init && vd.init->kind == NodeKind::StringLiteralExpr) {
                str_data = vd.init->as<StringLiteralExpr>()->cooked;
            } else if (vd.init) {
                // Try compile-time string concatenation (e.g. a + b)
                auto [ok, val] = try_resolve_string(*vd.init);
                if (ok) str_data = val;
            }
            locals_[vd.name] = {local_offset_, "string", str_data};
            if (!str_data.empty()) {
                // Ensure the string is in the pool (append if new)
                ensure_string_in_pool(str_data);
                // Store string address (pointer to rodata) on the stack
                emit_load_string_addr(Reg::RAX, str_data);
                emit_.mov_mem_reg(Reg::RBP, local_offset_, Reg::RAX);
            } else {
                emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
                emit_.mov_mem_reg(Reg::RBP, local_offset_, Reg::RAX);
            }
        } else if (type_hint == "double") {
            locals_[vd.name] = {local_offset_, "double"};
            if (vd.init) {
                emit_expr_double(*vd.init);  // Result in XMM0
                emit_.movsd_mem_xmm0(Reg::RBP, local_offset_);
            } else {
                emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
                emit_.mov_mem_reg(Reg::RBP, local_offset_, Reg::RAX);
            }
        } else {
            locals_[vd.name] = {local_offset_, "int"};
            if (vd.init) {
                emit_expr(*vd.init, Reg::RAX);
                emit_.mov_mem_reg(Reg::RBP, local_offset_, Reg::RAX);
            } else {
                emit_.xor_reg_reg(Reg::RAX, Reg::RAX);
                emit_.mov_mem_reg(Reg::RBP, local_offset_, Reg::RAX);
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //  String address loading
    // ═══════════════════════════════════════════════════════════════

    void emit_load_string_addr(Reg dest, const std::string& cooked_str) {
        for (auto& sc : strings_) {
            if (sc.data == cooked_str) {
                // Use RIP-relative addressing for string literals.
                // This avoids absolute VA patching issues on Windows PE.
                emit_.lea_rip_label(dest, sc.label_id);
                return;
            }
        }
        emit_.xor_reg_reg(dest, dest);
    }

    // ═══════════════════════════════════════════════════════════════
    //  Rodata patching
    // ═══════════════════════════════════════════════════════════════

    void patch_rodata_refs(uint64_t code_vaddr) {
        uint64_t rodata_base = code_vaddr + emit_.code().size();
        auto& code = emit_.code();
        for (auto& ref : rodata_refs_) {
            // Sanity check: ensure offset is within bounds
            if (ref.instruction_offset + 8 > code.size()) {
                std::cerr << "[REXC ERROR] rodata ref offset out of bounds: "
                          << ref.instruction_offset << " + 8 > " << code.size()
                          << " (rodata_base=" << std::hex << rodata_base << ")\n";
                continue;
            }
            uint64_t addr = rodata_base + ref.rodata_offset;
            for (int i = 0; i < 8; i++)
                code[ref.instruction_offset + i] = (addr >> (i * 8)) & 0xFF;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //  Helpers
    // ═══════════════════════════════════════════════════════════════

    int count_locals(const Node* node) {
        if (!node) return 0;
        int count = 0;
        if (node->kind == NodeKind::VarDecl) count++;
        if (node->kind == NodeKind::DeclStmt) {
            for (auto& d : node->as<DeclStmt>()->decls)
                if (d && d->kind == NodeKind::VarDecl) count++;
        }
        visit_children(node, [&](const Node* child) {
            count += count_locals(child);
        });
        return count;
    }

    template<typename Func>
    static void visit_children(const Node* node, Func fn) {
        if (!node) return;
        switch (node->kind) {
            case NodeKind::CompoundStmt:
                for (auto& s : node->as<CompoundStmt>()->stmts) if (s) fn(s.get());
                break;
            case NodeKind::ExprStmt:
                if (node->as<ExprStmt>()->expr) fn(node->as<ExprStmt>()->expr.get());
                break;
            case NodeKind::ReturnStmt:
                if (node->as<ReturnStmt>()->value) fn(node->as<ReturnStmt>()->value.get());
                break;
            case NodeKind::DeclStmt:
                for (auto& d : node->as<DeclStmt>()->decls) if (d) fn(d.get());
                break;
            case NodeKind::VarDecl:
                if (node->as<VarDecl>()->init) fn(node->as<VarDecl>()->init.get());
                break;
            case NodeKind::FunctionDecl: {
                auto* fd = node->as<FunctionDecl>();
                if (fd->body) fn(fd->body.get());
                for (auto& p : fd->params) if (p) fn(p.get());
                break;
            }
            case NodeKind::NamespaceDecl:
                for (auto& d : node->as<NamespaceDecl>()->decls) if (d) fn(d.get());
                break;
            case NodeKind::BinaryExpr: {
                auto* be = node->as<BinaryExpr>();
                if (be->left) fn(be->left.get());
                if (be->right) fn(be->right.get());
                break;
            }
            case NodeKind::UnaryExpr:
                if (node->as<UnaryExpr>()->operand) fn(node->as<UnaryExpr>()->operand.get());
                break;
            case NodeKind::CallExpr: {
                auto* ce = node->as<CallExpr>();
                if (ce->callee) fn(ce->callee.get());
                for (auto& a : ce->args) if (a) fn(a.get());
                break;
            }
            case NodeKind::IfStmt: {
                auto* is = node->as<IfStmt>();
                if (is->condition) fn(is->condition.get());
                if (is->then_branch) fn(is->then_branch.get());
                if (is->else_branch) fn(is->else_branch.get());
                break;
            }
            case NodeKind::WhileStmt: {
                auto* ws = node->as<WhileStmt>();
                if (ws->condition) fn(ws->condition.get());
                if (ws->body) fn(ws->body.get());
                break;
            }
            case NodeKind::ForStmt: {
                auto* fs = node->as<ForStmt>();
                if (fs->init) fn(fs->init.get());
                if (fs->condition) fn(fs->condition.get());
                if (fs->increment) fn(fs->increment.get());
                if (fs->body) fn(fs->body.get());
                break;
            }
            case NodeKind::AssignExpr: {
                auto* ae = node->as<AssignExpr>();
                if (ae->target) fn(ae->target.get());
                if (ae->value) fn(ae->value.get());
                break;
            }
            case NodeKind::ScopeExpr:
                break;
            case NodeKind::ParenExpr:
                if (node->as<ParenExpr>()->inner) fn(node->as<ParenExpr>()->inner.get());
                break;
            case NodeKind::MemberExpr:
                if (node->as<MemberExpr>()->object) fn(node->as<MemberExpr>()->object.get());
                break;
            case NodeKind::TernaryExpr: {
                auto* te = node->as<TernaryExpr>();
                if (te->condition) fn(te->condition.get());
                if (te->then_expr) fn(te->then_expr.get());
                if (te->else_expr) fn(te->else_expr.get());
                break;
            }
            default: break;
        }
    }
};

} // namespace rexc
