#include "elf.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: elfinfo <file>\n"; return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    std::string err;
    auto h = parse_elf_header(data, err);
    if (!h) { std::cerr << "error: " << err << "\n"; return 1; }
    std::cout << std::hex
              << "type:      0x" << h->type << "\n"
              << "machine:   0x" << h->machine << " (0x3e = x86-64)\n"
              << "entry:     0x" << h->entry << "\n"
              << "ph offset: 0x" << h->phoff << "\n"
              << "ph count:  " << std::dec << h->phnum << "\n";
}
