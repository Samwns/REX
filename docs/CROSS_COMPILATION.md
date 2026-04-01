# REX Cross-Compilation Guide

REX supports building C++ programs for multiple platforms and architectures from a single host machine.

---

## Quick Start

```bash
# Compile for ARM64 Linux
rex run main.cpp --target=aarch64-linux-gnu

# Build project for Windows
rex build --target=x86_64-w64-mingw32

# List all supported targets
rex targets
```

---

## Supported Targets

| Target Triple | Platform | Notes |
|---|---|---|
| `x86_64-linux-gnu` | Linux x86_64 (glibc) | Default on x86_64 Linux hosts |
| `x86_64-linux-musl` | Linux x86_64 (musl) | Fully static binaries |
| `aarch64-linux-gnu` | Linux ARM64 (glibc) | Raspberry Pi 4+, ARM servers |
| `aarch64-linux-musl` | Linux ARM64 (musl) | Fully static ARM64 |
| `arm-linux-gnueabihf` | Linux ARM 32-bit | Raspberry Pi 2/3, hard float |
| `i686-linux-gnu` | Linux x86 32-bit | Legacy 32-bit x86 |
| `x86_64-w64-mingw32` | Windows x86_64 | Requires MinGW cross-compiler |
| `i686-w64-mingw32` | Windows x86 32-bit | Requires MinGW cross-compiler |
| `x86_64-apple-macos` | macOS x86_64 | Requires macOS SDK or osxcross |
| `arm64-apple-macos` | macOS ARM64 | Apple Silicon (M1/M2/M3) |
| `wasm32-unknown-unknown` | WebAssembly 32-bit | Experimental |

Run `rex targets` to see all targets with your host architecture highlighted.

---

## Usage

### Single File

```bash
rex run main.cpp --target=aarch64-linux-gnu
```

