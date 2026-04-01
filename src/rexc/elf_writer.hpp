#pragma once
/*
 * elf_writer.hpp  –  Minimal ELF64 Executable Writer
 *
 * Produces static x86_64 and ARM64 Linux ELF executables without any
 * external linker or assembler.  Used by the rexc native backend to
 * make REX completely independent from external toolchains on Linux.
 *
 * Limitations (intentional for simplicity):
 *   - Static executables only (no dynamic linking)
 *   - Linux only (Windows uses PE writer, macOS uses Mach-O writer)
 *   - Two-segment layout: code+rodata (RE) and data+bss (RW)
 *   - No debug info / DWARF
 */

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  ELF64 structure definitions (self-contained, no elf.h needed)
// ─────────────────────────────────────────────────────────────────

// ELF identification
static constexpr uint8_t ELFMAG0 = 0x7f;
static constexpr uint8_t ELFMAG1 = 'E';
static constexpr uint8_t ELFMAG2 = 'L';
static constexpr uint8_t ELFMAG3 = 'F';

// ELF class / data / OS
static constexpr uint8_t ELFCLASS64    = 2;
static constexpr uint8_t ELFDATA2LSB   = 1;  // little-endian
static constexpr uint8_t ELFOSABI_NONE = 0;

// Object file types
static constexpr uint16_t ET_EXEC = 2;

// Machine types
static constexpr uint16_t EM_X86_64  = 62;
static constexpr uint16_t EM_AARCH64 = 183;

// Program header types
static constexpr uint32_t PT_LOAD = 1;

// Program header flags
static constexpr uint32_t PF_X = 1;  // execute
static constexpr uint32_t PF_W = 2;  // write
static constexpr uint32_t PF_R = 4;  // read

// ELF64 Header (64 bytes)
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

// ELF64 Program Header (56 bytes)
struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ─────────────────────────────────────────────────────────────────
//  ElfWriter  –  produces a static ELF64 executable
// ─────────────────────────────────────────────────────────────────
class ElfWriter {
public:
    // Base virtual address for code segment (standard Linux convention)
    static constexpr uint64_t CODE_BASE  = 0x400000;
    static constexpr uint64_t PAGE_SIZE  = 0x1000;

