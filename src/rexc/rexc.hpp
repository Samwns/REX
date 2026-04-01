#pragma once
/*
 * rexc.hpp  –  REXC Main Driver
 *
 * Public API:
 *
 *   rexc::CompileResult rexc::compile(input_file, output_binary, ...)
 *   rexc::CompileResult rexc::compile_native(input_file, output_binary, ...)
 *   bool                rexc::has_native_backend()
 *
 * Two compilation pipelines:
 *
 *   Native (preferred – no external tools needed):
 *     1. Read source file
 *     2. Lex + Parse → AST
 *     3. Semantic analysis → SemanticContext
 *     4. Native code generation → x86_64 machine code
 *     5. ELF writer → static executable
 *
 *   C backend (fallback):
 *     1. Read source file
 *     2. Lex + Parse → AST
 *     3. Semantic analysis → SemanticContext
 *     4. Code generation → C source string
 *     5. Write C source to a temp file
 *     6. Invoke system C compiler to produce binary
 *     7. Return CompileResult
 *
 * The generated C file is written next to the output binary with a
 * ".rexc.c" suffix and is kept by default for debugging.  Pass
 * keep_intermediate=false to delete it after compilation.
 */

#include "lexer.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "codegen.hpp"
#include "native_codegen.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace rexc {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────
//  CompileResult
// ─────────────────────────────────────────────────────────────────
struct CompileResult {
    bool                     success  = false;
    std::string              binary;            // path to output binary
    std::string              c_source_path;     // path to generated .c file
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};

// ─────────────────────────────────────────────────────────────────
//  Compiler options
// ─────────────────────────────────────────────────────────────────
struct CompileOptions {
    std::string              cpp_std        = "c++17";     // for compatibility info (not used in C output)
    std::string              c_std          = "c11";       // C standard for generated code
    std::vector<std::string> flags;                        // extra flags for the C compiler
    std::string              include_path;                 // -I path
    std::string              runtime_dir;                  // where runtime.h lives
    std::string              cc_override;                  // override C compiler (for cross-compilation)
    bool                     keep_c_source  = true;
    bool                     verbose        = false;
    bool                     optimise       = false;
    bool                     debug_symbols  = false;
};

// ─────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────
namespace detail {

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return bool(f);
}

// Locate the system C compiler
inline std::string find_c_compiler() {
    // Try in order: cc, gcc, clang, tcc
    for (const char* c : {"cc", "gcc", "clang", "tcc"}) {
#ifdef _WIN32
        std::string check = "where ";
        check += c;
        check += " >NUL 2>&1";
#else
        std::string check = "command -v ";
        check += c;
        check += " >/dev/null 2>&1";
#endif
        if (std::system(check.c_str()) == 0) return c;
    }
    return "";
}

// Build the C compiler command line
inline std::string build_cc_command(
    const std::string& compiler,
    const std::string& c_source,
    const std::string& output,
    const std::string& c_std,
    const std::string& include_path,
    const std::string& runtime_dir,
    const std::vector<std::string>& extra_flags,
    bool optimise,
    bool debug_symbols) {

    std::string cmd = compiler;
    cmd += " -x c";              // force C mode (in case of .c extension mismatch)
    cmd += " -std=" + c_std;
    cmd += " \"" + c_source + "\"";
    cmd += " -o \"" + output + "\"";

    // Include dirs
    if (!include_path.empty())  cmd += " -I\"" + include_path + "\"";
    if (!runtime_dir.empty())   cmd += " -I\"" + runtime_dir  + "\"";

    // Optimisation
    if (optimise)       cmd += " -O2";
    if (debug_symbols)  cmd += " -g";

    // Warnings
    cmd += " -Wall -Wno-unused-variable -Wno-unused-function";
    cmd += " -Wno-incompatible-pointer-types";

    // Math library (for any math calls in the generated code)
    cmd += " -lm";

    // Extra user flags
    for (auto& f : extra_flags) cmd += " " + f;

    return cmd;
}

