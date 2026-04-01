#pragma once

#ifndef _WIN32

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <chrono>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "utils.hpp"

namespace fs = std::filesystem;

// ─── ANSI helpers (installer uses its own for bold/dim/etc) ───
#define INS_RESET   "\033[0m"
#define INS_BOLD    "\033[1m"
#define INS_DIM     "\033[2m"
#define INS_RED     "\033[31m"
#define INS_GREEN   "\033[32m"
#define INS_YELLOW  "\033[33m"
#define INS_CYAN    "\033[36m"
#define INS_WHITE   "\033[37m"
#define INS_BG_BLUE "\033[44m"

// ─── Small delay for visual effect ────────────────────────────
static void ins_delay(int ms = 300) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─── Step logging ─────────────────────────────────────────────
static void ins_step(int n, int total, const std::string& label,
                     const std::string& value = "", bool ok = true) {
    std::string status = ok
        ? (std::string(INS_GREEN) + "[OK]" + INS_RESET)
        : (std::string(INS_RED)   + "[FAIL]" + INS_RESET);

    std::cout << "  " << INS_DIM << "[" << n << "/" << total << "]" << INS_RESET
              << " " << label;
    if (!value.empty()) std::cout << " " << INS_CYAN << value << INS_RESET;
    std::cout << "  " << status << "\n";
    ins_delay();
}

static void ins_log(const std::string& msg) {
    std::cout << "  " << INS_DIM << "→" << INS_RESET << " " << msg << "\n";
}

static void ins_warn(const std::string& msg) {
    std::cout << "  " << INS_YELLOW << "[!]" << INS_RESET << " " << msg << "\n";
}

// ─── Read a file into string ──────────────────────────────────
static std::string read_file_str(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ─── Detect Linux distro ─────────────────────────────────────
struct DistroInfo {
    std::string name;     // e.g. "Ubuntu", "Arch Linux", "Fedora"
    std::string id;       // e.g. "ubuntu", "arch", "fedora"
    std::string version;  // e.g. "22.04"
    std::string family;   // "debian", "arch", "rhel", "macos", "unknown"
};

static DistroInfo detect_distro() {
    DistroInfo info;

#ifdef __APPLE__
    info.name    = "macOS";
    info.id      = "macos";
    info.version = "";
    info.family  = "macos";

    // Try to get macOS version
    FILE* fp = popen("sw_vers -productVersion 2>/dev/null", "r");
    if (fp) {
        char buf[128] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            info.version = std::string(buf);
            // trim newline
            while (!info.version.empty() && (info.version.back() == '\n' || info.version.back() == '\r'))
                info.version.pop_back();
        }
        pclose(fp);
    }
    return info;
#else
    // Read /etc/os-release
    std::string os_release = read_file_str("/etc/os-release");
    if (os_release.empty()) {
        info.name = "Linux";
        info.id = "linux";
        info.family = "unknown";
        return info;
    }

    // Parse key=value pairs
    auto get_value = [&](const std::string& key) -> std::string {
        std::string search = key + "=";
        auto pos = os_release.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        if (pos < os_release.size() && os_release[pos] == '"') {
            pos++;
            auto end = os_release.find('"', pos);
            if (end == std::string::npos) return "";
            return os_release.substr(pos, end - pos);
        }
        auto end = os_release.find('\n', pos);
        return os_release.substr(pos, end - pos);
    };

    info.name    = get_value("PRETTY_NAME");
    info.id      = get_value("ID");
    info.version = get_value("VERSION_ID");

    if (info.name.empty()) info.name = get_value("NAME");
    if (info.name.empty()) info.name = "Linux";
    if (info.id.empty())   info.id   = "linux";

    // Determine family
    std::string id_like = get_value("ID_LIKE");
    if (info.id == "ubuntu" || info.id == "debian" || info.id == "pop" ||
        info.id == "linuxmint" || info.id == "elementary" ||
        id_like.find("debian") != std::string::npos ||
        id_like.find("ubuntu") != std::string::npos) {
        info.family = "debian";
    } else if (info.id == "arch" || info.id == "manjaro" || info.id == "endeavouros" ||
               id_like.find("arch") != std::string::npos) {
        info.family = "arch";
    } else if (info.id == "fedora" || info.id == "rhel" || info.id == "centos" ||
               info.id == "rocky" || info.id == "alma" ||
               id_like.find("fedora") != std::string::npos ||
               id_like.find("rhel") != std::string::npos) {
        info.family = "rhel";
    } else if (info.id == "opensuse" || info.id == "sles" ||
               id_like.find("suse") != std::string::npos) {
        info.family = "suse";
    } else {
        info.family = "unknown";
    }

    return info;
#endif
}

