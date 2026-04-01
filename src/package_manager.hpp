#pragma once
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include "utils.hpp"
#include "config.hpp"

// ─── Known library registry ───────────────────────────────────
// Format: name -> { url, description, header_only }
struct LibInfo {
    std::string url;
    std::string description;
    bool header_only;
};

// ─── Community package from rex-packages registry ─────────────
struct CommunityPackage {
    std::string name;
    std::string version;
    std::string description;
    std::string path;        // e.g. "packages/mathlib"
    std::string include_dir; // e.g. "include"
    std::string author;
    std::string license;
};

static const std::string REX_PACKAGES_REGISTRY_URL =
    "https://raw.githubusercontent.com/Samwns/rex-packages/main/registry.json";
static const std::string REX_PACKAGES_ARCHIVE_URL =
    "https://github.com/Samwns/rex-packages/archive/refs/heads/main.tar.gz";

inline std::map<std::string, LibInfo> get_registry() {
    return {
        { "nlohmann-json", {
            "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp",
            "JSON for Modern C++ (header-only)",
            true
        }},
        { "stb", {
            "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h",
            "STB single-file libraries (image loading)",
            true
        }},
        { "glm", {
            "https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz",
            "OpenGL Mathematics library",
            true
        }},
        { "magic-enum", {
            "https://raw.githubusercontent.com/Neargye/magic_enum/master/include/magic_enum/magic_enum.hpp",
            "Enum reflection for C++17 (header-only)",
            true
        }},
        { "toml11", {
            "https://raw.githubusercontent.com/ToruNiina/toml11/master/toml.hpp",
            "TOML parser for C++11/14/17",
            true
        }},
        { "argparse", {
            "https://raw.githubusercontent.com/p-ranav/argparse/master/include/argparse/argparse.hpp",
            "Argument parser for C++17",
            true
        }},
        { "fmt", {
            "https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.tar.gz",
            "Formatting library for C++",
            false
        }},
        { "spdlog", {
            "https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz",
            "Fast logging library",
            false
        }},
        { "termcolor", {
            "https://raw.githubusercontent.com/Samwns/rex-packages/main/packages/termcolor/include/termcolor.hpp",
            "Lightweight ANSI terminal color and style library for C++",
            true
        }},
    };
}

// ─── Download a file via curl or wget ─────────────────────────
inline bool download_file(const std::string& url, const std::string& dest) {
    std::string cmd;
    if (command_exists("curl")) {
        cmd = "curl -L -o \"" + dest + "\" --progress-bar \"" + url + "\"";
    } else if (command_exists("wget")) {
        cmd = "wget -O \"" + dest + "\" \"" + url + "\"";
    } else {
        rex_err("curl or wget not found. Install one to download libraries.");
        return false;
    }
    return system(cmd.c_str()) == 0;
}

// ─── Extract a tar.gz archive ─────────────────────────────────
inline bool extract_tar(const std::string& archive, const std::string& dest_dir) {
    fs::create_directories(dest_dir);
#ifdef _WIN32
    // Windows 10+ has tar
    std::string cmd = "tar -xzf \"" + archive + "\" -C \"" + dest_dir + "\" --strip-components=1";
#else
    std::string cmd = "tar -xzf \"" + archive + "\" -C \"" + dest_dir + "\" --strip-components=1";
#endif
    return system(cmd.c_str()) == 0;
}

// ─── Check if a directory has any files ───────────────────────
inline bool has_directory_files(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return false;
    for (auto& e : fs::directory_iterator(dir)) {
        (void)e;
        return true;
    }
    return false;
}

// ─── Minimal JSON string value extractor ──────────────────────
// Finds "key": "value" and returns value (for flat JSON objects)
inline std::string json_string_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
           json[pos] == '\n' || json[pos] == '\r'))
        pos++;

    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";

    return json.substr(pos, end - pos);
}

// ─── Parse registry.json content into packages ────────────────
inline std::vector<CommunityPackage> parse_registry_json(const std::string& content) {
    std::vector<CommunityPackage> packages;

    size_t arr_start = content.find("\"packages\"");
    if (arr_start == std::string::npos) return packages;

    arr_start = content.find('[', arr_start);
    if (arr_start == std::string::npos) return packages;

    size_t arr_end = content.rfind(']');
    if (arr_end == std::string::npos || arr_end <= arr_start) return packages;

    size_t pos = arr_start;
    while (pos < arr_end) {
        size_t obj_start = content.find('{', pos);
        if (obj_start == std::string::npos || obj_start > arr_end) break;

        size_t obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos || obj_end > arr_end) break;

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

        CommunityPackage pkg;
        pkg.name        = json_string_value(obj, "name");
        pkg.version     = json_string_value(obj, "version");
        pkg.description = json_string_value(obj, "description");
        pkg.path        = json_string_value(obj, "path");
        pkg.include_dir = json_string_value(obj, "include");
        pkg.author      = json_string_value(obj, "author");
        pkg.license     = json_string_value(obj, "license");

        if (!pkg.name.empty()) {
            packages.push_back(pkg);
        }

        pos = obj_end + 1;
    }

    return packages;
}

