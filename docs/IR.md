# REX IR — Intermediate Representation

This document describes the SSA-based Intermediate Representation (IR) introduced
in rexc v0.5.  The IR sits between the frontend (lexer/parser/semantic) and the
backend (x86/ARM64 emitters + executable writers).

---

## Pipeline

```
C++ source
    │
    ▼
┌───────────┐
│  Lexer    │   src/rexc/lexer.hpp
└─────┬─────┘
      ▼
┌───────────┐
│  Parser   │   src/rexc/parser.hpp
└─────┬─────┘
      ▼
┌───────────┐
│ Semantic  │   src/rexc/semantic.hpp
└─────┬─────┘
      ▼
┌───────────┐
│ IR Gen    │   src/rexc/ir_gen.hpp     (NEW in v0.5)
│ AST → SSA │
└─────┬─────┘
      ▼
┌───────────┐
│ IR Opt    │   src/rexc/ir_opt.hpp     (NEW in v0.5)
│ Passes    │
└─────┬─────┘
      ▼
┌───────────┐
│ Code Gen  │   x86_64 / ARM64 emitter
└─────┬─────┘
      ▼
┌───────────┐
│ Writer    │   ELF64 / PE32+ / Mach-O
└─────┬─────┘
      ▼
  Binary 🎉
```

---

## IR Types

| IRType   | Textual | Size (bytes) |
|----------|---------|--------------|
| Void     | `void`  | 0            |
| Bool     | `i1`    | 1            |
| Int8     | `i8`    | 1            |
| Int16    | `i16`   | 2            |
| Int32    | `i32`   | 4            |
| Int64    | `i64`   | 8            |
| Float32  | `f32`   | 4            |
| Float64  | `f64`   | 8            |
| Ptr      | `ptr`   | 8            |

---

## SSA Values

Every value in the IR is assigned exactly once (Static Single Assignment):

```
%1 = const i32 42
%2 = add i32 %1, %1
```

Values are numbered sequentially within each function starting at 1.

---

## IR Operations (Opcodes)

### Arithmetic
| Op  | Description |
|-----|-------------|
| Add | Integer/float addition |
| Sub | Subtraction |
| Mul | Multiplication |
| Div | Division |
| Mod | Modulo |
| Neg | Unary negation |

### Logic / Bitwise
| Op  | Description |
|-----|-------------|
| And | Bitwise AND |
| Or  | Bitwise OR |
| Xor | Bitwise XOR |
| Not | Bitwise NOT |
| Shl | Shift left |
| Shr | Shift right |

### Comparison
| Op    | Description |
|-------|-------------|
| CmpEq | Equal |
| CmpNe | Not equal |
| CmpLt | Less than |
| CmpLe | Less or equal |
| CmpGt | Greater than |
| CmpGe | Greater or equal |

### Memory
| Op             | Description |
|----------------|-------------|
| Alloca         | Allocate stack slot, returns pointer |
| Load           | Load from pointer |
| Store          | Store value to pointer |
| GetElementPtr  | Compute offset in struct/array |

### Control Flow
| Op      | Description |
|---------|-------------|
| Br      | Unconditional branch |
| BrCond  | Conditional branch (true/false labels) |
| Ret     | Return from function |
| Call    | Function call |
| Phi     | SSA phi node (merge point) |

### Conversion / Constants
| Op    | Description |
|-------|-------------|
| Cast  | Type conversion |
| Const | Constant value (integer or float) |

---

## Textual Format

The IR printer (`src/rexc/ir_printer.hpp`) produces human-readable output:

```
function main() -> i32:
  entry:
    %1 = alloca ptr
    %2 = const i32 5
    store i32 %2, ptr %1
    %3 = load i32, ptr %1
    ret i32 %3
```

Global strings appear as:

```
@str.0 = "Hello, world!\n"
```

---

## Optimization Passes

The IR optimizer (`src/rexc/ir_opt.hpp`) runs the following passes in order:

1. **mem2reg** — Promotes single-block alloca/load/store to SSA registers
2. **constant_fold** — Evaluates constant expressions at compile time  
   (e.g. `Add(Const(2), Const(3))` → `Const(5)`)
3. **dce** — Dead Code Elimination: removes unused instructions  
   (never removes Store, Call, Ret, Br, BrCond)
4. **cfg_simplify** — Eliminates empty blocks, merges linear chains
5. **inline** — Inlines small (≤ 10 instructions) single-call, single-block functions
6. **constant_fold** — Second pass (inlining may expose new constants)
7. **dce** — Second pass (removes newly dead code)

---

## Source Files

| File | Purpose |
|------|---------|
| `src/rexc/ir.hpp` | IR structure definitions (IRType, IRValue, IROp, IRInstr, IRBlock, IRFunction, IRModule) |
| `src/rexc/ir_gen.hpp` | IRGenerator: AST → SSA IR (alloca-load-store lowering) |
| `src/rexc/ir_opt.hpp` | IROptimizer: optimization passes |
| `src/rexc/ir_printer.hpp` | IRPrinter: human-readable textual output |

---

## Variable Lowering (alloca-load-store)

Every local variable follows the alloca-load-store pattern:

```
int x = 42;     →   %ptr = alloca i32
                     %c   = const i32 42
                     store i32 %c, ptr %ptr

... = x;         →   %v   = load i32, ptr %ptr

x = 10;          →   %c2  = const i32 10
                     store i32 %c2, ptr %ptr
```

The `mem2reg` pass promotes simple allocas to SSA registers, removing unnecessary memory traffic.

---

## Future Work (v0.6+)

- Classes and vtable support in IR
- Name mangling (Itanium ABI)
- IR-based code emission replacing direct AST-to-machine-code path
- Graph coloring register allocator
