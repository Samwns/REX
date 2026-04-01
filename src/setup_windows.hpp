#pragma once

#ifdef _WIN32

#include <iostream>
#include <string>
#include <windows.h>
#include <conio.h>
#include <filesystem>
#include <cstdlib>
#include "utils.hpp"

namespace fs = std::filesystem;

// ─── Installer ANSI helpers (match Unix installer style) ──────
#define INS_RESET   "\033[0m"
#define INS_BOLD    "\033[1m"
#define INS_DIM     "\033[2m"
#define INS_RED     "\033[31m"
#define INS_GREEN   "\033[32m"
#define INS_YELLOW  "\033[33m"
#define INS_CYAN    "\033[36m"
#define INS_WHITE   "\033[37m"
#define INS_BG_BLUE "\033[44m"

// ─── Lowercase string ─────────────────────────────────────────
static std::string str_lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ─── Check if a dir is already in HKCU PATH ──────────────────
static bool is_in_user_path(const std::string& dir) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    char buf[32767] = {};
    DWORD bufSize = sizeof(buf);
    DWORD type    = REG_EXPAND_SZ;
    RegQueryValueExA(hKey, "Path", nullptr, &type, (LPBYTE)buf, &bufSize);
    RegCloseKey(hKey);

    return str_lower(std::string(buf)).find(str_lower(dir)) != std::string::npos;
}

// ─── Write dir into HKCU PATH permanently ────────────────────
static bool add_to_user_path(const std::string& dir) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    char buf[32767] = {};
    DWORD bufSize = sizeof(buf);
    DWORD type    = REG_EXPAND_SZ;
    RegQueryValueExA(hKey, "Path", nullptr, &type, (LPBYTE)buf, &bufSize);

    std::string current(buf);
    if (str_lower(current).find(str_lower(dir)) != std::string::npos) {
        RegCloseKey(hKey);
        return true; // already there
    }

    if (!current.empty() && current.back() != ';') current += ";";
    current += dir;

    LONG result = RegSetValueExA(hKey, "Path", 0, REG_EXPAND_SZ,
        (const BYTE*)current.c_str(), (DWORD)current.size() + 1);
    RegCloseKey(hKey);

    // Broadcast so Explorer and new terminals pick it up immediately
    DWORD_PTR dwResult;
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)"Environment",
                        SMTO_ABORTIFHUNG, 2000, &dwResult);

    return result == ERROR_SUCCESS;
}

// ─── Detect double-click ──────────────────────────────────────
static bool is_double_clicked() {
    HWND con = GetConsoleWindow();
    if (!con) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(con, &pid);
    return pid == GetCurrentProcessId();
}

// ─── Wait for any key ─────────────────────────────────────────
static void wait_key(const std::string& msg = "Press any key to close...") {
    std::cout << "\n  " << msg << "\n";
    _getch();
}

// ─── Step printer with total count ────────────────────────────
static void setup_step(int n, int total, const std::string& label,
                        const std::string& value = "", bool ok = true) {
    std::string status = ok
        ? (std::string(INS_GREEN) + "[OK]" + INS_RESET)
        : (std::string(INS_RED)   + "[FAIL]" + INS_RESET);

    std::cout << "  " << INS_DIM << "[" << n << "/" << total << "]" << INS_RESET
              << " " << label;
    if (!value.empty()) std::cout << " " << INS_CYAN << value << INS_RESET;
    std::cout << "  " << status << "\n";
    Sleep(400);
}

// ─── Log helper ───────────────────────────────────────────────
static void setup_log(const std::string& msg) {
    std::cout << "  " << INS_DIM << "->" << INS_RESET << " " << msg << "\n";
}

