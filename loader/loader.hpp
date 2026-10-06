#pragma once
#include "elf.hpp"

// What the runtime needs to know about a loaded image.
struct LoadInfo {
    uint64_t entry = 0;
    uint64_t phdr = 0;      // address of the program headers in guest memory
    uint64_t phent = 0;
    uint64_t phnum = 0;
};

// Maps the LOAD segments of a static, non-PIE x86-64 ELF into this process.
bool load_image(const std::vector<uint8_t>& data, LoadInfo& info, std::string& error);
