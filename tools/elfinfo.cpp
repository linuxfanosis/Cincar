#include "elf.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>

static const char* type_name(uint32_t t) {
    switch (t) {
        case 1: return "LOAD";
        case 2: return "DYNAMIC";
        case 3: return "INTERP";
        case 4: return "NOTE";
        case 6: return "PHDR";
        case 7: return "TLS";
        case 0x6474e550: return "GNU_EH_FRAME";
        case 0x6474e551: return "GNU_STACK";
        case 0x6474e552: return "GNU_RELRO";
        case 0x6474e553: return "GNU_PROPERTY";
        default: return "OTHER";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: elfinfo <file>\n"; return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    std::string err;
    auto h = parse_elf_header(data, err);
    if (!h) { std::cerr << "error: " << err << "\n"; return 1; }
    std::printf("type 0x%x  machine 0x%x  entry 0x%llx  segments %u\n\n",
                h->type, h->machine, (unsigned long long)h->entry, h->phnum);

    auto ph = parse_program_headers(data, *h, err);
    if (!ph) { std::cerr << "error: " << err << "\n"; return 1; }
    std::printf("%-14s %-4s %-10s %-10s %-8s %-8s\n",
                "type", "flg", "offset", "vaddr", "filesz", "memsz");
    for (const auto& p : *ph) {
        char flg[4] = {(p.flags & 4) ? 'R' : '-', (p.flags & 2) ? 'W' : '-',
                       (p.flags & 1) ? 'X' : '-', 0};
        std::printf("%-14s %-4s 0x%-8llx 0x%-8llx 0x%-6llx 0x%-6llx\n",
                    type_name(p.type), flg,
                    (unsigned long long)p.offset, (unsigned long long)p.vaddr,
                    (unsigned long long)p.filesz, (unsigned long long)p.memsz);
    }
}
