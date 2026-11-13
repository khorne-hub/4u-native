/*
 * toolrun.c: the two small programs with which the app builds the game's code library on the phone (Settings, Game
 * folder, "Build on this phone"; android/.../BuilderService.java). The same file in MHGU for Android and MH4U for Android.
 *
 * Android runs a program of an app only from the app's native library folder (the APK's lib/arm64-v8a, unpacked at
 * install: android:extractNativeLibs), never from the app's own files (W^X, Android 10 and later). The builder's tools,
 * CPython and Zig, are downloaded into the app's files the first time (a few hundred MB that the APK doesn't carry), so
 * these two programs, which come in the APK as lib*.so, run them from there:
 *
 *   libpyrun.so (built with -DTOOLRUN_PYTHON): `python3 ARGS...`. It opens $PYTHONHOME/lib/libpython3.N.so (the official
 *     CPython for Android from python.org, which is a library and has no python3 program) and calls Py_BytesMain.
 *     sys.executable is this program, so Python's own subprocesses (the MHGU translator) run it again.
 *
 *   libzigcc.so (built with -DTOOLRUN_ZIG): Zig's C compiler, `zig cc -fno-sanitize=undefined -g0 ARGS...` (what the
 *     PC tools' `zigcc` script runs), with the static Zig program in $BUILDER_ZIG. Opening a file of the app's own as
 *     code is allowed (dlopen does it for the game's code library), running it as a program is not: so this program
 *     loads Zig's ELF into its own address space and jumps to it, as the kernel would after an execve. Zig runs itself
 *     again for clang and lld (/proc/self/exe, which is this program): with BUILDER_ZIG_INNER set (by the first one)
 *     such a run passes its arguments on unchanged. Zig finds its lib folder through ZIG_LIB_DIR.
 *
 *     It asks for its memory at fixed places (fixed_layout): Zig 0.13's clang makes one of a few slightly different
 *     objects of a very large function from one run to the next, following where its memory is.
 *
 * Both die with the process that started them (PR_SET_PDEATHSIG): a build that is cancelled, or whose app is closed,
 * leaves no compiler running. And both tell Android's low-memory killer to take them before the app (a higher
 * oom_score_adj, which a process may always raise for itself): on a phone short of memory the build stops with an
 * error the app can show, instead of the whole app closing.
 */
#include <alloca.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* Android's low-memory killer takes processes with a higher oom_score_adj first: the build's go before the app's own
 * (a foreground app is 0, a perceptible one 200). Raising one's own needs no permission; a failure changes nothing. */
static void kill_me_first(void) {
    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char now[16] = {0};
    int keep = 0;
    int rd = open("/proc/self/oom_score_adj", O_RDONLY | O_CLOEXEC);
    if (rd >= 0) {
        if (read(rd, now, sizeof now - 1) > 0 && atoi(now) >= 700) keep = 1;  /* (already that low: a zig run of a zig run) */
        close(rd);
    }
    if (!keep) {
        ssize_t w = write(fd, "700", 3);
        (void)w;
    }
    close(fd);
}

static void die(const char *what, const char *detail) {
    dprintf(2, "toolrun: %s%s%s%s%s\n", what, detail ? ": " : "", detail ? detail : "", errno ? ": " : "",
            errno ? strerror(errno) : "");
    _exit(127);
}

#if defined(TOOLRUN_PYTHON)

/* $PYTHONHOME/lib/libpython3.N.so (not the stable-ABI stub libpython3.so) */
static int find_libpython(const char *home, char *out, size_t size) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/lib", home);
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (strncmp(n, "libpython3.", 11) == 0 && n[11] >= '0' && n[11] <= '9' && len > 3 && strcmp(n + len - 3, ".so") == 0) {
            snprintf(out, size, "%s/%s", dir, n);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    kill_me_first();
    const char *home = getenv("PYTHONHOME");
    if (!home || !*home) die("PYTHONHOME is not set", NULL);
    char lib[4096];
    errno = 0;
    if (!find_libpython(home, lib, sizeof lib)) die("no libpython3.N.so in", home);
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) die("dlopen", dlerror());
    int (*py_main)(int, char **) = (int (*)(int, char **))dlsym(h, "Py_BytesMain");
    if (!py_main) die("no Py_BytesMain in", lib);
    return py_main(argc, argv);
}

#elif defined(TOOLRUN_ZIG)

/* The kernel's sigaction for arm64 (bionic's wrapper refuses the signals it keeps for itself) */
struct ksigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    uint64_t mask;
};

/* What bionic set up for this program that Zig's C library (musl) doesn't expect: its signal handlers (debuggerd's),
 * blocked signals and an alternate signal stack. */
