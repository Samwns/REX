// rexc_startup.hpp — REX Compiler Phase 3 (v0.7) program startup / exit.
//
// Header-only startup and exit routines.  rexc_startup() initialises the
// heap then calls main().  rexc_exit() terminates the process via the
// platform's exit syscall.
//
// Platform-specific _start symbol:
//   Linux  — The native backend will emit a _start label that sets up the
//            stack pointer, loads argc/argv, calls rexc_startup(), then
//            issues syscall exit_group (nr 231).
//   macOS  — Similar _start, syscall exit (nr 0x2000001).
//   Windows — An entry point calls rexc_startup(); exit via ExitProcess.
//
// NOTE: Compiled by the host toolchain during the REX build.
// Generated native binaries will use raw syscall stubs instead.

#pragma once

#include "rexc_alloc.hpp"
#include <cstdint>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "Unsupported platform"
#endif

namespace rexc_rt {

// ---------------------------------------------------------------------------
// rexc_exit — terminate the process
// ---------------------------------------------------------------------------

[[noreturn]] inline void rexc_exit(int code) {
#if defined(__linux__)
    // In generated code: syscall exit_group (nr 231)
    _exit(code);
#elif defined(__APPLE__)
    // In generated code: syscall exit (nr 1, with 0x2000000 class mask)
    _exit(code);
#elif defined(_WIN32)
    ExitProcess(static_cast<UINT>(code));
#endif
    // Unreachable, but keeps the compiler happy.
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// rexc_startup — called before user code
// ---------------------------------------------------------------------------

inline void rexc_startup() {
    rexc_heap_init();
}

} // namespace rexc_rt