When cross-compiling, REX compiles the binary but does not attempt to run it (since it's for a different platform):

```
[REX] ✔ Cross-compiled: build/rex_tmp
[REX] Binary built for aarch64-linux-gnu (cannot run on host)
```

### Project (rex.toml)

Set the target in your project configuration:

```toml
[project]
name    = "my_app"
version = "1.0.0"
std     = "c++17"
entry   = "src/main.cpp"
output  = "build/my_app"
target  = "aarch64-linux-gnu"
```

Or override from the command line:

```bash
rex build --target=x86_64-w64-mingw32
```

The `--target` flag overrides the `target` field in rex.toml.

---

## Target Triple Format

A target triple has the format: `<arch>-<os>-<abi>`

| Component | Values | Description |
|---|---|---|
| **arch** | `x86_64`, `i686`, `aarch64`, `arm`, `wasm32` | CPU architecture |
| **os** | `linux`, `windows`, `macos`, `unknown` | Operating system |
| **abi** | `gnu`, `musl`, `mingw32`, `msvc`, `gnueabihf`, `unknown` | ABI / C library |

Examples:
- `x86_64-linux-gnu` → 64-bit Linux with glibc
- `aarch64-linux-musl` → 64-bit ARM Linux with musl (static linking)
- `x86_64-w64-mingw32` → 64-bit Windows via MinGW

---

## Cross-Compiler Setup

REX auto-detects cross-compilers by looking for prefixed executables:

### Linux → Linux ARM64

```bash
# Ubuntu/Debian
sudo apt install gcc-aarch64-linux-gnu

# Usage
rex run main.cpp --target=aarch64-linux-gnu
# REX detects: aarch64-linux-gnu-gcc
```

### Linux → Linux ARM 32-bit

```bash
# Ubuntu/Debian
sudo apt install gcc-arm-linux-gnueabihf

# Usage
rex run main.cpp --target=arm-linux-gnueabihf
```

### Linux → Windows (MinGW)

```bash
# Ubuntu/Debian
sudo apt install gcc-mingw-w64-x86-64

# Usage
rex run main.cpp --target=x86_64-w64-mingw32
```

### Linux → Linux x86 32-bit

```bash
# Ubuntu/Debian
sudo apt install gcc-multilib

# Usage
rex run main.cpp --target=i686-linux-gnu
```

### Linux → Linux (musl, static)

```bash
# Install musl-tools
sudo apt install musl-tools

# Usage
rex run main.cpp --target=x86_64-linux-musl
```

---

## Cross-Compiler Detection

REX searches for cross-compilers in this order:

1. **Prefixed compiler** — `<triple>-gcc` or `<triple>-cc` (e.g., `aarch64-linux-gnu-gcc`)
2. **Architecture-specific** — variant prefix patterns (e.g., `arm-linux-gnueabihf-gcc`)
3. **Error message** — if no cross-compiler found, REX reports the expected package name

Example detection output:
```
[REX] Target   : aarch64-linux-gnu
[REX] Compiler : rexc (backend: aarch64-linux-gnu-gcc)
```

---

## Platform-Specific Flags

REX automatically adds platform-specific flags based on the target:

| Target OS | Flags |
|---|---|
| Linux (gnu) | `-target <triple>` |
| Linux (musl) | `-static` |
| Windows (MinGW) | `-target <triple>` (generates .exe) |
| macOS | `-target <triple>` |

### Binary Extensions

| Target OS | Extension |
|---|---|
| Linux | (none) |
| macOS | (none) |
| Windows | `.exe` |
| WebAssembly | `.wasm` |

---

## How Cross-Compilation Works with rexc

Cross-compilation uses the C transpiler backend when targeting a different architecture/OS than the host:

```
C++ source → rexc (Lexer → Parser → Semantic → C CodeGen) → C source
    → cross-compiler (e.g. aarch64-linux-gnu-gcc) → target binary
```

When the target matches the host platform, the native backend is used instead for zero-dependency compilation.

---

## Multi-Target Builds

You can build for multiple targets by running `rex build` multiple times:

```bash
# Build for host
rex build

# Build for ARM64
rex build --target=aarch64-linux-gnu

# Build for Windows
rex build --target=x86_64-w64-mingw32
```

Each target produces a separate binary with the appropriate extension.

---

## Cross-Platform Support Analysis

REX's cross-compilation architecture enables running **the same C++ code** on multiple platforms. Here is the current support status:

### Compilation Pipeline per Platform

| Host → Target | Compiler Chain | Status |
|---|---|---|
| Linux x86_64 → Linux x86_64 | rexc native (ELF64 direct) | ✔ Full support |
| Linux x86_64 → Linux ARM64 | rexc C backend → cross-compiler | ✔ Full support |
| Linux x86_64 → Linux ARM32 | rexc C backend → cross-compiler | ✔ Full support |
| Linux x86_64 → Windows x64 | rexc C backend → cross-compiler | ✔ Full support |
| Linux x86_64 → macOS x64/ARM | rexc C backend → cross-compiler | ⚠ Requires osxcross SDK |
| Linux x86_64 → WebAssembly | rexc C backend → cross-compiler | ⚠ Experimental |
| Windows → Windows | rexc native (PE32+ direct) | ✔ Full support |
| Windows → Linux | rexc C backend → cross-compiler | ⚠ Requires cross-tools |
| macOS x64 → macOS x64 | rexc native (Mach-O direct) | ✔ Full support |
| macOS ARM → macOS ARM | rexc native (Mach-O direct) | ✔ Full support |
| macOS → Linux | rexc C backend → cross-compiler | ⚠ Requires cross-tools |
| ARM64 Linux → ARM64 Linux | rexc native (ELF64 direct) | ✔ Full support |

### Key Architecture Details

1. **Fallback chain**: REX tries native backend → rexc C backend → system C++ compiler. This ensures maximum compatibility even when rexc doesn't support all C++ features.

2. **Static linking**: For distribution, REX produces fully static binaries with no external dependencies on the native backend. The C backend adds appropriate static linking flags when available.

3. **Cross-compiler detection**: REX auto-detects prefixed compilers (`{triple}-gcc`) and compilers that support `--target=` flag for cross-compilation.

4. **Native backend on all platforms**: The native x86_64/ARM64 codegen supports ELF (Linux), PE (Windows), and Mach-O (macOS) formats. For cross-compilation to a different architecture, the C transpiler backend is used.

### Recommended Setup for Multi-Platform Development

```bash
# On Ubuntu/Debian, install all common cross-compilers:
sudo apt install gcc-aarch64-linux-gnu    # → ARM64 Linux
sudo apt install gcc-arm-linux-gnueabihf  # → ARM32 Linux
sudo apt install gcc-mingw-w64-x86-64    # → Windows x64
sudo apt install gcc-i686-linux-gnu      # → x86 32-bit Linux
sudo apt install musl-tools              # → musl static builds

# Verify available targets:
rex targets
```