// ─── Fetch community registry from rex-packages ──────────────
inline std::vector<CommunityPackage> fetch_community_registry() {
    std::vector<CommunityPackage> packages;

    fs::path tmp_file = rex_home() / "registry_cache.json";

    if (!download_file(REX_PACKAGES_REGISTRY_URL, tmp_file.string())) {
        return packages;
    }

    std::ifstream f(tmp_file);
    if (!f.is_open()) return packages;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();
    fs::remove(tmp_file);

    return parse_registry_json(content);
}

// ─── Install a package from the community registry ────────────
inline bool pkg_add_community(const CommunityPackage& pkg, RexConfig& cfg) {
    rex_info("Installing from rex-packages: " + pkg.name + " v" + pkg.version);
    rex_info(pkg.description);

    fs::path lib_dir = rex_home() / "libs" / pkg.name;
    fs::create_directories(lib_dir);

    // Use a temp directory for extraction
    fs::path tmp_dir = rex_home() / "libs" / ("_tmp_" + pkg.name);
    fs::create_directories(tmp_dir);
    std::string archive_path = (tmp_dir / "rex-packages.tar.gz").string();

    rex_info("Downloading package...");
    if (!download_file(REX_PACKAGES_ARCHIVE_URL, archive_path)) {
        rex_err("Download failed.");
        fs::remove_all(tmp_dir);
        return false;
    }

    rex_info("Extracting...");
    // extract_tar uses --strip-components=1, removing the top-level archive dir
    if (!extract_tar(archive_path, tmp_dir.string())) {
        rex_err("Extraction failed.");
        fs::remove_all(tmp_dir);
        return false;
    }

    // Package files are at: tmp_dir/<pkg.path>/  (e.g., tmp_dir/packages/mathlib/)
    fs::path pkg_dir = tmp_dir / pkg.path;
    if (!fs::exists(pkg_dir)) {
        rex_err("Package directory not found in registry repository.");
        fs::remove_all(tmp_dir);
        return false;
    }

    // Copy include/ headers directly into lib_dir/ for easy #include "<name>/header.hpp"
    std::string include_subdir = pkg.include_dir.empty() ? "include" : pkg.include_dir;
    fs::path include_src = pkg_dir / include_subdir;
    if (fs::exists(include_src)) {
        for (auto& entry : fs::recursive_directory_iterator(include_src)) {
            auto rel = fs::relative(entry.path(), include_src);
            auto dest = lib_dir / rel;
            if (fs::is_directory(entry)) {
                fs::create_directories(dest);
            } else {
                fs::create_directories(dest.parent_path());
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
            }
        }
    }

    // Copy src/ files to lib_dir/src/ (if any)
    fs::path src_src = pkg_dir / "src";
    if (fs::exists(src_src)) {
        fs::path dest_src = lib_dir / "src";
        fs::create_directories(dest_src);
        for (auto& entry : fs::recursive_directory_iterator(src_src)) {
            auto rel = fs::relative(entry.path(), src_src);
            auto dest = dest_src / rel;
            if (fs::is_directory(entry)) {
                fs::create_directories(dest);
            } else {
                fs::create_directories(dest.parent_path());
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
            }
        }
    }

    // Cleanup temp directory
    fs::remove_all(tmp_dir);

    // Update rex.toml if it exists
    cfg.dependencies[pkg.name] = pkg.version;
    if (fs::exists("rex.toml")) {
        write_config(cfg);
    }

    rex_ok("Installed: " + pkg.name + " v" + pkg.version + " at: " + lib_dir.string());
    return true;
}

// ─── Add a library to the project ────────────────────────────
inline bool pkg_add(const std::string& lib_name, RexConfig& cfg) {
    auto registry = get_registry();
    auto it = registry.find(lib_name);

    if (it == registry.end()) {
        // Not in built-in registry — try the community registry (rex-packages)
        rex_info("Not in built-in registry. Searching rex-packages...");
        auto community = fetch_community_registry();
        for (auto& pkg : community) {
            if (pkg.name == lib_name) {
                return pkg_add_community(pkg, cfg);
            }
        }
        rex_err("Library '" + lib_name + "' not found in any registry.");
        rex_info("Built-in: nlohmann-json, stb, glm, magic-enum, toml11, argparse, fmt, spdlog, termcolor");
        rex_info("Community: https://github.com/Samwns/rex-packages");
        rex_info("Or use: rex add <github-user/repo> to add from GitHub directly.");
        return false;
    }

    auto& info = it->second;

    // Check if library is already installed
    fs::path lib_dir = rex_home() / "libs" / lib_name;
    if (has_directory_files(lib_dir)) {
        std::cout << COLOR_YELLOW << "[REX] " << COLOR_RESET
                  << "Library '" << lib_name << "' is already installed.\n"
                  << "  Replace with latest version? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
            rex_info("Skipped.");
            return false;
        }
        // Remove old version before reinstalling
        fs::remove_all(lib_dir);
        rex_info("Removed old version.");
    }

    rex_info("Adding: " + lib_name + " — " + info.description);

    fs::create_directories(lib_dir);

    std::string url = info.url;

    if (info.header_only && ends_with(url, ".hpp")) {
        // Single header download
        std::string header_name = lib_name + ".hpp";
        std::string dest = (lib_dir / header_name).string();
        rex_info("Downloading header...");
        if (!download_file(url, dest)) {
            rex_err("Download failed.");
            return false;
        }
    } else if (ends_with(url, ".tar.gz")) {
        // Download and extract
        std::string archive = (lib_dir / (lib_name + ".tar.gz")).string();
        rex_info("Downloading archive...");
        if (!download_file(url, archive)) {
            rex_err("Download failed.");
            return false;
        }
        rex_info("Extracting...");
        if (!extract_tar(archive, lib_dir.string())) {
            rex_err("Extraction failed.");
            return false;
        }
        fs::remove(archive); // cleanup
    }

    // Update rex.toml
    cfg.dependencies[lib_name] = "latest";
    write_config(cfg);

    rex_ok("Library '" + lib_name + "' installed at: " + lib_dir.string());
    return true;
}

