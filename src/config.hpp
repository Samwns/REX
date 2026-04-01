#pragma once
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <iostream>
#include "utils.hpp"

// ─── Represents rex.toml ──────────────────────────────────────
struct RexConfig {
    std::string name       = "my_project";
    std::string version    = "0.1.0";
    std::string cpp_std    = "c++17";
    std::string entry      = "src/main.cpp";
    std::string output     = "build/main";
    std::string target;    // cross-compilation target triple (e.g. "aarch64-linux-gnu")
    std::vector<std::string> sources;
    std::vector<std::string> flags;
    std::map<std::string, std::string> dependencies; // name -> version
};

// ─── Parse a very simple TOML-like config ──────────────────────
inline RexConfig parse_config(const std::string& path = "rex.toml") {
    RexConfig cfg;
    std::ifstream f(path);
    if (!f.is_open()) return cfg;

    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Section header: [dependencies]
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        // Remove surrounding quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        if (section == "dependencies") {
            cfg.dependencies[key] = val;
        } else if (section == "project" || section.empty()) {
            if      (key == "name")    cfg.name    = val;
            else if (key == "version") cfg.version = val;
            else if (key == "std")     cfg.cpp_std = val;
            else if (key == "entry")   cfg.entry   = val;
            else if (key == "output")  cfg.output  = val;
            else if (key == "target")  cfg.target  = val;
        } else if (section == "build") {
            if (key == "flags") {
                // flags = ["-O2", "-Wall"]
                std::string inner = val;
                if (inner.front() == '[') inner = inner.substr(1);
                if (inner.back()  == ']') inner.pop_back();
                for (auto& f : split(inner, ',')) {
                    std::string flag = trim(f);
                    if (flag.front() == '"') flag = flag.substr(1, flag.size() - 2);
                    if (!flag.empty()) cfg.flags.push_back(flag);
                }
            }
        }
    }
    return cfg;
}

// ─── Write a new rex.toml ─────────────────────────────────────
inline void write_config(const RexConfig& cfg, const std::string& path = "rex.toml") {
    std::ofstream f(path);
    f << "[project]\n";
    f << "name    = \"" << cfg.name    << "\"\n";
    f << "version = \"" << cfg.version << "\"\n";
    f << "std     = \"" << cfg.cpp_std << "\"\n";
    f << "entry   = \"" << cfg.entry   << "\"\n";
    f << "output  = \"" << cfg.output  << "\"\n";
    if (!cfg.target.empty())
        f << "target  = \"" << cfg.target  << "\"\n";
    f << "\n[build]\n";
    f << "flags = [\"-O2\", \"-Wall\"]\n";
    f << "\n[dependencies]\n";
    for (auto& [k, v] : cfg.dependencies)
        f << k << " = \"" << v << "\"\n";
}
