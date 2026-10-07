#include "translate.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: shadercc <file.ir>   (SPIR-V assembly goes to stdout)\n"; return 1; }
    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "error: cannot open " << argv[1] << "\n"; return 1; }
    std::string ir((std::istreambuf_iterator<char>(f)), {});
    std::string spv, err;
    if (!translate_ir_to_spvasm(ir, spv, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    std::cout << spv;
    return 0;
}
