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

std::optional<std::vector<Elf64Phdr>> parse_program_headers(
    const std::vector<uint8_t>& d, const Elf64Header& h, std::string& error) {
    if (h.phentsize < 56) { error = "program header entry too small"; return std::nullopt; }
    // Check bounds before reading: never trust offsets from the file.
    if (h.phoff > d.size()) { error = "program header offset past end of file"; return std::nullopt; }
    uint64_t table_size = (uint64_t)h.phnum * h.phentsize;
    if (table_size > d.size() - h.phoff) {
        error = "program header table extends past end of file"; return std::nullopt;
    }

    std::vector<Elf64Phdr> out;
    out.reserve(h.phnum);
    for (uint16_t i = 0; i < h.phnum; ++i) {
        size_t o = h.phoff + (size_t)i * h.phentsize;
        Elf64Phdr p{};
        p.type   = read_le<uint32_t>(d, o + 0);
        p.flags  = read_le<uint32_t>(d, o + 4);
        p.offset = read_le<uint64_t>(d, o + 8);
        p.vaddr  = read_le<uint64_t>(d, o + 16);
        p.paddr  = read_le<uint64_t>(d, o + 24);
        p.filesz = read_le<uint64_t>(d, o + 32);
        p.memsz  = read_le<uint64_t>(d, o + 40);
        p.align  = read_le<uint64_t>(d, o + 48);
        out.push_back(p);
    }
    return out;
}
