#include "runtime.hpp"
#include "loader.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace {

// ---- Guest memory layout: one reserved arena, managed by *our* allocator ----
//   [ARENA_BASE ........ BRK_END)   brk heap
//   [BRK_END ........ MMAP_END)     anonymous mmap area (bump allocator)
//   [MMAP_END ........ ARENA_END)   initial stack (grows down from ARENA_END)
constexpr uint64_t ARENA_BASE = 0x10000000;
constexpr uint64_t ARENA_SIZE = 256ull << 20;
constexpr uint64_t BRK_END    = ARENA_BASE + (64ull << 20);
constexpr uint64_t ARENA_END  = ARENA_BASE + ARENA_SIZE;
constexpr uint64_t MMAP_END   = ARENA_END - (8ull << 20);
constexpr uint64_t PAGE       = 4096;

struct Ctx {
    pid_t pid;
    bool trace;
    uint64_t brk_cur = ARENA_BASE;
    uint64_t mmap_cur = BRK_END;
};

struct Result { bool exited; int code; long ret; };
using Regs = user_regs_struct;
using Handler = Result (*)(Ctx&, Regs&);
struct Entry { const char* name; Handler fn; };

bool read_guest(pid_t pid, uint64_t addr, void* out, size_t len) {
    iovec local{out, len};
    iovec remote{(void*)addr, len};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}
bool write_guest(pid_t pid, uint64_t addr, const void* in, size_t len) {
    iovec local{(void*)in, len};
    iovec remote{(void*)addr, len};
    return process_vm_writev(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}

// ---- The system layer: one function per guest syscall ----------------------
Result ok(long v = 0) { return {false, 0, v}; }
Result err(int e)     { return {false, 0, -e}; }

Result sys_write(Ctx& c, Regs& r) {
    uint64_t fd = r.rdi, buf = r.rsi, len = r.rdx;
    if ((fd != 1 && fd != 2) || len > (1u << 20)) return err(EBADF);
    std::vector<char> tmp(len);
    if (len && !read_guest(c.pid, buf, tmp.data(), len)) return err(EFAULT);
    ssize_t n = ::write((int)fd, tmp.data(), len);
    return n < 0 ? err(errno) : ok(n);
}

Result sys_exit(Ctx&, Regs& r) { return {true, (int)(r.rdi & 0xff), 0}; }

Result sys_brk(Ctx& c, Regs& r) {
    uint64_t want = r.rdi;
    if (want >= ARENA_BASE && want <= BRK_END) c.brk_cur = want;
    return ok((long)c.brk_cur);
}

Result sys_mmap(Ctx& c, Regs& r) {
    uint64_t len = r.rsi;
    int flags = (int)r.r10;
    if (len == 0) return err(EINVAL);
    if (!(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) {
        std::fprintf(stderr, "[runtime] mmap: only non-fixed anonymous mappings supported (flags=0x%x)\n", flags);
        return err(EINVAL);
    }
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1);
    if (size > MMAP_END - c.mmap_cur) return err(ENOMEM);
    uint64_t addr = c.mmap_cur;
    c.mmap_cur += size;
    return ok((long)addr);   // fresh arena pages are already zero
}

Result sys_munmap(Ctx&, Regs&)   { return ok(); }   // memory is never reused yet
Result sys_mprotect(Ctx&, Regs&) { return ok(); }   // arena is RW; not enforced yet

// Thread-local storage: the guest sets its FS base through arch_prctl.
Result sys_arch_prctl(Ctx&, Regs& r) {
    constexpr uint64_t ARCH_SET_FS = 0x1002;
    if (r.rdi == ARCH_SET_FS) { r.fs_base = r.rsi; return ok(); }
    return err(EINVAL);
}

Result sys_set_tid_address(Ctx&, Regs&) { return ok(1); }   // our one guest thread has tid 1
Result sys_set_robust_list(Ctx&, Regs&) { return ok(); }
Result sys_rseq(Ctx&, Regs&)            { return err(ENOSYS); }   // optional; libc copes

Result sys_getrandom(Ctx& c, Regs& r) {
    uint64_t buf = r.rdi, len = r.rsi;
    if (len > (1u << 20)) return err(EINVAL);
    std::vector<uint8_t> tmp(len);
    uint64_t x = 0x9E3779B97F4A7C15ull;               // deterministic on purpose: reproducible runs
    for (auto& b : tmp) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b = (uint8_t)x; }
    if (len && !write_guest(c.pid, buf, tmp.data(), len)) return err(EFAULT);
    return ok((long)len);
}

