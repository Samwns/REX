# REX Changelog

All notable changes to the REX compiler project are documented in this file.

---

## v1.0.18 — Replace `brun` with `-b` flag

**`rex brun` is replaced by `rex -b`.** The compile+run step is now a flag instead of a subcommand.

### Changed
- **`rex -b <file.cpp>`** — compile and immediately run (replaces `rex brun`)
- Removed `brun` subcommand completely; `rex -b` is the new interface
- Updated help text, examples, and documentation throughout

---

## v1.0.17 — End-to-End Test Suite + Native x86_64 Codegen Fixes

### Added
- **End-to-end run/brun test suite** (`src/test_run_brun.cpp`) — tests interpreter vs compiler output for a broad set of C++ programs

### Fixed
- Native x86_64 codegen bugs: parser double-wrapped multi-var `DeclStmt`; `i++`/`--i` not writing back to memory; `+=`/`-=`/`*=` not implemented

---

## v1.0.16 — `B > run` Help Display + Interpreter Builtin Tests

### Changed
- Renamed `brun` help display to `B > run` for visual clarity
- Added interpreter builtin tests

---

## v1.0.15 — Windows Native Backend Fix + Interpreter Builtins

### Fixed
- Windows native backend crash in rexc
- Interpreter builtins library include resolution

### Added
- Additional interpreter builtin functions

---

## v1.0.14 — Windows/MSYS2 Math Constants Fix

