#include "log.h"
#include "version.h"
#include "font.h"
#include "render.h"
#include "savefix.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/syscall.h>

namespace {

constexpr char kVersion[] = EU4CJK_VERSION;
constexpr char kLogDirName[] = "eu4cjk";

int open_log() {
    char dir_path[1024];
    char file_path[1080];
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        std::snprintf(dir_path, sizeof(dir_path), "%s/%s", xdg, kLogDirName);
        std::snprintf(file_path, sizeof(file_path), "%s/eu4cjk.log", dir_path);
        mkdir(dir_path, 0755);
        int fd = open(file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) return fd;
    }
    return open("/tmp/eu4cjk.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
}

bool is_target_process() {
    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    const char* base = std::strrchr(exe, '/');
    base = base ? base + 1 : exe;
    return std::strcmp(base, "eu4") == 0;
}

}

namespace {

// Crash forensics: SIGSEGV/SIGBUS handler logging rip/fault address and a
// backtrace to eu4cjk.log, then re-raising for the default action. Uses only
// async-signal-safe primitives (manual hex, write, backtrace_symbols_fd).
void hex64(char* out, uint64_t v) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        unsigned d = static_cast<unsigned>((v >> (60 - i * 4)) & 0xF);
        out[2 + i] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    }
}

void crash_log(int sig, siginfo_t* si, void* ctx) {
    int fd = open_log();
    if (fd >= 0) {
        char buf[512];
        int o = 0;
        std::memcpy(buf, "[eu4cjk] CRASH sig=", 20);
        o = 20;
        buf[o++] = static_cast<char>('0' + (sig / 10) % 10);
        buf[o++] = static_cast<char>('0' + sig % 10);
        std::memcpy(buf + o, " addr=", 6); o += 6;
        hex64(buf + o, reinterpret_cast<uint64_t>(si->si_addr)); o += 18;
        if (ctx) {
            auto& g = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
            static const int regs[9] = { REG_RIP, REG_RBX, REG_R14, REG_R15,
                                         REG_RAX, REG_RDI, REG_RSI, REG_RDX,
                                         REG_R12 };
            static const char* names[9] = { " rip", " rbx", " r14", " r15",
                                            " rax", " rdi", " rsi", " rdx",
                                            " r12" };
            for (int i = 0; i < 9; ++i) {
                std::memcpy(buf + o, names[i], std::strlen(names[i]));
                o += static_cast<int>(std::strlen(names[i]));
                buf[o++] = '=';
                hex64(buf + o, static_cast<uint64_t>(g[regs[i]]));
                o += 18;
                buf[o++] = ' ';
            }
        }
        buf[o++] = '\n';
        ssize_t u = write(fd, buf, static_cast<size_t>(o));
        (void)u;
        if (ctx) {
            // Scan above the faulting rsp for probable return addresses in
            // the main executable's text (0x400000..0x2c00000): identifies
            // the caller chain even when frame pointers are absent.
            auto& g2 = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
            uint64_t sp = static_cast<uint64_t>(g2[REG_RSP]);
            uint64_t bp = static_cast<uint64_t>(g2[REG_RBP]);
            char b2[900];
            int o2 = snprintf(b2, sizeof(b2),
                              "[eu4cjk] crashscan rsp=0x%llx rbp=0x%llx:",
                              static_cast<unsigned long long>(sp),
                              static_cast<unsigned long long>(bp));
            int shown = 0;
            if (sp && sp < 0x7fffffffe000ull) {
                const uint64_t* q = reinterpret_cast<const uint64_t*>(sp);
                for (int i = 0; i < 256 && shown < 30; ++i) {
                    uint64_t v = q[i];
                    if (v > 0x400000 && v < 0x3600000) {
                        o2 += snprintf(b2 + o2, sizeof(b2) - static_cast<size_t>(o2),
                                       " [%d]0x%llx", i,
                                       static_cast<unsigned long long>(v));
                        ++shown;
                    }
                }
            }
            b2[o2++] = '\n';
            ssize_t u2 = write(fd, b2, static_cast<size_t>(o2));
            (void)u2;
        }
        void* frames[32];
        int n = backtrace(frames, 32);
        if (ctx) {
            // show the faulting frame, not the handler's
            uint64_t rip = static_cast<uint64_t>(
                static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
            for (int i = 0; i < n; ++i)
                if (frames[i] == reinterpret_cast<void*>(rip) || i == 1) {
                    frames[1] = reinterpret_cast<void*>(rip);
                    break;
                }
        }
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
}

// The game installs its own SIGSEGV handler (which converts crashes into a
// clean _exit(11), hiding the fault context). Interpose sigaction so ours
// stays installed: log full registers + backtrace, then chain to the game's.
struct sigaction g_game_segv;
bool g_game_segv_set = false;

void chained_segv(int sig, siginfo_t* si, void* ctx) {
    crash_log(sig, si, ctx);
    if (g_game_segv_set) {
        if (g_game_segv.sa_flags & SA_SIGINFO)
            g_game_segv.sa_sigaction(sig, si, ctx);
        else
            g_game_segv.sa_handler(sig);
        return;
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace - interposers below need EXTERNAL linkage to be interposed

#pragma GCC visibility push(default)
extern "C" int sigaction(int sig, const struct sigaction* act,
                         struct sigaction* oldact) {
    using Fn = int (*)(int, const struct sigaction*, struct sigaction*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "sigaction"));
    if ((sig == SIGSEGV || sig == SIGBUS) && act &&
        act->sa_sigaction != chained_segv) {
        // remember the game's handler, install ours in front of it
        g_game_segv = *act;
        g_game_segv_set = true;
        struct sigaction mine = *act;
        mine.sa_sigaction = chained_segv;
        mine.sa_flags |= SA_SIGINFO | SA_ONSTACK;
        return real(sig, &mine, oldact);
    }
    return real(sig, act, oldact);
}

// signal() interposition as well: glibc's signal() calls sigaction
// internally (bypassing PLT interposition), so the game registering via
// signal(SIGSEGV, h) would silently replace our handler.
using SignalFn = void (*)(int);
SignalFn g_prev_signal = nullptr;

int real_sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    using Fn = int (*)(int, const struct sigaction*, struct sigaction*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "sigaction"));
    return real(sig, act, old);
}

void install_chained(int sig) {
    struct sigaction sa{};
    sa.sa_sigaction = chained_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    real_sigaction(sig, &sa, nullptr);
}

extern "C" SignalFn signal(int sig, SignalFn h) {
    using Fn = SignalFn (*)(int, SignalFn);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "signal"));
    if ((sig == SIGSEGV || sig == SIGBUS) && h != SIG_DFL && h != SIG_IGN &&
        h != g_prev_signal) {
        g_game_segv = {};
        g_game_segv.sa_handler = h;
        g_game_segv_set = true;
        g_prev_signal = h;
        install_chained(sig);           // keep full siginfo context
        return h;                        // pretend the game's handler is set
    }
    return real(sig, h);
}
#pragma GCC visibility pop

