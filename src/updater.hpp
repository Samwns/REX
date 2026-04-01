#pragma once
#include <string>
#include <fstream>
#include <ctime>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "utils.hpp"

// ─── GitHub release API URL ───────────────────────────────────
static const std::string REX_RELEASES_API =
    "https://api.github.com/repos/Samwns/REX/releases/latest";
static const std::string REX_RELEASES_URL =
    "https://github.com/Samwns/REX/releases/latest";

static const int UPDATE_CHECK_INTERVAL = 86400; // seconds (24 hours)

// ─── Download a string from a URL (silent, stdout capture) ────
inline std::string download_string(const std::string& url) {
    fs::path tmp = rex_home() / "update_check.tmp";
    // Redirect stderr to the platform null device so no error noise leaks out
#ifdef _WIN32
    const std::string null_redirect = " 2>NUL";
#else
    const std::string null_redirect = " 2>/dev/null";
#endif
    std::string cmd;
    if (command_exists("curl")) {
        cmd = "curl -sL -o \"" + tmp.string() + "\" -H \"Accept: application/vnd.github.v3+json\" \"" + url + "\"" + null_redirect;
    } else if (command_exists("wget")) {
        cmd = "wget -qO \"" + tmp.string() + "\" --header=\"Accept: application/vnd.github.v3+json\" \"" + url + "\"" + null_redirect;
    } else {
        return "";
    }

    int ret = system(cmd.c_str());
    if (ret != 0) {
        fs::remove(tmp);
        return "";
    }

    std::ifstream f(tmp);
    if (!f.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();
    fs::remove(tmp);
    return content;
}

// ─── Extract a JSON string value for a given key ──────────────
// Simple helper: finds "key": "value" and returns value.
inline std::string extract_json_string(const std::string& json,
                                        const std::string& key,
                                        size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, start);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
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

// ─── Extract tag_name from GitHub API JSON ────────────────────
inline std::string extract_latest_tag(const std::string& json) {
    return extract_json_string(json, "tag_name");
}

// ─── Format byte size as human-readable string ────────────────
inline std::string format_size(long long bytes) {
    if (bytes <= 0) return "";
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        unit++;
    }
    char buf[64];
    if (unit == 0)
        snprintf(buf, sizeof(buf), "%lld %s", bytes, units[unit]);
    else
        snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

// ─── Find platform asset download URL from GitHub API JSON ────
// Parses the "assets" array to find the browser_download_url
// matching the current platform. This is more robust than
// constructing URLs from naming patterns.
struct AssetInfo {
    std::string download_url;
    std::string name;
    long long size = 0;
};

inline AssetInfo find_platform_asset(const std::string& json,
                                      const std::string& os) {
    AssetInfo info;

    // Platform keyword to match in asset names/URLs
    std::string platform_key;
    if (os == "windows")  platform_key = "windows";
    else if (os == "macos") platform_key = "macos";
    else                    platform_key = "linux";

    // Iterate through "browser_download_url" entries in the JSON
    std::string url_key = "\"browser_download_url\"";
    size_t pos = 0;

    while ((pos = json.find(url_key, pos)) != std::string::npos) {
        // Extract the URL value
        size_t colon = json.find(':', pos + url_key.size());
        if (colon == std::string::npos) break;

        size_t q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;

        std::string url = json.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        // Check if this URL matches our platform
        if (url.find(platform_key) == std::string::npos) continue;

        info.download_url = url;

        // Extract asset name from the URL (last path segment)
        size_t last_slash = url.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < url.size())
            info.name = url.substr(last_slash + 1);

        // Try to find "size" for this asset by searching backwards
        // for the enclosing "name" key, then forward for "size"
        std::string name_key = "\"name\"";
        // Search window: each asset object in the GitHub API JSON is
        // typically ~800-1200 bytes; 2000 bytes gives comfortable margin.
        static constexpr size_t ASSET_SEARCH_WINDOW = 2000;
        size_t search_back = (q1 > ASSET_SEARCH_WINDOW) ? q1 - ASSET_SEARCH_WINDOW : 0;
        size_t name_pos = std::string::npos;
        size_t tmp = search_back;
        while ((tmp = json.find(name_key, tmp)) != std::string::npos && tmp < q1) {
            name_pos = tmp;
            tmp += name_key.size();
        }
        if (name_pos != std::string::npos) {
            // Look for "size": <number> between name_pos and our url position
            std::string size_key = "\"size\"";
            size_t size_pos = json.find(size_key, name_pos);
            if (size_pos != std::string::npos && size_pos < q1) {
                size_t sc = json.find(':', size_pos + size_key.size());
                if (sc != std::string::npos) {
                    sc++;
                    while (sc < json.size() && (json[sc] == ' ' || json[sc] == '\t'))
                        sc++;
                    std::string num_str;
                    while (sc < json.size() && json[sc] >= '0' && json[sc] <= '9') {
                        num_str += json[sc++];
                    }
                    if (!num_str.empty()) {
                        try { info.size = std::stoll(num_str); }
                        catch (const std::exception&) { /* non-numeric, ignore */ }
                    }
                }
            }
        }

        return info;
    }

    return info;
}

