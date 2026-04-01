#pragma once
/*
 * cross_compile.hpp  –  Cross-compilation support for REX v0.3
 *
 * Provides:
 *   - Target triple parsing  (arch-os-abi)
 *   - Cross-compiler detection  (e.g. aarch64-linux-gnu-gcc)
 *   - Platform-specific flag generation
 *   - Host target detection
 *   - Known targets listing
 */

#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include "utils.hpp"

// ─── Target triple ────────────────────────────────────────────
struct TargetTriple {
    std::string arch;    // e.g. x86_64, aarch64, arm, i686, wasm32
    std::string os;      // e.g. linux, windows, macos, unknown
    std::string abi;     // e.g. gnu, musl, mingw32, msvc, eabi, eabihf, unknown

    std::string str() const {
        if (arch.empty()) return "";
        return arch + "-" + os + "-" + abi;
    }

    bool empty() const { return arch.empty(); }
};

// ─── Known supported targets ─────────────────────────────────
struct KnownTarget {
    std::string triple;
    std::string description;
};

inline std::vector<KnownTarget> get_known_targets() {
    return {
        {"x86_64-linux-gnu",      "Linux x86_64 (glibc)"},
        {"x86_64-linux-musl",     "Linux x86_64 (musl, static)"},
        {"aarch64-linux-gnu",     "Linux ARM64 (glibc)"},
        {"aarch64-linux-musl",    "Linux ARM64 (musl, static)"},
        {"arm-linux-gnueabihf",   "Linux ARM 32-bit (hard float)"},
        {"i686-linux-gnu",        "Linux x86 32-bit"},
        {"x86_64-w64-mingw32",    "Windows x86_64 (MinGW)"},
        {"i686-w64-mingw32",      "Windows x86 32-bit (MinGW)"},
        {"x86_64-apple-macos",    "macOS x86_64"},
        {"arm64-apple-macos",     "macOS ARM64 (Apple Silicon)"},
        {"wasm32-unknown-unknown","WebAssembly 32-bit"},
    };
}

// ─── Parse a target triple string ────────────────────────────
inline TargetTriple parse_target_triple(const std::string& triple) {
    TargetTriple t;
    if (triple.empty()) return t;

    // Split on '-'
    std::vector<std::string> parts;
    std::string current;
    for (char c : triple) {
        if (c == '-') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);

    if (parts.empty()) return t;

    // First part is always arch
    t.arch = parts[0];

    // Normalise common aliases
    if (t.arch == "arm64") t.arch = "aarch64";

    if (parts.size() == 2) {
        // arch-os  (e.g. x86_64-linux)
        t.os  = parts[1];
        t.abi = "unknown";
    } else if (parts.size() >= 3) {
        // arch-os-abi or arch-vendor-os[-abi]
        // Handle 'apple' as vendor: arm64-apple-macos → aarch64-macos-unknown
        if (parts[1] == "apple") {
            t.os  = parts[2];
            t.abi = parts.size() > 3 ? parts[3] : "unknown";
        } else if (parts[1] == "w64") {
            // x86_64-w64-mingw32 → arch=x86_64, os=windows, abi=mingw32
            t.os  = "windows";
            t.abi = parts[2];
        } else if (parts[1] == "unknown") {
            // wasm32-unknown-unknown
            t.os  = parts[2];
            t.abi = "unknown";
        } else {
            t.os  = parts[1];
            t.abi = parts[2];
            // Some triples have 4+ parts where everything after os
            // forms the ABI (e.g. arm-linux-gnueabihf from
            // arm-linux-gnueabi-hf, though typically written as 3 parts)
            for (size_t i = 3; i < parts.size(); i++)
                t.abi += "-" + parts[i];
        }
    }

    // Normalise OS names (applied after vendor-specific handling above,
    // so e.g. "w64" vendor already sets os="windows" and won't match here)
    if (t.os == "macos" || t.os == "darwin" || t.os == "macosx") t.os = "macos";
    if (t.os == "win32" || t.os == "mingw32")                    t.os = "windows";

    return t;
}

// ─── Detect the host target triple ──────────────────────────
inline TargetTriple get_host_target() {
    TargetTriple t;

    // Detect architecture
#if defined(__x86_64__) || defined(_M_X64)
    t.arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    t.arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    t.arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
    t.arch = "i686";
#else
    t.arch = "unknown";
#endif

    // Detect OS
#ifdef _WIN32
    t.os = "windows";
#elif __APPLE__
    t.os = "macos";
#else
    t.os = "linux";
#endif

    // Detect ABI
#ifdef _WIN32
    #ifdef __MINGW32__
        t.abi = "mingw32";
    #else
        t.abi = "msvc";
    #endif
#elif __APPLE__
    t.abi = "unknown";
#else
    t.abi = "gnu";
#endif

    return t;
}

