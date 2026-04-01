#pragma once
/*
 * pe_writer.hpp  –  Minimal PE32+ Executable Writer
 *
 * Produces x86_64 Windows PE executables without any external
 * linker or assembler.  Used by the rexc native backend to make REX
 * completely independent from MSVC / MinGW / ld on Windows.
 *
 * Supports:
 *   - x86_64 Windows only
 *   - Two-section layout: .text (code+rodata) and .data (writable)
 *   - Import table for kernel32.dll (GetStdHandle, WriteFile, ExitProcess)
 *   - Windows subsystem console
 *   - No debug info / PDB
 *
 * The import table (.idata) is built as a third section so the
 * generated code can call kernel32 functions through the IAT.
 */

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

namespace rexc {

// ─────────────────────────────────────────────────────────────────
//  PE32+ structure definitions (self-contained, no windows.h needed)
// ─────────────────────────────────────────────────────────────────

// DOS header (minimal stub)
struct PE_DOS_Header {
    uint16_t e_magic;      // 0x5A4D ('MZ')
    uint8_t  pad[58];      // zeroed
    uint32_t e_lfanew;     // offset to PE signature
};

// PE signature
static constexpr uint32_t PE_SIGNATURE = 0x00004550; // 'PE\0\0'

// COFF file header
struct PE_COFF_Header {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

// PE32+ optional header
struct PE_Optional_Header {
    uint16_t Magic;                 // 0x020B = PE32+
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    // Data directories follow (16 entries, 8 bytes each = 128 bytes)
    uint8_t  DataDirectories[128];
};

// Section header
struct PE_Section_Header {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};

// Machine types
static constexpr uint16_t PE_MACHINE_AMD64  = 0x8664;
static constexpr uint16_t PE_MACHINE_ARM64  = 0xAA64;

// Characteristics
static constexpr uint16_t PE_CHAR_EXECUTABLE     = 0x0002;
static constexpr uint16_t PE_CHAR_LARGE_ADDR     = 0x0020;
static constexpr uint16_t PE_CHAR_NO_RELOC       = 0x0001;

// Section characteristics
static constexpr uint32_t PE_SCN_MEM_EXECUTE     = 0x20000000;
static constexpr uint32_t PE_SCN_MEM_READ        = 0x40000000;
static constexpr uint32_t PE_SCN_MEM_WRITE       = 0x80000000;
static constexpr uint32_t PE_SCN_CNT_CODE        = 0x00000020;
static constexpr uint32_t PE_SCN_CNT_INIT_DATA   = 0x00000040;

// Subsystem
static constexpr uint16_t PE_SUBSYS_CONSOLE      = 3;

// ─────────────────────────────────────────────────────────────────
//  Import table info returned to the code generator
// ─────────────────────────────────────────────────────────────────
struct PeImportInfo {
    // RVAs of IAT entries (each is an 8-byte pointer slot).
    // Code uses  CALL [RIP + (iat_rva - (rip))]  to call these.
    uint32_t iat_GetStdHandle  = 0;
    uint32_t iat_WriteFile     = 0;
    uint32_t iat_ExitProcess   = 0;
};

// ─────────────────────────────────────────────────────────────────
//  PeWriter  –  produces a PE32+ executable with import table
// ─────────────────────────────────────────────────────────────────
class PeWriter {
public:
    static constexpr uint64_t IMAGE_BASE      = 0x00400000;
    static constexpr uint32_t SECTION_ALIGN   = 0x1000;
    static constexpr uint32_t FILE_ALIGN      = 0x200;