// ─── Get architecture string ──────────────────────────────────
static std::string get_arch() {
    FILE* fp = popen("uname -m 2>/dev/null", "r");
    if (!fp) return "unknown";
    char buf[128] = {};
    if (fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        std::string arch(buf);
        while (!arch.empty() && (arch.back() == '\n' || arch.back() == '\r'))
            arch.pop_back();
        return arch;
    }
    pclose(fp);
    return "unknown";
}

// ─── Detect C compiler for rexc backend ──────────────────────
static std::string detect_c_compiler() {
    for (const auto& cc : {"cc", "gcc", "clang"}) {
        if (command_exists(cc)) return cc;
    }
    // Fallback: check C++ compilers
    for (const auto& cxx : {"g++", "clang++"}) {
        if (command_exists(cxx)) return std::string(cxx) + " (fallback)";
    }
    return "";
}

// ─── Suggest how to install a C compiler ──────────────────────
static void suggest_compiler_install(const DistroInfo& distro) {
    std::cout << "\n";
    ins_warn("No C compiler found. rexc needs a C compiler backend.");
    std::cout << "  " << INS_DIM << "Install one of:" << INS_RESET << "\n\n";

    if (distro.family == "debian") {
        std::cout << "    " << INS_CYAN << "sudo apt install gcc" << INS_RESET << "\n";
    } else if (distro.family == "arch") {
        std::cout << "    " << INS_CYAN << "sudo pacman -S gcc" << INS_RESET << "\n";
    } else if (distro.family == "rhel") {
        std::cout << "    " << INS_CYAN << "sudo dnf install gcc" << INS_RESET << "\n";
    } else if (distro.family == "suse") {
        std::cout << "    " << INS_CYAN << "sudo zypper install gcc" << INS_RESET << "\n";
    } else if (distro.family == "macos") {
        std::cout << "    " << INS_CYAN << "xcode-select --install" << INS_RESET << "\n";
        std::cout << "    " << INS_DIM << "or" << INS_RESET << "\n";
        std::cout << "    " << INS_CYAN << "brew install gcc" << INS_RESET << "\n";
    } else {
        std::cout << "    " << INS_CYAN << "Install gcc or clang for your distro" << INS_RESET << "\n";
    }
    std::cout << "\n";
}

