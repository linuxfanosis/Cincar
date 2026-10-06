#include "runtime.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: runelf <file>   (CINCAR_TRACE=1 to log guest syscalls)\n"; return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    std::string err;
    int code = run_guest(data, std::getenv("CINCAR_TRACE") != nullptr, err);
    if (code < 0) { std::cerr << "error: " << err << "\n"; return 1; }
    return code;
}
