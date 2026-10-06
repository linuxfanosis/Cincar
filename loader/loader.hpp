#pragma once
#include "elf.hpp"

// Maps a static, non-PIE x86-64 ELF into this process and jumps to its entry.
// Returns false with `error` set if loading fails. On success the guest
// normally exits the process itself and this never returns.
bool load_and_run(const std::vector<uint8_t>& data, std::string& error);