// Minimal "stdout is a pipe" answer, so libc chooses full buffering.
Result fill_stat(Ctx& c, uint64_t addr) {
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    st.st_mode = S_IFIFO | 0600;
    st.st_blksize = 4096;
    return write_guest(c.pid, addr, &st, sizeof(st)) ? ok() : err(EFAULT);
}
Result sys_fstat(Ctx& c, Regs& r)      { return r.rdi <= 2 ? fill_stat(c, r.rsi) : err(EBADF); }
Result sys_newfstatat(Ctx& c, Regs& r) { return r.rdi <= 2 ? fill_stat(c, r.rdx) : err(ENOENT); }

Result sys_readlinkat(Ctx&, Regs&) { return err(ENOENT); }
Result sys_prlimit64(Ctx&, Regs&)  { return err(ENOSYS); }

// ---- HLE imports: the guest calls host functions *by name* -------------------
// A guest import stub puts the address of its function name in r10 and runs
// `syscall` with number HLE_CALL. We read the name, look it up in our own table
// and run the host function with the guest's SysV arguments (rdi, rsi, rdx).
// Note: the `syscall` instruction itself overwrites rcx and r11, so imports are
// limited to 3 register arguments for now (a trap-based stub would lift this).
// An unknown name fails loudly with ENOSYS, exactly like an unknown syscall.
constexpr long HLE_CALL = 0x4000;

// Read a NUL-terminated string from the guest, page-safe, at most `max` bytes.
bool read_guest_cstr(pid_t pid, uint64_t addr, std::string& out, size_t max) {
    out.clear();
    while (out.size() < max) {
        size_t chunk = std::min<uint64_t>(PAGE - (addr % PAGE), 256);
        char buf[256];
        if (!read_guest(pid, addr, buf, chunk)) return false;
        for (size_t i = 0; i < chunk; ++i) {
            if (buf[i] == 0) return true;
            out.push_back(buf[i]);
        }
        addr += chunk;
    }
    return false;   // too long / unterminated
}

using HleFn = long (*)(Ctx&, const Regs&);

long hle_puts(Ctx& c, const Regs& r) {
    std::string str;
    if (!read_guest_cstr(c.pid, r.rdi, str, 4096)) return -EFAULT;
    size_t n = str.size();
    str.push_back('\n');
    return ::write(1, str.data(), str.size()) < 0 ? -errno : (long)n;
}
long hle_add(Ctx&, const Regs& r) { return (long)r.rdi + (long)r.rsi; }

const std::unordered_map<std::string, HleFn>& hle_table() {
    static const std::unordered_map<std::string, HleFn> t = {
        {"cincar_puts", hle_puts},
        {"cincar_add",  hle_add},
    };
    return t;
}

Result sys_hle_call(Ctx& c, Regs& r) {
    std::string name;
    if (!read_guest_cstr(c.pid, r.r10, name, 64)) return err(EFAULT);
    auto it = hle_table().find(name);
    if (it == hle_table().end()) {
        std::fprintf(stderr, "[runtime] unresolved import '%s'\n", name.c_str());
        return err(ENOSYS);
    }
    if (c.trace)
        std::fprintf(stderr, "[hle] %s(%lld, %lld)\n", name.c_str(),
                     (long long)r.rdi, (long long)r.rsi);
    return ok(it->second(c, r));
}

const std::unordered_map<long, Entry>& syscall_table() {
    static const std::unordered_map<long, Entry> t = {
        {1,   {"write",           sys_write}},
        {5,   {"fstat",           sys_fstat}},
        {9,   {"mmap",            sys_mmap}},
        {10,  {"mprotect",        sys_mprotect}},
        {11,  {"munmap",          sys_munmap}},
        {12,  {"brk",             sys_brk}},
        {60,  {"exit",            sys_exit}},
        {158, {"arch_prctl",      sys_arch_prctl}},
        {218, {"set_tid_address", sys_set_tid_address}},
        {231, {"exit_group",      sys_exit}},
        {262, {"newfstatat",      sys_newfstatat}},
        {267, {"readlinkat",      sys_readlinkat}},
        {273, {"set_robust_list", sys_set_robust_list}},
        {302, {"prlimit64",       sys_prlimit64}},
        {318, {"getrandom",       sys_getrandom}},
        {334, {"rseq",            sys_rseq}},
        {HLE_CALL, {"hle_call",   sys_hle_call}},
    };
    return t;
}

