#pragma once
/*
 * rexc_except.hpp  –  REX Compiler Phase 4 exception handling.
 *
 * Implements a setjmp/longjmp-based exception mechanism.  The compiler
 * emits setjmp at try-block entry and rexc_throw at throw-expressions.
 * Each ExceptionFrame is linked into a thread-local stack so nested
 * try/catch blocks work correctly.
 */

#include <csetjmp>
#include <cstddef>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rexc_rt {

// ---------------------------------------------------------------------------
// ExceptionFrame — one per try-block, linked in a stack
// ---------------------------------------------------------------------------

struct alignas(16) ExceptionFrame {
    jmp_buf     env;            // setjmp/longjmp state (needs 16-byte alignment for XMM save on Windows x64)
    ExceptionFrame* prev;       // enclosing frame (or nullptr at top level)
    void*       exception_obj;  // pointer to the thrown object
    const char* type_name;      // type identifier string for catch matching
};

// ---------------------------------------------------------------------------
// Thread-local frame stack
// ---------------------------------------------------------------------------

inline thread_local ExceptionFrame* current_frame = nullptr;

// ---------------------------------------------------------------------------
// Push / pop helpers (used by compiler-generated try blocks)
// ---------------------------------------------------------------------------

inline void rexc_push_frame(ExceptionFrame* frame) {
    frame->prev = current_frame;
    frame->exception_obj = nullptr;
    frame->type_name = nullptr;
    current_frame = frame;
}

inline void rexc_pop_frame() {
    if (current_frame) {
        current_frame = current_frame->prev;
    }
}

// ---------------------------------------------------------------------------
// rexc_throw — jump to nearest catch frame
//
// On Windows x64 (MinGW-w64), longjmp triggers SEH-based stack unwinding
// via RtlUnwindEx.  When longjmp lives inside an inline [[noreturn]]
// function, the compiler may omit the .pdata/.xdata unwind metadata the
// unwinder needs, causing a SIGSEGV.  REXC_THROW is a macro that keeps
// the longjmp call in the *caller's* stack frame (the same function that
// called setjmp), side-stepping the issue entirely.  This is the same
// pattern used by Lua (LUAI_THROW) and other portable C exception libs.
// ---------------------------------------------------------------------------

// Prepare the exception state and pop the current frame.
// Returns the target frame (caller must longjmp to it) or nullptr.
inline ExceptionFrame* rexc_setup_throw_(void* obj, const char* type_name) {
    if (current_frame) {
        current_frame->exception_obj = obj;
        current_frame->type_name = type_name;
        ExceptionFrame* target = current_frame;
        current_frame = target->prev;
        return target;
    }
    return nullptr;
}

// Abort when no enclosing try-block exists.
[[noreturn]] inline void rexc_abort_no_handler_() {
#if defined(__linux__) || defined(__APPLE__)
    const char msg[] = "rexc_rt: unhandled exception\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    _exit(134);
#elif defined(_WIN32)
    ExitProcess(134);
#else
    __builtin_trap();
#endif
}

// Preferred throw entry-point — keeps longjmp in the caller's frame.
#define REXC_THROW(obj, type_name) do {                                    \
        rexc_rt::ExceptionFrame* tgt_ =                                    \
            rexc_rt::rexc_setup_throw_((obj), (type_name));                \
        if (tgt_) longjmp(tgt_->env, 1);                                   \
        rexc_rt::rexc_abort_no_handler_();                                 \
    } while (0)

// Legacy function form — still usable on platforms without SEH issues.
[[noreturn]] inline void rexc_throw(void* obj, const char* type_name) {
    ExceptionFrame* tgt = rexc_setup_throw_(obj, type_name);
    if (tgt) longjmp(tgt->env, 1);
    rexc_abort_no_handler_();
}

// ---------------------------------------------------------------------------
// rexc_get_exception / rexc_get_type_name — accessors for catch blocks
// ---------------------------------------------------------------------------

inline void* rexc_get_exception(ExceptionFrame* frame) {
    return frame ? frame->exception_obj : nullptr;
}

inline const char* rexc_get_type_name(ExceptionFrame* frame) {
    return frame ? frame->type_name : nullptr;
}

} // namespace rexc_rt
