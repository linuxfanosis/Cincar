#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Elf64Header {
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
};

struct Elf64Phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

std::optional<Elf64Header> parse_elf_header(const std::vector<uint8_t>& data,
                                            std::string& error);

std::optional<std::vector<Elf64Phdr>> parse_program_headers(
    const std::vector<uint8_t>& data, const Elf64Header& hdr, std::string& error);
