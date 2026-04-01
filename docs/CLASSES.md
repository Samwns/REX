# REX Native Backend — Classes and Virtual Dispatch

This document describes how the REX native backend (rexc) handles C++ classes, inheritance, virtual methods, and object lifetime in the generated machine code.

---

## Object Memory Layout

REX uses the **Itanium C++ ABI** for class memory layout on all platforms. This ensures binary compatibility with standard C++ code on Linux and macOS.

### Layout Rules

1. **Virtual pointer (`__vptr`)** — If a class has virtual methods, the first 8 bytes of the object hold a pointer to the vtable.
2. **Base-class fields first** — Inherited fields are laid out before the derived class's own fields.
3. **Declaration order** — Fields within each class are laid out in the order they appear in the source.
4. **Natural alignment** — Each field is aligned to its natural alignment boundary (e.g., `int` at 4-byte, `double` at 8-byte boundaries).
5. **Total size** — Rounded up to the largest alignment of any field.
6. **Minimum size** — Every object occupies at least 1 byte, even if it has no fields.

### Example

```cpp
class Foo {
public:
    int x;      // offset 0, size 4
    double y;   // offset 8, size 8 (4 bytes padding after x)
};
// total_size = 16, align = 8
```

With a virtual method:

```cpp
class Animal {
public:
    int age;
    virtual void speak() {}
};
// Layout:
//   [0..7]   __vptr (8 bytes)
//   [8..11]  age (4 bytes)
//   [12..15] padding
// total_size = 16, align = 8
```

---

## VTable Structure

Each class with virtual methods gets a **vtable** — an array of function pointers stored in the `.rodata` section of the generated executable.

| Slot | Content |
|------|---------|
| 0 | Pointer to first virtual method implementation |
| 1 | Pointer to second virtual method implementation |
| ... | ... |

### Inheritance and Overriding

- Base class virtual methods occupy the first slots.
- When a derived class **overrides** a virtual method, the derived class's vtable contains a pointer to the derived implementation in the same slot.
- New virtual methods introduced by the derived class are appended after inherited slots.

```cpp
class Shape {
public:
    virtual int area() { return 0; }       // slot 0
    virtual int perimeter() { return 0; }  // slot 1
};

class Square : public Shape {
public:
    int side;
    int area() { return side * side; }     // overrides slot 0
    // perimeter() inherited from Shape    // slot 1
};
```

Square's vtable:
| Slot | Points to |
|------|-----------|
| 0 | `Square::area` (overridden) |
| 1 | `Shape::perimeter` (inherited) |

---

## Name Mangling

REX uses **Itanium ABI name mangling** for all symbols:

| Symbol type | Example | Mangled form |
|---|---|---|
| Method | `Foo::bar(int)` | `_ZN3Foo3barEi` |
| Constructor | `Foo::Foo()` | `_ZN3FooC1Ev` |
| Destructor | `Foo::~Foo()` | `_ZN3FooD1Ev` |
| Vtable | `Foo` vtable | `_ZTV3Foo` |
| Free function | `add(int, int)` | `_Z3addii` |

### Type Encodings

| C++ type | Itanium code |
|---|---|
| `void` | `v` |
| `bool` | `b` |
| `char` / `int8_t` | `a` |
| `short` / `int16_t` | `s` |
| `int` / `int32_t` | `i` |
| `long` / `int64_t` | `l` |
| `float` | `f` |
| `double` | `d` |
| `void*` | `Pv` |

---

## Virtual Method Calls

Virtual method calls use indirect dispatch through the vtable:

```
// obj->method(args...)
1. Load __vptr from obj (offset 0)
2. Load function pointer from vtable[slot_index]
3. Call function pointer with (obj, args...)
```

In IR form:
```
%vptr_slot = getelementptr ptr, ptr %obj, i64 0
%vptr      = load ptr, ptr %vptr_slot
%fn_slot   = getelementptr ptr, ptr %vptr, i64 <slot>
%fn_ptr    = load ptr, ptr %fn_slot
%result    = call %fn_ptr(%obj, args...)
```

Non-virtual method calls use direct dispatch (no vtable lookup).

---

## Constructor / Destructor

### Constructor

1. Allocate memory for the object (stack or heap via `new`)
2. Initialize `__vptr` to point to the class's vtable
3. Initialize base-class fields (call base constructor if present)
4. Initialize own fields
5. Execute constructor body

### Destructor

1. Execute destructor body
2. Destroy own fields (in reverse declaration order)
3. Destroy base-class fields

### `new` / `delete`

- `new T(args...)` → allocate `sizeof(T)` bytes + call constructor
- `delete p` → call destructor + free memory

---

## Implementation Files

| File | Purpose |
|---|---|
| `src/rexc/object_layout.hpp` | Compute class memory layouts (field offsets, vtable slots) |
| `src/rexc/mangler.hpp` | Itanium ABI name mangling |
| `src/rexc/ir_gen.hpp` | IR generation for class operations |
| `src/rexc/native_codegen.hpp` | Machine code emission for class support |

---

## Platform Support

Class support works on all five target platforms:

| Platform | ABI | Status |
|---|---|---|
| x86_64 Linux | Itanium / System V | ✅ |
| x86_64 Windows | Itanium (MSVC-compatible layout) | ✅ |
| x86_64 macOS | Itanium / System V | ✅ |
| ARM64 Linux | Itanium / AAPCS64 | ✅ |
| ARM64 macOS | Itanium / AAPCS64 | ✅ |
