#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include "utils.hpp"
#include "i18n.hpp"
#include "signal_handler.hpp"
#include "config.hpp"
#include "compiler.hpp"
#include "cross_compile.hpp"
#include "package_manager.hpp"
#include "updater.hpp"
#include "interpreter/interpreter.hpp"

#ifdef _WIN32
#include "setup_windows.hpp"
#else
#include "setup_unix.hpp"
#endif

namespace fs = std::filesystem;

using rex_i18n::msg;

// ─── Version ──────────────────────────────────────────────────
static const std::string REX_VERSION = "v1.0.32";

// ─── Supported C++ standards ──────────────────────────────────
static const std::string DEFAULT_CPP_STD = "c++20";

inline bool is_valid_cpp_std(const std::string& std_str) {
    static const std::vector<std::string> valid = {
        "c++11", "c++14", "c++17", "c++20", "c++23"
    };
    return std::find(valid.begin(), valid.end(), std_str) != valid.end();
}

// ─── Banner ───────────────────────────────────────────────────
void print_banner() {
    std::cout << COLOR_CYAN << R"(
 ██████╗ ███████╗██╗  ██╗
 ██╔══██╗██╔════╝╚██╗██╔╝
 ██████╔╝█████╗   ╚███╔╝ 
 ██╔══██╗██╔══╝   ██╔██╗ 
 ██║  ██║███████╗██╔╝ ██╗
 ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝
)" << COLOR_RESET;
    std::cout << msg("banner_tagline") << "   " << REX_VERSION << "\n\n";
}