void install_crash_handler() {
    static char altstack[256 * 1024];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = chained_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

// exit() interposition: the game exits with code 11 (no signal, no fatal log)
// shortly after 3D label generation starts - log who calls exit(non-zero).
// Gated on the game process: foreign preloaded processes (timeout, shells)
// must pass through untouched - the up-stack forensics scan can walk into
// their guard pages (observed: SIGSEGV in `timeout`'s _exit path).
extern "C" void exit(int code) {
    if (code != 0 && is_target_process()) {
        int fd = open_log();
        if (fd >= 0) {
            char buf[64];
            std::memcpy(buf, "[eu4cjk] exit() called, code=", 29);
            buf[29] = static_cast<char>('0' + (code / 10) % 10);
            buf[30] = static_cast<char>('0' + code % 10);
            buf[31] = '\n';
            ssize_t u = write(fd, buf, 32);
            (void)u;
            void* frames[24];
            int n = backtrace(frames, 24);
            backtrace_symbols_fd(frames, n, fd);
            close(fd);
        }
        _exit(code);
    }
    using ExitFn = void (*)(int);
    static ExitFn real_exit = nullptr;
    if (!real_exit)
        real_exit = reinterpret_cast<ExitFn>(
            dlsym(RTLD_NEXT, "exit"));
    if (real_exit) { real_exit(code); }
    _exit(code);
}

// _exit interposition: the code-11 death skips atexit handlers, so the game
// (or a library) calls _exit directly - capture the backtrace there.
// Game-process only (see exit() above).
extern "C" void _exit(int code) {
    if (code != 0 && is_target_process()) {
        int fd = open_log();
        if (fd >= 0) {
            char buf[64];
            std::memcpy(buf, "[eu4cjk] _exit() called, code=", 30);
            buf[30] = static_cast<char>('0' + (code / 10) % 10);
            buf[31] = static_cast<char>('0' + code % 10);
            buf[32] = '\n';
            ssize_t u = write(fd, buf, 33);
            (void)u;
            void* frames[24];
            int n = backtrace(frames, 24);
            backtrace_symbols_fd(frames, n, fd);
            // The death runs on the game's signal-handler stack: the kernel's
            // rt_sigframe (with the faulting ucontext) is still above us.
            // Scan for UC_MAGIC (0x5341474D at uc_flags) to recover the
            // fault-time registers.
            volatile char probe = 0;
            uintptr_t sp = reinterpret_cast<uintptr_t>(&probe) & ~7UL;
            const uint64_t* q = reinterpret_cast<const uint64_t*>(sp);
            for (size_t i = 0; i < 2048; ++i) {   // <=16KB up-stack
                if ((q[i] & 0xFFFFFFFFULL) != 0x5341474DULL) continue;
                auto* uc = reinterpret_cast<const ucontext_t*>(&q[i]);
                uint64_t rip =
                    static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
                if (rip <= 0x400000 || rip >= 0x3000000) continue;
                char rb[700];
                int o = std::sprintf(rb,
                    "[eu4cjk] faultctx rip=0x%llx rbx=0x%llx r14=0x%llx"
                    " r15=0x%llx rax=0x%llx rdi=0x%llx rsi=0x%llx"
                    " rdx=0x%llx rcx=0x%llx r12=0x%llx rsp=0x%llx"
                    " rbp=0x%llx\n",
                    (unsigned long long)rip,
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R15],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBP]);
                ssize_t u2 = write(fd, rb, static_cast<size_t>(o));
                (void)u2;
                break;
            }
            close(fd);
        }
    }
    // real _exit via syscall (avoid recursion into the interposed symbol)
    syscall(SYS_exit_group, code);
    __builtin_unreachable();
}

namespace eu4cjk {

void log_line(const char* fmt, ...)
{
    int fd = open_log();
    if (fd < 0) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t unused = write(fd, line, static_cast<size_t>(n));
        (void)unused;
    }
    close(fd);
}

}

__attribute__((constructor)) static void eu4cjk_entry() {
    if (!is_target_process()) return;
#ifdef EU4CJK_BUILD_STAMP
    eu4cjk::log_line("[eu4cjk %s | build %s] loaded pid=%ld\n", kVersion,
                     EU4CJK_BUILD_STAMP, static_cast<long>(getpid()));
#else
    eu4cjk::log_line("[eu4cjk %s] loaded pid=%ld\n", kVersion,
                     static_cast<long>(getpid()));
#endif
    if (eu4cjk::version::detect() != eu4cjk::version::v1_37_0_0) return;
    install_crash_handler();
    eu4cjk::font::install_glyph_gate();
    eu4cjk::font::install_texture_size_cap_fix();
    eu4cjk::render::install();
    eu4cjk::savefix::install();
    std::atexit(eu4cjk::savefix::log_stats);
}
