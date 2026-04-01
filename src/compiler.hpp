#pragma once
#include <string>
#include <vector>
#include <fstream>
#include "utils.hpp"
#include "i18n.hpp"
#include "signal_handler.hpp"
#include "config.hpp"
#include "cross_compile.hpp"
#include "rexc/rexc.hpp"
#include "rexc/embedded_runtime.hpp"

// ─── Collect per-package include directories ──────────────────
// Each subdirectory under ~/.rex/libs/ is an installed package;
// adding -I for each allows #include "header.hpp" to work
// without requiring the package-name prefix.
inline std::vector<std::string> collect_lib_include_dirs(const std::string& libs_root) {
    std::vector<std::string> dirs;
    if (libs_root.empty() || !fs::exists(libs_root)) return dirs;

    auto push_unique = [&](const fs::path& p) {
        if (!fs::exists(p) || !fs::is_directory(p)) return;
        std::string s = p.string();
        if (std::find(dirs.begin(), dirs.end(), s) == dirs.end()) dirs.push_back(s);
    };

    for (auto& entry : fs::directory_iterator(libs_root)) {
        if (!fs::is_directory(entry)) continue;

        const fs::path pkg_dir = entry.path();
        const fs::path include_dir = pkg_dir / "include";

        // Support both layouts:
        // 1) ~/.rex/libs/<pkg>/<headers>
        // 2) ~/.rex/libs/<pkg>/include/<headers>
        push_unique(pkg_dir);
        push_unique(include_dir);
    }
    return dirs;
}

// ─── Detect if source uses preprocessor includes ──────────────
inline bool source_has_include_directive(const std::string& file) {
    std::ifstream in(file);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos && line.compare(pos, 8, "#include") == 0)
            return true;
    }
    return false;
}

// ─── Ensure runtime.h is installed in ~/.rex/libs/ ────────────
inline void ensure_runtime_header() {
    fs::path runtime_path = rex_home() / "libs" / "runtime.h";
    // Always write the runtime to keep it in sync with this binary
    fs::create_directories(runtime_path.parent_path());
    std::ofstream f(runtime_path);
    f << rexc_embedded::runtime_h_content();
}

// ─── Detect system C compiler (excludes native backend) ───────
inline std::string detect_system_c_compiler() {
    for (const char* c : {"cc", "gcc", "clang", "tcc"}) {
        if (command_exists(c)) return c;
    }
    return "";
}

// ─── Detects the backend C compiler used by rexc ──────────────
inline std::string detect_backend_compiler() {
    // Check native backend first (no external tools needed)
    if (rexc::has_native_backend()) return "__native__";
    return detect_system_c_compiler();
}

// ─── Detects the best available C++ compiler (fallback) ───────
inline std::string detect_cpp_compiler() {
    if (command_exists("clang++")) return "clang++";
    if (command_exists("g++"))     return "g++";
#ifdef _WIN32
    if (command_exists("cl"))      return "cl";
#endif
    return "";
}

// ─── Returns the compiler description for `rex version` ───────
inline std::string detect_compiler() {
    if (rexc::has_native_backend()) {
#if defined(__x86_64__) || defined(_M_X64)
        return "rexc (native x86_64)";
#elif defined(__aarch64__) || defined(_M_ARM64)
        return "rexc (native ARM64)";
#else
        return "rexc (native)";
#endif
    }
    std::string backend = detect_backend_compiler();
    if (!backend.empty() && backend != "__native__") return "rexc (backend: " + backend + ")";
    std::string cpp = detect_cpp_compiler();
    if (!cpp.empty()) return cpp + " (fallback)";
    return "";
}

// ─── Builds the compiler command from config ──────────────────
struct BuildResult {
    bool success = false;
    std::string binary;
};

