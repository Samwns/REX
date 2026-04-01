/**
 * example.cpp — Demonstração da biblioteca termcolor
 *
 * Compile:
 *   g++ -std=c++17 example.cpp -o example && ./example
 */

#include "termcolor.hpp"
#include <iostream>

using namespace termcolor;

int main() {

    // ── Cores básicas de foreground ───────────────────────────────────────
    std::cout << "\n=== Cores de Texto (Foreground) ===\n\n";

    std::cout << fg::red     << "  Vermelho   " << reset;
    std::cout << fg::green   << "  Verde      " << reset;
    std::cout << fg::yellow  << "  Amarelo    " << reset;
    std::cout << fg::blue    << "  Azul       " << reset;
    std::cout << fg::magenta << "  Magenta    " << reset;
    std::cout << fg::cyan    << "  Ciano      " << reset;
    std::cout << fg::white   << "  Branco     " << reset;
    std::cout << "\n";

    // ── Cores brilhantes ─────────────────────────────────────────────────
    std::cout << "\n=== Cores Brilhantes ===\n\n";

    std::cout << fg::bright_red     << "  Vermelho   " << reset;
    std::cout << fg::bright_green   << "  Verde      " << reset;
    std::cout << fg::bright_yellow  << "  Amarelo    " << reset;
    std::cout << fg::bright_blue    << "  Azul       " << reset;
    std::cout << fg::bright_magenta << "  Magenta    " << reset;
    std::cout << fg::bright_cyan    << "  Ciano      " << reset;
    std::cout << fg::bright_white   << "  Branco     " << reset;
    std::cout << "\n";

    // ── Cores de background ───────────────────────────────────────────────
    std::cout << "\n=== Cores de Fundo (Background) ===\n\n";

    std::cout << bg::red     << fg::white << "  Vermelho   " << reset << " ";
    std::cout << bg::green   << fg::black << "  Verde      " << reset << " ";
    std::cout << bg::yellow  << fg::black << "  Amarelo    " << reset << " ";
    std::cout << bg::blue    << fg::white << "  Azul       " << reset << " ";
    std::cout << bg::magenta << fg::white << "  Magenta    " << reset << " ";
    std::cout << bg::cyan    << fg::black << "  Ciano      " << reset;
    std::cout << "\n";

    // ── Estilos de texto ──────────────────────────────────────────────────
    std::cout << "\n=== Estilos de Texto ===\n\n";

    std::cout << bold          << "  Negrito (bold)            " << reset << "\n";
    std::cout << dim           << "  Escuro (dim)              " << reset << "\n";
    std::cout << italic        << "  Itálico (italic)          " << reset << "\n";
    std::cout << underline     << "  Sublinhado (underline)    " << reset << "\n";
    std::cout << blink         << "  Piscando (blink)          " << reset << "\n";
    std::cout << strikethrough << "  Tachado (strikethrough)   " << reset << "\n";
    std::cout << reverse       << "  Invertido (reverse)       " << reset << "\n";

    // ── Combinações ──────────────────────────────────────────────────────
    std::cout << "\n=== Combinações ===\n\n";

    std::cout << fg::cyan << bold << underline
              << "  Ciano + Negrito + Sublinhado  "
              << reset << "\n";

    std::cout << bg::blue << fg::bright_white << bold
              << "  Fundo Azul + Texto Branco Brilhante  "
              << reset << "\n";

    // ── True Color RGB ────────────────────────────────────────────────────
    std::cout << "\n=== True Color (RGB) ===\n\n";

    std::cout << fg::rgb(255, 100,  50) << "  RGB Laranja        " << reset;
    std::cout << fg::rgb( 50, 200, 255) << "  RGB Azul Claro     " << reset;
    std::cout << fg::rgb(180,  50, 255) << "  RGB Roxo           " << reset;
    std::cout << "\n";

    std::cout << bg::rgb(255, 80,  80) << fg::white << "  BG Vermelho RGB   " << reset;
    std::cout << bg::rgb( 30, 180, 80) << fg::black << "  BG Verde RGB      " << reset;
    std::cout << "\n";

    // ── Funções utilitárias ───────────────────────────────────────────────
    std::cout << "\n=== colored() e styled() ===\n\n";

    std::cout << colored("  Texto em Magenta com reset automático  ", fg::magenta) << "\n";
    std::cout << colored("  Texto branco em fundo vermelho  ", fg::white, bg::red) << "\n";
    std::cout << styled("  Ciano + Negrito com reset automático  ", fg::cyan, bold) << "\n";

    // ── Print helpers ─────────────────────────────────────────────────────
    std::cout << "\n=== Helpers de Print ===\n\n";

    print_error  ("Algo deu errado!");
    print_success("Operação concluída com sucesso!");
    print_warning("Atenção: isso pode causar problemas.");
    print_info   ("Versão 1.0.0 carregada.");

    std::cout << "\n";
    return 0;
}
