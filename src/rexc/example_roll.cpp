// example_roll.cpp  –  Dice rolling library demo for REX/rexc
//
// Demonstrates the built-in roll:: namespace provided by the REXC
// runtime.  Compile with:
//   rex run example_roll.cpp
//
// roll::dX()    – roll a single X-sided die
// roll::dX(n)   – roll n X-sided dice and return their sum
// roll::roll(f,n) – roll n f-sided dice and return a detailed result struct
// roll::advantage()    – roll 2d20, keep higher
// roll::disadvantage() – roll 2d20, keep lower
// roll::chance(p)      – return 1 with probability p% (0-100)
// roll::critical()     – return 1 if a d20 roll equals 20
// roll::parse("NdF+M") – evaluate dice-notation string

#include <iostream>
#include <string>
#include <vector>

int main() {
    // ── Basic die rolls ──────────────────────────────────────────────
    int d6  = roll::d6();
    int d20 = roll::d20();
    std::cout << "d6  = " << d6  << "\n";
    std::cout << "d20 = " << d20 << "\n";

    // ── Multiple dice: roll 3d8 ──────────────────────────────────────
    int three_d8 = roll::d8(3);
    std::cout << "3d8 = " << three_d8 << "\n";

    // ── Full roll result (per-die breakdown) ─────────────────────────
    auto result = roll::roll(6, 4);
    std::cout << result.to_string() << "\n";
    std::cout << "  Max single die: " << result.max() << "\n";
    std::cout << "  Min single die: " << result.min() << "\n";

    // ── Advantage / disadvantage (2d20) ─────────────────────────────
    int adv    = roll::advantage();
    int disadv = roll::disadvantage();
    std::cout << "Advantage:    " << adv    << "\n";
    std::cout << "Disadvantage: " << disadv << "\n";

    // ── Critical hit / fail ──────────────────────────────────────────
    if (roll::critical()) {
        std::cout << "CRITICAL HIT!\n";
    } else {
        std::cout << "No critical this turn.\n";
    }

    // ── Percentage chance ────────────────────────────────────────────
    if (roll::chance(50)) {
        std::cout << "50% chance succeeded\n";
    } else {
        std::cout << "50% chance failed\n";
    }

    // ── Dice notation parser ─────────────────────────────────────────
    int custom = roll::parse("2d6+3");
    std::cout << "2d6+3 = " << custom << "\n";

    return 0;
}
