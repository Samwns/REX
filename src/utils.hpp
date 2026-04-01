#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ─── ANSI Colors (Windows 10+ supports ANSI via VT Processing) ─
#ifdef _WIN32
  #include <windows.h>
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

#ifdef _WIN32
  #define SYM_OK   "[OK]"
  #define SYM_ERR  "[ERR]"
  #define SYM_WARN "[!]"
  #define DIVIDER  "---------------------------------"
#else
  #define SYM_OK   "\xe2\x9c\x94"
  #define SYM_ERR  "\xe2\x9c\x98"
  #define SYM_WARN "!"
  #define DIVIDER  "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
#endif

// ─── Enable ANSI on Windows ────────────────────────────────────
#ifdef _WIN32
inline void enable_ansi_colors() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;
    dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    SetConsoleMode(hOut, dwMode);
    SetConsoleOutputCP(65001); // UTF-8
}
#else
inline void enable_ansi_colors() { /* no-op on Unix */ }
#endif

// ─── Log helpers ───────────────────────────────────────────────
inline void rex_info(const std::string& msg) {
    std::cout << COLOR_CYAN << "[REX] " << COLOR_RESET << msg << "\n";
}
inline void rex_ok(const std::string& msg) {
    std::cout << COLOR_GREEN << "[REX] " << SYM_OK << " " << COLOR_RESET << msg << "\n";
}
inline void rex_err(const std::string& msg) {
    std::cerr << COLOR_RED << "[REX] " << SYM_ERR << " " << COLOR_RESET << msg << "\n";
}
inline void rex_warn(const std::string& msg) {
    std::cout << COLOR_YELLOW << "[REX] " << SYM_WARN << " " << COLOR_RESET << msg << "\n";
}

// ─── String helpers ────────────────────────────────────────────
inline std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim))
        if (!token.empty()) out.push_back(token);
    return out;
}

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ─── OS detection ──────────────────────────────────────────────
inline std::string detect_os() {
#ifdef _WIN32
    return "windows";
#elif __APPLE__
    return "macos";
#else
    return "linux";
#endif
}

// ─── Check if command exists ───────────────────────────────────
inline bool command_exists(const std::string& cmd) {
#ifdef _WIN32
    std::string check = "where " + cmd + " >NUL 2>&1";
#else
    std::string check = "command -v " + cmd + " >/dev/null 2>&1";
#endif
    return system(check.c_str()) == 0;
}

// ─── Rex home directory (~/.rex) ──────────────────────────────
inline fs::path rex_home() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    fs::path p = home ? fs::path(home) / ".rex" : fs::path(".rex");
    fs::create_directories(p / "libs");
    return p;
}