// ─── Compile a single file using rexc pipeline ────────────────
inline BuildResult compile_file(const std::string& file,
                                const std::string& out,
                                const std::string& cpp_std  = "c++20",
                                const std::vector<std::string>& extra_flags = {},
                                const std::string& libs_path = "",
                                const std::string& target_triple = "") {
    BuildResult res;

    std::string os = detect_os();

    // Parse cross-compilation target (if provided)
    TargetTriple target = parse_target_triple(target_triple);
    bool cross = !target.empty() && !is_native_target(target);

    // Output binary name
    std::string binary = out;
    if (cross) {
        // Use target-appropriate extension
        std::string ext = target_binary_extension(target);
        if (!ext.empty() && !ends_with(binary, ext)) binary += ext;
    } else {
#ifdef _WIN32
        if (!ends_with(binary, ".exe")) binary += ".exe";
#endif
    }

    // Collect per-package include directories
    std::string libs_root = libs_path.empty() ? (rex_home() / "libs").string() : libs_path;
    auto pkg_dirs = collect_lib_include_dirs(libs_root);

    // Find the appropriate compiler
    CrossCompiler cross_cc;
    if (cross) {
        cross_cc = find_cross_compiler(target);
        if (cross_cc.compiler.empty()) {
            rex_err(rex_i18n::msg("compiler_no_cross") + target.str());
            rex_info(rex_i18n::msg("compiler_install_hint") + target.str());
            rex_info(rex_i18n::msg("compiler_clang_hint"));
            return res;
        }
    }

    // Try native backend first (no external compiler needed)
    // but skip it for include-heavy sources that require header semantics.
    bool has_includes = source_has_include_directive(file);
    if (!cross && rexc::has_native_backend() && !has_includes) {
        rex_info(rex_i18n::msg("compiler_rexc_native"));
        rex_info(rex_i18n::msg("compiler_platform") + os);

        auto native_res = rexc::compile_native(file, binary, false);
        if (native_res.success) {
            res.success = true;
            res.binary  = native_res.binary;
            return res;
        }
        // Native backend failed (possibly unsupported feature), fall through
        for (auto& e : native_res.errors) rex_warn("rexc-native: " + e);
        rex_info(rex_i18n::msg("compiler_falling_back_c"));
    } else if (!cross && has_includes) {
        rex_info("Source uses #include; using preprocessor-aware backend.");
    }

    // Try rexc C pipeline (needs external C compiler)
    std::string backend = cross ? cross_cc.compiler : detect_backend_compiler();
    // Skip "__native__" sentinel since we already tried native above
    if (backend == "__native__")
        backend = detect_system_c_compiler();
    if (!backend.empty()) {
        if (cross) {
            rex_info(rex_i18n::msg("run_target") + target.str());
            rex_info(rex_i18n::msg("compiler_rexc_backend") + backend + ")");
        } else {
            rex_info(rex_i18n::msg("compiler_rexc_backend") + backend + ")");
            rex_info(rex_i18n::msg("compiler_platform") + os);
        }

        // Ensure runtime.h is available for rexc
        ensure_runtime_header();

        // Add per-package -I flags so headers are found directly
        std::vector<std::string> rexc_flags = extra_flags;
        for (auto& d : pkg_dirs) rexc_flags.push_back("-I\"" + d + "\"");

        // Add cross-compilation flags
        if (cross) {
            auto cross_flags = get_cross_compile_flags(target, cross_cc);
            for (auto& f : cross_flags) rexc_flags.push_back(f);
        }

        auto rexc_res = rexc::compile(file, binary, cpp_std, rexc_flags, libs_root, false,
                                       false, cross ? cross_cc.compiler : "");
        if (rexc_res.success) {
            res.success = true;
            res.binary  = rexc_res.binary;
            return res;
        }
        // Print rexc errors and fall back to system compiler
        for (auto& e : rexc_res.errors)   rex_warn("rexc: " + e);
        rex_info(rex_i18n::msg("compiler_falling_back"));
    }

    // Fallback: use system C++ compiler directly
    std::string compiler = cross ? cross_cc.cpp_compiler : detect_cpp_compiler();
    if (compiler.empty()) {
        if (cross)
            rex_err(rex_i18n::msg("compiler_no_cross") + target.str());
        else
            rex_err(rex_i18n::msg("compiler_no_compiler"));
        return res;
    }

    // Build command
    std::string cmd = compiler;
    cmd += " \"" + file + "\"";
    cmd += " -o \"" + binary + "\"";
    cmd += " -std=" + cpp_std;

    // Include libs path and per-package subdirectories
    if (!libs_root.empty()) {
        cmd += " -I\"" + libs_root + "\"";
        for (auto& d : pkg_dirs) cmd += " -I\"" + d + "\"";
    }

    // Cross-compilation or native linking flags
    if (cross) {
        auto cross_flags = get_cross_compile_flags(target, cross_cc);
        for (auto& f : cross_flags) cmd += " " + f;
    } else {
        if (os == "linux")   cmd += " -static-libgcc -static-libstdc++";
        if (os == "windows") cmd += " -static";
    }

    // Extra flags
    for (auto& f : extra_flags) cmd += " " + f;

    if (cross)
        rex_info(rex_i18n::msg("run_target") + target.str());
    rex_info(rex_i18n::msg("compiler_fallback") + compiler + " (fallback)");
    if (!cross)
        rex_info(rex_i18n::msg("compiler_platform") + os);
    rex_info(rex_i18n::msg("compiler_command") + cmd);
    std::cout << "\n";

    int ret = system(cmd.c_str());

    if (ret == 0) {
        res.success = true;
        res.binary  = binary;
    }
    return res;
}

