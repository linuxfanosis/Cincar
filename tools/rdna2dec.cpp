#include "rdna2.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

// usage: rdna2dec program.hex --inputs N --out vK     (prints the decoded program as shader IR)
//        rdna2dec --list-supported                    (prints: NAME ENCODING OPCODE)
// The .hex file holds 32-bit instruction words in hex, whitespace separated, '#' starts a comment.
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--list-supported") {
        for (const auto& s : rdna2_supported()) std::printf("%s %s %d\n", s.name, s.encoding, s.opcode);
        return 0;
    }
    if (argc < 2) { std::cerr << "usage: rdna2dec <file.hex> --inputs N --out vK\n"; return 1; }

    Rdna2Options opts;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--inputs" && i + 1 < argc) opts.num_inputs = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) {
            std::string r = argv[++i];
            if (r.size() < 2 || r[0] != 'v') { std::cerr << "error: --out expects a register like v3\n"; return 1; }
            opts.out_vgpr = std::atoi(r.c_str() + 1);
        } else { std::cerr << "error: unknown argument '" << a << "'\n"; return 1; }
    }

    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "error: cannot open " << argv[1] << "\n"; return 1; }
    std::vector<uint32_t> words;
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream ls(line);
        for (std::string tok; ls >> tok;) {
            char* end = nullptr;
            unsigned long v = std::strtoul(tok.c_str(), &end, 16);
            if (end == tok.c_str() || *end != 0 || v > 0xFFFFFFFFul) {
                std::cerr << "error: bad hex word '" << tok << "'\n";
                return 1;
            }
            words.push_back((uint32_t)v);
        }
    }

    Program prog;
    std::string err;
    if (!decode_rdna2(words, opts, prog, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    std::cout << print_ir(prog);
    return 0;
}
