// End-to-end pipeline test: parse animals.cpp and dump generated C
#include "rexc.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    std::string input = (argc > 1) ? argv[1] : "src/rexc/test_animals.cpp";
    std::string output = (argc > 2) ? argv[2] : "build/test_animals.rexc.c";

    // Read source
    std::ifstream f(input);
    if (!f) { std::cerr << "Cannot open " << input << "\n"; return 1; }
    std::ostringstream ss; ss << f.rdbuf();
    std::string src = ss.str();

    // Parse
    std::vector<rexc::Diagnostic> diags;
    auto tu = rexc::parse_translation_unit(src, input, diags);

    // Semantic
    rexc::SemanticAnalyzer sem;
    sem.analyze(*tu);
    for (auto& d : sem.diagnostics()) diags.push_back(d);

    // Print diagnostics
    for (auto& d : diags) {
        if (d.severity == rexc::Diagnostic::Severity::Error)
            std::cerr << "ERROR: " << d.message << "\n";
        else if (d.severity == rexc::Diagnostic::Severity::Warning)
            std::cerr << "WARN:  " << d.message << "\n";
    }

    // Codegen
    rexc::CodeGenerator gen(sem.context(), "src/rexc/runtime.h");
    std::string c_src = gen.generate_source(*tu);

    // Write
    std::ofstream out(output);
    out << c_src;
    std::cout << "Generated: " << output << " (" << c_src.size() << " bytes)\n";

    // Try to compile the C code  
    std::string runtime_dir = "src/rexc";
    std::string cmd = "cc -x c -std=gnu11 " + output
                    + " -I " + runtime_dir
                    + " -o build/test_animals"
                    + " -Wall -Wno-unused-variable -Wno-unused-function"
                    + " -Wno-incompatible-pointer-types"
                    + " -lm 2>&1";
    std::cout << "Compiling: " << cmd << "\n";
    int ret = system(cmd.c_str());
    if (ret == 0) {
        std::cout << "C compilation: OK\n";
        std::cout << "Running binary:\n";
        system("./build/test_animals");
    } else {
        std::cout << "C compilation failed with code " << ret << "\n";
        // Show the generated C for debugging
        std::cout << "\n--- Generated C source ---\n" << c_src << "\n";
    }
    return ret == 0 ? 0 : 1;
}
