#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Runs a static x86-64 ELF as a guest in a separate, traced process.
// Every guest syscall is intercepted and handled by our own code
// (runtime.cpp) instead of reaching the Linux kernel.
// Returns the guest's exit code, or -1 with `error` set.
int run_guest(const std::vector<uint8_t>& image, bool trace, std::string& error);
