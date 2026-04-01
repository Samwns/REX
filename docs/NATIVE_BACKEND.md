# REX Native Backend

The native backend is rexc's built-in code generator that produces executables **directly from the AST**, without invoking any external compiler, assembler, or linker.

This makes REX **completely independent** from any external toolchain on all supported platforms.

---

## Architecture

| Component | File | Purpose |
|---|---|---|
| IR Structures | `src/rexc/ir.hpp` | SSA IR types, instructions, blocks, module |
| IR Generator | `src/rexc/ir_gen.hpp` | AST → SSA IR conversion |
| IR Optimizer | `src/rexc/ir_opt.hpp` | mem2reg, constant folding, DCE, inlining, CFG simplify |
| IR Printer | `src/rexc/ir_printer.hpp` | Human-readable IR text output |
| Native CodeGen | `src/rexc/native_codegen.hpp` | IR → machine code (x86_64 or ARM64) |
| x86 Emitter | `src/rexc/x86_emitter.hpp` | x86_64 instruction encoding |
| ARM64 Emitter | `src/rexc/arm64_emitter.hpp` | ARM64 (AArch64) instruction encoding |
| ELF Writer | `src/rexc/elf_writer.hpp` | Write static ELF64 executable (Linux) |
| PE Writer | `src/rexc/pe_writer.hpp` | Write static PE32+ executable (Windows) |
| Mach-O Writer | `src/rexc/macho_writer.hpp` | Write static Mach-O executable (macOS) |

### Pipeline

```
 AST (from parser + semantic analysis)
    |
    v
 +---------------------+
 |  IRGenerator         |   AST -> SSA IR (ir_gen.hpp)
 +----------+-----------+
            |
 +----------+-----------+
 |  IROptimizer         |   mem2reg, constant folding,
 |                      |   DCE, inlining, CFG simplify
 +----------+-----------+
            |
 +----------+-----------+
 |  NativeCodeGenerator |   IR -> machine code via
 |                      |   x86 or ARM64 emitter
 +----------+-----------+
            |
 +----------+-----------+
 |  Code Emitter        |   x86_64: X86Emitter
 |                      |   ARM64:  ARM64Emitter
 +----------+-----------+
            |
 +----------+-----------+
 |  Executable Writer   |   Linux:   ElfWriter  (ELF64)
 |                      |   Windows: PeWriter   (PE32+)
 |                      |   macOS:   MachOWriter (Mach-O)
 +----------+-----------+
            |
            v
    Static executable
    (no libc, no dynamic linking)
```

---

## Supported Features

The native backend currently supports:

| Feature | Status | Notes |
|---|---|---|
| String output (`cout << "..."`) | ✅ | Via `write` syscall |
| Integer output (`cout << int`) | ✅ | Built-in int-to-string conversion |
| `endl` / newline | ✅ | Writes `\n` |
| Integer variables | ✅ | Stack-allocated, 64-bit |
| Arithmetic (+, -, *, /) | ✅ | Full integer math |
| Comparisons (<, >, ==, !=, <=, >=) | ✅ | With SETcc instructions |
| Function declarations | ✅ | System V AMD64 ABI |
| Function calls (up to 6 args) | ✅ | Args in RDI, RSI, RDX, RCX, R8, R9 |
| Return statements | ✅ | Value in RAX |
| If/else branching | ✅ | Conditional jumps |
| While loops | ✅ | Loop with condition |
| For loops | ✅ | Init, condition, increment |
| Boolean literals | ✅ | true = 1, false = 0 |
| Character literals | ✅ | As integer values |
| Unary negation | ✅ | NEG instruction |
| Classes / structs | ✅ | Object layout, vtable, virtual dispatch |
| Templates | ✅ | Monomorphized at compile time |
| `rexc_string` operations | ✅ | Own runtime (no libstdc++) |
| `rexc_vector` | ✅ | Own runtime container |
| `rexc_map` | ✅ | Own runtime sorted-array map |
| `rexc_sort` | ✅ | Own stdlib sort algorithms |
| `rexc_unique_ptr` | ✅ | Own smart pointer |
| `rexc_function` / lambdas | ✅ | Own callable wrapper |
| `rexc_optional` | ✅ | Own optional type |
| Exception handling | ✅ | `try`/`catch`/`throw` support |
| `std::cin` input | ❌ | Falls back to C backend |

Features not supported by the native backend transparently fall back to the C transpiler pipeline.

---

## How It Works

### 1. Code Generation

`NativeCodeGenerator` walks the AST and emits platform-appropriate instructions:

**x86_64 (via X86Emitter):**
- **Functions** → System V AMD64 ABI (prologue, epilogue, parameter registers)
- **Variables** → Stack slots at `[rbp - offset]`
- **Expressions** → Register-based evaluation (result in RAX)
- **I/O** → OS-native `write` syscall
- **Program exit** → OS-native `exit` syscall

