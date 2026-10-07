#include "translate.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

// usage: shaderrun kernel.ir name=1,2,3 other=4,5,6
// Runs the CPU reference evaluator and prints the output buffer.
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: shaderrun <file.ir> [name=v1,v2,...]...\n"; return 1; }
    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "error: cannot open " << argv[1] << "\n"; return 1; }
    std::string text((std::istreambuf_iterator<char>(f)), {});
    Program prog;
    std::string err;
    if (!parse_ir(text, prog, err)) { std::cerr << "error: " << err << "\n"; return 1; }

    std::map<std::string, std::vector<float>> inputs;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos) { std::cerr << "error: expected name=values, got '" << arg << "'\n"; return 1; }
        std::vector<float> vals;
        std::istringstream vs(arg.substr(eq + 1));
        for (std::string tok; std::getline(vs, tok, ',');) {
            char* end = nullptr;
            float v = std::strtof(tok.c_str(), &end);
            if (end == tok.c_str() || *end != 0) { std::cerr << "error: bad number '" << tok << "'\n"; return 1; }
            vals.push_back(v);
        }
        inputs[arg.substr(0, eq)] = vals;
    }

    std::vector<float> out;
    if (!evaluate(prog, inputs, out, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    for (size_t i = 0; i < out.size(); ++i) std::printf("%s%g", i ? " " : "", (double)out[i]);
    std::printf("\n");
    return 0;
}
