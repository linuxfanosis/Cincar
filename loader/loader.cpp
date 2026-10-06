#include "loader.hpp"
#include <sys/mman.h>
#include <cstring>

static int prot_from_flags(uint32_t f) {
    int p = 0;
    if (f & 4) p |= PROT_READ;
    if (f & 2) p |= PROT_WRITE;
    if (f & 1) p |= PROT_EXEC;
    return p;
}

bool load_image(const std::vector<uint8_t>& d, LoadInfo& info, std::string& error) {
    auto h = parse_elf_header(d, error);
    if (!h) return false;
    if (h->machine != 0x3e) { error = "not an x86-64 executable"; return false; }
    if (h->type != 2) { error = "only static non-PIE (ET_EXEC) files supported for now"; return false; }
    auto ph = parse_program_headers(d, *h, error);
    if (!ph) return false;

    const uint64_t PAGE = 4096;
    for (const auto& p : *ph) {
        if (p.type == 3) { error = "dynamic executables (INTERP) not supported yet"; return false; }
        if (p.type != 1) continue;
        if (p.filesz > p.memsz) { error = "segment filesz > memsz"; return false; }
        if (p.memsz > (1ull << 32)) { error = "segment unreasonably large"; return false; }
        if (p.vaddr < 0x10000) { error = "segment address too low"; return false; }
        if (p.offset > d.size() || p.filesz > d.size() - p.offset) {
            error = "segment data extends past end of file"; return false;
        }
        uint64_t start = p.vaddr & ~(PAGE - 1);
        uint64_t end = (p.vaddr + p.memsz + PAGE - 1) & ~(PAGE - 1);
        void* m = mmap((void*)start, end - start, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m == MAP_FAILED) { error = "mmap failed (address range in use?)"; return false; }
        std::memcpy((void*)p.vaddr, d.data() + p.offset, p.filesz);
        // The segment holding file offset 0 contains the ELF + program headers.
        if (p.offset == 0 && h->phoff < p.filesz) info.phdr = p.vaddr + h->phoff;
        if (mprotect((void*)start, end - start, prot_from_flags(p.flags)) != 0) {
            error = "mprotect failed"; return false;
        }
    }
    info.entry = h->entry;
    info.phent = h->phentsize;
    info.phnum = h->phnum;
    return true;
}
