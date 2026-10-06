#pragma once
#include "elf.hpp"

// Maps the LOAD segments of a static, non-PIE x86-64 ELF into this process.
// On success returns true and sets `entry` to the guest entry point.
bool load_image(const std::vector<uint8_t>& data, uint64_t& entry, std::string& error);
