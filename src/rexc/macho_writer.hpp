#pragma once
/*
 * macho_writer.hpp  –  Minimal Mach-O Executable Writer
 *
 * Produces static x86_64 and ARM64 macOS Mach-O executables without
 * any external linker or assembler.  Used by the rexc native backend
 * to make REX completely independent from Clang / ld on macOS.
 *
 * Limitations (intentional for simplicity):
 *   - Static executables only (no dylib linking)
 *   - x86_64 and ARM64 macOS only
 *   - Two-segment layout: __TEXT (code+rodata) and __DATA (writable)
 *   - No debug info / DWARF / dSYM
 *   - No code signing (macOS 11+ may need ad-hoc signing)
 */

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  Mach-O structure definitions (self-contained, no mach-o/loader.h)
// ─────────────────────────────────────────────────────────────────

// Mach-O magic numbers
static constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF;

// CPU types
static constexpr uint32_t CPU_TYPE_X86_64    = 0x01000007;  // CPU_TYPE_X86 | CPU_ARCH_ABI64
static constexpr uint32_t CPU_TYPE_ARM64     = 0x0100000C;  // CPU_TYPE_ARM | CPU_ARCH_ABI64

// CPU subtypes
static constexpr uint32_t CPU_SUBTYPE_X86_ALL   = 3;
static constexpr uint32_t CPU_SUBTYPE_ARM64_ALL = 0;

// File types
static constexpr uint32_t MH_EXECUTE = 2;

// Flags
static constexpr uint32_t MH_NOUNDEFS  = 0x00000001;
static constexpr uint32_t MH_PIE       = 0x00200000;

// Load command types
static constexpr uint32_t LC_SEGMENT_64     = 0x19;
static constexpr uint32_t LC_MAIN           = 0x80000028;
static constexpr uint32_t LC_UNIXTHREAD     = 0x05;

// VM protection
static constexpr uint32_t VM_PROT_READ    = 0x01;
static constexpr uint32_t VM_PROT_WRITE   = 0x02;
static constexpr uint32_t VM_PROT_EXECUTE = 0x04;

// Mach-O 64-bit header (32 bytes)
struct MachO_Header {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

// LC_SEGMENT_64 command (72 bytes)
struct MachO_Segment64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

// Section 64 (80 bytes)
struct MachO_Section64 {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};

// LC_UNIXTHREAD command for x86_64 (contains thread state)
// x86_64 thread state has flavor=4 (x86_THREAD_STATE64), count=42
struct MachO_UnixThread_x86_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t flavor;
    uint32_t count;
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rdi, rsi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, fs, gs;
};

// LC_UNIXTHREAD command for ARM64 (contains thread state)
// ARM64 thread state has flavor=6 (ARM_THREAD_STATE64), count=68
struct MachO_UnixThread_ARM64 {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t flavor;
    uint32_t count;
    uint64_t x[29];   // x0-x28
    uint64_t fp;       // x29 (frame pointer)
    uint64_t lr;       // x30 (link register)
    uint64_t sp;       // stack pointer
    uint64_t pc;       // program counter
    uint32_t cpsr;     // flags
    uint32_t pad;      // alignment padding
};

// Section flags
static constexpr uint32_t S_REGULAR            = 0x00000000;
static constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000;
static constexpr uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400;

// ─────────────────────────────────────────────────────────────────
//  MachOWriter  –  produces a static Mach-O 64-bit executable
// ─────────────────────────────────────────────────────────────────
class MachOWriter {
public:
    static constexpr uint64_t TEXT_BASE     = 0x100000000ULL;  // Standard macOS base
    static constexpr uint64_t PAGE_SIZE     = 0x4000;          // 16KB pages (ARM64 / modern macOS)

    enum class Arch { X86_64, ARM64 };

