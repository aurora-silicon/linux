// SPDX-License-Identifier: GPL-2.0-only
/* Three generations; actual PFNs distinguish same-native-folio COW coverage. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define LEAF 4096UL
#define NATIVE 16384UL
#define TRIALS 16
#define PFN_MASK ((UINT64_C(1) << 55) - 1)
struct result { uint64_t old_pfn, new_pfn, held_pfn; unsigned quarter; };
static volatile sig_atomic_t group;
static unsigned char *data;
static void stop(int sig)
{
    if (group > 0) kill(-group, SIGKILL);
    _exit(128 + sig);
}
static bool io_full(int fd, void *buf, size_t bytes, bool output)
{
    unsigned char *p = buf;
    while (bytes) {
        ssize_t n = output ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; bytes -= n;
    }
    return true;
}
static unsigned char pattern(size_t at, unsigned generation, unsigned quarter)
{
    unsigned salt = at / LEAF == quarter ? generation * 53 : 0;
    return (unsigned char)((at * 17 + at / LEAF * 29 + at / 256 + salt) % 251 + 1);
}
static void change(unsigned generation, unsigned quarter)
{
    size_t at;
    for (at = quarter * LEAF; at < (quarter + 1) * LEAF; at++)
        data[at] = pattern(at, generation, quarter);
}
static bool verify(unsigned generation, unsigned quarter)
{
    size_t at;
    for (at = 0; at < NATIVE; at++)
        if (data[at] != pattern(at, generation, quarter)) return false;
    return true;
}
static bool pfn(int fd, unsigned quarter, uint64_t *out)
{
    uint64_t entry;
    off_t where = ((uintptr_t)data / LEAF + quarter) * 8;
    if (pread(fd, &entry, 8, where) != 8 || !(entry & (UINT64_C(1) << 63)) ||
        (entry & (UINT64_C(1) << 62)) || !(entry & PFN_MASK)) return false;
    *out = entry & PFN_MASK;
    return true;
}
static bool wait_ok(pid_t child)
{
    int status; pid_t got;
    do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
    return got == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static bool caps(void)
{
    FILE *f = fopen("/proc/self/status", "re");
    char line[256]; unsigned long long effective = 0;
    if (!f) return false;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "CapEff: %llx", &effective) == 1) break;
    fclose(f); return !!(effective & (1ULL << 21));
}
static void owner(int output, unsigned quarter)
{
    struct result result = { .quarter = quarter };
    int go[2], answer[2], mapfd;
    char token;
    pid_t held;
    alarm(5);
    if (pipe(go) || pipe(answer)) _exit(10);
    /* Open in this mm, never inherit an ancestor's already-open pagemap fd. */
    mapfd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (mapfd < 0) _exit(11);
    change(1, quarter);
    if (!pfn(mapfd, quarter, &result.old_pfn) || !verify(1, quarter)) _exit(12);
    held = fork();
    if (held < 0) _exit(13);
    if (!held) {
        uint64_t held_pfn;
        close(go[1]); close(answer[0]); close(output); close(mapfd);
        alarm(4);
        if (!io_full(go[0], &token, 1, false) || token != 'r') _exit(14);
        mapfd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (mapfd < 0 || !pfn(mapfd, quarter, &held_pfn) ||
            held_pfn != result.old_pfn || !verify(1, quarter) ||
            !io_full(answer[1], &held_pfn, sizeof(held_pfn), true)) _exit(15);
        close(mapfd); close(go[0]); close(answer[1]); _exit(0);
    }
    /* Before libc/pipe bookkeeping, consume a free sibling if allocator picks it. */
    change(2, quarter);
    if (!pfn(mapfd, quarter, &result.new_pfn) ||
        result.new_pfn == result.old_pfn || !verify(2, quarter)) _exit(16);
    close(go[0]); close(answer[1]); token = 'r';
    if (!io_full(go[1], &token, 1, true) ||
        !io_full(answer[0], &result.held_pfn, sizeof(result.held_pfn), false) ||
        !wait_ok(held) || !verify(2, quarter)) _exit(17);
    close(go[1]); close(answer[0]); close(mapfd);
    if (!io_full(output, &result, sizeof(result), true)) _exit(18);
    close(output); _exit(0);
}
int main(int argc, char **argv)
{
    unsigned same = 0;
    if (argc == 2 && !strcmp(argv[1], "--selftest")) {
        for (unsigned q = 0; q < 4; q++)
            for (size_t at = 0; at < NATIVE; at++)
                if ((pattern(at, 1, q) != pattern(at, 2, q)) != (at / LEAF == q)) return 1;
        puts("PASS generation pattern selftest"); return 0;
    }
    if (argc != 2 || strcmp(argv[1], "--native-16k") ||
        getauxval(AT_PAGESZ) != LEAF || !caps()) {
        fputs("Requires established4K launcher/native16K kernel and visible PFNs\n", stderr); return 77;
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) return 1;
    signal(SIGPIPE, SIG_IGN); signal(SIGALRM, stop); signal(SIGTERM, stop); signal(SIGINT, stop);
    setvbuf(stdout, NULL, _IONBF, 0); alarm(20);
    void *map = mmap(NULL, 2 * NATIVE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return 1;
    data = (void *)(((uintptr_t)map + NATIVE - 1) & ~(NATIVE - 1));
    for (size_t at = 0; at < NATIVE; at++) data[at] = pattern(at, 0, 0);
    for (unsigned trial = 0; trial < TRIALS; trial++) {
        int output[2]; struct result result; pid_t pid;
        if (pipe(output)) return 1;
        pid = fork();
        if (pid < 0) return 1;
        if (!pid) {
            group = 0;
            if (setpgid(0, 0)) _exit(1);
            close(output[0]); owner(output[1], trial % 4);
        }
        group = pid;
        if (setpgid(pid, pid) && errno != EACCES && errno != ESRCH) {
            kill(pid, SIGKILL); stop(SIGTERM);
        }
        close(output[1]);
        bool received = io_full(output[0], &result, sizeof(result), false);
        close(output[0]);
        bool good = wait_ok(pid);
        /* Reap any orphaned held generation on failure; no child can escape group. */
        kill(-pid, SIGKILL);
        while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
        group = 0;
        if (!received || !good || result.held_pfn != result.old_pfn ||
            result.new_pfn == result.old_pfn || !verify(0, 0)) {
            fprintf(stderr, "FAIL trial %u generation/PFN/isolation/cleanup\n", trial); return 1;
        }
        bool native_equal = result.old_pfn / 4 == result.new_pfn / 4;
        same += native_equal;
        printf("{\"trial\":%u,\"logical_quarter\":%u,\"old_pfn4k\":%"PRIu64",\"new_pfn4k\":%"PRIu64",\"held_pfn4k\":%"PRIu64",\"same_native\":%s,\"three_generation_bytes_ok\":true}\n",
               trial, result.quarter, result.old_pfn, result.new_pfn, result.held_pfn, native_equal ? "true" : "false");
    }
    munmap(map, 2 * NATIVE); alarm(0);
    printf("{\"result\":\"%s\",\"trials\":%u,\"same_native_exercised\":%u,\"payload_bytes\":%lu}\n", same ? "PASS" : "UNEXERCISED", TRIALS, same, NATIVE);
    return same ? 0 : 77;
}
