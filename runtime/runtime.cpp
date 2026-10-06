#include "runtime.hpp"
#include "loader.hpp"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Syscall numbers we implement (Linux x86-64 numbering, used as our guest ABI for now).
enum : long { SYS_write_ = 1, SYS_exit_ = 60, SYS_exit_group_ = 231 };

// Copy bytes out of the guest's address space.
bool read_guest(pid_t pid, uint64_t addr, void* out, size_t len) {
    iovec local{out, len};
    iovec remote{(void*)addr, len};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}

struct Result { bool exited; int code; long ret; };

// The "system layer": one case per guest syscall, all ours.
Result handle_syscall(pid_t pid, const user_regs_struct& r, bool trace) {
    long nr = (long)r.orig_rax;
    switch (nr) {
    case SYS_write_: {
        uint64_t fd = r.rdi, buf = r.rsi, len = r.rdx;
        if (trace) std::fprintf(stderr, "[guest] write(%llu, 0x%llx, %llu)\n",
                                (unsigned long long)fd, (unsigned long long)buf, (unsigned long long)len);
        if ((fd != 1 && fd != 2) || len > (1u << 20)) return {false, 0, -EBADF};
        std::vector<char> tmp(len);
        if (len && !read_guest(pid, buf, tmp.data(), len)) return {false, 0, -EFAULT};
        ssize_t n = ::write((int)fd, tmp.data(), len);
        return {false, 0, n < 0 ? -errno : n};
    }
    case SYS_exit_:
    case SYS_exit_group_:
        if (trace) std::fprintf(stderr, "[guest] exit(%lld)\n", (long long)r.rdi);
        return {true, (int)(r.rdi & 0xff), 0};
    default:
        // Fail loudly: name the missing syscall, return ENOSYS to the guest.
        std::fprintf(stderr, "[runtime] unimplemented guest syscall %ld\n", nr);
        return {false, 0, -ENOSYS};
    }
}

} // namespace

int run_guest(const std::vector<uint8_t>& image, bool trace, std::string& error) {
    int errpipe[2];
    if (pipe(errpipe) != 0) { error = "pipe failed"; return -1; }

    pid_t pid = fork();
    if (pid < 0) { error = "fork failed"; return -1; }

    if (pid == 0) {
        // Child: map the image *before* we are intercepted, then stop and
        // let the parent take over just before the first guest instruction.
        close(errpipe[0]);
        uint64_t entry = 0;
        std::string err;
        if (!load_image(image, entry, err)) {
            (void)!::write(errpipe[1], err.data(), err.size());
            _exit(127);
        }
        close(errpipe[1]);
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        raise(SIGSTOP);
        // Jump to the guest. It never returns here.
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

    for (;;) {
        // SYSEMU: stop at every syscall *without* running it in the kernel.
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
            user_regs_struct regs;
            ptrace(PTRACE_GETREGS, pid, nullptr, &regs);
            Result res = handle_syscall(pid, regs, trace);
            if (res.exited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return res.code; }
            regs.rax = (unsigned long long)res.ret;   // our return value
            ptrace(PTRACE_SETREGS, pid, nullptr, &regs);
        } else {
            // A real fault (e.g. SIGSEGV) in the guest: report it and stop.
            error = std::string("guest stopped by signal ") + std::to_string(sig);
            kill(pid, SIGKILL); waitpid(pid, &status, 0);
            return -1;
        }
    }
}