    // Write an ELF64 executable to disk.
    //   code   – machine code (.text)
    //   rodata – read-only data (.rodata, e.g. string literals)
    //   data   – writable initialised data (.data)
    //   bss_sz – uninitialised writable data (.bss)
    //   entry_offset – offset into code[] where execution starts
    //
    // Memory layout:
    //   [ELF header + phdrs] [code] [rodata]   [data] [bss]
    //   |<--- segment 1 (RE) --->|  |<-- segment 2 (RW) -->|
    //
    // Returns true on success.
    static bool write(const std::string& path,
                      const std::vector<uint8_t>& code,
                      const std::vector<uint8_t>& rodata,
                      const std::vector<uint8_t>& data = {},
                      uint64_t bss_sz = 0,
                      uint64_t entry_offset = 0) {

        // ── Calculate layout ────────────────────────────────────
        const uint64_t ehdr_size = sizeof(Elf64_Ehdr);
        const uint64_t phdr_size = sizeof(Elf64_Phdr);

        // We need at least 1 program header (code+rodata) and
        // optionally a second (data+bss) if there's writable data.
        bool has_rw = !data.empty() || bss_sz > 0;
        uint16_t phnum = has_rw ? 2 : 1;

        uint64_t headers_size = ehdr_size + phnum * phdr_size;
        // Align code start to 16 bytes after headers
        uint64_t code_file_offset = align_up(headers_size, 16);

        // Segment 1: code + rodata (RE)
        uint64_t seg1_file_offset = code_file_offset;
        uint64_t seg1_size = code.size() + rodata.size();
        uint64_t seg1_vaddr = CODE_BASE + code_file_offset;

        // Entry point
        uint64_t entry_vaddr = seg1_vaddr + entry_offset;

        // Segment 2: data + bss (RW) — starts on next page boundary
        uint64_t seg2_file_offset = 0;
        uint64_t seg2_vaddr = 0;
        uint64_t seg2_filesz = 0;
        uint64_t seg2_memsz = 0;
        if (has_rw) {
            seg2_file_offset = align_up(code_file_offset + seg1_size, PAGE_SIZE);
            seg2_vaddr = CODE_BASE + seg2_file_offset;
            seg2_filesz = data.size();
            seg2_memsz  = data.size() + bss_sz;
        }

        // ── Build ELF header ────────────────────────────────────
        Elf64_Ehdr ehdr;
        memset(&ehdr, 0, sizeof(ehdr));
        ehdr.e_ident[0] = ELFMAG0;
        ehdr.e_ident[1] = ELFMAG1;
        ehdr.e_ident[2] = ELFMAG2;
        ehdr.e_ident[3] = ELFMAG3;
        ehdr.e_ident[4] = ELFCLASS64;
        ehdr.e_ident[5] = ELFDATA2LSB;
        ehdr.e_ident[6] = 1;  // EV_CURRENT
        ehdr.e_ident[7] = ELFOSABI_NONE;
        ehdr.e_type      = ET_EXEC;
        ehdr.e_machine   = EM_X86_64;
        ehdr.e_version   = 1;
        ehdr.e_entry     = entry_vaddr;
        ehdr.e_phoff     = ehdr_size;
        ehdr.e_shoff     = 0;  // no section headers
        ehdr.e_flags     = 0;
        ehdr.e_ehsize    = ehdr_size;
        ehdr.e_phentsize = phdr_size;
        ehdr.e_phnum     = phnum;
        ehdr.e_shentsize = 0;
        ehdr.e_shnum     = 0;
        ehdr.e_shstrndx  = 0;

        // ── Build program headers ───────────────────────────────
        Elf64_Phdr phdr_code;
        memset(&phdr_code, 0, sizeof(phdr_code));
        phdr_code.p_type   = PT_LOAD;
        phdr_code.p_flags  = PF_R | PF_X;
        phdr_code.p_offset = seg1_file_offset;
        phdr_code.p_vaddr  = seg1_vaddr;
        phdr_code.p_paddr  = seg1_vaddr;
        phdr_code.p_filesz = seg1_size;
        phdr_code.p_memsz  = seg1_size;
        phdr_code.p_align  = PAGE_SIZE;

        Elf64_Phdr phdr_data;
        if (has_rw) {
            memset(&phdr_data, 0, sizeof(phdr_data));
            phdr_data.p_type   = PT_LOAD;
            phdr_data.p_flags  = PF_R | PF_W;
            phdr_data.p_offset = seg2_file_offset;
            phdr_data.p_vaddr  = seg2_vaddr;
            phdr_data.p_paddr  = seg2_vaddr;
            phdr_data.p_filesz = seg2_filesz;
            phdr_data.p_memsz  = seg2_memsz;
            phdr_data.p_align  = PAGE_SIZE;
        }

        // ── Write to file ───────────────────────────────────────
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        // ELF header
        f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
        // Program headers
        f.write(reinterpret_cast<const char*>(&phdr_code), sizeof(phdr_code));
        if (has_rw) {
            f.write(reinterpret_cast<const char*>(&phdr_data), sizeof(phdr_data));
        }

        // Pad to code offset
        pad_to(f, code_file_offset);

        // Code
        if (!code.empty())
            f.write(reinterpret_cast<const char*>(code.data()), code.size());
        // Rodata (immediately follows code)
        if (!rodata.empty())
            f.write(reinterpret_cast<const char*>(rodata.data()), rodata.size());

        // Data segment
        if (has_rw) {
            pad_to(f, seg2_file_offset);
            if (!data.empty())
                f.write(reinterpret_cast<const char*>(data.data()), data.size());
        }

        f.close();

        // Make executable (chmod +x)
#ifndef _WIN32
        std::string chmod_cmd = "chmod +x \"" + path + "\"";
        std::system(chmod_cmd.c_str());
#endif
        return true;
    }

    // Return the virtual address of the rodata section, given code size.
    // Useful for computing string addresses in generated code.
    static uint64_t rodata_vaddr(uint64_t code_size, bool has_rw = false) {
        return code_vaddr(has_rw) + code_size;
    }

    // Return the virtual address of the code section start.
    static uint64_t code_vaddr(bool has_rw = false) {
        uint64_t ehdr_size = sizeof(Elf64_Ehdr);
        uint64_t phdr_size = sizeof(Elf64_Phdr);
        uint16_t phnum = has_rw ? 2 : 1;
        uint64_t headers = ehdr_size + phnum * phdr_size;
        uint64_t code_offset = align_up(headers, 16);
        return CODE_BASE + code_offset;
    }

private:
    static uint64_t align_up(uint64_t val, uint64_t align) {
        return (val + align - 1) & ~(align - 1);
    }

    static void pad_to(std::ofstream& f, uint64_t offset) {
        auto pos = static_cast<uint64_t>(f.tellp());
        if (pos < offset) {
            std::vector<uint8_t> zeros(offset - pos, 0);
            f.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
        }
    }
};

} // namespace rexc
