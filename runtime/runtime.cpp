#include "runtime.hpp"
#include "loader.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace {

// ---- Guest memory: one reserved arena, managed by *our* allocator ----------
// The child process maps this whole region before the guest starts. The guest
// never asks the host kernel for memory; brk/mmap just hand out slices of it.
constexpr uint64_t ARENA_BASE = 0x10000000;
constexpr uint64_t ARENA_SIZE = 256ull << 20;
constexpr uint64_t BRK_END    = ARENA_BASE + (64ull << 20);   // brk heap: first 64 MiB
constexpr uint64_t ARENA_END  = ARENA_BASE + ARENA_SIZE;      // mmap area: the rest
constexpr uint64_t PAGE       = 4096;

struct Ctx {
    pid_t pid;
    bool trace;
    uint64_t brk_cur = ARENA_BASE;   // current program break
    uint64_t mmap_cur = BRK_END;     // bump pointer for mmap (never reuses memory yet)
};

struct Result { bool exited; int code; long ret; };
using Regs = user_regs_struct;
using Handler = Result (*)(Ctx&, const Regs&);
struct Entry { const char* name; Handler fn; };

// Copy bytes out of the guest's address space.
bool read_guest(pid_t pid, uint64_t addr, void* out, size_t len) {
    iovec local{out, len};
    iovec remote{(void*)addr, len};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}

// ---- The system layer: one function per guest syscall ----------------------
Result sys_write(Ctx& c, const Regs& r) {
    uint64_t fd = r.rdi, buf = r.rsi, len = r.rdx;
    if ((fd != 1 && fd != 2) || len > (1u << 20)) return {false, 0, -EBADF};
    std::vector<char> tmp(len);
    if (len && !read_guest(c.pid, buf, tmp.data(), len)) return {false, 0, -EFAULT};
    ssize_t n = ::write((int)fd, tmp.data(), len);
    return {false, 0, n < 0 ? -errno : n};
}

Result sys_exit(Ctx&, const Regs& r) { return {true, (int)(r.rdi & 0xff), 0}; }

// brk(0) returns the current break; brk(addr) moves it if it stays in range.
Result sys_brk(Ctx& c, const Regs& r) {
    uint64_t want = r.rdi;
    if (want >= ARENA_BASE && want <= BRK_END) c.brk_cur = want;
    return {false, 0, (long)c.brk_cur};
}

// Anonymous private mappings only, carved from the arena.
Result sys_mmap(Ctx& c, const Regs& r) {
    uint64_t len = r.rsi;
    int flags = (int)r.r10;
    if (len == 0) return {false, 0, -EINVAL};
    if (!(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) {
        std::fprintf(stderr, "[runtime] mmap: only non-fixed anonymous mappings supported (flags=0x%x)\n", flags);
        return {false, 0, -EINVAL};
    }
    uint64_t size = (len + PAGE - 1) & ~(PAGE - 1);
    if (size > ARENA_END - c.mmap_cur) return {false, 0, -ENOMEM};
    uint64_t addr = c.mmap_cur;
    c.mmap_cur += size;
    return {false, 0, (long)addr};   // fresh arena pages are already zero
}

// Memory is never reused yet, so unmapping is a no-op.
Result sys_munmap(Ctx&, const Regs&) { return {false, 0, 0}; }

const std::unordered_map<long, Entry>& syscall_table() {
    static const std::unordered_map<long, Entry> t = {
        {1,   {"write",      sys_write}},
        {9,   {"mmap",       sys_mmap}},
        {11,  {"munmap",     sys_munmap}},
        {12,  {"brk",        sys_brk}},
        {60,  {"exit",       sys_exit}},
        {231, {"exit_group", sys_exit}},
    };
    return t;
}

Result dispatch(Ctx& c, const Regs& r) {
    long nr = (long)r.orig_rax;
    auto it = syscall_table().find(nr);
    if (it == syscall_table().end()) {
        // Fail loudly: name the missing syscall, return ENOSYS to the guest.
        std::fprintf(stderr, "[runtime] unimplemented guest syscall %ld\n", nr);
        return {false, 0, -ENOSYS};
    }
    if (c.trace)
        std::fprintf(stderr, "[guest] %s(%lld, %lld, %lld)\n", it->second.name,
                     (long long)r.rdi, (long long)r.rsi, (long long)r.rdx);
    return it->second.fn(c, r);
}

} // namespace

int run_guest(const std::vector<uint8_t>& image, bool trace, std::string& error) {
    int errpipe[2];
    if (pipe(errpipe) != 0) { error = "pipe failed"; return -1; }

    pid_t pid = fork();
    if (pid < 0) { error = "fork failed"; return -1; }

    if (pid == 0) {
        // Child: set up guest memory *before* we are intercepted, then stop and
        // let the parent take over just before the first guest instruction.
        close(errpipe[0]);
        uint64_t entry = 0;
        std::string err;
        void* arena = mmap((void*)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
        if (arena == MAP_FAILED) err = "could not reserve guest memory arena";
        if (!err.empty() || !load_image(image, entry, err)) {
            (void)!::write(errpipe[1], err.data(), err.size());
            _exit(127);
        }
        close(errpipe[1]);
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        raise(SIGSTOP);
        asm volatile("xor %%rbp, %%rbp\n\tjmp *%0" :: "r"(entry) : "memory");
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
            ptrace(PTRACE_SETREGS, pid, nullptr, &regs);
        } else {
            error = std::string("guest stopped by signal ") + std::to_string(sig);
            kill(pid, SIGKILL); waitpid(pid, &status, 0);
            return -1;
        }
    }
}
