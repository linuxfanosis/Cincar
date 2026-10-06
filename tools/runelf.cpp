#include "loader.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: runelf <file>\n"; return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    std::string err;
    std::fflush(stdout);
    if (!load_and_run(data, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    return 0;
}