// Convert diagnostic list to strings
inline void diags_to_strings(const std::vector<Diagnostic>& diags,
                               std::vector<std::string>& errors,
                               std::vector<std::string>& warnings,
                               std::vector<std::string>& notes) {
    for (auto& d : diags) {
        switch (d.severity) {
            case Diagnostic::Severity::Error:   errors.push_back(d.str());   break;
            case Diagnostic::Severity::Warning: warnings.push_back(d.str()); break;
            case Diagnostic::Severity::Note:    notes.push_back(d.str());    break;
        }
    }
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────
//  compile()  –  main entry point
// ─────────────────────────────────────────────────────────────────
inline CompileResult compile(
    const std::string& input_file,
    const std::string& output_binary,
    const std::string& cpp_std       = "c++17",
    const std::vector<std::string>& flags = {},
    const std::string& include_path  = "",
    bool               keep_c_source = true,
    bool               verbose       = false,
    const std::string& cc_override   = "") {

    CompileResult result;

    // ── Step 1: Read source ───────────────────────────────────────
    std::string source = detail::read_file(input_file);
    if (source.empty() && !fs::exists(input_file)) {
        result.errors.push_back("rexc: cannot read input file: " + input_file);
        return result;
    }

    if (verbose) std::cerr << "[rexc] Lexing " << input_file << " ...\n";

    // ── Step 2: Lex ───────────────────────────────────────────────
    Lexer lexer(source, input_file);
    auto  tokens = lexer.tokenize();
    for (auto& e : lexer.errors()) result.errors.push_back(e);

    // ── Step 3: Parse ─────────────────────────────────────────────
    if (verbose) std::cerr << "[rexc] Parsing ...\n";

    std::vector<Diagnostic> diags;
    Parser parser(tokens);
    auto   tu = parser.parse();
    detail::diags_to_strings(parser.diagnostics(), result.errors, result.warnings, result.notes);

    // Continue even on parse errors (best-effort compilation)

    // ── Step 4: Semantic analysis ─────────────────────────────────
    if (verbose) std::cerr << "[rexc] Semantic analysis ...\n";

    SemanticAnalyzer sem;
    sem.analyze(*tu);
    detail::diags_to_strings(sem.diagnostics(), result.errors, result.warnings, result.notes);

    const SemanticContext& ctx = sem.context();

    // ── Step 5: Determine runtime.h path ─────────────────────────
    // Runtime header lives next to this header file; find it via the
    // include_path hint or relative to the output binary.
    fs::path runtime_h_path;
    {
        // Names to search for (generated code uses #include "runtime.h")
        const char* names[] = {"runtime.h", "rexc_runtime.h"};

        // Try next to the input file
        fs::path input_dir = fs::path(input_file).parent_path();
        for (auto name : names) {
            fs::path candidate = input_dir / name;
            if (fs::exists(candidate)) { runtime_h_path = candidate; break; }
        }

        // Use include_path hint
        if (runtime_h_path.empty() && !include_path.empty()) {
            for (auto name : names) {
                fs::path candidate = fs::path(include_path) / name;
                if (fs::exists(candidate)) { runtime_h_path = candidate; break; }
            }
        }

        if (runtime_h_path.empty()) {
            // Fall back to relative path from CWD
            runtime_h_path = "runtime.h";
        }
    }

    // ── Step 6: Code generation ───────────────────────────────────
    if (verbose) std::cerr << "[rexc] Code generation ...\n";

    CodeGenerator gen(ctx, runtime_h_path.string());
    std::string c_source;
    try {
        c_source = gen.generate_source(*tu);
    } catch (const std::exception& ex) {
        result.errors.push_back(std::string("rexc: code generation error: ") + ex.what());
        return result;
    }

    // ── Step 7: Write generated C source ─────────────────────────
    fs::path output_path(output_binary);
    fs::path c_file_path = output_path.parent_path() / (output_path.stem().string() + ".rexc.c");
    if (c_file_path.parent_path().empty()) c_file_path = fs::path(".") / c_file_path.filename();

    if (!detail::write_file(c_file_path.string(), c_source)) {
        result.errors.push_back("rexc: cannot write intermediate C file: " + c_file_path.string());
        return result;
    }
    result.c_source_path = c_file_path.string();

    if (verbose) {
        std::cerr << "[rexc] Generated C source: " << c_file_path << "\n";
    }

    // ── Step 8: Compile with system C compiler ────────────────────
    std::string compiler = cc_override.empty() ? detail::find_c_compiler() : cc_override;
    if (compiler.empty()) {
        result.errors.push_back("rexc: no C compiler found (tried cc, gcc, clang, tcc)");
        return result;
    }

    // Determine runtime directory (directory containing runtime.h)
    std::string runtime_dir = runtime_h_path.parent_path().string();
    if (runtime_dir.empty()) runtime_dir = ".";

    std::string cmd = detail::build_cc_command(
        compiler,
        c_file_path.string(),
        output_binary,
        "c11",
        include_path,
        runtime_dir,
        flags,
        false,   // optimise
        false    // debug_symbols
    );

    if (verbose) std::cerr << "[rexc] Compiling: " << cmd << "\n";

    int ret = std::system(cmd.c_str());

    // ── Step 9: Clean up ──────────────────────────────────────────
    if (!keep_c_source && fs::exists(c_file_path)) {
        fs::remove(c_file_path);
        result.c_source_path = "";
    }

    // ── Step 10: Report ───────────────────────────────────────────
    if (ret == 0) {
        result.success = true;
        result.binary  = output_binary;
        if (verbose) std::cerr << "[rexc] Binary written to: " << output_binary << "\n";
    } else {
        result.errors.push_back("rexc: C compiler exited with code " + std::to_string(ret));
        result.errors.push_back("rexc: check generated C source: " + c_file_path.string());
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────
//  Overload with full CompileOptions struct
// ─────────────────────────────────────────────────────────────────
inline CompileResult compile(
    const std::string& input_file,
    const std::string& output_binary,
    const CompileOptions& opts) {

    return compile(input_file, output_binary, opts.cpp_std, opts.flags,
                   opts.include_path, opts.keep_c_source, opts.verbose,
                   opts.cc_override);
}

// ─────────────────────────────────────────────────────────────────
//  compile_to_c()  –  only produce C source (no binary)
// ─────────────────────────────────────────────────────────────────
inline CompileResult compile_to_c(
    const std::string& input_file,
    const std::string& output_c_file) {

    CompileResult result;

    std::string source = detail::read_file(input_file);
    if (source.empty() && !fs::exists(input_file)) {
        result.errors.push_back("rexc: cannot read input file: " + input_file);
        return result;
    }

    Lexer  lexer(source, input_file);
    auto   tokens = lexer.tokenize();
    for (auto& e : lexer.errors()) result.errors.push_back(e);

    Parser parser(tokens);
    auto   tu = parser.parse();
    std::vector<std::string> dummy_notes;
    detail::diags_to_strings(parser.diagnostics(), result.errors, result.warnings, dummy_notes);

    SemanticAnalyzer sem;
    sem.analyze(*tu);
    detail::diags_to_strings(sem.diagnostics(), result.errors, result.warnings, dummy_notes);

    CodeGenerator gen(sem.context(), "rexc_runtime.h");
    std::string   c_source = gen.generate_source(*tu);

    if (detail::write_file(output_c_file, c_source)) {
        result.success      = true;
        result.c_source_path= output_c_file;
    } else {
        result.errors.push_back("rexc: cannot write C file: " + output_c_file);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────
//  has_native_backend()  –  check if native compilation is available
// ─────────────────────────────────────────────────────────────────
inline bool has_native_backend() {
    // Native backend is available on all supported platforms:
    //   - x86_64 Linux  (ELF64, Linux syscalls)
    //   - x86_64 Windows (PE32+, kernel32.dll imports)
    //   - x86_64 macOS  (Mach-O, BSD syscalls)
    //   - ARM64 Linux   (ELF64, Linux syscalls)
    //   - ARM64 macOS   (Mach-O, BSD syscalls)
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__linux__)
    return true;
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(_WIN32)
    return true;
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__APPLE__)
    return true;
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__linux__)
    return true;
#elif (defined(__aarch64__) || defined(_M_ARM64)) && defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────
//  compile_native()  –  produce binary without external compiler
// ─────────────────────────────────────────────────────────────────
//  Pipeline:
//    1. Read source file
//    2. Lex + Parse → AST
//    3. Semantic analysis
//    4. Native code generation → machine code (x86_64 or ARM64)
//    5. Executable writer → ELF64 (Linux), PE32+ (Windows), or Mach-O (macOS)
//
//  No external compiler or linker is needed.
// ─────────────────────────────────────────────────────────────────
inline CompileResult compile_native(
    const std::string& input_file,
    const std::string& output_binary,
    bool verbose = false) {

    CompileResult result;

    if (!has_native_backend()) {
        result.errors.push_back("rexc: native backend not available on this platform");
        return result;
    }

    // ── Step 1: Read source ───────────────────────────────────────
    std::string source = detail::read_file(input_file);
    if (source.empty() && !fs::exists(input_file)) {
        result.errors.push_back("rexc: cannot read input file: " + input_file);
        return result;
    }

    if (verbose) std::cerr << "[rexc-native] Lexing " << input_file << " ...\n";

    // ── Step 2: Lex ───────────────────────────────────────────────
    Lexer lexer(source, input_file);
    auto  tokens = lexer.tokenize();
    for (auto& e : lexer.errors()) result.errors.push_back(e);

    // ── Step 3: Parse ─────────────────────────────────────────────
    if (verbose) std::cerr << "[rexc-native] Parsing ...\n";

    Parser parser(tokens);
    auto   tu = parser.parse();
    detail::diags_to_strings(parser.diagnostics(), result.errors, result.warnings, result.notes);

    // ── Step 4: Semantic analysis ─────────────────────────────────
    if (verbose) std::cerr << "[rexc-native] Semantic analysis ...\n";

    SemanticAnalyzer sem;
    sem.analyze(*tu);
    detail::diags_to_strings(sem.diagnostics(), result.errors, result.warnings, result.notes);

    const SemanticContext& ctx = sem.context();

    // ── Step 5: Check native compatibility ────────────────────────
    std::string native_check = NativeCodeGenerator::can_compile_natively(*tu);
    if (!native_check.empty()) {
        result.errors.push_back("rexc: " + native_check);
        return result;
    }

    // ── Step 6: Native code generation ────────────────────────────
    if (verbose) std::cerr << "[rexc-native] Generating x86_64 machine code ...\n";

    NativeCodeGenerator gen(ctx);
    bool ok = false;
    try {
        ok = gen.generate(*tu, output_binary);
    } catch (const std::exception& ex) {
        result.errors.push_back(std::string("rexc: native codegen error: ") + ex.what());
        return result;
    }

    if (ok) {
        result.success = true;
        result.binary  = output_binary;
        if (verbose) std::cerr << "[rexc-native] Binary written to: " << output_binary << "\n";
    } else {
        result.errors.push_back("rexc: native code generation failed for: " + input_file);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────
//  Diagnostic pretty-printer
// ─────────────────────────────────────────────────────────────────
inline void print_result(const CompileResult& r) {
    for (auto& e : r.errors)   std::cerr << "\033[31m[rexc error]\033[0m "   << e << "\n";
    for (auto& w : r.warnings) std::cerr << "\033[33m[rexc warn]\033[0m "    << w << "\n";
    for (auto& n : r.notes)    std::cerr << "\033[36m[rexc note]\033[0m "    << n << "\n";
    if (r.success)
        std::cerr << "\033[32m[rexc] OK – binary: " << r.binary << "\033[0m\n";
}

} // namespace rexc
