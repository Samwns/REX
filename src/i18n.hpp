#pragma once
/*
 * i18n.hpp  –  Internationalization support for REX
 *
 * Provides:
 *   - Language detection (system locale, REX_LANG env var)
 *   - Message translation (key → localized string)
 *   - Built-in support for: English (en), Português-BR (pt)
 */

#include <string>
#include <map>
#include <cstdlib>

namespace rex_i18n {

// ─── Supported languages ─────────────────────────────────────
enum class Lang { EN, PT };

// ─── Message keys ────────────────────────────────────────────
// Each key maps to a localized string in each language.
using MessageMap = std::map<std::string, std::string>;

inline MessageMap english_messages() {
    return {
        // General
        {"banner_tagline",          " Compile. Run. Execute."},
        {"unknown_command",         "Unknown command: "},
        {"run_help_hint",           "Run: rex help"},

        // Help
        {"help_usage",              "USAGE:"},
        {"help_commands",           "COMMANDS:"},
        {"help_examples",           "EXAMPLES:"},
        {"help_run_desc",           "Interpret a file directly (no compilation)"},
        {"help_brun_desc",          "Compile and immediately run a single file (-b)"},
        {"help_repl_desc",          "Start interactive interpreter"},
        {"help_std_desc",           "Set C++ standard (c++11, c++14, c++17, c++20, c++23)"},
        {"help_target_desc",        "Cross-compile for a target (e.g. aarch64-linux-gnu)"},
        {"help_build_desc",         "Build project defined in rex.toml"},
        {"help_target_override",    "Override target from rex.toml"},
        {"help_exec_desc",          "Execute the compiled project binary"},
        {"help_init_desc",          "Initialize a new REX project"},
        {"help_add_desc",           "Add a library (registry or user/repo@version)"},
        {"help_remove_desc",        "Remove an installed library"},
        {"help_list_desc",          "List installed libraries"},
        {"help_registry_desc",      "Show all available libraries"},
        {"help_search_desc",        "Search for packages in all registries"},
        {"help_clean_desc",         "Remove build artifacts"},
        {"help_targets_desc",       "List supported cross-compilation targets"},
        {"help_update_desc",        "Check for REX & library updates"},
        {"help_install_desc",       "Run the REX installer"},
        {"help_version_desc",       "Show REX version"},
        {"help_help_desc",          "Show this help"},
        {"help_lang_desc",          "Set output language (en, pt)"},

        // cmd: run
        {"run_usage",               "Usage: rex run <file.cpp> [--std=c++XX] [--target=TRIPLE]"},
        {"b_usage",                 "Usage: rex -b <file.cpp> [--std=c++XX] [--target=TRIPLE]"},
        {"run_invalid_std",         "Invalid C++ standard: "},
        {"run_supported_stds",      "Supported: c++11, c++14, c++17, c++20, c++23"},
        {"run_empty_target",        "Empty target triple. Use --target=<arch-os-abi>"},
        {"run_target_hint",         "Run 'rex targets' to see supported targets."},
        {"run_file_not_found",      "File not found: "},
        {"run_standard",            "Standard : "},
        {"run_target",              "Target   : "},
        {"run_compiled_running",    "Compiled. Running..."},
        {"run_compilation_failed",  "Compilation failed."},
        {"run_cross_compiled",      "Cross-compiled: "},
        {"run_binary_for_target",   "Binary built for "},
        {"run_cannot_run_on_host",  " (cannot run on host)"},
        {"run_exit_code",           "Exit code: "},
        {"run_interrupted",         "Program interrupted (Ctrl+C)."},

        // cmd: build
        {"build_no_toml",           "No rex.toml found. Run: rex init"},
        {"build_failed",            "Build failed."},
        {"build_complete",          "Build complete: "},
        {"build_target",            "Target: "},

        // cmd: exec
        {"exec_no_toml",            "No rex.toml found. Run: rex init"},
        {"exec_binary_not_found",   "Binary not found: "},
        {"exec_binary_hint",        ". Run: rex build"},
        {"exec_running",            "Running: "},

        // cmd: init
        {"init_already_exists",     "rex.toml already exists. Skipping."},
        {"init_success",            "Project initialized!"},
        {"init_structure",          "  Structure:"},
        {"init_run_build",          " to compile"},
        {"init_run_exec",           " to execute"},

        // cmd: clean
        {"clean_done",              "Cleaned build/"},
        {"clean_nothing",           "Nothing to clean."},

        // cmd: add / remove
        {"add_usage",               "Usage: rex add <library> or rex add <user/repo>"},
        {"remove_usage",            "Usage: rex remove <library>"},

        // cmd: search
        {"search_results",          "Search results:"},
        {"search_no_results",       "No packages found."},
        {"search_no_match",         "No packages found matching '"},

        // cmd: targets
        {"targets_header",          "Supported cross-compilation targets:"},
        {"targets_host",            "(host)"},
        {"targets_usage",           "Usage:"},
        {"targets_toml_hint",       "Or set 'target' in rex.toml [project] section"},

        // version
        {"version_os",              "OS: "},
        {"version_host",            "Host: "},
        {"version_compiler",        "Compiler: "},
        {"version_default_std",     "Default C++ standard: "},
        {"version_supported_stds",  "Supported standards: c++11, c++14, c++17, c++20, c++23"},

        // compiler
        {"compiler_rexc_native",    "Compiler : rexc (native x86_64)"},
        {"compiler_rexc_backend",   "Compiler : rexc (backend: "},
        {"compiler_fallback",       "Compiler : "},
        {"compiler_platform",       "Platform : "},
        {"compiler_command",        "Command  : "},
        {"compiler_falling_back_c", "Falling back to C backend..."},
        {"compiler_falling_back",   "Falling back to system C++ compiler..."},
        {"compiler_no_cross",       "No cross-compiler found for target: "},
        {"compiler_install_hint",   "Install a cross-toolchain, e.g.: sudo apt install gcc-"},
        {"compiler_clang_hint",     "Or install clang which supports --target=<triple>"},
        {"compiler_no_compiler",    "No compiler found! Install gcc/clang (for rexc) or g++/clang++."},
        {"compiler_building",       "Building: "},
        {"compiler_no_cpp_files",   "No .cpp files found in src/"},

        // signal
        {"signal_interrupted",      "Process interrupted by user (Ctrl+C)."},
        {"signal_terminated",       "Process terminated."},
    };
}

inline MessageMap portuguese_messages() {
    return {
        // General
        {"banner_tagline",          " Compilar. Executar. Rodar."},
        {"unknown_command",         "Comando desconhecido: "},
        {"run_help_hint",           "Execute: rex help"},

        // Help
        {"help_usage",              "USO:"},
        {"help_commands",           "COMANDOS:"},
        {"help_examples",           "EXEMPLOS:"},
        {"help_run_desc",           "Interpretar diretamente, sem compilar"},
        {"help_brun_desc",          "Compilar e executar imediatamente (-b)"},
        {"help_repl_desc",          "Iniciar interpretador interativo"},
        {"help_std_desc",           "Definir padrão C++ (c++11, c++14, c++17, c++20, c++23)"},
        {"help_target_desc",        "Cross-compilar para um alvo (ex: aarch64-linux-gnu)"},
        {"help_build_desc",         "Compilar projeto definido no rex.toml"},
        {"help_target_override",    "Substituir alvo do rex.toml"},
        {"help_exec_desc",          "Executar o binário compilado do projeto"},
        {"help_init_desc",          "Inicializar um novo projeto REX"},
        {"help_add_desc",           "Adicionar uma biblioteca (registro ou user/repo@versão)"},
        {"help_remove_desc",        "Remover uma biblioteca instalada"},
        {"help_list_desc",          "Listar bibliotecas instaladas"},
        {"help_registry_desc",      "Mostrar todas as bibliotecas disponíveis"},
        {"help_search_desc",        "Buscar pacotes em todos os registros"},
        {"help_clean_desc",         "Remover artefatos de compilação"},
        {"help_targets_desc",       "Listar alvos de cross-compilação suportados"},
        {"help_update_desc",        "Verificar atualizações do REX e bibliotecas"},
        {"help_install_desc",       "Executar o instalador do REX"},
        {"help_version_desc",       "Mostrar versão do REX"},
        {"help_help_desc",          "Mostrar esta ajuda"},
        {"help_lang_desc",          "Definir idioma de saída (en, pt)"},

        // cmd: run
        {"run_usage",               "Uso: rex run <arquivo.cpp> [--std=c++XX] [--target=TRIPLE]"},
        {"b_usage",                 "Uso: rex -b <arquivo.cpp> [--std=c++XX] [--target=TRIPLE]"},
        {"run_invalid_std",         "Padrão C++ inválido: "},
        {"run_supported_stds",      "Suportados: c++11, c++14, c++17, c++20, c++23"},
        {"run_empty_target",        "Triple do alvo vazio. Use --target=<arch-os-abi>"},
        {"run_target_hint",         "Execute 'rex targets' para ver os alvos suportados."},
        {"run_file_not_found",      "Arquivo não encontrado: "},
        {"run_standard",            "Padrão   : "},
        {"run_target",              "Alvo     : "},
        {"run_compiled_running",    "Compilado. Executando..."},
        {"run_compilation_failed",  "Compilação falhou."},
        {"run_cross_compiled",      "Cross-compilado: "},
        {"run_binary_for_target",   "Binário criado para "},
        {"run_cannot_run_on_host",  " (não pode rodar no host)"},
        {"run_exit_code",           "Código de saída: "},
        {"run_interrupted",         "Programa interrompido (Ctrl+C)."},

        // cmd: build
        {"build_no_toml",           "rex.toml não encontrado. Execute: rex init"},
        {"build_failed",            "Compilação falhou."},
        {"build_complete",          "Compilação concluída: "},
        {"build_target",            "Alvo: "},

        // cmd: exec
        {"exec_no_toml",            "rex.toml não encontrado. Execute: rex init"},
        {"exec_binary_not_found",   "Binário não encontrado: "},
        {"exec_binary_hint",        ". Execute: rex build"},
        {"exec_running",            "Executando: "},

        // cmd: init
        {"init_already_exists",     "rex.toml já existe. Ignorando."},
        {"init_success",            "Projeto inicializado!"},
        {"init_structure",          "  Estrutura:"},
        {"init_run_build",          " para compilar"},
        {"init_run_exec",           " para executar"},

        // cmd: clean
        {"clean_done",              "build/ limpo"},
        {"clean_nothing",           "Nada para limpar."},

        // cmd: add / remove
        {"add_usage",               "Uso: rex add <biblioteca> ou rex add <user/repo>"},
        {"remove_usage",            "Uso: rex remove <biblioteca>"},

        // cmd: search
        {"search_results",          "Resultados da busca:"},
        {"search_no_results",       "Nenhum pacote encontrado."},
        {"search_no_match",         "Nenhum pacote encontrado para '"},

        // cmd: targets
        {"targets_header",          "Alvos de cross-compilação suportados:"},
        {"targets_host",            "(host)"},
        {"targets_usage",           "Uso:"},
        {"targets_toml_hint",       "Ou defina 'target' na seção [project] do rex.toml"},

        // version
        {"version_os",              "SO: "},
        {"version_host",            "Host: "},
        {"version_compiler",        "Compilador: "},
        {"version_default_std",     "Padrão C++ padrão: "},
        {"version_supported_stds",  "Padrões suportados: c++11, c++14, c++17, c++20, c++23"},

        // compiler
        {"compiler_rexc_native",    "Compilador : rexc (nativo x86_64)"},
        {"compiler_rexc_backend",   "Compilador : rexc (backend: "},
        {"compiler_fallback",       "Compilador : "},
        {"compiler_platform",       "Plataforma : "},
        {"compiler_command",        "Comando    : "},
        {"compiler_falling_back_c", "Voltando para o backend C..."},
        {"compiler_falling_back",   "Voltando para compilador C++ do sistema..."},
        {"compiler_no_cross",       "Cross-compilador não encontrado para alvo: "},
        {"compiler_install_hint",   "Instale um cross-toolchain, ex: sudo apt install gcc-"},
        {"compiler_clang_hint",     "Ou instale clang que suporta --target=<triple>"},
        {"compiler_no_compiler",    "Compilador não encontrado! Instale gcc/clang (para rexc) ou g++/clang++."},
        {"compiler_building",       "Compilando: "},
        {"compiler_no_cpp_files",   "Nenhum arquivo .cpp encontrado em src/"},

        // signal
        {"signal_interrupted",      "Processo interrompido pelo usuário (Ctrl+C)."},
        {"signal_terminated",       "Processo terminado."},
    };
}

// ─── Global language state ──────────────────────────────────

inline Lang& current_lang() {
    static Lang lang = Lang::EN;
    return lang;
}

inline const MessageMap& english_messages_cached() {
    static MessageMap en_msgs = english_messages();
    return en_msgs;
}

inline const MessageMap& messages() {
    static MessageMap pt_msgs = portuguese_messages();
    switch (current_lang()) {
        case Lang::PT: return pt_msgs;
        default:       return english_messages_cached();
    }
}

// Get a translated message by key.  Falls back to English if key is
// missing in the current language, or returns the key itself as
// a last resort.
inline std::string msg(const std::string& key) {
    auto& m = messages();
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    // Fallback to English when using a non-English language
    if (current_lang() != Lang::EN) {
        auto& en = english_messages_cached();
        auto it2 = en.find(key);
        if (it2 != en.end()) return it2->second;
    }
    return key;
}

// ─── Detect language from environment ───────────────────────

inline Lang detect_language() {
    // 1. Check REX_LANG env var first (highest priority)
    const char* rex_lang = std::getenv("REX_LANG");
    if (rex_lang) {
        std::string lang(rex_lang);
        if (lang == "pt" || lang == "pt-BR" || lang == "pt_BR" ||
            lang == "pt-br" || lang == "pt_br")
            return Lang::PT;
        if (lang == "en" || lang == "en-US" || lang == "en_US")
            return Lang::EN;
    }

    // 2. Check system locale (LANG, LC_ALL, LC_MESSAGES)
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* val = std::getenv(var);
        if (val) {
            std::string locale(val);
            if (locale.find("pt_BR") != std::string::npos ||
                locale.find("pt_PT") != std::string::npos ||
                locale.rfind("pt", 0) == 0)
                return Lang::PT;
        }
    }

    return Lang::EN;
}

inline void init_language() {
    current_lang() = detect_language();
}

inline void set_language(const std::string& lang_str) {
    if (lang_str == "pt" || lang_str == "pt-BR" || lang_str == "pt_BR")
        current_lang() = Lang::PT;
    else
        current_lang() = Lang::EN;
}

inline std::string current_lang_name() {
    switch (current_lang()) {
        case Lang::PT: return "pt";
        default:       return "en";
    }
}

} // namespace rex_i18n