// ─── Show license ─────────────────────────────────────────────
static void show_windows_license() {
    std::cout << "\n";
    std::cout << "  " << INS_BG_BLUE << INS_WHITE << INS_BOLD << " LICENSE " << INS_RESET << "\n\n";
    std::cout << "  " << INS_BOLD << "MIT License" << INS_RESET << "\n";
    std::cout << "  Copyright (c) 2025 Azathoth (github.com/Samwns)\n\n";
    std::cout << "  " << INS_DIM << "Permission is hereby granted, free of charge, to any person\n";
    std::cout << "  obtaining a copy of this software and associated documentation\n";
    std::cout << "  files (the \"Software\"), to deal in the Software without\n";
    std::cout << "  restriction, including without limitation the rights to use,\n";
    std::cout << "  copy, modify, merge, publish, distribute, sublicense, and/or\n";
    std::cout << "  sell copies of the Software, and to permit persons to whom the\n";
    std::cout << "  Software is furnished to do so, subject to the following\n";
    std::cout << "  conditions:\n\n";
    std::cout << "  The above copyright notice and this permission notice shall be\n";
    std::cout << "  included in all copies or substantial portions of the Software.\n\n";
    std::cout << "  THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n";
    std::cout << "  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES\n";
    std::cout << "  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND\n";
    std::cout << "  NONINFRINGEMENT." << INS_RESET << "\n\n";
}

