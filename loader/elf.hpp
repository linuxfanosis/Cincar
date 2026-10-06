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

// Returns the parsed header, or nullopt with a message in `error`.
std::optional<Elf64Header> parse_elf_header(const std::vector<uint8_t>& data,
                                            std::string& error);