// ─── Compare semver strings (vX.Y.Z) ─────────────────────────
// Returns true if remote is newer than local
inline bool is_newer_version(const std::string& local, const std::string& remote) {
    auto parse = [](const std::string& v, int& major, int& minor, int& patch, int& build) {
        std::string s = v;
        if (!s.empty() && s[0] == 'v') s = s.substr(1);
        major = minor = patch = build = 0;
        if (sscanf(s.c_str(), "%d.%d.%d.%d", &major, &minor, &patch, &build) < 1)
            return false;
        return true;
    };

    int lmaj, lmin, lpat, lbld;
    int rmaj, rmin, rpat, rbld;
    if (!parse(local, lmaj, lmin, lpat, lbld)) return false;
    if (!parse(remote, rmaj, rmin, rpat, rbld)) return false;

    if (rmaj != lmaj) return rmaj > lmaj;
    if (rmin != lmin) return rmin > lmin;
    if (rpat != lpat) return rpat > lpat;
    return rbld > lbld;
}

// ─── Cache file for last update check ─────────────────────────
inline fs::path update_cache_path() {
    return rex_home() / "last_update_check";
}

// ─── Check if enough time has passed since last check ─────────
inline bool should_check_update() {
    fs::path cache = update_cache_path();
    if (!fs::exists(cache)) return true;

    std::ifstream f(cache);
    if (!f.is_open()) return true;

    std::time_t last_check = 0;
    f >> last_check;
    f.close();

    std::time_t now = std::time(nullptr);
    // Check at most once per day
    return (now - last_check) > UPDATE_CHECK_INTERVAL;
}

// ─── Save the current timestamp as last check ─────────────────
inline void save_update_check_time() {
    std::ofstream f(update_cache_path());
    if (f.is_open()) {
        f << std::time(nullptr);
    }
}

// ─── Background update check (called on startup) ─────────────
// Shows a notification if a newer version is available.
// Only checks once per day to avoid slowing down every invocation.
inline void check_for_updates(const std::string& current_version) {
    if (!should_check_update()) return;

    save_update_check_time();

    std::string json = download_string(REX_RELEASES_API);
    if (json.empty()) return;

    std::string latest = extract_latest_tag(json);
    if (latest.empty()) return;

    if (is_newer_version(current_version, latest)) {
        std::cout << "\n"
                  << COLOR_YELLOW << COLOR_BOLD
                  << "  [UPDATE] " << COLOR_RESET
                  << "REX " << latest << " is available! "
                  << "(current: " << current_version << ")\n"
                  << "           Download: " << COLOR_CYAN
                  << REX_RELEASES_URL << COLOR_RESET << "\n"
                  << "           Or run:   " << COLOR_CYAN
                  << "rex update" << COLOR_RESET << "\n\n";
    }
}