### Fixed
- `M_PI` / `M_E` undeclared on Windows/MSYS2 build (#38)

---

## v1.0.13 — C++20 Default + CI Build Fix

### Changed
- **Default C++ standard changed from C++17 to C++20**
- Fixed constructor init list regression

### Fixed
- CI build failures

---

## v1.0.12 — Roadmap/Changelog Update + Interpreter Fix

### Fixed
- Interpreter constructor field sync bug

### Changed
- Updated roadmap and changelog entries for v1.0.1–v1.0.11

---

## v1.0.11 — Interpreter CLI Integration

**`rex run` now interprets C++ directly.** The tree-walking interpreter is fully integrated into the CLI, with `rex brun` for compile+run and `rex repl` for interactive mode.

### Added
- **`rex run <file.cpp>`** — interprets a file directly using the tree-walking interpreter (no compilation)
- **`rex brun <file.cpp>`** — compile and immediately run a single file (previous `rex run` behavior)
- **`rex repl`** — interactive C++ interpreter (Read-Eval-Print-Loop) with multi-line support
- **i18n support** — English and Portuguese-BR translations for all help text and messages (`src/i18n.hpp`)
- **`--lang=XX` flag** — override output language from command line (e.g. `--lang=pt`)
- Updated help text to show all new commands (`run`, `brun`, `repl`)
- Interpreter fixes: VarDecl constructor-syntax handling, LambdaExpr body_ast support, default class construction, implicit `this->field` access in methods

---

## v1.0.10 — Tree-Walking Interpreter Module

### Added
- **Tree-walking interpreter** (`src/interpreter/`) — executes C++ AST directly without compilation
- `value.hpp` — universal runtime value type (null, bool, int, float, string, function, object, array)
- `environment.hpp` — chained variable scopes with control flow signals (break, continue, return)
- `builtins.hpp/cpp` — built-in functions: `cout`, `cin`, `cerr`, `endl`, math (`sqrt`, `pow`, `sin`, `cos`, etc.), string methods, `to_string`, `stoi`, `stof`, `rand`/`srand`, `exit`
- `interpreter.hpp/cpp` — tree-walking interpreter engine supporting variables, functions, classes, inheritance, lambdas, vectors, try/catch, loops, conditionals
- `test_interpreter.cpp` — 20 comprehensive tests (arithmetic, variables, loops, functions, recursion, classes, inheritance, vectors, lambdas, try/catch, range-for, FizzBuzz)

---

## v1.0.9 — Updater Improvements

### Fixed
- **Updater 404 fix** — parse asset URLs from GitHub API response instead of hardcoded URL patterns (`src/updater.hpp`)
- Improved download progress display with animations

---

## v1.0.8 — Versioned Release Binaries

### Added
- **Versioned release binary names** — release assets now include version tag (e.g. `rex-v1.0.8-linux-x86_64`)
- **Changelog in releases** — GitHub releases now include auto-generated changelog from commits
- **`docs/CHANGELOG.md`** — this file

### Fixed
- **Windows x64 setjmp/longjmp segfault** — isolate setjmp/longjmp in dedicated `noinline` functions to ensure correct SEH unwind metadata

---

## v1.0.5 — v1.0.7 — Signal Handling Fixes

### Fixed
- **Integer signedness comparison warning** in signal handler exit code decoding
- Various stability fixes for signal handling on POSIX and Windows

---

## v1.0.3 — v1.0.4 — Windows rexc_function Fixes

### Fixed
- **rexc_function template constructor** hijacking copy/move on Windows (#27)
- **rexc_function SBO object lifetime** UB exposed by GCC 15 -O2 on Windows (#28)

---

## v1.0.1 — v1.0.2 — Interpreter Planning

### Added
- **`REX_INTERPRETER_INSTRUCTIONS.md`** — detailed specification for the tree-walking interpreter
- Removed old compiler roadmap (`REX_COMPILER_ROADMAP_INSTRUCTIONS.md`)

---

## v1.0.0 — Fully Independent Compiler

**The REX compiler is now completely self-contained.** No external compiler, assembler, or linker is required to compile C++ programs on any supported platform.

### Added
- **Graph coloring register allocator** (Chaitin-Briggs) — replaces simple linear scan for optimal register usage (`src/rexc/regalloc.hpp`)
- **Lambda support** via `rexc_function` type-erased callable
- **Complete test suite** — 66 tests covering all compiler phases (lexer, parser, semantic, codegen, IR, optimizer, classes, runtime, stdlib, register allocator)
- **`docs/CLASSES.md`** — documentation for class layout, vtable, and name mangling
- **`docs/CHANGELOG.md`** — this file

### Changed
- Native backend is now the **default compilation path** (C backend available as optional fallback with `--use-c-backend` or `--allow-fallback`)
- Updated README roadmap to reflect compiler development phases v0.5–v1.0
- Updated documentation to remove external compiler requirements
- Updated `docs/NATIVE_BACKEND.md` with IR pipeline, class support, and runtime/stdlib layers

### Fixed
- `ObjectLayoutBuilder` now correctly handles class members parsed as `VarDecl` (in addition to `FieldDecl`)
- `insertion_sort` in `rexc_algorithm.hpp` now uses `operator<` instead of `operator>` for compatibility with types that only define `<`

---

## v0.8.0 — Templates + Standard Library

### Added
- **Template instantiation** in IR — function and class templates compile to separate IR functions
- **Standard library implementations** (all in `src/rexc/stdlib/`):
  - `rexc_algorithm.hpp` — sort (introsort), find, find_if, copy, fill, min, max, swap
  - `rexc_memory.hpp` — `rexc_unique_ptr<T>`, `rexc_shared_ptr<T>`, `make_unique`, `make_shared`
  - `rexc_functional.hpp` — `rexc_function<R(Args...)>` type-erased callable with SBO
  - `rexc_optional.hpp` — `rexc_optional<T>` with in-place storage
  - `rexc_cstring.hpp` — strlen, strcpy, strcmp, memcpy, memset, memmove
  - `rexc_cstdlib.hpp` — atoi, atof, rand/srand, exit, abort
- **Basic exception handling** via setjmp/longjmp (`src/rexc/runtime/rexc_except.hpp`)
- Tests 51–57

---

## v0.7.0 — Own Runtime (rexc_rt)

### Added
- **Standalone memory allocator** (`src/rexc/runtime/rexc_alloc.hpp`):
  - Free-list allocator with best-fit and coalescing
  - Uses mmap (Linux/macOS) or VirtualAlloc (Windows) — no libc malloc
  - `rexc_malloc`, `rexc_free`, `rexc_realloc`, `rexc_calloc`
- **Standalone string** (`src/rexc/runtime/rexc_string.hpp`):
  - Small String Optimization (SSO) for strings ≤ 15 bytes
  - Full string operations: concat, compare, find, substr, conversions
- **Standalone containers** (`src/rexc/runtime/rexc_containers.hpp`):
  - `rexc_vector<T>` — dynamic array with push_back, pop_back, resize
  - `rexc_map<K,V>` — sorted-array map with binary search
  - `rexc_pair<A,B>` and `make_pair`
- **Standalone I/O** (`src/rexc/runtime/rexc_io.hpp`):
  - Direct syscall I/O (write/read on Unix, WriteFile/ReadFile on Windows)
  - `OStream` / `IStream` with `operator<<` / `operator>>`
  - Global `cout`, `cerr`, `cin` instances
- **Program startup** (`src/rexc/runtime/rexc_startup.hpp`):
  - `rexc_startup()` initializes heap before user code
  - `rexc_exit()` via platform-native exit syscall
- **Compatibility headers** (`src/rexc/compat/`):
  - `rexc_iostream`, `rexc_vector`, `rexc_string`, `rexc_map`
  - Map `std::*` types to `rexc_rt::*` implementations
- Tests 45–50

---

## v0.6.0 — Classes and Vtable in Native Backend

### Added
- **Object memory layout** (`src/rexc/object_layout.hpp`):
  - Itanium ABI-compatible class layout with natural alignment
  - `__vptr` at offset 0 for classes with virtual methods
  - VTable slot assignment for virtual dispatch
- **Name mangling** (`src/rexc/mangler.hpp`):
  - Itanium ABI mangling for methods, constructors, destructors, vtables, free functions
- **IR generation for classes** — constructors, virtual calls, field access
- **`docs/CLASSES.md`** — class layout and vtable documentation
- Tests 38–44

---

## v0.5.0 — IR/SSA Layer + Optimizations

### Added
- **SSA Intermediate Representation** (`src/rexc/ir.hpp`):
  - IR types: Void, Bool, Int8–Int64, Float32, Float64, Ptr
  - IR opcodes: arithmetic, logic, comparison, memory, control flow, Phi, Cast
  - IR structures: IRInstr, IRBlock, IRFunction, IRModule
- **IR Generator** (`src/rexc/ir_gen.hpp`):
  - Converts AST → SSA IR (mem2reg style: alloca → load/store → SSA promotion)
  - Supports functions, variables, arithmetic, control flow, function calls
- **IR Optimizer** (`src/rexc/ir_opt.hpp`):
  - Constant folding: `2 + 3` → `Const(5)`
  - Dead code elimination (DCE)
  - mem2reg: promote stack allocas to SSA values
  - Basic inlining for small functions
  - CFG simplification
- **IR Printer** (`src/rexc/ir_printer.hpp`) — human-readable IR output for debugging
- Updated `native_codegen.hpp` to use IR pipeline: AST → IR → optimize → codegen
- **`docs/IR.md`** — IR documentation
- Updated `docs/ARCHITECTURE.md` with IR layer in pipeline diagram
- Tests 31–37

---

## v0.4.0 — Multi-Platform Native Backend

### Added
- **PE writer** (`src/rexc/pe_writer.hpp`) — Windows PE32+ executable generation
- **Mach-O writer** (`src/rexc/macho_writer.hpp`) — macOS Mach-O executable generation
- **ARM64 emitter** (`src/rexc/arm64_emitter.hpp`) — AArch64 instruction encoding
- Native backend now supports **5 platforms**: x86_64 Linux, x86_64 Windows, x86_64 macOS, ARM64 Linux, ARM64 macOS
- Tests 20–30

---

## v0.3.0 — Cross-Compilation + Native x86_64 Backend

### Added
- **Cross-compilation support** — 11+ target triples via `--target=TRIPLE`
- **Native x86_64 backend** (`src/rexc/native_codegen.hpp`):
  - Direct AST → machine code compilation (no external compiler)
  - x86 emitter (`src/rexc/x86_emitter.hpp`)
  - ELF writer (`src/rexc/elf_writer.hpp`)
- **Compiler independence** on Linux x86_64 — first platform with zero external dependencies
- `rex targets` command to list available cross-compilation targets
- Target triple parsing and flag generation
- Tests 15–19

### Releases
- v0.3.1 through v0.3.7 — bug fixes, stability improvements

---

## v0.2.0 — Built-in rexc Compiler

### Added
- **rexc compiler** — built-in C++ → C transpiler:
  - Lexer (`src/rexc/lexer.hpp`)
  - Parser (`src/rexc/parser.hpp`)
  - Semantic analysis (`src/rexc/semantic.hpp`)
  - C code generation (`src/rexc/codegen.hpp`)
  - Runtime library (`src/rexc/runtime.h`)
- Class/struct support with inheritance and virtual methods (C backend)
- Template instantiation (C backend)

### v0.2.1
- `std::cin` / `std::getline` support
- Input/output stream handling in rexc

### v0.2.2
- Multi C++ standard support: c++11, c++14, c++17, c++20, c++23

### v0.2.3
- ANSI terminal colors
- `termcolor` support in rexc

### v0.2.4
- Auto-update checking (`src/updater.hpp`)
- Community package registry support
- i18n support (English + Portuguese)

---

## v0.1.0 — Initial Release

### Added
- `rex run` — compile and run single file
- `rex build` — build project from `rex.toml`
- `rex init` — initialize new project
- `rex add` / `rex remove` — package management
- `rex clean` — clean build artifacts
- Built-in library registry (nlohmann-json, stb, glm, etc.)
- `rex.toml` project configuration