// ─── Confirm prompt (Windows) ─────────────────────────────────
static bool win_confirm(const std::string& msg) {
    std::cout << "  " << msg << " [Y/n] ";
    int ch = _getch();
    if (ch == 'n' || ch == 'N') {
        std::cout << "N\n";
        return false;
    }
    std::cout << "Y\n";
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  WINDOWS INSTALLER — Full TUI
// ═══════════════════════════════════════════════════════════════
static void run_windows_setup(const std::string& rex_version) {
    const int TOTAL_STEPS = 9;
    enable_ansi_colors();
    system("cls");

    // ── Banner ────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << INS_CYAN;
    std::cout << "  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97  \xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\n";
    std::cout << "  \xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\xe2\x95\x9a\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d\n";
    std::cout << "  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97   \xe2\x95\x9a\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d \n";
    std::cout << "  \xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d   \xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97 \n";
    std::cout << "  \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91  \xe2\x96\x88\xe2\x96\x88\xe2\x95\x91\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\xe2\x96\x88\xe2\x96\x88\xe2\x95\x94\xe2\x95\x9d \xe2\x96\x88\xe2\x96\x88\xe2\x95\x97\n";
    std::cout << "  \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d  \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d  \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d\n";
    std::cout << INS_RESET << "\n";
    std::cout << "  " << INS_BOLD << "REX v" << rex_version << INS_RESET
              << " - Compile. Run. Execute.\n";
    std::cout << "  " << INS_DIM << "by Azathoth  |  github.com/Samwns/REX" << INS_RESET << "\n";
    std::cout << "\n";

    Sleep(500);

    // ── Detect if already installed ──────────────────────────
    std::string install_dir = "C:\\REX";
    std::string install_exe = install_dir + "\\rex.exe";
    bool already_installed = fs::exists(install_exe);

    if (already_installed) {
        std::cout << "  " << INS_DIM << "+==========================================+" << INS_RESET << "\n";
        std::cout << "  " << INS_DIM << "|" << INS_RESET << "   " << INS_YELLOW << INS_BOLD
                  << "UPDATE / REINSTALL" << INS_RESET
                  << "                      " << INS_DIM << "|" << INS_RESET << "\n";
        std::cout << "  " << INS_DIM << "+==========================================+" << INS_RESET << "\n\n";
        setup_log("REX is already installed at: " + install_exe);
        setup_log("Updating to v" + rex_version + "...\n");
    } else {
        std::cout << "  " << INS_DIM << "+==========================================+" << INS_RESET << "\n";
        std::cout << "  " << INS_DIM << "|" << INS_RESET << "   " << INS_GREEN << INS_BOLD
                  << "REX INSTALLER" << INS_RESET << " - Windows"
                  << "              " << INS_DIM << "|" << INS_RESET << "\n";
        std::cout << "  " << INS_DIM << "+==========================================+" << INS_RESET << "\n\n";
    }

    Sleep(300);

    // ── Show License ──────────────────────────────────────────
    show_windows_license();

    if (!win_confirm("Do you accept the MIT license and wish to continue?")) {
        std::cout << "\n  Installation cancelled.\n";
        wait_key();
        return;
    }

    std::cout << "\n  " << INS_DIM << "------------------------------------------" << INS_RESET << "\n";
    std::cout << "  " << INS_BOLD << "Installing REX v" << rex_version << "..." << INS_RESET << "\n";
    std::cout << "  " << INS_DIM << "------------------------------------------" << INS_RESET << "\n\n";

    Sleep(300);

    bool all_ok = true;

    // ── Step 1: Locate self ───────────────────────────────────
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, selfPath, MAX_PATH);
    std::string exePath(selfPath);
    setup_step(1, TOTAL_STEPS, "Located rex.exe:      ", exePath);

    // ── Step 2: Detect OS ────────────────────────────────────
    BOOL is64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &is64);
    std::string arch = is64 ? "x64" : "x86";
    // On 64-bit native process, IsWow64 returns FALSE, but the binary is already 64-bit
    #ifdef _WIN64
    arch = "x64";
    #endif
    setup_step(2, TOTAL_STEPS, "Detected OS:          ", "Windows " + arch);

    // ── Step 3: Detect compiler ──────────────────────────────
    bool has_cc    = (system("where gcc >NUL 2>&1") == 0) || (system("where cc >NUL 2>&1") == 0);
    bool has_clang = (system("where clang >NUL 2>&1") == 0);
    std::string compiler = has_clang ? "clang" : (has_cc ? "gcc" : "NOT FOUND");
    bool comp_ok = (compiler != "NOT FOUND");

    bool has_gpp     = (system("where g++ >NUL 2>&1")     == 0);
    bool has_clangpp = (system("where clang++ >NUL 2>&1") == 0);
    if (!comp_ok && (has_gpp || has_clangpp)) {
        compiler = has_clangpp ? "clang++ (fallback)" : "g++ (fallback)";
        comp_ok = true;
    }

    if (comp_ok) {
        setup_step(3, TOTAL_STEPS, "C compiler backend:   ", "rexc (" + compiler + ")");
    } else {
        setup_step(3, TOTAL_STEPS, "C compiler backend:   ", "NOT FOUND", false);
    }

    // ── Step 4: Create C:\REX ────────────────────────────────
    bool dir_ok = true;
    try {
        fs::create_directories(install_dir);
    } catch (...) {
        dir_ok = false;
    }

    if (already_installed) {
        setup_step(4, TOTAL_STEPS, "Install folder:       ", install_dir + " (exists)");
    } else {
        setup_step(4, TOTAL_STEPS, "Created folder:       ", install_dir, dir_ok);
    }

    if (!dir_ok) {
        std::cout << "\n  " << INS_RED << "[!] Could not create C:\\REX" << INS_RESET << "\n";
        std::cout << "      " << INS_DIM << "Try running as Administrator." << INS_RESET << "\n\n";
        all_ok = false;
    }

    // ── Step 5: Copy rex.exe ─────────────────────────────────
    bool copy_ok = false;
    if (dir_ok) {
        try {
            fs::copy_file(exePath, install_exe,
                          fs::copy_options::overwrite_existing);
            copy_ok = true;
        } catch (...) {
            copy_ok = false;
        }
    }

    if (already_installed) {
        setup_step(5, TOTAL_STEPS, "Updated rex.exe:      ", install_exe, copy_ok);
    } else {
        setup_step(5, TOTAL_STEPS, "Installed rex.exe:    ", install_exe, copy_ok);
    }

    if (!copy_ok && dir_ok) {
        std::cout << "\n  " << INS_RED << "[!] Could not copy rex.exe. Try running as Administrator." << INS_RESET << "\n\n";
        all_ok = false;
    }

    // ── Step 6: Create ~/.rex/libs ───────────────────────────
    const char* home = getenv("USERPROFILE");
    std::string rex_libs = std::string(home ? home : "C:\\Users\\user") + "\\.rex\\libs";
    bool libs_ok = true;
    try {
        fs::create_directories(rex_libs);
    } catch (...) {
        libs_ok = false;
    }
    setup_step(6, TOTAL_STEPS, "Libs folder:          ", rex_libs, libs_ok);

    // ── Step 7: Add to PATH ──────────────────────────────────
    bool path_already = is_in_user_path(install_dir);
    if (path_already) {
        setup_step(7, TOTAL_STEPS, "PATH:                 ", "already configured");
    } else {
        bool path_ok = add_to_user_path(install_dir);
        setup_step(7, TOTAL_STEPS, "Added to PATH:        ", install_dir, path_ok);
        if (!path_ok) {
            std::cout << "\n  " << INS_RED << "[!] Could not write to registry." << INS_RESET << "\n";
            std::cout << "      " << INS_DIM << "Add manually: " << install_dir << INS_RESET << "\n\n";
            all_ok = false;
        }
    }

    // ── Step 8: Create desktop shortcut info ─────────────────
    setup_step(8, TOTAL_STEPS, "Config folder:        ", std::string(home ? home : "") + "\\.rex");

    // ── Step 9: Verify ───────────────────────────────────────
    if (copy_ok && fs::exists(install_exe)) {
        setup_step(9, TOTAL_STEPS, "Verification:         ", "rex.exe ready");
    } else {
        setup_step(9, TOTAL_STEPS, "Verification:         ", "check above", !copy_ok);
    }

    // ── Summary ──────────────────────────────────────────────
    std::cout << "\n  " << INS_DIM << "==========================================" << INS_RESET << "\n";
    if (all_ok) {
        if (already_installed) {
            std::cout << "  " << INS_GREEN << INS_BOLD << "[OK] REX updated to v"
                      << rex_version << " at C:\\REX\\rex.exe" << INS_RESET << "\n";
        } else {
            std::cout << "  " << INS_GREEN << INS_BOLD << "[OK] REX v"
                      << rex_version << " installed at C:\\REX\\rex.exe" << INS_RESET << "\n";
        }
    } else {
        std::cout << "  " << INS_YELLOW << INS_BOLD << "[!] Installation completed with warnings (see above)"
                  << INS_RESET << "\n";
    }
    std::cout << "  " << INS_DIM << "==========================================" << INS_RESET << "\n\n";

    std::cout << "  " << INS_BOLD << "Get started (open a NEW terminal):" << INS_RESET << "\n\n";
    std::cout << "    " << INS_CYAN << "rex help" << INS_RESET
              << "              " << INS_DIM << "— show all commands" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex run main.cpp" << INS_RESET
              << "      " << INS_DIM << "— compile and run a file" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex init my_project" << INS_RESET
              << "   " << INS_DIM << "— start a new project" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex add nlohmann-json" << INS_RESET
              << " " << INS_DIM << "— add a library" << INS_RESET << "\n\n";

    if (!comp_ok) {
        std::cout << "  " << INS_YELLOW << "[!] No C compiler found. rexc needs a C compiler backend." << INS_RESET << "\n";
        std::cout << "      " << INS_DIM << "Install one of:" << INS_RESET << "\n";
        std::cout << "        " << INS_CYAN << "MinGW  -> https://winlibs.com" << INS_RESET << "\n";
        std::cout << "        " << INS_CYAN << "LLVM   -> https://releases.llvm.org" << INS_RESET << "\n\n";
    }

    std::cout << "  " << INS_DIM << "Support: https://ko-fi.com/samns" << INS_RESET << "\n\n";

    wait_key("Press any key to close...");
}

#endif // _WIN32