**ARM64 (via ARM64Emitter):**
- **Functions** → AAPCS64 (STP/LDP for frame, parameter registers X0-X7)
- **Variables** → Stack slots at `[FP - offset]`
- **Expressions** → Register-based evaluation (result in X0)
- **I/O** → SVC #0 (Linux) or SVC #0x80 (macOS) syscalls
- **Program exit** → OS-native `exit` syscall

### 2. Instruction Encoding

**x86_64 (X86Emitter):**

```
Registers (System V AMD64 ABI):
  RAX=0  RCX=1  RDX=2  RBX=3  RSP=4  RBP=5  RSI=6  RDI=7
  R8=8   R9=9   R10=10 R11=11 R12=12 R13=13 R14=14 R15=15

Supported instructions:
  MOV reg, imm64        MOV reg, reg         MOV reg, [rbp+disp]
  MOV [rbp+disp], reg   ADD reg, reg         SUB reg, reg
  IMUL reg, reg         NEG reg              CMP reg, reg
  CMP reg, imm32        PUSH reg             POP reg
  CALL label            RET                  JMP label
  JE label              JNE label            JGE label
  SYSCALL               NOP                  XOR reg, reg
  SETcc AL              MOVZX RAX, AL        LEA reg, [rip+disp]
```

**ARM64 (ARM64Emitter):**

```
Registers (AAPCS64):
  X0-X7   (arguments/return)    X8      (syscall number on Linux)
  X9-X15  (scratch)             X16-X17 (syscall number on macOS)
  X19-X28 (callee-saved)        X29/FP  (frame pointer)
  X30/LR  (link register)       SP      (stack pointer)

Supported instructions:
  MOVZ Xd, #imm16       MOVK Xd, #imm16     MOV Xd, Xn
  STR Xd, [Xn, #off]    LDR Xd, [Xn, #off]  ADD Xd, Xn, Xm
  SUB Xd, Xn, Xm        MUL Xd, Xn, Xm      SDIV Xd, Xn, Xm
  CMP Xn, Xm            B label              BL label
  B.EQ/NE/LT/GE/etc.    CBZ/CBNZ             SVC #0 / SVC #0x80
  STP X29, X30, [SP]!   LDP X29, X30, [SP]   RET
  CSET Xd, cond         NOP
```

Labels are assigned symbolic IDs during generation and resolved to concrete offsets in a fixup pass.

### 3. Executable Writers

The appropriate writer is selected based on the host platform:

**ElfWriter (Linux):**
- ELF header → Magic, class (64-bit), little-endian, executable type
- Program headers → One or two LOAD segments (code+rodata RE, data+bss RW)
- Code segment → Machine code starting at `_start` entry point
- Rodata → String constants placed after code
- Fully static, no libc dependency

**PeWriter (Windows):**
- DOS stub + PE signature → Standard MZ header with PE32+ magic
- COFF header → x86_64 machine type, console subsystem
- .text section → Code + rodata (Read + Execute)
- .data section → Writable data (optional)
- Static executable, no DLL imports

**MachOWriter (macOS):**
- Mach-O header → Magic, CPU type (x86_64 or ARM64)
- __TEXT segment → Code + rodata with LC_UNIXTHREAD entry point
- __DATA segment → Writable data (optional)
- Static executable, no dylib dependencies

---

## Automatic Fallback

When the native backend encounters an unsupported feature, it reports the reason and REX transparently falls back to the C transpiler:

```
1. Try native compilation
2. If native reports "classes not supported" → fall back
3. Try C backend (rexc → system C compiler)
4. If C backend fails → try system C++ compiler
```

This happens automatically — the user sees the same `rex run` output regardless of which backend was used.

---

## Platform Support

| Platform | Native Backend | Executable Format |
|---|---|---|
| x86_64 Linux | ✅ | ELF64 |
| x86_64 Windows | ✅ | PE32+ |
| x86_64 macOS | ✅ | Mach-O |
| ARM64 Linux | ✅ | ELF64 |
| ARM64 macOS (Apple Silicon) | ✅ | Mach-O |

The native backend is available on all major platforms. For unsupported platforms, REX falls back to the C transpiler backend automatically.

---

## Example

This program compiles and runs entirely through the native backend (no external compiler needed):

```cpp
#include <iostream>
using namespace std;

int add(int a, int b) {
    return a + b;
}

int main() {
    int x = 10;
    int y = 20;
    cout << "Result: ";
    cout << add(x, y);
    cout << endl;
    return 0;
}
```

```bash
$ rex run main.cpp
[REX] Standard : c++17
[REX] Compiler : rexc (native x86_64)
[REX] ✔ Compiled. Running...
─────────────────────────────────
Result: 30
─────────────────────────────────
[REX] Exit code: 0
```

The output binary is a static executable with no external dependencies — it uses OS-native syscalls directly for I/O.
