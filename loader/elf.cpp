#include "elf.hpp"
#include <cstring>

// Assumes a little-endian host, which is true for x86-64.
template <typename T>
static T read_le(const std::vector<uint8_t>& d, size_t off) {
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

std::optional<Elf64Header> parse_elf_header(const std::vector<uint8_t>& d,
                                            std::string& error) {
    if (d.size() < 64) { error = "file too small for an ELF64 header"; return std::nullopt; }
    if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') {
        error = "bad ELF magic"; return std::nullopt;
    }
    if (d[4] != 2) { error = "not a 64-bit ELF"; return std::nullopt; }
    if (d[5] != 1) { error = "not little-endian"; return std::nullopt; }

    Elf64Header h{};
    h.type      = read_le<uint16_t>(d, 16);
    h.machine   = read_le<uint16_t>(d, 18);
    h.entry     = read_le<uint64_t>(d, 24);
    h.phoff     = read_le<uint64_t>(d, 32);
    h.phentsize = read_le<uint16_t>(d, 54);
    h.phnum     = read_le<uint16_t>(d, 56);
    return h;
}