static void plain_signals(void) {
    struct ksigaction dfl = {SIG_DFL, 0, NULL, 0};
    for (int s = 1; s <= 64; s++)
        if (s != SIGKILL && s != SIGSTOP) syscall(SYS_rt_sigaction, s, &dfl, NULL, 8);
    uint64_t none = 0;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &none, NULL, 8);
    stack_t ss = {.ss_sp = NULL, .ss_flags = SS_DISABLE, .ss_size = 0};
    sigaltstack(&ss, NULL);
}

/* Maps a static (ET_EXEC) AArch64 ELF program at its own addresses; returns its entry point and where its program
 * headers are in memory. */
static uintptr_t load_elf(const char *path, Elf64_Ehdr *eh, uintptr_t *phdr_at) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open", path);
    if (pread(fd, eh, sizeof *eh, 0) != (ssize_t)sizeof *eh) die("read", path);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64)
        die("not a 64-bit ARM program", path);
    if (eh->e_type != ET_EXEC || eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 64)
        die("not a static program", path);
    Elf64_Phdr ph[64];
    size_t phsize = (size_t)eh->e_phnum * sizeof(Elf64_Phdr);
    if (pread(fd, ph, phsize, (off_t)eh->e_phoff) != (ssize_t)phsize) die("read", path);
    uintptr_t ps = (uintptr_t)sysconf(_SC_PAGESIZE);
    *phdr_at = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *p = &ph[i];
        if (p->p_type == PT_PHDR) *phdr_at = p->p_vaddr;
        if (p->p_type == PT_INTERP) die("not a static program", path);
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        if ((p->p_vaddr - p->p_offset) % ps) die("a segment not aligned to the page size", path);
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) | (p->p_flags & PF_X ? PROT_EXEC : 0);
        uintptr_t start = p->p_vaddr & ~(ps - 1);
        uintptr_t file_end = p->p_vaddr + p->p_filesz, mem_end = p->p_vaddr + p->p_memsz;
        uintptr_t page_end = (file_end + ps - 1) & ~(ps - 1);
        if (p->p_filesz) {
            void *m = mmap((void *)start, file_end - start, prot, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd,
                           (off_t)(p->p_offset & ~(ps - 1)));
            if (m == MAP_FAILED) die("mmap", path);
            if ((uintptr_t)m != start) die("the program's addresses are taken", path);
            /* the rest of the last page of the file's part belongs to the zeroed part (.bss) */
            if (mem_end > file_end && (prot & PROT_WRITE)) memset((void *)file_end, 0, page_end - file_end);
        } else {
            page_end = start;
        }
        uintptr_t end = (mem_end + ps - 1) & ~(ps - 1);
        if (end > page_end) {
            void *m = mmap((void *)page_end, end - page_end, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m == MAP_FAILED || (uintptr_t)m != page_end) die("mmap (zeroed part)", path);
        }
        if (!*phdr_at && p->p_offset <= eh->e_phoff && eh->e_phoff + phsize <= p->p_offset + p->p_filesz)
            *phdr_at = p->p_vaddr + (eh->e_phoff - p->p_offset);
    }
    close(fd);
    if (!*phdr_at) die("no program headers in memory", path);
    return eh->e_entry;
}

static char *push(char **top, const void *data, size_t size) {
    *top -= size;
    memcpy(*top, data, size);
    return *top;
}

extern char **environ;

/* Zig 0.13's clang compiles one very large function of MH4U's game code (f_00ce7920 with the single-screen mod: 300 KB
 * of machine code) into one of a few slightly different objects, following where its memory is: on an RG406V four
 * compiles of the same C in a shell gave two objects with the address space layout randomized, and with it off
 * (personality ADDR_NO_RANDOMIZE) the same object 16 times out of 16. So this program asks for it and runs itself again
 * (/proc/self/exe; Zig's own runs of itself keep it). Inside the app it is not enough: two whole builds on the RG406V
 * still differed in that one function (1.0), as two builds from the start can; every other object comes out the same.
 * A system that refuses the call (an error, or a seccomp filter's SIGSYS, which is caught here) leaves it as it was. */
static volatile sig_atomic_t g_sigsys;
static void on_sigsys(int sig) {
    (void)sig;
    g_sigsys = 1;
}
static void fixed_layout(char **argv) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigsys;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, &old) != 0) return;
    int now = personality(0xffffffff);
    int set = !g_sigsys && now != -1 && !(now & ADDR_NO_RANDOMIZE) &&
              personality((unsigned long)now | ADDR_NO_RANDOMIZE) != -1 && !g_sigsys;
    sigaction(SIGSYS, &old, NULL);
    if (!set) return;
    execv("/proc/self/exe", argv);
    personality((unsigned long)now);  /* (not run again: as it was) */
}