Result dispatch(Ctx& c, Regs& r) {
    long nr = (long)r.orig_rax;
    auto it = syscall_table().find(nr);
    if (it == syscall_table().end()) {
        std::fprintf(stderr, "[runtime] unimplemented guest syscall %ld\n", nr);
        return err(ENOSYS);
    }
    if (c.trace)
        std::fprintf(stderr, "[guest] %s(%lld, %lld, %lld)\n", it->second.name,
                     (long long)r.rdi, (long long)r.rsi, (long long)r.rdx);
    return it->second.fn(c, r);
}

// Build the initial stack the kernel would normally give a new process:
// argc, argv[], envp[], and the auxiliary vector libc reads at startup.
// Runs in the child, where the arena is already mapped. Returns the new rsp.
uint64_t build_stack(const LoadInfo& li) {
    enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6,
           AT_ENTRY = 9, AT_SECURE = 23, AT_RANDOM = 25 };
    uint64_t sp = ARENA_END;
    sp -= 16;                                   // 16 "random" bytes for stack protector / malloc
    uint64_t rnd = sp;
    for (int i = 0; i < 16; ++i) ((uint8_t*)rnd)[i] = (uint8_t)(0xA5 ^ (i * 37));
    sp -= 16;
    uint64_t argv0 = sp;
    std::memcpy((void*)argv0, "guest", 6);

    uint64_t words[] = {
        1, argv0, 0,                 // argc, argv[0], argv terminator
        0,                           // envp terminator (empty environment)
        AT_PHDR, li.phdr, AT_PHENT, li.phent, AT_PHNUM, li.phnum,
        AT_PAGESZ, PAGE, AT_ENTRY, li.entry, AT_SECURE, 0, AT_RANDOM, rnd,
        AT_NULL, 0,
    };
    sp -= sizeof(words);
    sp &= ~15ull;                                // rsp must be 16-byte aligned at entry
    std::memcpy((void*)sp, words, sizeof(words));
    return sp;
}

} // namespace

int run_guest(const std::vector<uint8_t>& image, bool trace, std::string& error) {
    int errpipe[2];
    if (pipe(errpipe) != 0) { error = "pipe failed"; return -1; }

    pid_t pid = fork();
    if (pid < 0) { error = "fork failed"; return -1; }

    if (pid == 0) {
        close(errpipe[0]);
        LoadInfo li;
        std::string e;
        void* arena = mmap((void*)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
        if (arena == MAP_FAILED) e = "could not reserve guest memory arena";
        if (!e.empty() || !load_image(image, li, e)) {
            (void)!::write(errpipe[1], e.data(), e.size());
            _exit(127);
        }
        uint64_t sp = build_stack(li);
        close(errpipe[1]);
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        raise(SIGSTOP);
        // Enter the guest: fresh stack, rdx = 0 (no exit hook), never returns.
        asm volatile("mov %1, %%rsp\n\txor %%rbp, %%rbp\n\txor %%edx, %%edx\n\tjmp *%0"
                     :: "r"(li.entry), "r"(sp) : "memory");
        __builtin_unreachable();
    }

    close(errpipe[1]);
    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        char buf[256] = {0};
        ssize_t n = ::read(errpipe[0], buf, sizeof(buf) - 1);
        error = n > 0 ? std::string(buf, n) : "guest failed to load";
        close(errpipe[0]);
        return -1;
    }
    close(errpipe[0]);

    ptrace(PTRACE_SETOPTIONS, pid, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);

    Ctx ctx{pid, trace};
    for (;;) {
        if (ptrace(PTRACE_SYSEMU, pid, nullptr, nullptr) != 0) { error = "ptrace(SYSEMU) failed"; return -1; }
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) {
            error = std::string("guest killed by signal ") + std::to_string(WTERMSIG(status));
            return -1;
        }
        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        if (sig == (SIGTRAP | 0x80)) {
            Regs regs;
            ptrace(PTRACE_GETREGS, pid, nullptr, &regs);
            Result res = dispatch(ctx, regs);
            if (res.exited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return res.code; }
            regs.rax = (unsigned long long)res.ret;
            ptrace(PTRACE_SETREGS, pid, nullptr, &regs);   // also carries any handler changes (fs_base)
        } else {
            error = std::string("guest stopped by signal ") + std::to_string(sig);
            kill(pid, SIGKILL); waitpid(pid, &status, 0);
            return -1;
        }
    }
}