// ─── Help ─────────────────────────────────────────────────────
void print_help() {
    print_banner();
    std::cout << COLOR_BOLD << msg("help_usage") << "\n" << COLOR_RESET;
    std::cout << "  rex <command> [args]\n\n";

    std::cout << COLOR_BOLD << msg("help_commands") << "\n" << COLOR_RESET;
    std::cout << "  " << COLOR_GREEN << "run    <file.cpp>" << COLOR_RESET
              << "         " << msg("help_run_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "repl" << COLOR_RESET
              << "                      " << msg("help_repl_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "-b [run] <file.cpp>" << COLOR_RESET
              << "         " << msg("help_brun_desc") << "\n";
    std::cout << "         " << COLOR_YELLOW << "--std=c++XX" << COLOR_RESET
              << "          " << msg("help_std_desc") << "\n";
    std::cout << "         " << COLOR_YELLOW << "--target=TRIPLE" << COLOR_RESET
              << "      " << msg("help_target_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "build" << COLOR_RESET
              << "                     " << msg("help_build_desc") << "\n";
    std::cout << "         " << COLOR_YELLOW << "--target=TRIPLE" << COLOR_RESET
              << "      " << msg("help_target_override") << "\n";
    std::cout << "  " << COLOR_GREEN << "exec" << COLOR_RESET
              << "                      " << msg("help_exec_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "init   [name]" << COLOR_RESET
              << "             " << msg("help_init_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "add    <lib>" << COLOR_RESET
              << "              " << msg("help_add_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "remove <lib>" << COLOR_RESET
              << "              " << msg("help_remove_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "list" << COLOR_RESET
              << "                      " << msg("help_list_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "registry" << COLOR_RESET
              << "                  " << msg("help_registry_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "search [query]" << COLOR_RESET
              << "            " << msg("help_search_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "clean" << COLOR_RESET
              << "                     " << msg("help_clean_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "targets" << COLOR_RESET
              << "                   " << msg("help_targets_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "update" << COLOR_RESET
              << "                    " << msg("help_update_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "install" << COLOR_RESET
              << "                   " << msg("help_install_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "version" << COLOR_RESET
              << "                   " << msg("help_version_desc") << "\n";
    std::cout << "  " << COLOR_GREEN << "help" << COLOR_RESET
              << "                      " << msg("help_help_desc") << "\n\n";

    std::cout << COLOR_BOLD << msg("help_examples") << "\n" << COLOR_RESET;
    std::cout << "  rex run main.cpp\n";
    std::cout << "  rex repl\n";
    std::cout << "  rex -b main.cpp\n";
    std::cout << "  rex -b run main.cpp\n";
    std::cout << "  rex -b main.cpp --target=aarch64-linux-gnu\n";
    std::cout << "  rex init my_project\n";
    std::cout << "  rex add nlohmann-json\n";
    std::cout << "  rex add nlohmann/json@v3.11.3\n";
    std::cout << "  rex build\n";
    std::cout << "  rex build --target=x86_64-w64-mingw32\n";
    std::cout << "  rex exec\n";
    std::cout << "  rex targets\n";
    std::cout << "  rex install\n\n";
}

// ─── cmd: init ────────────────────────────────────────────────
int cmd_init(const std::vector<std::string>& args) {
    std::string name = args.empty() ? "my_project" : args[0];

    if (fs::exists("rex.toml")) {
        rex_warn(msg("init_already_exists"));
        return 1;
    }

    // Create structure
    fs::create_directories("src");
    fs::create_directories("build");

    // Write rex.toml
    RexConfig cfg;
    cfg.name = name;
    write_config(cfg);

    // Write starter main.cpp
    std::ofstream f("src/main.cpp");
    f << "#include <iostream>\n\n";
    f << "int main() {\n";
    f << "    std::cout << \"Hello from " << name << "!\\n\";\n";
    f << "    return 0;\n";
    f << "}\n";
    f.close();

    rex_ok(msg("init_success") + " '" + name + "'");
    std::cout << "\n" << msg("init_structure") << "\n";
    std::cout << "    " << name << "/\n";
    std::cout << "    ├── src/main.cpp\n";
    std::cout << "    ├── build/\n";
    std::cout << "    └── rex.toml\n\n";
    std::cout << "  Run: " << COLOR_CYAN << "rex build" << COLOR_RESET << msg("init_run_build") << "\n";
    std::cout << "  Run: " << COLOR_CYAN << "rex exec"  << COLOR_RESET << msg("init_run_exec") << "\n\n";
    return 0;
}

// Forward declaration: used by cmd_run fallback path.
int cmd_brun(const std::vector<std::string>& args);

// ─── cmd: run (interpret without compiling) ──────────────────
int cmd_run(const std::vector<std::string>& args) {
    if (args.empty()) {
        rex_err(msg("run_usage"));
        return 1;
    }

    std::string file;
    bool verbose = false;
    for (auto& a : args) {
        if (a == "--verbose" || a == "-V") verbose = true;
        else if (file.empty()) file = a;
    }

    if (file.empty()) {
        rex_err(msg("run_usage"));
        return 1;
    }

    if (!fs::exists(file)) {
        rex_err(msg("run_file_not_found") + file);
        return 1;
    }

    // Interpreter does not fully support external headers/libraries yet.
    // If the source uses includes, transparently fall back to compiled run.
    {
        std::ifstream in(file);
        if (in.is_open()) {
            std::string line;
            bool has_include = false;
            while (std::getline(in, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos && line.compare(pos, 8, "#include") == 0) {
                    has_include = true;
                    break;
                }
            }
            if (has_include) {
                rex_info("Includes detected; using compiled mode for library compatibility.");
                return cmd_brun({file});
            }
        }
    }

    try {
        rex::interp::Interpreter interp;
        if (verbose) interp.set_verbose(true);
        return interp.run_file(file);
    } catch (const rex::interp::RuntimeError& e) {
        std::cerr << COLOR_RED << "error: " << COLOR_RESET << e.what();
        if (e.line > 0) std::cerr << " (line " << e.line << ")";
        std::cerr << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << COLOR_RED << "error: " << COLOR_RESET << e.what() << "\n";
        return 1;
    }
}

// ─── cmd: -b (compile + run) ─────────────────────────────────
int cmd_brun(const std::vector<std::string>& args) {
    if (args.empty()) {
        rex_err(msg("b_usage"));
        return 1;
    }

    // Parse arguments: extract --std=c++XX, --target=TRIPLE, and the source file
    std::string file;
    std::string cpp_std = DEFAULT_CPP_STD;
    std::string target_triple;
    std::vector<std::string> prog_args;
    bool found_file = false;

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind("--std=", 0) == 0) {
            cpp_std = args[i].substr(6);  // after "--std="
            if (!is_valid_cpp_std(cpp_std)) {
                rex_err(msg("run_invalid_std") + cpp_std);
                rex_info(msg("run_supported_stds"));
                return 1;
            }
        } else if (args[i].rfind("--target=", 0) == 0) {
            target_triple = args[i].substr(9);  // after "--target="
            if (target_triple.empty()) {
                rex_err(msg("run_empty_target"));
                rex_info(msg("run_target_hint"));
                return 1;
            }
        } else if (!found_file) {
            // Allow "run" as an optional subcommand: rex -b run <file.cpp>
            // Only skip "run" if there is a following argument to serve as the filename
            if (args[i] == "run" && i + 1 < args.size()) continue;
            file = args[i];
            found_file = true;
        } else {
            prog_args.push_back(args[i]);
        }
    }

    if (file.empty()) {
        rex_err(msg("run_usage"));
        return 1;
    }

    if (!fs::exists(file)) {
        rex_err(msg("run_file_not_found") + file);
        return 1;
    }

    std::string out = "build/rex_tmp";
    fs::create_directories("build");

    std::string libs_include = (rex_home() / "libs").string();
    rex_info(msg("run_standard") + cpp_std);
    if (!target_triple.empty())
        rex_info(msg("run_target") + target_triple);
    auto res = compile_file(file, out, cpp_std, {"-Wall"}, libs_include, target_triple);

    if (!res.success) {
        rex_err(msg("run_compilation_failed"));
        return 1;
    }

    // If cross-compiling, we can't run the binary on the host
    if (!target_triple.empty()) {
        TargetTriple target = parse_target_triple(target_triple);
        if (!is_native_target(target)) {
            rex_ok(msg("run_cross_compiled") + res.binary);
            rex_info(msg("run_binary_for_target") + target.str() + msg("run_cannot_run_on_host"));
            return 0;
        }
    }

    rex_ok(msg("run_compiled_running") + "\n");
    std::cout << DIVIDER << "\n";

    int raw_ret = run_binary(res.binary, prog_args);

    std::cout << "\n" << DIVIDER << "\n";

    // Decode exit code and detect signals
    if (rex_signal::was_signaled(raw_ret)) {
        int sig = rex_signal::signal_number(raw_ret);
        if (sig == SIGINT)
            rex_warn(msg("run_interrupted"));
        else
            rex_warn(msg("signal_terminated"));
        rex_info(msg("run_exit_code") + std::to_string(128 + sig));
        return 128 + sig;
    }

    int exit_code = rex_signal::decode_exit_code(raw_ret);
    rex_info(msg("run_exit_code") + std::to_string(exit_code));
    return exit_code;
}

// ─── cmd: build ───────────────────────────────────────────────
int cmd_build(const std::vector<std::string>& args) {
    if (!fs::exists("rex.toml")) {
        rex_err(msg("build_no_toml"));
        return 1;
    }

    RexConfig cfg = parse_config("rex.toml");

    // Allow --target= override from command line
    for (auto& a : args) {
        if (a.rfind("--target=", 0) == 0) {
            cfg.target = a.substr(9);
        }
    }

    auto res = compile_project(cfg);

    if (!res.success) {
        rex_err(msg("build_failed"));
        return 1;
    }

    rex_ok(msg("build_complete") + res.binary);
    if (!cfg.target.empty()) {
        TargetTriple target = parse_target_triple(cfg.target);
        if (!is_native_target(target))
            rex_info(msg("build_target") + target.str());
    }
    return 0;
}

// ─── cmd: exec ────────────────────────────────────────────────
int cmd_exec(const std::vector<std::string>& args) {
    if (!fs::exists("rex.toml")) {
        rex_err(msg("exec_no_toml"));
        return 1;
    }

    RexConfig cfg = parse_config("rex.toml");
    std::string binary = cfg.output;
#ifdef _WIN32
    if (!ends_with(binary, ".exe")) binary += ".exe";
#endif

    if (!fs::exists(binary)) {
        rex_err(msg("exec_binary_not_found") + binary + msg("exec_binary_hint"));
        return 1;
    }

    rex_ok(msg("exec_running") + binary + "\n");
    std::cout << DIVIDER << "\n";
    int raw_ret = run_binary(binary, args);
    std::cout << "\n" << DIVIDER << "\n";

    // Decode exit code and detect signals
    if (rex_signal::was_signaled(raw_ret)) {
        int sig = rex_signal::signal_number(raw_ret);
        if (sig == SIGINT)
            rex_warn(msg("run_interrupted"));
        else
            rex_warn(msg("signal_terminated"));
        rex_info(msg("run_exit_code") + std::to_string(128 + sig));
        return 128 + sig;
    }

    int exit_code = rex_signal::decode_exit_code(raw_ret);
    rex_info(msg("run_exit_code") + std::to_string(exit_code));
    return exit_code;
}

// ─── cmd: add ─────────────────────────────────────────────────
int cmd_add(const std::vector<std::string>& args) {
    if (args.empty()) {
        rex_err(msg("add_usage"));
        return 1;
    }

    RexConfig cfg;
    if (fs::exists("rex.toml")) cfg = parse_config("rex.toml");

    std::string lib = args[0];

    // If it contains '/', treat as GitHub repo
    if (lib.find('/') != std::string::npos) {
        return pkg_add_github(lib, cfg) ? 0 : 1;
    }

    return pkg_add(lib, cfg) ? 0 : 1;
}

// ─── cmd: remove ──────────────────────────────────────────────
int cmd_remove(const std::vector<std::string>& args) {
    if (args.empty()) {
        rex_err(msg("remove_usage"));
        return 1;
    }

    RexConfig cfg;
    if (fs::exists("rex.toml")) cfg = parse_config("rex.toml");

    return pkg_remove(args[0], cfg) ? 0 : 1;
}

// ─── cmd: clean ───────────────────────────────────────────────
int cmd_clean(const std::vector<std::string>&) {
    if (fs::exists("build")) {
        fs::remove_all("build");
        rex_ok(msg("clean_done"));
    } else {
        rex_info(msg("clean_nothing"));
    }
    return 0;
}

// ─── cmd: search ──────────────────────────────────────────────
int cmd_search(const std::vector<std::string>& args) {
    std::string query = args.empty() ? "" : args[0];

    std::cout << COLOR_BOLD << "\n" << msg("search_results") << "\n" << COLOR_RESET;
    bool found = false;

    // Search built-in registry
    auto registry = get_registry();
    for (auto& [name, info] : registry) {
        if (query.empty() || name.find(query) != std::string::npos ||
            info.description.find(query) != std::string::npos) {
            std::string kind = info.header_only ? "[header-only]" : "[compiled]";
            std::cout << "  " << COLOR_CYAN << name << COLOR_RESET
                      << " " << COLOR_YELLOW << kind << COLOR_RESET
                      << "  " << info.description << "\n";
            found = true;
        }
    }

    // Search community registry
    auto community = fetch_community_registry();
    for (auto& pkg : community) {
        if (query.empty() || pkg.name.find(query) != std::string::npos ||
            pkg.description.find(query) != std::string::npos) {
            std::cout << "  " << COLOR_CYAN << pkg.name << COLOR_RESET
                      << " " << COLOR_YELLOW << "[community]" << COLOR_RESET
                      << " v" << pkg.version
                      << "  " << pkg.description << "\n";
            found = true;
        }
    }

    if (!found) {
        if (query.empty())
            rex_info(msg("search_no_results"));
        else
            rex_info(msg("search_no_match") + query + "'.");
    }

    std::cout << "\n";
    return 0;
}

// ─── MAIN ─────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    enable_ansi_colors();
    rex_i18n::init_language();
    rex_signal::install_signal_handlers();

#ifdef _WIN32
    // If launched by double-click (no parent console), run installer UI
    if (argc == 1 && is_double_clicked()) {
        run_windows_setup(REX_VERSION);
        return 0;
    }
#endif

    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    // --lang=XX flag: override language from command line
    for (auto it = args.begin(); it != args.end(); ) {
        if (it->rfind("--lang=", 0) == 0) {
            rex_i18n::set_language(it->substr(7));
            it = args.erase(it);
        } else {
            ++it;
        }
    }
    // Also check if it's a top-level --lang=XX before the command
    if (command.rfind("--lang=", 0) == 0) {
        rex_i18n::set_language(command.substr(7));
        if (argc < 3) { print_help(); return 0; }
        command = argv[2];
        args = std::vector<std::string>(argv + 3, argv + argc);
    }

    // Check for updates in the background (at most once per day)
    check_for_updates(REX_VERSION);

    if (command == "help"     || command == "--help" || command == "-h") {
        print_help();
    } else if (command == "version"  || command == "--version" || command == "-v") {
        std::cout << "REX " << REX_VERSION << "\n";
        std::cout << msg("version_os") << detect_os() << "\n";
        std::cout << msg("version_host") << get_host_target().str() << "\n";
        std::cout << msg("version_compiler") << detect_compiler() << "\n";
        std::cout << msg("version_default_std") << DEFAULT_CPP_STD << "\n";
        std::cout << msg("version_supported_stds") << "\n";
    } else if (command == "targets") {
        std::cout << COLOR_BOLD << "\n" << msg("targets_header") << "\n" << COLOR_RESET;
        auto host = get_host_target();
        auto targets = get_known_targets();
        const int col_width = 28;
        for (auto& t : targets) {
            auto parsed = parse_target_triple(t.triple);
            bool is_host = (parsed.arch == host.arch && parsed.os == host.os);
            std::cout << "  " << COLOR_CYAN << t.triple << COLOR_RESET;
            std::cout << std::string(std::max(1, col_width - (int)t.triple.size()), ' ');
            std::cout << t.description;
            if (is_host) std::cout << COLOR_GREEN << " " << msg("targets_host") << COLOR_RESET;
            std::cout << "\n";
        }
        std::cout << "\n" << COLOR_BOLD << msg("targets_usage") << "\n" << COLOR_RESET;
        std::cout << "  rex -b main.cpp --target=<triple>\n";
        std::cout << "  rex build --target=<triple>\n";
        std::cout << "  " << msg("targets_toml_hint") << "\n\n";
    } else if (command == "init") {
        return cmd_init(args);
    } else if (command == "run") {
        return cmd_run(args);
    } else if (command == "-b") {
        return cmd_brun(args);
    } else if (command == "repl") {
        rex::interp::Interpreter interp;
        interp.run_repl();
        return 0;
    } else if (command == "build") {
        return cmd_build(args);
    } else if (command == "exec") {
        return cmd_exec(args);
    } else if (command == "add") {
        return cmd_add(args);
    } else if (command == "remove" || command == "rm") {
        return cmd_remove(args);
    } else if (command == "list") {
        pkg_list();
    } else if (command == "registry") {
        pkg_registry();
    } else if (command == "search") {
        return cmd_search(args);
    } else if (command == "clean") {
        return cmd_clean(args);
    } else if (command == "update") {
        return cmd_update(REX_VERSION);
    } else if (command == "install" || command == "setup") {
#ifdef _WIN32
        run_windows_setup(REX_VERSION);
        return 0;
#else
        return run_unix_setup(REX_VERSION);
#endif
    } else {
        rex_err(msg("unknown_command") + command);
        std::cout << msg("run_help_hint") << "\n";
        return 1;
    }

    return 0;
}