// ─── Add from GitHub directly ─────────────────────────────────
inline bool pkg_add_github(const std::string& repo, RexConfig& cfg) {
    // repo format: user/name or user/name@version
    std::string slug = repo;
    std::string version = "main";

    auto at = repo.find('@');
    if (at != std::string::npos) {
        slug    = repo.substr(0, at);
        version = repo.substr(at + 1);
    }

    std::string lib_name = split(slug, '/').back();
    std::string url = "https://github.com/" + slug + "/archive/refs/heads/" + version + ".tar.gz";

    // Check if library is already installed
    fs::path lib_dir = rex_home() / "libs" / lib_name;
    if (has_directory_files(lib_dir)) {
        std::cout << COLOR_YELLOW << "[REX] " << COLOR_RESET
                  << "Library '" << lib_name << "' is already installed.\n"
                  << "  Replace with version " << version << "? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
            rex_info("Skipped.");
            return false;
        }
        fs::remove_all(lib_dir);
        rex_info("Removed old version.");
    }

    rex_info("Installing from GitHub: " + slug + " @ " + version);

    fs::create_directories(lib_dir);

    std::string archive = (lib_dir / (lib_name + ".tar.gz")).string();
    if (!download_file(url, archive)) {
        rex_err("Download failed.");
        return false;
    }

    extract_tar(archive, lib_dir.string());
    fs::remove(archive);

    cfg.dependencies[lib_name] = version;
    write_config(cfg);

    rex_ok("Installed: " + lib_name + " @ " + version);
    return true;
}

// ─── Remove a library ─────────────────────────────────────────
inline bool pkg_remove(const std::string& lib_name, RexConfig& cfg) {
    fs::path lib_dir = rex_home() / "libs" / lib_name;
    if (!fs::exists(lib_dir)) {
        rex_warn("Library '" + lib_name + "' is not installed.");
        return false;
    }
    fs::remove_all(lib_dir);
    cfg.dependencies.erase(lib_name);
    write_config(cfg);
    rex_ok("Removed: " + lib_name);
    return true;
}

// ─── List installed libraries ─────────────────────────────────
inline void pkg_list() {
    fs::path libs = rex_home() / "libs";
    if (!fs::exists(libs)) {
        rex_info("No libraries installed.");
        return;
    }

    std::cout << COLOR_BOLD << "\nInstalled libraries:\n" << COLOR_RESET;
    bool any = false;
    for (auto& entry : fs::directory_iterator(libs)) {
        if (fs::is_directory(entry)) {
            std::cout << "  • " << entry.path().filename().string() << "\n";
            any = true;
        }
    }
    if (!any) rex_info("None yet. Try: rex add nlohmann-json");
    std::cout << "\n";
}

// ─── Show available registry ──────────────────────────────────
inline void pkg_registry() {
    auto reg = get_registry();
    std::cout << COLOR_BOLD << "\nREX Built-in Libraries:\n" << COLOR_RESET;
    for (auto& [name, info] : reg) {
        std::string kind = info.header_only ? "[header-only]" : "[compiled]";
        std::cout << "  " << COLOR_CYAN << name << COLOR_RESET
                  << "\t" << COLOR_YELLOW << kind << COLOR_RESET
                  << "\t" << info.description << "\n";
    }

    // Show community packages from rex-packages
    std::cout << "\n" << COLOR_BOLD << "Community Packages (rex-packages):\n" << COLOR_RESET;
    auto community = fetch_community_registry();
    if (community.empty()) {
        std::cout << "  (could not fetch — check your internet connection)\n";
    } else {
        for (auto& pkg : community) {
            std::cout << "  " << COLOR_CYAN << pkg.name << COLOR_RESET
                      << "\tv" << pkg.version
                      << "\t" << pkg.description << "\n";
        }
    }

    std::cout << "\n  To add a library:  rex add <name>\n";
    std::cout << "  From GitHub:       rex add <github-user/repo>\n";
    std::cout << "  Community registry: https://github.com/Samwns/rex-packages\n\n";
}