    // Write a PE32+ executable to disk.
    //   code   – machine code (.text)
    //   rodata – read-only data (appended to .text section)
    //   data   – writable initialised data (.data)
    //   bss_sz – uninitialised writable data (.bss)
    //   entry_offset – offset into code[] where execution starts
    //   imports – if non-null, receives the IAT RVAs so code can
    //             reference them.  Pass nullptr for legacy no-import mode.
    //
    // Returns true on success.
    static bool write(const std::string& path,
                      const std::vector<uint8_t>& code,
                      const std::vector<uint8_t>& rodata,
                      const std::vector<uint8_t>& data = {},
                      uint64_t bss_sz = 0,
                      uint64_t entry_offset = 0,
                      PeImportInfo* imports = nullptr) {

        // ── Build the .idata section content ────────────────────
        // We always include .idata so the binary can call kernel32.
        //
        // Layout inside .idata:
        //   [0]  Import Directory Table  (2 entries: kernel32 + null terminator)
        //   [40] IAT  (Import Address Table) – 4 slots (3 funcs + null)
        //   [72] ILT  (Import Lookup Table) – 4 slots (same as IAT before binding)
        //  [104] DLL name: "kernel32.dll\0"
        //  [117] Hint/Name entries for each function

        // Sizes
        static constexpr uint32_t IDT_ENTRY_SIZE = 20;  // IMAGE_IMPORT_DESCRIPTOR
        static constexpr uint32_t IDT_SIZE       = IDT_ENTRY_SIZE * 2; // 1 entry + null terminator = 40
        static constexpr uint32_t IAT_ENTRIES    = 4;   // 3 funcs + null
        static constexpr uint32_t IAT_SIZE       = IAT_ENTRIES * 8; // 32
        static constexpr uint32_t ILT_SIZE       = IAT_SIZE;         // 32

        // Function names (with 2-byte hint prefix)
        const char* fn_names[] = {"ExitProcess", "GetStdHandle", "WriteFile"};
        static constexpr int NUM_FUNCS = 3;

        // Build hint/name entries
        struct HintName { uint16_t hint; std::string name; };
        HintName hint_names[NUM_FUNCS] = {
            {0, fn_names[0]},
            {0, fn_names[1]},
            {0, fn_names[2]},
        };

        const std::string dll_name = "kernel32.dll";

        // Calculate offsets within .idata (relative to section start)
        uint32_t idt_off  = 0;
        uint32_t iat_off  = IDT_SIZE;                    // 40
        uint32_t ilt_off  = iat_off + IAT_SIZE;           // 72
        uint32_t dll_off  = ilt_off + ILT_SIZE;           // 104
        uint32_t hn_off   = dll_off + (uint32_t)(dll_name.size() + 1); // after dll name

        // Compute hint/name entry offsets
        uint32_t hn_offsets[NUM_FUNCS];
        uint32_t cursor = hn_off;
        for (int i = 0; i < NUM_FUNCS; i++) {
            // Align to 2-byte boundary
            if (cursor & 1) cursor++;
            hn_offsets[i] = cursor;
            cursor += 2 + (uint32_t)hint_names[i].name.size() + 1;
        }
        uint32_t idata_raw_size = cursor;

        // ── Calculate overall layout ────────────────────────────
        bool has_data = !data.empty() || bss_sz > 0;
        // Sections: .text, [.data], .idata
        uint16_t num_sections = (has_data ? 2 : 1) + 1; // +1 for .idata

        uint32_t dos_size = sizeof(PE_DOS_Header);
        uint32_t pe_sig_size = 4;
        uint32_t coff_size = sizeof(PE_COFF_Header);
        uint32_t opt_size = sizeof(PE_Optional_Header);
        uint32_t sec_hdr_size = sizeof(PE_Section_Header) * num_sections;

        uint32_t headers_raw = dos_size + pe_sig_size + coff_size + opt_size + sec_hdr_size;
        uint32_t headers_aligned = align_up(headers_raw, FILE_ALIGN);

        // .text section (code + rodata)
        uint32_t text_raw_size = static_cast<uint32_t>(code.size() + rodata.size());
        uint32_t text_file_size = align_up(text_raw_size, FILE_ALIGN);
        uint32_t text_rva = SECTION_ALIGN;
        uint32_t text_file_offset = headers_aligned;

        // .data section (optional)
        uint32_t data_rva = 0, data_file_offset = 0;
        uint32_t data_raw_size_ = 0, data_file_size = 0, data_virtual_size = 0;
        uint32_t next_rva = align_up(text_rva + text_raw_size, SECTION_ALIGN);
        uint32_t next_file = text_file_offset + text_file_size;
        if (has_data) {
            data_rva = next_rva;
            data_file_offset = next_file;
            data_raw_size_ = static_cast<uint32_t>(data.size());
            data_file_size = align_up(data_raw_size_, FILE_ALIGN);
            data_virtual_size = static_cast<uint32_t>(data.size() + bss_sz);
            next_rva = align_up(data_rva + data_virtual_size, SECTION_ALIGN);
            next_file = data_file_offset + data_file_size;
        }

        // .idata section
        uint32_t idata_rva = next_rva;
        uint32_t idata_file_offset = next_file;
        uint32_t idata_file_size = align_up(idata_raw_size, FILE_ALIGN);

        uint32_t image_size = align_up(idata_rva + idata_raw_size, SECTION_ALIGN);

        // Fill in import info for the code generator
        if (imports) {
            // IAT entries order: ExitProcess, GetStdHandle, WriteFile, NULL
            imports->iat_ExitProcess  = idata_rva + iat_off + 0 * 8;
            imports->iat_GetStdHandle = idata_rva + iat_off + 1 * 8;
            imports->iat_WriteFile    = idata_rva + iat_off + 2 * 8;
        }

        // ── Build .idata bytes ──────────────────────────────────
        std::vector<uint8_t> idata(idata_raw_size, 0);

        // --- Import Directory Table (1 real entry + null) ---
        auto write_u32 = [&](std::vector<uint8_t>& buf, uint32_t off, uint32_t val) {
            buf[off+0] = val & 0xFF;
            buf[off+1] = (val>>8) & 0xFF;
            buf[off+2] = (val>>16) & 0xFF;
            buf[off+3] = (val>>24) & 0xFF;
        };
        // Field 0: OriginalFirstThunk (RVA of ILT)
        write_u32(idata, idt_off + 0,  idata_rva + ilt_off);
        // Field 1: TimeDateStamp = 0
        // Field 2: ForwarderChain = 0
        // Field 3: Name (RVA of DLL name)
        write_u32(idata, idt_off + 12, idata_rva + dll_off);
        // Field 4: FirstThunk (RVA of IAT)
        write_u32(idata, idt_off + 16, idata_rva + iat_off);
        // Second entry is all zeros (null terminator) — already zeroed

        // --- IAT and ILT entries ---
        for (int i = 0; i < NUM_FUNCS; i++) {
            uint64_t hn_rva = idata_rva + hn_offsets[i];
            // IAT[i]
            for (int b = 0; b < 8; b++)
                idata[iat_off + i*8 + b] = (hn_rva >> (b*8)) & 0xFF;
            // ILT[i] — identical before binding
            for (int b = 0; b < 8; b++)
                idata[ilt_off + i*8 + b] = (hn_rva >> (b*8)) & 0xFF;
        }
        // Null terminators (IAT[3] and ILT[3]) already zeroed

        // --- DLL name ---
        for (size_t i = 0; i < dll_name.size(); i++)
            idata[dll_off + i] = (uint8_t)dll_name[i];
        // null terminator already zeroed

        // --- Hint/Name entries ---
        for (int i = 0; i < NUM_FUNCS; i++) {
            uint32_t off = hn_offsets[i];
            idata[off+0] = hint_names[i].hint & 0xFF;
            idata[off+1] = (hint_names[i].hint >> 8) & 0xFF;
            for (size_t j = 0; j < hint_names[i].name.size(); j++)
                idata[off+2+j] = (uint8_t)hint_names[i].name[j];
            // null terminator already zeroed
        }

        // ── Build DOS header ────────────────────────────────────
        PE_DOS_Header dos;
        memset(&dos, 0, sizeof(dos));
        dos.e_magic = 0x5A4D;  // 'MZ'
        dos.e_lfanew = dos_size;

        // ── Build COFF header ───────────────────────────────────
        PE_COFF_Header coff;
        memset(&coff, 0, sizeof(coff));
        coff.Machine = PE_MACHINE_AMD64;
        coff.NumberOfSections = num_sections;
        coff.TimeDateStamp = 0;
        coff.SizeOfOptionalHeader = opt_size;
        coff.Characteristics = PE_CHAR_EXECUTABLE | PE_CHAR_LARGE_ADDR;

        // ── Build optional header ───────────────────────────────
        PE_Optional_Header opt;
        memset(&opt, 0, sizeof(opt));
        opt.Magic = 0x020B;  // PE32+
        opt.MajorLinkerVersion = 1;
        opt.SizeOfCode = text_file_size;
        opt.SizeOfInitializedData = (has_data ? data_file_size : 0) + idata_file_size;
        opt.AddressOfEntryPoint = text_rva + static_cast<uint32_t>(entry_offset);
        opt.BaseOfCode = text_rva;
        opt.ImageBase = IMAGE_BASE;
        opt.SectionAlignment = SECTION_ALIGN;
        opt.FileAlignment = FILE_ALIGN;
        opt.MajorOperatingSystemVersion = 6;
        opt.MinorOperatingSystemVersion = 0;
        opt.MajorSubsystemVersion = 6;
        opt.MinorSubsystemVersion = 0;
        opt.SizeOfImage = image_size;
        opt.SizeOfHeaders = headers_aligned;
        opt.Subsystem = PE_SUBSYS_CONSOLE;
        opt.DllCharacteristics = 0x8160;
        opt.SizeOfStackReserve = 0x100000;
        opt.SizeOfStackCommit  = 0x1000;
        opt.SizeOfHeapReserve  = 0x100000;
        opt.SizeOfHeapCommit   = 0x1000;
        opt.NumberOfRvaAndSizes = 16;

        // Data directory[1] = Import Table
        // Offset of directory[1] in DataDirectories: index 1 * 8 = 8
        write_u32_arr(opt.DataDirectories, 8,  idata_rva + idt_off); // RVA
        write_u32_arr(opt.DataDirectories, 12, IDT_SIZE);             // Size

        // Data directory[12] = IAT
        // Offset: 12 * 8 = 96
        write_u32_arr(opt.DataDirectories, 96,  idata_rva + iat_off); // RVA
        write_u32_arr(opt.DataDirectories, 100, IAT_SIZE);             // Size

        // ── Build section headers ───────────────────────────────
        PE_Section_Header text_sec;
        memset(&text_sec, 0, sizeof(text_sec));
        memcpy(text_sec.Name, ".text\0\0\0", 8);
        text_sec.VirtualSize = text_raw_size;
        text_sec.VirtualAddress = text_rva;
        text_sec.SizeOfRawData = text_file_size;
        text_sec.PointerToRawData = text_file_offset;
        text_sec.Characteristics = PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ | PE_SCN_CNT_CODE;

        PE_Section_Header data_sec;
        memset(&data_sec, 0, sizeof(data_sec));
        if (has_data) {
            memcpy(data_sec.Name, ".data\0\0\0", 8);
            data_sec.VirtualSize = data_virtual_size;
            data_sec.VirtualAddress = data_rva;
            data_sec.SizeOfRawData = data_file_size;
            data_sec.PointerToRawData = data_file_offset;
            data_sec.Characteristics = PE_SCN_MEM_READ | PE_SCN_MEM_WRITE | PE_SCN_CNT_INIT_DATA;
        }

        PE_Section_Header idata_sec;
        memset(&idata_sec, 0, sizeof(idata_sec));
        memcpy(idata_sec.Name, ".idata\0\0", 8);
        idata_sec.VirtualSize = idata_raw_size;
        idata_sec.VirtualAddress = idata_rva;
        idata_sec.SizeOfRawData = idata_file_size;
        idata_sec.PointerToRawData = idata_file_offset;
        idata_sec.Characteristics = PE_SCN_MEM_READ | PE_SCN_MEM_WRITE | PE_SCN_CNT_INIT_DATA;

        // ── Write to file ───────────────────────────────────────
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        f.write(reinterpret_cast<const char*>(&dos), sizeof(dos));

        uint32_t sig = PE_SIGNATURE;
        f.write(reinterpret_cast<const char*>(&sig), 4);
        f.write(reinterpret_cast<const char*>(&coff), sizeof(coff));
        f.write(reinterpret_cast<const char*>(&opt), sizeof(opt));

        f.write(reinterpret_cast<const char*>(&text_sec), sizeof(text_sec));
        if (has_data)
            f.write(reinterpret_cast<const char*>(&data_sec), sizeof(data_sec));
        f.write(reinterpret_cast<const char*>(&idata_sec), sizeof(idata_sec));

        pad_to(f, text_file_offset);

        if (!code.empty())
            f.write(reinterpret_cast<const char*>(code.data()), code.size());
        if (!rodata.empty())
            f.write(reinterpret_cast<const char*>(rodata.data()), rodata.size());
        pad_to(f, text_file_offset + text_file_size);

        if (has_data) {
            if (!data.empty())
                f.write(reinterpret_cast<const char*>(data.data()), data.size());
            pad_to(f, data_file_offset + data_file_size);
        }

        f.write(reinterpret_cast<const char*>(idata.data()), idata.size());
        pad_to(f, idata_file_offset + idata_file_size);

        f.close();
        return true;
    }

    // Return the virtual address of the rodata area, given code size.
    static uint64_t rodata_vaddr(uint64_t code_size, bool /*has_rw*/ = false) {
        return code_vaddr() + code_size;
    }

    // Return the virtual address of the code section start.
    static uint64_t code_vaddr(bool /*has_rw*/ = false) {
        return IMAGE_BASE + SECTION_ALIGN;
    }

private:
    static uint32_t align_up(uint32_t val, uint32_t align) {
        return (val + align - 1) & ~(align - 1);
    }

    static void pad_to(std::ofstream& f, uint64_t offset) {
        auto pos = static_cast<uint64_t>(f.tellp());
        if (pos < offset) {
            std::vector<uint8_t> zeros(static_cast<size_t>(offset - pos), 0);
            f.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
        }
    }

    static void write_u32_arr(uint8_t* arr, uint32_t off, uint32_t val) {
        arr[off+0] = val & 0xFF;
        arr[off+1] = (val>>8) & 0xFF;
        arr[off+2] = (val>>16) & 0xFF;
        arr[off+3] = (val>>24) & 0xFF;
    }
};

} // namespace rexc