// ─── Compile a full project (all .cpp in src/) ────────────────
inline BuildResult compile_project(const RexConfig& cfg) {
    BuildResult res;

    // Collect all .cpp sources
    std::vector<std::string> cpp_files;
    if (!cfg.sources.empty()) {
        cpp_files = cfg.sources;
    } else {
        // Auto-discover src/ directory
        for (auto& entry : fs::recursive_directory_iterator("src")) {
            if (entry.path().extension() == ".cpp")
                cpp_files.push_back(entry.path().string());
        }
    }

    if (cpp_files.empty()) {
        rex_err(rex_i18n::msg("compiler_no_cpp_files"));
        return res;
    }

    // Ensure build dir exists
    fs::create_directories(fs::path(cfg.output).parent_path());

    std::string os = detect_os();

    // Parse cross-compilation target (if provided)
    TargetTriple target = parse_target_triple(cfg.target);
    bool cross = !target.empty() && !is_native_target(target);

    std::string binary = cfg.output;
    if (cross) {
        std::string ext = target_binary_extension(target);
        if (!ext.empty() && !ends_with(binary, ext)) binary += ext;
    } else {
#ifdef _WIN32
        if (!ends_with(binary, ".exe")) binary += ".exe";
#endif
    }

    std::string libs_include = (rex_home() / "libs").string();
    auto pkg_dirs = collect_lib_include_dirs(libs_include);

    // Find the appropriate compiler for cross-compilation
    CrossCompiler cross_cc;
    if (cross) {
        cross_cc = find_cross_compiler(target);
        if (cross_cc.compiler.empty()) {
            rex_err(rex_i18n::msg("compiler_no_cross") + target.str());
            rex_info(rex_i18n::msg("compiler_install_hint") + target.str());
            return res;
        }
    }

    // For single-file projects, try native backend first, then rexc C pipeline
    if (cpp_files.size() == 1) {
        // Try native backend (no external compiler needed)
        // but skip it for include-heavy sources that require header semantics.
        bool has_includes = source_has_include_directive(cpp_files[0]);
        if (!cross && rexc::has_native_backend() && !has_includes) {
            rex_info(rex_i18n::msg("compiler_building") + cfg.name + " v" + cfg.version);
            rex_info(rex_i18n::msg("compiler_rexc_native"));

            auto native_res = rexc::compile_native(cpp_files[0], binary, false);
            if (native_res.success) {
                res.success = true;
                res.binary  = native_res.binary;
                return res;
            }
            for (auto& e : native_res.errors) rex_warn("rexc-native: " + e);
            rex_info(rex_i18n::msg("compiler_falling_back_c"));
        } else if (!cross && has_includes) {
            rex_info("Source uses #include; using preprocessor-aware backend.");
        }

        std::string backend = cross ? cross_cc.compiler : detect_backend_compiler();
        // Skip "__native__" sentinel
        if (backend == "__native__")
            backend = detect_system_c_compiler();
        if (!backend.empty()) {
            rex_info(rex_i18n::msg("compiler_building") + cfg.name + " v" + cfg.version);
            if (cross)
                rex_info(rex_i18n::msg("run_target") + target.str());
            rex_info(rex_i18n::msg("compiler_rexc_backend") + backend + ")");

            // Ensure runtime.h is available for rexc
            ensure_runtime_header();

            // Add per-package -I flags so headers are found directly
            std::vector<std::string> rexc_flags = cfg.flags;
            for (auto& d : pkg_dirs) rexc_flags.push_back("-I\"" + d + "\"");

            // Add cross-compilation flags
            if (cross) {
                auto cross_flags = get_cross_compile_flags(target, cross_cc);
                for (auto& f : cross_flags) rexc_flags.push_back(f);
            }

            auto rexc_res = rexc::compile(cpp_files[0], binary, cfg.cpp_std,
                                           rexc_flags, libs_include, false,
                                           false, cross ? cross_cc.compiler : "");
            if (rexc_res.success) {
                res.success = true;
                res.binary  = rexc_res.binary;
                return res;
            }
            for (auto& e : rexc_res.errors) rex_warn("rexc: " + e);
            rex_info(rex_i18n::msg("compiler_falling_back"));
        }
    }

    // Fallback / multi-file: use system C++ compiler
    std::string compiler = cross ? cross_cc.cpp_compiler : detect_cpp_compiler();
    if (compiler.empty()) {
        if (cross)
            rex_err(rex_i18n::msg("compiler_no_cross") + target.str());
        else
            rex_err(rex_i18n::msg("compiler_no_compiler"));
        return res;
    }

    std::string cmd = compiler;
    for (auto& f : cpp_files) cmd += " \"" + f + "\"";
    cmd += " -o \"" + binary + "\"";
    cmd += " -std=" + cfg.cpp_std;
    cmd += " -I\"" + libs_include + "\"";
    for (auto& d : pkg_dirs) cmd += " -I\"" + d + "\"";

    // Cross-compilation or native linking flags
    if (cross) {
        auto cross_flags = get_cross_compile_flags(target, cross_cc);
        for (auto& f : cross_flags) cmd += " " + f;
    } else {
        if (os == "linux")   cmd += " -static-libgcc -static-libstdc++";
        if (os == "windows") cmd += " -static";
    }

    for (auto& f : cfg.flags) cmd += " " + f;

    rex_info(rex_i18n::msg("compiler_building") + cfg.name + " v" + cfg.version);
    if (cross)
        rex_info(rex_i18n::msg("run_target") + target.str());
    rex_info(rex_i18n::msg("compiler_fallback") + compiler + " (fallback)");
    rex_info(rex_i18n::msg("compiler_command") + cmd);
    std::cout << "\n";

    int ret = system(cmd.c_str());
    if (ret == 0) { res.success = true; res.binary = binary; }
    return res;
}

// ─── Run a compiled binary ────────────────────────────────────
inline int run_binary(const std::string& binary, const std::vector<std::string>& args = {}) {
    // Get absolute path to avoid "not recognized" errors on Windows
    std::string abs_bin = fs::absolute(binary).string();

    std::string cmd;
#ifdef _WIN32
    // On Windows wrap binary path in quotes
    cmd = "\"" + abs_bin + "\"";
    for (auto& a : args) cmd += " " + a;
#else
    cmd = "\"" + abs_bin + "\"";
    for (auto& a : args) cmd += " " + a;
#endif
    return system(cmd.c_str());
}
