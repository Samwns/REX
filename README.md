<div align="center">

```
 ██████╗ ███████╗██╗  ██╗
 ██╔══██╗██╔════╝╚██╗██╔╝
 ██████╔╝█████╗   ╚███╔╝ 
 ██╔══██╗██╔══╝   ██╔██╗ 
 ██║  ██║███████╗██╔╝ ██╗
 ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝
```

**Compile. Run. Execute — with zero external dependencies.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![GitHub release](https://img.shields.io/github/v/release/Samwns/REX)](https://github.com/Samwns/REX/releases)
[![Build & Release](https://github.com/Samwns/REX/actions/workflows/release.yml/badge.svg)](https://github.com/Samwns/REX/actions)
[![Ko-fi](https://img.shields.io/badge/Support-Ko--fi-ff5f5f?logo=ko-fi)](https://ko-fi.com/samns)

</div>

---

## What is REX?

REX is a **fully self-contained** C++ development tool with its own built-in compiler (**rexc**) that produces executables **without requiring GCC, Clang, or any external compiler**.

- 🔥 **Native backend** — compiles C++ directly to machine code on all platforms (no external tools needed)
- 🔄 **C transpiler fallback** — for complex features, transpiles C++ → C using any available system C compiler
- 🐍 **Interpreter mode** — `rex run` executes C++ directly via tree-walking interpreter (no compilation)
- 💬 **Interactive REPL** — `rex repl` starts a live C++ prompt
- 🌍 **Cross-compilation** — build for 11+ targets including Linux, Windows, macOS, ARM, and WebAssembly
- ✅ **Supports** C++11, C++14, C++17, **C++20** (default) and C++23
- 🔍 **Detects** your OS and architecture automatically
- ⚡ **Zero-config** — `rex run main.cpp` just works
- 📦 **Package manager** — built-in registry + community packages
- 🖥️ **Cross-platform** — works on Linux, macOS and Windows
- 🌐 **Multilingual** — English and Portuguese (auto-detected or `--lang=XX`)
- 🔄 **Auto-updates** — checks for new versions automatically

No CMakeLists. No Makefile. No pain. **No external compiler required.**

> rexc generates native executables directly on x86_64 Linux (ELF64), x86_64/ARM64 macOS (Mach-O), and x86_64 Windows (PE32+) — completely independent from any external toolchain.  
> For complex features (classes, templates), REX transparently falls back to the C transpiler backend.

---

## Compiler Independence

REX's goal is to be **completely independent** from all external compilers. This is achieved through two compilation pipelines:

### 🔥 Native Backend (preferred)

rexc generates machine code directly on all supported platforms — no external tools needed:

```
 C++ source
    │
    ▼
 ┌──────────┐
 │  Lexer   │   Tokenize source code
 └────┬─────┘
      ▼
 ┌──────────┐
 │  Parser  │   Build AST (Abstract Syntax Tree)
 └────┬─────┘
      ▼
 ┌──────────┐
 │ Semantic │   Analyze types, classes, vtables
 └────┬─────┘
      ▼
 ┌──────────────────┐
 │ IR Generator     │   AST → SSA Intermediate Representation
 └────┬─────────────┘
      ▼
 ┌──────────────────┐
 │ IR Optimizer     │   Constant folding, DCE, mem2reg, inlining
 └────┬─────────────┘
      ▼
 ┌──────────────────┐
 │ Native CodeGen   │   Emit machine code (x86_64 or ARM64)
 └────┬─────────────┘
      ▼
 ┌──────────────────┐
 │ Executable Writer│   ELF64 (Linux) / PE32+ (Windows) / Mach-O (macOS)
 └────┬─────────────┘
      ▼
  Binary 🎉  (no external compiler, no linker!)
```

**Native backend supports:** string output, integer output, variables, arithmetic, function calls, if/else, while/for loops, return statements — all using OS-native syscalls directly (no libc).

| Platform | Executable Format | Status |
|---|---|---|
| x86_64 Linux | ELF64 | ✅ Full support |
| x86_64 Windows | PE32+ | ✅ Full support |
| x86_64 macOS | Mach-O | ✅ Full support |
| ARM64 Linux | ELF64 | ✅ Full support |
| ARM64 macOS (Apple Silicon) | Mach-O | ✅ Full support |

### 🔄 C Backend (automatic fallback)

For features not yet in the native backend, rexc transparently falls back to C transpilation:

```
 C++ source
    │
    ▼
 ┌──────────┐
 │  Lexer   │   Tokenize source code
 └────┬─────┘
      ▼
 ┌──────────┐
 │  Parser  │   Build AST (Abstract Syntax Tree)
 └────┬─────┘
      ▼
 ┌──────────┐
 │ Semantic │   Analyze types, classes, vtables
 └────┬─────┘
      ▼
 ┌──────────┐
 │ CodeGen  │   Emit C11 source code
 └────┬─────┘
      ▼
 ┌───────────┐
 │ C Compiler│  System C compiler (if available)
 └────┬──────┘
      ▼
  Binary 🎉
```

**C backend supports:** classes, inheritance, virtual methods, `std::string`, `std::vector`, `std::cout`, `std::cin`, `std::getline`, exceptions (stub), `new`/`delete`, namespaces, templates, ANSI terminal colors, and more.

> 📖 See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full compiler architecture.

---

## C++ Standards

REX supports multiple C++ standards. The default is **C++20**.

| Standard | Flag | Status |
|---|---|---|
| C++11 | `--std=c++11` | ✅ Supported |
| C++14 | `--std=c++14` | ✅ Supported |
| C++17 | `--std=c++17` | ✅ Supported |
| **C++20** | `--std=c++20` | ✅ **Default** |
| C++23 | `--std=c++23` | ✅ Supported |

```bash
# Use default (C++20)
rex run main.cpp

# Use a specific standard
rex run main.cpp --std=c++17

# Set in rex.toml for projects
# std = "c++20"
```

---

## Cross-Compilation

REX supports building for multiple platforms and architectures from a single host. Use `--target=TRIPLE` to cross-compile:

```bash
# Compile for ARM64 Linux
rex run main.cpp --target=aarch64-linux-gnu

# Build project for Windows (MinGW)
rex build --target=x86_64-w64-mingw32

# Or set in rex.toml:
# target = "aarch64-linux-gnu"
```

### Supported Targets

| Target Triple | Platform |
|---|---|
| `x86_64-linux-gnu` | Linux x86_64 (glibc) |
| `x86_64-linux-musl` | Linux x86_64 (musl, static) |
| `aarch64-linux-gnu` | Linux ARM64 (glibc) |
| `aarch64-linux-musl` | Linux ARM64 (musl, static) |
| `arm-linux-gnueabihf` | Linux ARM 32-bit (hard float) |
| `i686-linux-gnu` | Linux x86 32-bit |
| `x86_64-w64-mingw32` | Windows x86_64 (MinGW) |
| `i686-w64-mingw32` | Windows x86 32-bit (MinGW) |
| `x86_64-apple-macos` | macOS x86_64 |
| `arm64-apple-macos` | macOS ARM64 (Apple Silicon) |
| `wasm32-unknown-unknown` | WebAssembly 32-bit |

Run `rex targets` to see all supported targets with your host marked.

> 📖 See [docs/CROSS_COMPILATION.md](docs/CROSS_COMPILATION.md) for setup and usage details.

---

## Install

Each release binary is a **full installer** — just download and run it.

### Download & Install (recommended)

Go to [Releases](https://github.com/Samwns/REX/releases) and download for your platform:

| Platform | File |
|---|---|
| 🐧 Linux | `rex-<version>-linux-x86_64` |
| 🍎 macOS | `rex-<version>-macos-universal` |
| 🪟 Windows | `rex-<version>-windows-x86_64.exe` |

**Linux / macOS:**
```bash
chmod +x rex-*-linux-x86_64
./rex-*-linux-x86_64 install
```

**Windows:**

Double-click `rex-<version>-windows-x86_64.exe` or run from command line:
```
rex-<version>-windows-x86_64.exe install
```

### What the installer does

The built-in installer runs step-by-step with detailed logging:

1. 🖥️ **Detects your system** — OS, architecture, Linux distro
2. 📋 **Shows the MIT license** for your acceptance
3. 🔍 **Locates the binary** and checks permissions
4. ⚙️ **Detects C compiler** backend (for rexc)
5. 📦 **Installs REX** to `/usr/local/bin/rex` (Linux/macOS) or `C:\REX\rex.exe` (Windows)
6. 📁 **Creates** `~/.rex/libs/` for packages
7. 🔗 **Configures PATH** (Windows)
8. ✅ **Verifies** the installation

**Supported distros:** Ubuntu, Debian, Arch, Manjaro, Fedora, RHEL, CentOS, Rocky, openSUSE, macOS, Windows

### Update

REX checks for updates automatically (once per day). You can also update manually:

```bash
rex update                  # check for new version
```

Or download the new release and run the installer again — it detects the existing installation and updates it automatically.

### Build from source

```bash
git clone https://github.com/Samwns/REX
cd REX
mkdir -p build
g++ src/main.cpp src/interpreter/interpreter.cpp src/interpreter/builtins.cpp \
    -I src -o build/rex -std=c++20 -O2 -Wall \
    -Wno-reorder -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-misleading-indentation
sudo cp build/rex /usr/local/bin/rex
```

**Build requirements:** A C++17 compiler (only needed to build REX itself — the built REX binary is self-contained).

**Runtime requirements:** **None** — the native backend generates executables directly on all supported platforms (Linux, macOS, Windows, x86_64, ARM64). For complex C++ features (classes, templates), a system C compiler is used automatically if available.

---

## Usage

### Interpret a file directly (no compilation)

```bash
rex run main.cpp
```

REX interprets the file directly using a tree-walking interpreter — no compilation step, instant execution.

### Compile and run a single file

```bash
rex -b main.cpp
```

REX compiles with **rexc** and runs the resulting binary.

### Interactive REPL

```bash
rex repl
```

Starts an interactive C++ prompt. Variables and functions persist across lines.

### Choose a C++ standard

```bash
rex run main.cpp --std=c++20
```

### Cross-compile for another platform

```bash
rex -b main.cpp --target=aarch64-linux-gnu
```

### Start a new project

```bash
rex init my_game
cd my_game
rex build
rex exec
```

Creates:
```
my_game/
├── src/
│   └── main.cpp
├── build/
└── rex.toml
```

### Add a library

```bash
rex add nlohmann-json          # from REX built-in registry
rex add mathlib                # from rex-packages community registry
rex add nothings/stb           # from GitHub
rex add nlohmann/json@v3.11.3  # specific version
```

Libraries are stored globally at `~/.rex/libs/` and automatically included in every build.

### Search for packages

```bash
rex search            # list all available packages
rex search math       # search by name or description
```

> 📖 See [docs/PACKAGE_MANAGER.md](docs/PACKAGE_MANAGER.md) for the full guide.

---

## All Commands

| Command | Description |
|---|---|
| `rex run <file.cpp>` | Interpret a file directly (no compilation) |
| `rex -b <file.cpp> [--std=c++XX] [--target=TRIPLE]` | Compile and immediately run a single file |
| `rex repl` | Start interactive C++ interpreter |
| `rex init [name]` | Initialize a new project |
| `rex build [--target=TRIPLE]` | Build project from rex.toml |
| `rex exec` | Execute the compiled binary |
| `rex add <lib>` | Add a library (registry, community, or GitHub) |
| `rex remove <lib>` | Remove a library |
| `rex list` | List installed libraries |
| `rex registry` | Show all available libraries |
| `rex search [query]` | Search packages in all registries |
| `rex targets` | List supported cross-compilation targets |
| `rex clean` | Remove build artifacts |
| `rex update` | Check for REX updates |
| `rex install` | Run the built-in installer |
| `rex version` | Show version, compiler and system info |
| `rex help` | Show help |

---

## rex.toml

```toml
[project]
name    = "my_project"
version = "0.1.0"
std     = "c++20"       # c++11, c++14, c++17, c++20, c++23
entry   = "src/main.cpp"
output  = "build/my_project"
# target  = "aarch64-linux-gnu"   # cross-compilation target (run 'rex targets')

[build]
flags = ["-O2", "-Wall"]

[dependencies]
nlohmann-json = "latest"
# stb           = "latest"
```

| Field | Section | Description |
|---|---|---|
| `name` | `[project]` | Project name |
| `version` | `[project]` | Project version |
| `std` | `[project]` | C++ standard (c++11, c++14, c++17, c++20, c++23) |
| `entry` | `[project]` | Main source file |
| `output` | `[project]` | Output binary path |
| `target` | `[project]` | Cross-compilation target triple |
| `flags` | `[build]` | Compiler flags |
| `<lib>` | `[dependencies]` | Library name = version |

---

## Library Registry

### Built-in Libraries

| Library | Type | Description |
|---|---|---|
| `nlohmann-json` | header-only | JSON for C++ |
| `stb` | header-only | Image loading |
| `glm` | header-only | Math for OpenGL |
| `magic-enum` | header-only | Enum reflection |
| `toml11` | header-only | TOML parser |
| `argparse` | header-only | Argument parser |
| `fmt` | compiled | Formatting library |
| `spdlog` | compiled | Fast logging |
| `termcolor` | header-only | Terminal colors |

### Community Packages ([rex-packages](https://github.com/Samwns/rex-packages))

Community-maintained C++ libraries that can be installed with `rex add <name>`.

REX automatically searches the community registry when a package is not found in the built-in list.

```bash
rex add mathlib       # installs from rex-packages
rex search            # shows all available packages (built-in + community)
rex registry          # shows full registry listing
```

### GitHub Direct Install

Install any C++ library directly from GitHub:

```bash
rex add user/repo              # latest version
rex add user/repo@v1.0.0       # specific version/tag
```

> Want to publish your own library? See [rex-packages](https://github.com/Samwns/rex-packages) for details.

---

## Documentation

| Document | Description |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | rexc compiler internals, dual pipeline, AST, code generation |
| [Native Backend](docs/NATIVE_BACKEND.md) | x86_64 native code generator, ELF writer, syscall I/O |
| [Cross-Compilation](docs/CROSS_COMPILATION.md) | Target triples, cross-compiler setup, platform flags |
| [Package Manager](docs/PACKAGE_MANAGER.md) | Built-in registry, community packages, GitHub installs |

---

## Roadmap

- [x] v0.1 — Compile, run, build, package manager
- [x] v0.2 — Built-in rexc compiler (C++ → C transpiler)
- [x] v0.2.1 — `std::cin` / `std::getline` support, input/output streams
- [x] v0.2.2 — Multi C++ standard support (c++11 to c++23)
- [x] v0.2.3 — ANSI terminal colors, `termcolor` support in rexc
- [x] v0.2.4 — Auto-update checking, community package registry
- [x] v0.3 — Cross-compilation support (11 targets)
- [x] v0.3 — Native x86_64 backend (compiler independence on Linux)
- [x] v0.4 — Multi-platform native backend (Windows PE, macOS Mach-O, ARM64)
- [x] v0.5 — SSA IR layer, optimization passes (mem2reg, constant folding, DCE, inlining)
- [x] v0.6 — Classes & vtable in native backend (object layout, name mangling, virtual dispatch)
- [x] v0.7 — Own runtime library (`rexc_malloc`/`free`, `rexc_string`, `rexc_vector`, `rexc_map`)
- [x] v0.8 — Templates & own stdlib (`rexc_sort`, `rexc_unique_ptr`, `rexc_function`, `rexc_optional`)
- [x] v1.0 — Fully independent compiler (no external toolchain required)
- [x] v1.0.3 — Windows fixes (rexc_function template constructor, SBO object lifetime)
- [x] v1.0.5 — Signal handling (SIGINT/SIGTERM, Ctrl+C)
- [x] v1.0.8 — Versioned release binaries, changelog in releases, Windows setjmp/longjmp fix
- [x] v1.0.9 — Updater improvements (GitHub API asset parsing, download progress)
- [x] v1.0.10 — Tree-walking interpreter module (`src/interpreter/`)
- [x] v1.0.11 — Interpreter CLI integration (`rex run`, `rex brun`, `rex repl`), i18n (EN/PT)
- [x] v1.0.12 — Roadmap/changelog update, interpreter constructor field sync fix
- [x] v1.0.13 — C++20 as default standard, CI build fix, constructor init list fix
- [x] v1.0.14 — Fix `M_PI`/`M_E` undeclared on Windows/MSYS2 (#38)
- [x] v1.0.15 — Windows native backend crash fix, interpreter builtins and library include resolution
- [x] v1.0.16 — Interpreter builtin tests
- [x] v1.0.17 — End-to-end run/brun test suite, native x86_64 codegen bug fixes
- [x] v1.0.18 — Replace `brun` subcommand with `-b` flag

---

## Support the project ☕

If REX saved you from CMake suffering, consider buying me a coffee:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/samns)

Every donation helps keep the project alive and growing. 🙏

---

## License

[MIT](./LICENSE) — free to use, modify and distribute.

---

<div align="center">

Made with 😈 by [**Azathoth**](https://github.com/Samwns)

**Stop fighting C++. Start building.**

</div>