// ─── Check if target is the native host ─────────────────────
inline bool is_native_target(const TargetTriple& target) {
    auto host = get_host_target();
    return target.arch == host.arch && target.os == host.os;
}

// ─── Find a cross-compiler for the given target ─────────────
// For GCC-based toolchains: try {triple}-gcc
// For Clang: the host clang with --target={triple}
struct CrossCompiler {
    std::string compiler;     // e.g. "aarch64-linux-gnu-gcc" or "clang"
    std::string cpp_compiler; // e.g. "aarch64-linux-gnu-g++" or "clang++"
    bool        uses_target_flag = false;  // true if clang --target=
};

inline CrossCompiler find_cross_compiler(const TargetTriple& target) {
    CrossCompiler cc;

    // If native target, use native compiler
    if (is_native_target(target)) {
        cc.compiler     = "cc";
        cc.cpp_compiler = "c++";
        // Fallback to detected compilers
        for (const char* c : {"cc", "gcc", "clang", "tcc"}) {
            if (command_exists(c)) { cc.compiler = c; break; }
        }
        for (const char* c : {"c++", "clang++", "g++"}) {
            if (command_exists(c)) { cc.cpp_compiler = c; break; }
        }
        return cc;
    }

    // Build the GCC-style cross-compiler name
    std::string gcc_prefix = target.str();
    std::string gcc_name = gcc_prefix + "-gcc";
    std::string gpp_name = gcc_prefix + "-g++";

    // Try GCC cross-compiler first
    if (command_exists(gcc_name)) {
        cc.compiler     = gcc_name;
        cc.cpp_compiler = gpp_name;
        return cc;
    }

    // Also try common naming variants
    // e.g. for windows cross-compilation on Linux
    if (target.os == "windows") {
        for (const char* prefix : {"x86_64-w64-mingw32", "i686-w64-mingw32"}) {
            if (target.arch == "x86_64" && std::string(prefix).find("x86_64") == 0) {
                std::string try_gcc = std::string(prefix) + "-gcc";
                std::string try_gpp = std::string(prefix) + "-g++";
                if (command_exists(try_gcc)) {
                    cc.compiler     = try_gcc;
                    cc.cpp_compiler = try_gpp;
                    return cc;
                }
            }
            if (target.arch == "i686" && std::string(prefix).find("i686") == 0) {
                std::string try_gcc = std::string(prefix) + "-gcc";
                std::string try_gpp = std::string(prefix) + "-g++";
                if (command_exists(try_gcc)) {
                    cc.compiler     = try_gcc;
                    cc.cpp_compiler = try_gpp;
                    return cc;
                }
            }
        }
    }

    // Try Clang with --target flag (Clang supports cross-compilation natively)
    if (command_exists("clang")) {
        cc.compiler         = "clang";
        cc.cpp_compiler     = "clang++";
        cc.uses_target_flag = true;
        return cc;
    }

    return cc; // empty = no cross-compiler found
}

// ─── Generate cross-compilation flags for the C compiler ────
inline std::vector<std::string> get_cross_compile_flags(
    const TargetTriple& target,
    const CrossCompiler& cc) {

    std::vector<std::string> flags;

    // Add --target flag for Clang
    if (cc.uses_target_flag && !target.empty()) {
        flags.push_back("--target=" + target.str());
    }

    // Static linking flags per target OS
    if (target.os == "linux") {
        flags.push_back("-static-libgcc");
        // For musl, prefer full static
        if (target.abi == "musl") {
            flags.push_back("-static");
        }
    } else if (target.os == "windows") {
        flags.push_back("-static");
    }

    // Windows-specific libraries
    if (target.os == "windows") {
        flags.push_back("-luser32");
        flags.push_back("-ladvapi32");
    }

    // macOS target version
    if (target.os == "macos") {
        if (target.arch == "aarch64" || target.arch == "arm64") {
            flags.push_back("-mmacosx-version-min=11.0");
        } else {
            flags.push_back("-mmacosx-version-min=10.15");
        }
    }

    return flags;
}

// ─── Output binary extension for target OS ──────────────────
inline std::string target_binary_extension(const TargetTriple& target) {
    if (target.os == "windows") return ".exe";
    if (target.os == "unknown" && target.arch == "wasm32") return ".wasm";
    return "";
}
