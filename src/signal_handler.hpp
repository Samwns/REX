#pragma once
/*
 * signal_handler.hpp  –  Signal handling for REX
 *
 * Provides:
 *   - SIGINT (Ctrl+C) / SIGTERM interception
 *   - Exit-code decoding for child processes run via system()
 *   - Signal detection from shell-convention exit codes (128+N)
 */

#include <csignal>
#include <cstdlib>
#include <atomic>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace rex_signal {

// Atomic flag so the signal handler (async-signal-safe context) can
// communicate with the main thread without data races.
inline std::atomic<bool>& interrupted() {
    static std::atomic<bool> flag{false};
    return flag;
}

// ─── Signal handler (must be async-signal-safe) ─────────────
// Signal handlers set the atomic flag and re-raise. Printing is
// done by the caller after system() returns, not in the handler.
inline void sigint_handler(int sig) {
    interrupted().store(true);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

inline void sigterm_handler(int sig) {
    interrupted().store(true);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// ─── Install signal handlers ────────────────────────────────
inline void install_signal_handlers() {
    std::signal(SIGINT,  sigint_handler);
    std::signal(SIGTERM, sigterm_handler);
}

// ─── Check if an exit code indicates the child was interrupted ──
// On POSIX, system() runs commands via /bin/sh, which converts
// signal exits to 128+signal_number. Direct WIFSIGNALED only
// triggers if the shell itself was killed.
inline bool was_signaled(int exit_code) {
#ifdef _WIN32
    // On Windows, Ctrl+C typically yields STATUS_CONTROL_C_EXIT (0xC000013A)
    // or negative values
    return exit_code < 0 || exit_code == static_cast<int>(0xC000013A);
#else
    // First check if the shell itself was killed by a signal
    if (WIFSIGNALED(exit_code))
        return true;
    // system() runs via /bin/sh which exits with 128+N on signal
    if (WIFEXITED(exit_code)) {
        int code = WEXITSTATUS(exit_code);
        return code > 128 && code <= 128 + 64;
    }
    return false;
#endif
}

inline int signal_number(int exit_code) {
#ifdef _WIN32
    return 2; // SIGINT equivalent
#else
    if (WIFSIGNALED(exit_code))
        return WTERMSIG(exit_code);
    if (WIFEXITED(exit_code)) {
        int code = WEXITSTATUS(exit_code);
        if (code > 128 && code <= 128 + 64)
            return code - 128;
    }
    return 0;
#endif
}

// ─── Decode exit status from system() ───────────────────────
inline int decode_exit_code(int raw) {
#ifdef _WIN32
    return raw;
#else
    if (WIFEXITED(raw))
        return WEXITSTATUS(raw);
    if (WIFSIGNALED(raw))
        return 128 + WTERMSIG(raw);
    return raw;
#endif
}

} // namespace rex_signal