// ─── cmd: update ──────────────────────────────────────────────
// Checks for REX updates and optionally downloads/installs them.
// Also checks installed libraries for updates.
inline int cmd_update(const std::string& current_version) {
    std::cout << COLOR_BOLD << "\nChecking for updates...\n" << COLOR_RESET;

    // ── Check REX version ─────────────────────────────────────
    std::string json = download_string(REX_RELEASES_API);
    if (json.empty()) {
        rex_err("Could not reach GitHub. Check your internet connection.");
        return 1;
    }

    std::string latest = extract_latest_tag(json);
    if (latest.empty()) {
        rex_err("Could not parse release information.");
        return 1;
    }

    save_update_check_time();

    if (is_newer_version(current_version, latest)) {
        std::cout << COLOR_GREEN << "  New REX version available: "
                  << COLOR_BOLD << latest << COLOR_RESET
                  << " (current: " << current_version << ")\n\n";

        // Detect platform and find the correct download URL from the API response
        std::string os = detect_os();
        AssetInfo asset = find_platform_asset(json, os);

        std::string download_url;
        std::string asset_name;

        if (!asset.download_url.empty()) {
            // Use the actual URL from the GitHub API — most robust approach
            download_url = asset.download_url;
            asset_name = asset.name;
        } else {
            // Fallback: construct URL from naming convention
            if (os == "windows") asset_name = "rex-" + latest + "-windows-x86_64.exe";
            else if (os == "macos") asset_name = "rex-" + latest + "-macos-universal";
            else asset_name = "rex-" + latest + "-linux-x86_64";
            download_url =
                "https://github.com/Samwns/REX/releases/download/" + latest + "/" + asset_name;
        }

        std::cout << "  Do you want to download and install " << latest << "? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);

        if (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y')) {
            // Determine install location
            std::string exe_path;
#ifdef _WIN32
            exe_path = "C:\\REX\\rex.exe";
#else
            // Try to detect the actual running binary path
            {
                char buf[4096] = {};
#ifdef __APPLE__
                // macOS: argv[0] fallback (best effort)
                exe_path = "/usr/local/bin/rex";
#else
                // Linux: use /proc/self/exe to find the real path
                ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = '\0';
                    exe_path = std::string(buf);
                } else {
                    exe_path = "/usr/local/bin/rex";
                }
#endif
            }
            // Fallback: check common install locations
            if (exe_path.empty() || !fs::exists(exe_path)) {
                exe_path = "/usr/local/bin/rex";
                if (fs::exists("/usr/bin/rex")) exe_path = "/usr/bin/rex";
            }
#endif
            fs::path tmp_file = rex_home() / (asset_name.empty() ? "rex_update_tmp" : asset_name);

            // Show download info with size if available
            std::string dl_msg = "Downloading " + latest;
            if (!asset_name.empty())
                dl_msg += " (" + asset_name + ")";
            if (asset.size > 0)
                dl_msg += " [" + format_size(asset.size) + "]";
            dl_msg += "...";
            rex_info(dl_msg);
            std::cout << "\n";

            std::string dl_cmd;
            if (command_exists("curl")) {
                // Use curl's default progress meter for real-time download stats
                // (shows percentage, speed, ETA, and a progress bar)
                dl_cmd = "curl -fL -o \"" + tmp_file.string() + "\" "
                         "-# \"" + download_url + "\"";
            } else if (command_exists("wget")) {
                dl_cmd = "wget --show-progress -O \"" + tmp_file.string() + "\" \"" + download_url + "\"";
            } else {
                rex_err("curl or wget not found.");
                return 1;
            }

            if (system(dl_cmd.c_str()) != 0) {
                std::cout << "\n";
                rex_err("Download failed. URL: " + download_url);
                fs::remove(tmp_file);
                return 1;
            }

            std::cout << "\n";

            // Validate the downloaded file (must be a reasonable binary size)
            {
                std::error_code ec;
                auto file_size = fs::file_size(tmp_file, ec);
                if (ec || file_size < 100000) {
                    rex_err("Downloaded file is invalid or too small (" +
                            std::to_string(file_size) + " bytes). Update aborted.");
                    fs::remove(tmp_file);
                    return 1;
                }
                rex_ok("Download complete (" + format_size(static_cast<long long>(file_size)) + ")");
            }

            // Replace the current binary
            rex_info("Installing...");

#ifdef _WIN32
            // On Windows, we can't replace a running binary directly
            // Create a batch script to replace after exit
            fs::path bat = rex_home() / "update_rex.bat";
            {
                std::ofstream bf(bat);
                bf << "@echo off\n";
                bf << "timeout /t 1 /nobreak >nul\n";
                bf << "copy /Y \"" << tmp_file.string() << "\" \"" << exe_path << "\"\n";
                bf << "del \"" << tmp_file.string() << "\"\n";
                bf << "del \"" << bat.string() << "\"\n";
                bf << "echo [REX] Update complete! Restart rex to use " << latest << "\n";
            }
            std::string start_cmd = "start /B cmd /C \"" + bat.string() + "\"";
            system(start_cmd.c_str());
            rex_ok("Update will complete after REX exits.");
            return 0;
#else
            // On Unix, make executable and replace
            std::string chmod_cmd = "chmod +x \"" + tmp_file.string() + "\"";
            system(chmod_cmd.c_str());

            // Try to replace the binary
            std::error_code ec;
            fs::copy_file(tmp_file, exe_path,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                // Try with sudo
                rex_warn("Need elevated permissions to update " + exe_path);
                std::string sudo_cmd = "sudo cp \"" + tmp_file.string() + "\" \"" + exe_path + "\" && sudo chmod +x \"" + exe_path + "\"";
                if (system(sudo_cmd.c_str()) != 0) {
                    rex_err("Update failed. You can manually copy:");
                    rex_info("  cp \"" + tmp_file.string() + "\" \"" + exe_path + "\"");
                    return 1;
                }
            }
            fs::remove(tmp_file);
            rex_ok("Updated to " + latest + "! Restart rex to use the new version.");
            return 0;
#endif
        } else {
            std::cout << "  Skipped. Download manually from:\n"
                      << "  " << COLOR_CYAN << REX_RELEASES_URL << COLOR_RESET << "\n";
        }
    } else {
        rex_ok("REX is up to date (" + current_version + ")");
    }

    // ── Check installed libraries ─────────────────────────────
    std::cout << "\n" << COLOR_BOLD << "Checking installed libraries...\n" << COLOR_RESET;

    fs::path libs_dir = rex_home() / "libs";
    if (!fs::exists(libs_dir)) {
        rex_info("No libraries installed.");
        std::cout << "\n";
        return 0;
    }

    bool any_libs = false;
    for (auto& entry : fs::directory_iterator(libs_dir)) {
        if (fs::is_directory(entry)) {
            std::string lib_name = entry.path().filename().string();
            // Skip internal dirs that start with _
            if (lib_name.empty() || lib_name[0] == '_') continue;
            any_libs = true;
            std::cout << "  " << COLOR_CYAN << lib_name << COLOR_RESET
                      << " — installed ✓\n";
        }
    }

    if (any_libs) {
        std::cout << "\n  To reinstall a library: " << COLOR_CYAN
                  << "rex add <name>" << COLOR_RESET
                  << " (will replace existing)\n";
    } else {
        rex_info("No libraries installed.");
    }

    std::cout << "\n";
    return 0;
}