// ─── Show license ─────────────────────────────────────────────
static void show_license() {
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

// ─── Confirm prompt ───────────────────────────────────────────
static bool confirm_prompt(const std::string& msg) {
    std::cout << "  " << INS_BOLD << msg << " [Y/n] " << INS_RESET;
    std::string input;
    std::getline(std::cin, input);
    // trim
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t'))
        input.erase(input.begin());
    if (input.empty() || input[0] == 'y' || input[0] == 'Y') return true;
    return false;
}

// ─── Check if running as root ─────────────────────────────────
static bool is_root() {
    return geteuid() == 0;
}

// ─── Get self path ────────────────────────────────────────────
static std::string get_self_path() {
#ifdef __APPLE__
    char buf[4096] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return std::string(buf);
    // fallback
    return std::string(buf);
#else
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
#endif
}

// ═══════════════════════════════════════════════════════════════
//  MAIN INSTALLER — Linux / macOS
// ═══════════════════════════════════════════════════════════════
static int run_unix_setup(const std::string& rex_version) {
    const int TOTAL_STEPS = 9;
    std::string install_dir = "/usr/local/bin";
    std::string install_bin = install_dir + "/rex";
    std::string home_dir    = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
    std::string rex_home_d  = home_dir + "/.rex";
    std::string rex_libs    = rex_home_d + "/libs";

    // ── Banner ────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << INS_CYAN;
    std::cout << "  ██████╗ ███████╗██╗  ██╗\n";
    std::cout << "  ██╔══██╗██╔════╝╚██╗██╔╝\n";
    std::cout << "  ██████╔╝█████╗   ╚███╔╝ \n";
    std::cout << "  ██╔══██╗██╔══╝   ██╔██╗ \n";
    std::cout << "  ██║  ██║███████╗██╔╝ ██╗\n";
    std::cout << "  ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝\n";
    std::cout << INS_RESET << "\n";
    std::cout << "  " << INS_BOLD << "REX v" << rex_version << INS_RESET
              << " — Compile. Run. Execute.\n";
    std::cout << "  " << INS_DIM << "by Azathoth  |  github.com/Samwns/REX" << INS_RESET << "\n\n";

    ins_delay(500);

    // ── Detect system ─────────────────────────────────────────
    DistroInfo distro = detect_distro();
    std::string arch  = get_arch();

    bool already_installed = fs::exists(install_bin);

    if (already_installed) {
        std::cout << "  ╔══════════════════════════════════════════╗\n";
        std::cout << "  ║   " << INS_YELLOW << INS_BOLD << "UPDATE / REINSTALL" << INS_RESET
                  << "                      ║\n";
        std::cout << "  ╚══════════════════════════════════════════╝\n\n";
        ins_log("REX is already installed at: " + install_bin);
        ins_log("Updating to v" + rex_version + "...\n");
    } else {
        std::cout << "  ╔══════════════════════════════════════════╗\n";
        std::cout << "  ║   " << INS_GREEN << INS_BOLD << "REX INSTALLER" << INS_RESET
                  << "                           ║\n";
        std::cout << "  ╚══════════════════════════════════════════╝\n\n";
    }

    ins_delay(300);

    // ── Show License ──────────────────────────────────────────
    show_license();

    if (!confirm_prompt("Do you accept the MIT license and wish to continue?")) {
        std::cout << "\n  " << INS_RED << "Installation cancelled." << INS_RESET << "\n\n";
        return 1;
    }

    std::cout << "\n  " << INS_DIM << "──────────────────────────────────────────" << INS_RESET << "\n";
    std::cout << "  " << INS_BOLD << "Installing REX v" << rex_version << "..." << INS_RESET << "\n";
    std::cout << "  " << INS_DIM << "──────────────────────────────────────────" << INS_RESET << "\n\n";

    bool all_ok = true;

    // ── Step 1: Detect OS / Distro ────────────────────────────
    std::string os_label = distro.name;
    if (!distro.version.empty()) os_label += " " + distro.version;
    os_label += " (" + arch + ")";
    ins_step(1, TOTAL_STEPS, "Detected system:    ", os_label);

    // ── Step 2: Detect distro family ──────────────────────────
    std::string family_label;
    if (distro.family == "debian")  family_label = "Debian-based (apt)";
    else if (distro.family == "arch")   family_label = "Arch-based (pacman)";
    else if (distro.family == "rhel")   family_label = "RHEL-based (dnf/yum)";
    else if (distro.family == "suse")   family_label = "SUSE-based (zypper)";
    else if (distro.family == "macos")  family_label = "macOS (brew)";
    else                                family_label = "Generic Linux";
    ins_step(2, TOTAL_STEPS, "Package family:     ", family_label);

    // ── Step 3: Locate self ───────────────────────────────────
    std::string self_path = get_self_path();
    if (self_path.empty()) {
        ins_step(3, TOTAL_STEPS, "Locate binary:      ", "FAILED", false);
        std::cout << "  " << INS_RED << "Could not determine binary path." << INS_RESET << "\n";
        return 1;
    }
    // Shorten display if path is long
    std::string self_display = self_path;
    if (self_display.size() > 40) {
        self_display = "..." + self_display.substr(self_display.size() - 37);
    }
    ins_step(3, TOTAL_STEPS, "Located binary:     ", self_display);

    // ── Step 4: Detect C compiler ─────────────────────────────
    std::string backend = detect_c_compiler();
    if (!backend.empty()) {
        ins_step(4, TOTAL_STEPS, "C compiler backend: ", "rexc (" + backend + ")");
    } else {
        ins_step(4, TOTAL_STEPS, "C compiler backend: ", "not found", false);
    }

    // ── Step 5: Check permissions ─────────────────────────────
    bool need_sudo = !is_root() && !fs::is_symlink(install_bin);
    // Check if we can write to install_dir
    bool can_write = false;
    try {
        can_write = (access(install_dir.c_str(), W_OK) == 0);
    } catch (...) {}

    if (can_write) {
        ins_step(5, TOTAL_STEPS, "Permissions:        ", "writable");
        need_sudo = false;
    } else if (is_root()) {
        ins_step(5, TOTAL_STEPS, "Permissions:        ", "root");
        need_sudo = false;
    } else {
        ins_step(5, TOTAL_STEPS, "Permissions:        ", "needs sudo");
        need_sudo = true;
    }

    // ── Step 6: Copy binary to install dir ────────────────────
    bool copy_ok = false;
    {
        std::string cmd;
        if (need_sudo) {
            cmd = "sudo cp \"" + self_path + "\" \"" + install_bin + "\" 2>/dev/null";
        } else {
            cmd = "cp \"" + self_path + "\" \"" + install_bin + "\" 2>/dev/null";
        }
        int ret = system(cmd.c_str());
        copy_ok = (ret == 0);

        if (copy_ok) {
            // Make executable
            std::string chmod_cmd;
            if (need_sudo) {
                chmod_cmd = "sudo chmod +x \"" + install_bin + "\"";
            } else {
                chmod_cmd = "chmod +x \"" + install_bin + "\"";
            }
            int chmod_ret = system(chmod_cmd.c_str());
            (void)chmod_ret;
        }
    }

    if (already_installed) {
        ins_step(6, TOTAL_STEPS, "Updated rex:        ", install_bin, copy_ok);
    } else {
        ins_step(6, TOTAL_STEPS, "Installed rex:      ", install_bin, copy_ok);
    }

    if (!copy_ok) {
        std::cout << "\n";
        ins_warn("Could not install to " + install_dir);
        if (!need_sudo) {
            ins_log("Try running with sudo:");
            std::cout << "    " << INS_CYAN << "sudo " << self_path << " install" << INS_RESET << "\n\n";
        }
        all_ok = false;
    }

    // ── Step 7: Create ~/.rex/libs ────────────────────────────
    bool libs_ok = true;
    try {
        fs::create_directories(rex_libs);
    } catch (...) {
        libs_ok = false;
    }
    ins_step(7, TOTAL_STEPS, "Libs folder:        ", rex_libs, libs_ok);

    // ── Step 8: Check PATH ────────────────────────────────────
    {
        std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
        bool in_path = (path_env.find(install_dir) != std::string::npos);
        if (in_path) {
            ins_step(8, TOTAL_STEPS, "PATH:               ", "already configured");
        } else {
            ins_step(8, TOTAL_STEPS, "PATH:               ", install_dir + " (check shell config)", true);
            ins_log(install_dir + " should already be in most shell PATHs.");
        }
    }

    // ── Step 9: Verify installation ───────────────────────────
    if (copy_ok) {
        int ret = system("rex version >/dev/null 2>&1");
        if (ret == 0) {
            // Get version output
            FILE* fp = popen("rex version 2>&1", "r");
            if (fp) {
                char buf[256] = {};
                std::string ver_out;
                while (fgets(buf, sizeof(buf), fp)) {
                    ver_out += buf;
                }
                pclose(fp);
                // Trim
                while (!ver_out.empty() && (ver_out.back() == '\n' || ver_out.back() == '\r'))
                    ver_out.pop_back();
                ins_step(9, TOTAL_STEPS, "Verification:       ", ver_out);
            } else {
                ins_step(9, TOTAL_STEPS, "Verification:       ", "installed");
            }
        } else {
            ins_step(9, TOTAL_STEPS, "Verification:       ", "binary installed (open new terminal)", true);
        }
    } else {
        ins_step(9, TOTAL_STEPS, "Verification:       ", "skipped (install failed)", false);
    }

    // ── Summary ───────────────────────────────────────────────
    std::cout << "\n  " << INS_DIM << "──────────────────────────────────────────" << INS_RESET << "\n";
    if (all_ok) {
        if (already_installed) {
            std::cout << "  " << INS_GREEN << INS_BOLD << "✔ REX updated to v"
                      << rex_version << " successfully!" << INS_RESET << "\n";
        } else {
            std::cout << "  " << INS_GREEN << INS_BOLD << "✔ REX v"
                      << rex_version << " installed successfully!" << INS_RESET << "\n";
        }
    } else {
        std::cout << "  " << INS_YELLOW << INS_BOLD << "! Installation completed with warnings"
                  << INS_RESET << "\n";
    }
    std::cout << "  " << INS_DIM << "──────────────────────────────────────────" << INS_RESET << "\n";

    // ── Getting started ───────────────────────────────────────
    std::cout << "\n  " << INS_BOLD << "Get started:" << INS_RESET << "\n\n";
    std::cout << "    " << INS_CYAN << "rex help" << INS_RESET
              << "              " << INS_DIM << "— show all commands" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex run main.cpp" << INS_RESET
              << "      " << INS_DIM << "— compile and run a file" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex init my_project" << INS_RESET
              << "   " << INS_DIM << "— start a new project" << INS_RESET << "\n";
    std::cout << "    " << INS_CYAN << "rex add nlohmann-json" << INS_RESET
              << " " << INS_DIM << "— add a library" << INS_RESET << "\n";
    std::cout << "\n";

    // ── Compiler warning ──────────────────────────────────────
    if (backend.empty()) {
        suggest_compiler_install(distro);
    }

    std::cout << "  " << INS_DIM << "Support: https://ko-fi.com/samns" << INS_RESET << "\n\n";

    return all_ok ? 0 : 1;
}

#endif // !_WIN32