int main(int argc, char **argv) {
    fixed_layout(argv);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    kill_me_first();
    const char *zig = getenv("BUILDER_ZIG");
    if (!zig || !*zig) die("BUILDER_ZIG is not set", NULL);
    /* the first run is the C compiler the build tools call: `zig cc` with the PC wrapper's flags; Zig's own runs of
     * itself (clang, lld) come with the variable set and keep their arguments */
    int inner = getenv("BUILDER_ZIG_INNER") != NULL;
    if (!inner) setenv("BUILDER_ZIG_INNER", "1", 1);
    const char *front[] = {"cc", "-fno-sanitize=undefined", "-g0"};
    int nfront = inner ? 0 : 3;
    int nargs = 1 + nfront + (argc - 1);
    int nenv = 0;
    while (environ[nenv]) nenv++;

    Elf64_Ehdr eh;
    uintptr_t phdr_at;
    uintptr_t entry = load_elf(zig, &eh, &phdr_at);

    /* the new program's stack, as the kernel lays it out: argc, argv, envp, auxv, then the strings. Zig asks for a big
     * one (its PT_GNU_STACK): 64 MB, given only as it is used */
    size_t stack_size = 64u << 20;
    char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) die("mmap (stack)", NULL);
    char *top = stack + stack_size;
    char **sargv = alloca(sizeof(char *) * (size_t)nargs);
    char **senv = alloca(sizeof(char *) * (size_t)(nenv + 1));
    sargv[0] = push(&top, argv[0], strlen(argv[0]) + 1);
    for (int i = 0; i < nfront; i++) sargv[1 + i] = push(&top, front[i], strlen(front[i]) + 1);
    for (int i = 1; i < argc; i++) sargv[nfront + i] = push(&top, argv[i], strlen(argv[i]) + 1);
    for (int i = 0; i < nenv; i++) senv[i] = push(&top, environ[i], strlen(environ[i]) + 1);
    char *execfn = push(&top, zig, strlen(zig) + 1);
    char *platform = push(&top, "aarch64", 8);
    unsigned char rnd[16];
    if (getentropy(rnd, sizeof rnd) != 0) {
        for (size_t i = 0; i < sizeof rnd; i++) rnd[i] = (unsigned char)(i * 131 + (uintptr_t)&rnd);
    }
    char *random = push(&top, rnd, sizeof rnd);

    uint64_t aux[][2] = {
        {AT_PHDR, phdr_at}, {AT_PHENT, sizeof(Elf64_Phdr)}, {AT_PHNUM, eh.e_phnum}, {AT_PAGESZ, (uint64_t)sysconf(_SC_PAGESIZE)},
        {AT_BASE, 0}, {AT_FLAGS, 0}, {AT_ENTRY, entry}, {AT_UID, getuid()}, {AT_EUID, geteuid()}, {AT_GID, getgid()},
        {AT_EGID, getegid()}, {AT_SECURE, 0}, {AT_RANDOM, (uint64_t)(uintptr_t)random}, {AT_HWCAP, getauxval(AT_HWCAP)},
        {AT_HWCAP2, getauxval(AT_HWCAP2)}, {AT_CLKTCK, getauxval(AT_CLKTCK) ? getauxval(AT_CLKTCK) : 100},
        {AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR)}, {AT_PLATFORM, (uint64_t)(uintptr_t)platform},
        {AT_EXECFN, (uint64_t)(uintptr_t)execfn}, {AT_NULL, 0},
    };
    size_t naux = sizeof aux / sizeof aux[0];
    size_t words = 1 + (size_t)nargs + 1 + (size_t)nenv + 1 + 2 * naux;
    uint64_t *sp = (uint64_t *)(((uintptr_t)top - words * 8) & ~(uintptr_t)15);
    uint64_t *w = sp;
    *w++ = (uint64_t)nargs;
    for (int i = 0; i < nargs; i++) *w++ = (uint64_t)(uintptr_t)sargv[i];
    *w++ = 0;
    for (int i = 0; i < nenv; i++) *w++ = (uint64_t)(uintptr_t)senv[i];
    *w++ = 0;
    for (size_t i = 0; i < naux; i++) {
        *w++ = aux[i][0];
        *w++ = aux[i][1];
    }

    plain_signals();
    register uint64_t new_sp __asm__("x1") = (uint64_t)(uintptr_t)sp;
    register uint64_t new_pc __asm__("x2") = entry;
    __asm__ volatile(
        "mov sp, x1\n"
        "mov x0, xzr\n"
        "mov x1, xzr\n"
        "mov x29, xzr\n"
        "mov x30, xzr\n"
        "br x2\n"
        :
        : "r"(new_sp), "r"(new_pc)
        : "memory");
    __builtin_unreachable();
}

#else
#error "build with -DTOOLRUN_PYTHON or -DTOOLRUN_ZIG"
#endif
