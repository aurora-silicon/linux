// SPDX-License-Identifier: GPL-2.0-only
/* Metadata-only native-backing comparison. Never changes memory policy. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#define BYTES (16UL << 20)
#define LEAVES (BYTES / LEAF)
#define WINDOWS (BYTES / NATIVE)
#define MAX_FRAMES (LEAVES * 2)

struct coverage {
    uint64_t native_units, quarters, occupancy[4];
};
struct result {
    unsigned arm;
    bool content_ok, disjoint_ok;
    struct coverage parent, child, modified, unchanged, combined;
    uint64_t modified_parent_native_overlap;
    uint64_t modified_unchanged_native_overlap;
    uint64_t modified_quarter_counts[4];
};
static volatile sig_atomic_t fixture_group;

static void stop_fixture(int sig)
{
    if (fixture_group > 0)
        kill(-fixture_group, SIGKILL);
    _exit(128 + sig);
}
static bool full_io(int fd, void *buffer, size_t bytes, bool writing)
{
    char *p = buffer;
    while (bytes) {
        ssize_t n = writing ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        bytes -= (size_t)n;
    }
    return true;
}
static bool wait_ok(pid_t pid)
{
    int status;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    return got > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
/* Input is actual4K PFN, never VMA-derived quarter position. Duplicates count once. */
static struct coverage summarize(uint64_t *frames, size_t count)
{
    struct coverage out = {};
    uint64_t last = UINT64_MAX;
    unsigned mask = 0;
    qsort(frames, count, sizeof(*frames), compare_u64);
    for (size_t i = 0; i < count; i++) {
        uint64_t native = frames[i] / 4;
        if (native != last) {
            if (mask) {
                unsigned n = __builtin_popcount(mask);
                out.native_units++;
                out.quarters += n;
                out.occupancy[n - 1]++;
            }
            last = native;
            mask = 0;
        }
        mask |= 1U << (frames[i] % 4);
    }
    if (mask) {
        unsigned n = __builtin_popcount(mask);
        out.native_units++;
        out.quarters += n;
        out.occupancy[n - 1]++;
    }
    return out;
}
/* Both arrays were sorted by summarize. Count each common nativeunit once. */
static uint64_t native_overlap(const uint64_t *a, size_t an, const uint64_t *b, size_t bn)
{
    size_t ai = 0, bi = 0;
    uint64_t count = 0;
    while (ai < an && bi < bn) {
        uint64_t av = a[ai] / 4, bv = b[bi] / 4;
        if (av == bv)
            count++;
        if (av <= bv)
            while (ai < an && a[ai] / 4 == av) ai++;
        if (bv <= av)
            while (bi < bn && b[bi] / 4 == bv) bi++;
    }
    return count;
}
static bool read_frames(pid_t pid, const unsigned char *data, uint64_t frames[LEAVES])
{
    char path[64];
    uint64_t entries[LEAVES];
    snprintf(path, sizeof(path), "/proc/%ld/pagemap", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t n = pread(fd, entries, sizeof(entries), (uintptr_t)data / LEAF * 8);
    close(fd);
    if (n != (ssize_t)sizeof(entries))
        return false;
    for (size_t i = 0; i < LEAVES; i++) {
        uint64_t pfn = entries[i] & ((UINT64_C(1) << 55) - 1);
        if (!(entries[i] & (UINT64_C(1) << 63)) || !pfn)
            return false; /* Swap/hiddenPFNs invalidate this bounded resident comparison. */
        frames[i] = pfn;
    }
    return true;
}
static unsigned chosen_quarter(unsigned arm, size_t window)
{
    return arm ? window % 4 : 0;
}
static unsigned char tag(size_t leaf, bool changed)
{
    unsigned char value = (unsigned char)(leaf % 251 + 1);
    return changed ? value ^ 0xa5 : value;
}
static bool contents(const unsigned char *data, unsigned arm, bool child)
{
    for (size_t leaf = 0; leaf < LEAVES; leaf++) {
        bool changed = child && leaf % 4 == chosen_quarter(arm, leaf / 4);
        if (data[leaf * LEAF] != tag(leaf, changed))
            return false;
        for (size_t byte = 1; byte < LEAF; byte++)
            if (data[leaf * LEAF + byte] != 0x5a)
                return false;
    }
    return true;
}
static void owner(unsigned arm, int output)
{
    uint64_t parent[LEAVES], child_frames[LEAVES], modified[WINDOWS];
    uint64_t unchanged[LEAVES - WINDOWS], combined[MAX_FRAMES];
    struct result result = {.arm = arm};
    int ready[2], release[2];
    char token;
    size_t mi = 0, ui = 0;
    alarm(20);
    void *mapping = mmap(NULL, BYTES + 2 * NATIVE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        _exit(2);
    unsigned char *data = (void *)(((uintptr_t)mapping + NATIVE - 1) & ~(NATIVE - 1));
    if (mprotect(data, BYTES, PROT_READ | PROT_WRITE))
        _exit(2);
    memset(data, 0x5a, BYTES);
    for (size_t leaf = 0; leaf < LEAVES; leaf++)
        data[leaf * LEAF] = tag(leaf, false);
    if (!contents(data, arm, false) || !read_frames(getpid(), data, parent))
        _exit(3);
    struct coverage initial = summarize(parent, LEAVES);
    if (initial.native_units * NATIVE != BYTES || initial.quarters != LEAVES)
        _exit(4); /* Stop beforefork if the32MiB payload ceiling is not established. */
    if (pipe(ready) || pipe(release))
        _exit(2);
    pid_t child = fork();
    if (child < 0)
        _exit(2);
    if (!child) {
        alarm(20);
        close(ready[0]); close(release[1]); close(output);
        for (size_t window = 0; window < WINDOWS; window++) {
            size_t leaf = window * 4 + chosen_quarter(arm, window);
            *(volatile unsigned char *)(data + leaf * LEAF) = tag(leaf, true);
        }
        if (!contents(data, arm, true))
            _exit(3);
        token = 'r';
        if (!full_io(ready[1], &token, 1, true))
            _exit(2);
        close(ready[1]);
        if (!full_io(release[0], &token, 1, false) || token != 'x')
            _exit(2);
        close(release[0]);
        _exit(contents(data, arm, true) ? 0 : 3);
    }
    close(ready[1]); close(release[0]);
    if (!full_io(ready[0], &token, 1, false) || token != 'r')
        _exit(2);
    close(ready[0]);
    result.content_ok = contents(data, arm, false);
    if (!result.content_ok || !read_frames(getpid(), data, parent) ||
        !read_frames(child, data, child_frames))
        _exit(3);
    result.disjoint_ok = true;
    for (size_t leaf = 0; leaf < LEAVES; leaf++) {
        bool changed = leaf % 4 == chosen_quarter(arm, leaf / 4);
        if (changed) {
            modified[mi++] = child_frames[leaf];
            result.modified_quarter_counts[child_frames[leaf] % 4]++;
            result.disjoint_ok &= parent[leaf] != child_frames[leaf];
        } else {
            unchanged[ui++] = child_frames[leaf];
            result.disjoint_ok &= parent[leaf] == child_frames[leaf];
        }
    }
    memcpy(combined, parent, sizeof(parent));
    memcpy(combined + LEAVES, child_frames, sizeof(child_frames));
    result.parent = summarize(parent, LEAVES);
    result.child = summarize(child_frames, LEAVES);
    result.modified = summarize(modified, mi);
    result.unchanged = summarize(unchanged, ui);
    result.combined = summarize(combined, MAX_FRAMES);
    result.modified_parent_native_overlap = native_overlap(modified, mi, parent, LEAVES);
    result.modified_unchanged_native_overlap = native_overlap(modified, mi, unchanged, ui);
    result.disjoint_ok &= mi == WINDOWS && result.modified.quarters == WINDOWS &&
        result.combined.quarters == LEAVES + WINDOWS &&
        result.modified_parent_native_overlap == 0 &&
        result.modified_unchanged_native_overlap == 0 &&
        result.combined.native_units * NATIVE <= 2 * BYTES;
    token = 'x';
    if (!full_io(release[1], &token, 1, true))
        _exit(2);
    close(release[1]);
    bool child_ok = wait_ok(child);
    result.content_ok &= child_ok && contents(data, arm, false);
    munmap(mapping, BYTES + 2 * NATIVE);
    if (!full_io(output, &result, sizeof(result), true))
        _exit(2);
    close(output);
    _exit(result.content_ok && result.disjoint_ok ? 0 : 3);
}
static bool visible_pfns(void)
{
    FILE *file = fopen("/proc/self/status", "re");
    char line[256];
    unsigned long long caps = 0;
    if (!file) return false;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "CapEff: %llx", &caps) == 1) break;
    fclose(file);
    return !!(caps & (1ULL << 21));
}
static void print_coverage(const char *name, struct coverage c)
{
    printf("\"%s\":{\"native_bytes\":%llu,\"represented_bytes\":%llu,"
           "\"quarter_occupancy\":[%llu,%llu,%llu,%llu]}", name,
           (unsigned long long)(c.native_units * NATIVE),
           (unsigned long long)(c.quarters * LEAF),
           (unsigned long long)c.occupancy[0], (unsigned long long)c.occupancy[1],
           (unsigned long long)c.occupancy[2], (unsigned long long)c.occupancy[3]);
}
static int selftest(void)
{
    uint64_t a[] = {400, 401, 401, 403, 404}, b[] = {402, 405};
    uint64_t combined[] = {400, 401, 401, 403, 404, 402, 405};
    struct coverage ca = summarize(a, 5), cb = summarize(b, 2);
    struct coverage cc = summarize(combined, 7);
    bool ok = ca.native_units == 2 && ca.quarters == 4 && ca.occupancy[0] == 1 &&
        ca.occupancy[2] == 1 && cb.native_units == 2 && cb.quarters == 2 &&
        cc.native_units == 2 && cc.quarters == 6 && cc.occupancy[1] == 1 &&
        cc.occupancy[3] == 1 && native_overlap(a, 5, b, 2) == 2;
    uint64_t sparse[] = {400, 404, 408, 412}, dense[] = {400, 401, 402, 403};
    ca = summarize(sparse, 4); cb = summarize(dense, 4);
    ok &= ca.native_units == 4 && cb.native_units == 1 && ca.quarters == cb.quarters;
    printf("metadata selftest: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc != 2 || strcmp(argv[1], "--native-16k")) { fprintf(stderr, "usage: %s --native-16k | --selftest\n", argv[0]); return 2; }
    if (getauxval(AT_PAGESZ) != LEAF || !visible_pfns()) {
        fprintf(stderr, "SKIP: established4K launcher, native16K kernel and visiblePFNs required\n");
        return 4;
    }
    signal(SIGPIPE, SIG_IGN); signal(SIGALRM, stop_fixture); signal(SIGTERM, stop_fixture);
    signal(SIGINT, stop_fixture);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) return 2;
    alarm(30);
    for (unsigned arm = 0; arm < 2; arm++) {
        int pipefd[2]; struct result result = {}; bool copied, success;
        if (pipe(pipefd)) return 2;
        pid_t child = fork();
        if (child < 0) return 2;
        if (!child) {
            if (setpgid(0, 0)) _exit(2);
            close(pipefd[0]); owner(arm, pipefd[1]);
        }
        fixture_group = child;
        if (setpgid(child, child) && errno != EACCES && errno != ESRCH) stop_fixture(SIGTERM);
        close(pipefd[1]);
        copied = full_io(pipefd[0], &result, sizeof(result), false);
        close(pipefd[0]); success = wait_ok(child);
        if (!copied || !success) {
            kill(-child, SIGKILL);
            while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
            fprintf(stderr, "FAIL: arm%u incomplete or integrity/coverage check failed\n", arm);
            return 1;
        }
        fixture_group = 0;
        printf("{\"arm\":\"%s\",\"logical_parent_bytes\":%lu,\"modified_child_bytes\":%lu,",
               arm ? "balanced" : "same-quarter-zero", BYTES, WINDOWS * LEAF);
        print_coverage("parent", result.parent); printf(",");
        print_coverage("child", result.child); printf(",");
        print_coverage("modified_child", result.modified); printf(",");
        print_coverage("unchanged_child", result.unchanged); printf(",");
        print_coverage("parent_child_union", result.combined);
        printf(",\"modified_parent_native_overlap\":%llu,\"modified_unchanged_native_overlap\":%llu,"
               "\"modified_physical_quarter_counts\":[%llu,%llu,%llu,%llu],\"content_ok\":true,\"disjoint_ok\":true}\n",
               (unsigned long long)result.modified_parent_native_overlap,
               (unsigned long long)result.modified_unchanged_native_overlap,
               (unsigned long long)result.modified_quarter_counts[0], (unsigned long long)result.modified_quarter_counts[1],
               (unsigned long long)result.modified_quarter_counts[2], (unsigned long long)result.modified_quarter_counts[3]);
        fflush(stdout);
    }
    alarm(0);
    return 0;
}