    // Write a Mach-O 64-bit executable to disk.
    //   arch   – target architecture (X86_64 or ARM64)
    //   code   – machine code (__text)
    //   rodata – read-only data (appended to __TEXT segment)
    //   data   – writable initialised data (__data)
    //   bss_sz – uninitialised writable data (__bss)
    //   entry_offset – offset into code[] where execution starts
    //
    // Returns true on success.
    static bool write(const std::string& path,
                      Arch arch,
                      const std::vector<uint8_t>& code,
                      const std::vector<uint8_t>& rodata,
                      const std::vector<uint8_t>& data = {},
                      uint64_t bss_sz = 0,
                      uint64_t entry_offset = 0) {

        bool has_data = !data.empty() || bss_sz > 0;
        bool is_arm64 = (arch == Arch::ARM64);

        // ── Calculate load command sizes ────────────────────────
        uint32_t seg_text_size = sizeof(MachO_Segment64) + sizeof(MachO_Section64);
        uint32_t seg_data_size = has_data ? sizeof(MachO_Segment64) + sizeof(MachO_Section64) : 0;

        uint32_t thread_cmd_size;
        if (is_arm64) {
            thread_cmd_size = sizeof(MachO_UnixThread_ARM64);
        } else {
            thread_cmd_size = sizeof(MachO_UnixThread_x86_64);
        }

        uint32_t ncmds = has_data ? 3 : 2;  // __TEXT + thread + optional __DATA
        uint32_t sizeofcmds = seg_text_size + seg_data_size + thread_cmd_size;

        // ── Calculate layout ────────────────────────────────────
        uint32_t header_size = sizeof(MachO_Header);
        uint64_t text_file_start = align_up64(header_size + sizeofcmds, 16);

        uint64_t text_raw_size = code.size() + rodata.size();
        uint64_t text_seg_filesize = align_up64(text_file_start + text_raw_size, PAGE_SIZE);
        uint64_t text_seg_vmsize = text_seg_filesize;

        uint64_t text_section_addr = TEXT_BASE + text_file_start;
        uint64_t entry_addr = text_section_addr + entry_offset;

        uint64_t data_seg_offset = 0;
        uint64_t data_seg_vmaddr = 0;
        uint64_t data_seg_filesize = 0;
        uint64_t data_seg_vmsize = 0;
        if (has_data) {
            data_seg_offset = text_seg_filesize;
            data_seg_vmaddr = TEXT_BASE + data_seg_offset;
            data_seg_filesize = align_up64(data.size(), PAGE_SIZE);
            data_seg_vmsize = align_up64(data.size() + bss_sz, PAGE_SIZE);
        }

        // ── Build Mach-O header ─────────────────────────────────
        MachO_Header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = MH_MAGIC_64;
        hdr.cputype = is_arm64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
        hdr.cpusubtype = is_arm64 ? CPU_SUBTYPE_ARM64_ALL : CPU_SUBTYPE_X86_ALL;
        hdr.filetype = MH_EXECUTE;
        hdr.ncmds = ncmds;
        hdr.sizeofcmds = sizeofcmds;
        hdr.flags = MH_NOUNDEFS;
        hdr.reserved = 0;

        // ── Build __TEXT segment ─────────────────────────────────
        MachO_Segment64 text_seg;
        memset(&text_seg, 0, sizeof(text_seg));
        text_seg.cmd = LC_SEGMENT_64;
        text_seg.cmdsize = seg_text_size;
        memcpy(text_seg.segname, "__TEXT\0\0\0\0\0\0\0\0\0\0", 16);
        text_seg.vmaddr = TEXT_BASE;
        text_seg.vmsize = text_seg_vmsize;
        text_seg.fileoff = 0;
        text_seg.filesize = text_seg_filesize;
        text_seg.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
        text_seg.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
        text_seg.nsects = 1;
        text_seg.flags = 0;

        MachO_Section64 text_sect;
        memset(&text_sect, 0, sizeof(text_sect));
        memcpy(text_sect.sectname, "__text\0\0\0\0\0\0\0\0\0\0", 16);
        memcpy(text_sect.segname, "__TEXT\0\0\0\0\0\0\0\0\0\0", 16);
        text_sect.addr = text_section_addr;
        text_sect.size = text_raw_size;
        text_sect.offset = static_cast<uint32_t>(text_file_start);
        text_sect.align = 4;  // 2^4 = 16-byte aligned
        text_sect.flags = S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;

        // ── Build __DATA segment (optional) ─────────────────────
        MachO_Segment64 data_seg;
        MachO_Section64 data_sect;
        if (has_data) {
            memset(&data_seg, 0, sizeof(data_seg));
            data_seg.cmd = LC_SEGMENT_64;
            data_seg.cmdsize = seg_data_size;
            memcpy(data_seg.segname, "__DATA\0\0\0\0\0\0\0\0\0\0", 16);
            data_seg.vmaddr = data_seg_vmaddr;
            data_seg.vmsize = data_seg_vmsize;
            data_seg.fileoff = data_seg_offset;
            data_seg.filesize = data_seg_filesize;
            data_seg.maxprot = VM_PROT_READ | VM_PROT_WRITE;
            data_seg.initprot = VM_PROT_READ | VM_PROT_WRITE;
            data_seg.nsects = 1;

            memset(&data_sect, 0, sizeof(data_sect));
            memcpy(data_sect.sectname, "__data\0\0\0\0\0\0\0\0\0\0", 16);
            memcpy(data_sect.segname, "__DATA\0\0\0\0\0\0\0\0\0\0", 16);
            data_sect.addr = data_seg_vmaddr;
            data_sect.size = data.size() + bss_sz;
            data_sect.offset = static_cast<uint32_t>(data_seg_offset);
            data_sect.align = 3;  // 2^3 = 8-byte aligned
        }

        // ── Write to file ───────────────────────────────────────
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        // Mach-O header
        f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // __TEXT segment command + section
        f.write(reinterpret_cast<const char*>(&text_seg), sizeof(text_seg));
        f.write(reinterpret_cast<const char*>(&text_sect), sizeof(text_sect));

        // __DATA segment command + section (optional)
        if (has_data) {
            f.write(reinterpret_cast<const char*>(&data_seg), sizeof(data_seg));
            f.write(reinterpret_cast<const char*>(&data_sect), sizeof(data_sect));
        }

        // LC_UNIXTHREAD command
        if (is_arm64) {
            MachO_UnixThread_ARM64 thread;
            memset(&thread, 0, sizeof(thread));
            thread.cmd = LC_UNIXTHREAD;
            thread.cmdsize = sizeof(thread);
            thread.flavor = 6;   // ARM_THREAD_STATE64
            thread.count = 68;   // sizeof(arm_thread_state64_t) / sizeof(uint32_t)
            thread.pc = entry_addr;
            f.write(reinterpret_cast<const char*>(&thread), sizeof(thread));
        } else {
            MachO_UnixThread_x86_64 thread;
            memset(&thread, 0, sizeof(thread));
            thread.cmd = LC_UNIXTHREAD;
            thread.cmdsize = sizeof(thread);
            thread.flavor = 4;   // x86_THREAD_STATE64
            thread.count = 42;   // sizeof(x86_thread_state64_t) / sizeof(uint32_t)
            thread.rip = entry_addr;
            f.write(reinterpret_cast<const char*>(&thread), sizeof(thread));
        }

        // Pad to code offset
        pad_to(f, text_file_start);

        // Code
        if (!code.empty())
            f.write(reinterpret_cast<const char*>(code.data()), code.size());
        // Rodata (immediately follows code)
        if (!rodata.empty())
            f.write(reinterpret_cast<const char*>(rodata.data()), rodata.size());

        // Pad to end of __TEXT segment
        pad_to(f, text_seg_filesize);

        // __DATA segment
        if (has_data) {
            if (!data.empty())
                f.write(reinterpret_cast<const char*>(data.data()), data.size());
            pad_to(f, data_seg_offset + data_seg_filesize);
        }

        f.close();

        // Make executable
#ifndef _WIN32
        std::string chmod_cmd = "chmod +x \"" + path + "\"";
        std::system(chmod_cmd.c_str());
#endif
        return true;
    }

    // Return the virtual address of the rodata area, given code size.
    static uint64_t rodata_vaddr(uint64_t code_size, Arch arch = Arch::X86_64) {
        return code_vaddr(arch) + code_size;
    }

    // Return the virtual address of the code section start.
    static uint64_t code_vaddr(Arch /*arch*/ = Arch::X86_64) {
        // Calculate the same way as write()
        // Minimal: header + 1 segment command + 1 section + thread command
        uint32_t header_size = sizeof(MachO_Header);
        uint32_t seg_text_size = sizeof(MachO_Segment64) + sizeof(MachO_Section64);
        uint32_t thread_size = sizeof(MachO_UnixThread_x86_64); // same rough size
        uint64_t text_file_start = align_up64(header_size + seg_text_size + thread_size, 16);
        return TEXT_BASE + text_file_start;
    }

private:
    static uint64_t align_up64(uint64_t val, uint64_t align) {
        return (val + align - 1) & ~(align - 1);
    }

    static void pad_to(std::ofstream& f, uint64_t offset) {
        auto pos = static_cast<uint64_t>(f.tellp());
        if (pos < offset) {
            std::vector<uint8_t> zeros(static_cast<size_t>(offset - pos), 0);
            f.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
        }
    }
};

} // namespace rexc
