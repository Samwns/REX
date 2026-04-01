# REX Compiler Architecture

This document describes the internal architecture of **rexc**, REX's built-in C++ compiler.

---

## Overview

rexc is a C++ compiler that can produce executables through two independent pipelines:

1. **Native Backend** — generates machine code directly on all supported platforms (no external tools)
2. **C Backend** — transpiles C++ to C, then invokes a system C compiler

The native backend makes REX completely independent from any external compiler on x86_64 Linux, x86_64 Windows, x86_64/ARM64 macOS, and ARM64 Linux. The C backend serves as a fallback for complex features not yet supported natively.

---

## Compilation Pipeline

Both pipelines share the same frontend (lexer → parser → semantic analysis), then diverge at code generation:

```
                     C++ Source Code
                          │
                    ┌─────┴─────┐
                    │   Lexer   │   src/rexc/lexer.hpp
                    │           │   Tokenizes source into token stream
                    └─────┬─────┘
                          │
                    ┌─────┴─────┐
                    │  Parser   │   src/rexc/parser.hpp
                    │           │   Builds Abstract Syntax Tree (AST)
                    └─────┬─────┘
                          │
                    ┌─────┴─────┐
                    │ Semantic  │   src/rexc/semantic.hpp
                    │ Analysis  │   Type checking, vtable resolution
                    └─────┬─────┘
                          │
              ┌───────────┴───────────┐
              │                       │
     ┌────────┴────────┐    ┌────────┴────────┐
     │  IR Gen (v0.5+) │    │   C CodeGen     │
     │  AST → SSA IR   │    │   C11 source    │
     │  ir_gen.hpp      │    │   output        │
     └────────┬────────┘    └────────┬────────┘
              │                       │
     ┌────────┴────────┐              │
     │  IR Optimizer   │              │
     │  ir_opt.hpp     │              │
     └────────┬────────┘              │
              │                       │
     ┌────────┴────────┐    ┌────────┴────────┐
     │  Native CodeGen │    │  System C       │
     │  x86_64 / ARM64 │    │  Compiler       │
     │  machine code   │    │  (if available) │
     └────────┬────────┘    └────────┬────────┘
              │                       │
     ┌────────┴────────┐              │
     │ Executable      │              │
     │ Writer (ELF /   │              │
     │ PE / Mach-O)    │              │
     └────────┬────────┘              │
              │                       │
              └───────────┬───────────┘
                          │
                      Binary 🎉
```

---

## Source Files

| File | Purpose |
|---|---|
| `src/rexc/rexc.hpp` | Main driver — public API, pipeline orchestration |
| `src/rexc/lexer.hpp` | Tokenizer — converts source code to token stream |
| `src/rexc/token.hpp` | Token type definitions |
| `src/rexc/parser.hpp` | Parser — builds AST from tokens |
| `src/rexc/ast.hpp` | AST node type definitions |
| `src/rexc/semantic.hpp` | Semantic analyzer — types, vtables, class hierarchy |
| `src/rexc/ir.hpp` | IR structures — SSA types, values, instructions, blocks, functions |
| `src/rexc/ir_gen.hpp` | IR generator — converts AST to SSA IR |
| `src/rexc/ir_opt.hpp` | IR optimizer — constant folding, DCE, mem2reg, inlining, CFG simplification |
| `src/rexc/ir_printer.hpp` | IR printer — human-readable textual output for debugging |
| `src/rexc/codegen.hpp` | C code generator — emits C11 source from AST |
| `src/rexc/native_codegen.hpp` | Native code generator — emits x86_64/ARM64 machine code |
| `src/rexc/x86_emitter.hpp` | x86_64 instruction encoder |
| `src/rexc/arm64_emitter.hpp` | ARM64 (AArch64) instruction encoder |
| `src/rexc/elf_writer.hpp` | ELF64 executable writer (Linux) |
| `src/rexc/pe_writer.hpp` | PE32+ executable writer (Windows) |
| `src/rexc/macho_writer.hpp` | Mach-O executable writer (macOS) |
| `src/rexc/runtime.h` | C runtime library (strings, vectors, I/O) |
| `src/rexc/embedded_runtime.hpp` | Runtime.h embedded as C++ string literal |

---

## Lexer

The lexer (`src/rexc/lexer.hpp`) converts C++ source code into a stream of tokens. It handles:

- Keywords (`int`, `class`, `return`, `if`, `while`, etc.)
- Identifiers and numbers
- String and character literals (with escape sequences)
- Operators and punctuation
- Preprocessor directives (`#include`, `#define`)
- Comments (single-line `//` and multi-line `/* */`)

---

## Parser

The parser (`src/rexc/parser.hpp`) builds an Abstract Syntax Tree (AST) from the token stream. It supports:

- Function declarations and definitions
- Class/struct declarations with inheritance
- Variable declarations with initializers
- Control flow: `if`/`else`, `while`, `for`, `do-while`, `switch`/`case`
- Expressions: arithmetic, comparison, logical, assignment
- Member access (`.` and `->`)
- `new`/`delete` expressions
- `using namespace` declarations
- Template instantiation
- Scope resolution (`::`)
- Preprocessor passthrough

---

## Semantic Analysis

The semantic analyzer (`src/rexc/semantic.hpp`) performs:

- **Type resolution** — resolves named types to their declarations
- **Class hierarchy** — tracks inheritance relationships
- **VTable detection** — identifies classes that need virtual method tables
- **Method resolution** — links method calls to their implementations
- **`using namespace std`** — maps bare `cout`, `cin`, `string` identifiers to `std::` equivalents

The output is a `SemanticContext` used by both code generators.

---

## C Code Generator

The C code generator (`src/rexc/codegen.hpp`) translates the AST into valid C11 source code:

| C++ Feature | C Translation |
|---|---|
| `std::string` | `rexc_str` (heap string with length) |
| `std::vector<T*>` | `rexc_vec` (void* dynamic array) |
| `std::cout << x` | `rexc_cout_*(stream, x)` chains |
| `std::cin >> x` | `rexc_cin_*(stream, &x)` chains |
| C++ class | C struct + vtable struct |
| Inheritance | First member is base (by value) |
| Methods | Free functions with `Self* self` parameter |
| Constructors | `_ctor(Self*, ...)` functions |
| Destructors | `_dtor(Self*)` functions |
| `new T(args)` | `REXC_NEW(T); T_ctor(ptr, args)` |
| `delete ptr` | `T_dtor(ptr); REXC_DELETE(ptr)` |
| Virtual calls | `self->__vt->method(self, ...)` |
| Namespaces | Name mangling `ns__name` |
| Range-for | Index-based for loop |
| Exceptions | `rexc_throw_msg()` / longjmp stubs |
| ANSI colors | Terminal color constants (`fg_red`, `bold`, etc.) |

---

## Native Code Generator

The native code generator (`src/rexc/native_codegen.hpp`) emits machine code directly from the AST:

- **Architectures:** x86_64, ARM64 (AArch64)
- **Platforms:** Linux (ELF64), Windows (PE32+), macOS (Mach-O)
- **Calling conventions:** System V AMD64 ABI (Linux/macOS), AAPCS64 (ARM64)
- **I/O:** OS-native syscalls (no libc dependency)
- **Executable formats:** Static executables with no external dependencies

See [NATIVE_BACKEND.md](NATIVE_BACKEND.md) for details.

---

## Runtime Library

The runtime (`src/rexc/runtime.h`) is a C99/C11 header providing the fundamental types used by compiled programs:

### Types

| Runtime Type | C++ Equivalent | Description |
|---|---|---|
| `rexc_str` | `std::string` | Heap string with `data`, `len`, `cap` |
| `rexc_vec` | `std::vector<T*>` | Type-erased dynamic array |
| `rexc_ostream` | `std::ostream` | Output stream (cout, cerr) |
| `rexc_istream` | `std::istream` | Input stream (cin) |

### Features

- String operations: create, copy, concat, compare, substr, find
- Vector operations: push, pop, at, size, clear
- I/O: cout/cin chaining with `<<`/`>>` operators
- Memory: `REXC_NEW` / `REXC_DELETE` wrappers
- ANSI terminal colors: `fg_red`, `bg_blue`, `bold`, `reset`, RGB helpers
- Print helpers: `print_error`, `print_success`, `print_warning`
- Exception stub: `rexc_throw_msg()` terminates with message

The runtime is embedded in the REX binary (`src/rexc/embedded_runtime.hpp`) and extracted to `~/.rex/libs/runtime.h` at compile time.

---

## Pipeline Selection

When `compile_file()` is called, REX selects the pipeline:

1. **Native backend check** — if `has_native_backend()` returns true (x86_64/ARM64 on Linux/Windows/macOS), try native compilation first
2. **Native compilation** — if the source uses only native-supported features, produce the binary directly
3. **Fallback to C backend** — if native compilation fails (unsupported features), transparently fall back to C transpilation
4. **C++ fallback** — if no C compiler is available, fall back to system C++ compiler

This ensures REX always finds a way to compile, while preferring the most independent path.

---

## Build System

REX is built as a single compilation unit. `src/main.cpp` includes all headers:

```
main.cpp
├── utils.hpp          (string utilities, ANSI colors, logging)
├── config.hpp         (rex.toml parsing)
├── compiler.hpp       (compilation pipeline)
│   ├── rexc/rexc.hpp  (rexc driver)
│   │   ├── lexer.hpp
│   │   ├── parser.hpp
│   │   ├── semantic.hpp
│   │   ├── codegen.hpp
│   │   └── native_codegen.hpp
│   │       ├── x86_emitter.hpp
│   │       ├── arm64_emitter.hpp
│   │       ├── elf_writer.hpp
│   │       ├── pe_writer.hpp
│   │       └── macho_writer.hpp
│   └── embedded_runtime.hpp
├── cross_compile.hpp  (target triples, cross-compiler detection)
├── package_manager.hpp (library management)
├── updater.hpp        (auto-update checking)
└── setup_{unix,windows}.hpp (installers)
```

Build command:
```bash
g++ src/main.cpp -o build/rex -std=c++17 -O2
```

---

## Testing

Unit tests are in `src/rexc/test_rexc.cpp` and cover:

- Lexer tokenization
- Parser AST construction
- Semantic analysis (vtable detection)
- C code generation (simple functions, classes, I/O)
- Native backend (availability, compatibility checks)
- Cross-compilation (target parsing, flag generation)
- ELF writer structure
- x86 emitter label resolution

Run tests:
```bash
g++ src/rexc/test_rexc.cpp -std=c++17 -o build/test_rexc \
    -Wno-reorder -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-misleading-indentation
./build/test_rexc
```
