// SPDX-License-Identifier: GPL-2.0-only
/* Owned memfd only: file COW, typed remote GUP and shifted mremap. */
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
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define LEAF 4096UL
#define NATIVE 16384UL
#define WINDOWS 64UL
#define BYTES (WINDOWS * NATIVE)
#define LEAVES (BYTES / LEAF)
#define PRESENT (UINT64_C(1) << 63)
#define FILE_PAGE (UINT64_C(1) << 61)
#define PFN_MASK ((UINT64_C(1) << 55) - 1)

struct message { uintptr_t address; uint64_t phase, detail; };
static volatile sig_atomic_t child_group;
static const char *stage = "startup";
static bool child_reaped;
static int child_status;
static void report_wait(pid_t pid, int status)
{
    fprintf(stderr, "CHILD_WAIT pid=%ld raw_status=%d exited=%d exit=%d signaled=%d signal=%d\n",
            (long)pid, status, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static void stop(int sig)
{
    if (child_group > 0) kill(-child_group, SIGKILL);
    _exit(128 + sig);
}
static bool io_full(int fd, void *buf, size_t n, bool write_it)
{
    char *p = buf;
    while (n) {
        ssize_t got = write_it ? write(fd, p, n) : read(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got; n -= (size_t)got;
    }
    return true;
}
static unsigned char original(size_t offset)
{
    size_t leaf = offset / LEAF, byte = offset % LEAF;
    return (unsigned char)(leaf * 17 + byte * 13 + byte / 256);
}
static bool verify(const unsigned char *data, unsigned kind, size_t gup_a_leaf)
{
    for (size_t at = 0; at < BYTES; at++) {
        size_t leaf = at / LEAF, byte = at % LEAF;
        unsigned char value = original(at);
        if (kind == 1 && leaf % 4 == 3 && byte == 0) value ^= 0x5a;
        if (kind == 1 && leaf == gup_a_leaf && byte >= 17 && byte < 49) value = 0xe3;
        if (kind == 2 && leaf % 4 == 2 && byte >= 37 && byte < 69) value = 0xc7;
        if (data[at] != value) {
            fprintf(stderr, "VERIFY_FAIL stage=%s kind=%u offset=0x%zx leaf=%zu byte=%zu expected=%02x actual=%02x\n",
                    stage, kind, at, leaf, byte, value, data[at]);
            return false;
        }
    }
    return true;
}
static bool visible_pfns(void)
{
    FILE *f = fopen("/proc/self/status", "re");
    char line[256]; unsigned long long caps = 0;
    if (!f) return false;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "CapEff: %llx", &caps) == 1) break;
    fclose(f); return !!(caps & (1ULL << 21));
}
static bool entries(pid_t pid, const void *base, uint64_t out[LEAVES])
{
    char path[64]; snprintf(path, sizeof(path), "/proc/%ld/pagemap", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "PAGEMAP_OPEN_FAIL stage=%s pid=%ld errno=%d\n", stage, (long)pid, errno); return false; }
    ssize_t n = pread(fd, out, LEAVES * 8, (uintptr_t)base / LEAF * 8);
    close(fd);
    if (n != LEAVES * 8) { fprintf(stderr, "PAGEMAP_READ_FAIL stage=%s bytes=%zd errno=%d\n", stage, n, errno); return false; }
    for (size_t i = 0; i < LEAVES; i++)
        if (!(out[i] & PRESENT) || !(out[i] & PFN_MASK)) {
            fprintf(stderr, "PAGEMAP_ENTRY_FAIL stage=%s pid=%ld leaf=%zu raw=%016llx present=%d file=%d pfn_visible=%d\n",
                    stage, (long)pid, i, (unsigned long long)out[i], !!(out[i] & PRESENT),
                    !!(out[i] & FILE_PAGE), !!(out[i] & PFN_MASK));
            return false;
        }
    return true;
}
static bool quarter_file_state(const uint64_t *map, unsigned quarter, bool file)
{
    for (size_t leaf = quarter; leaf < LEAVES; leaf += 4)
        if (!!(map[leaf] & FILE_PAGE) != file) {
            fprintf(stderr, "PTE_KIND_FAIL stage=%s leaf=%zu expected_file=%d raw=%016llx\n",
                    stage, leaf, file, (unsigned long long)map[leaf]);
            return false;
        }
    return true;
}
static unsigned mismatches(const uint64_t *map, unsigned logical_quarter, size_t *first)
{
    unsigned count = 0;
    for (size_t leaf = logical_quarter; leaf < LEAVES; leaf += 4) {
        if ((map[leaf] & PFN_MASK) % 4 != logical_quarter) {
            if (!count && first) *first = leaf;
            count++;
        }
    }
    fprintf(stderr, "MISMATCH_WITNESS stage=%s logical_quarter=%u mismatches=%u/%lu\n",
            stage, logical_quarter, count, WINDOWS);
    return count;
}
/* Does not fault payload or require present entries: proves the fork premise. */
static bool entry_summary(const void *base, const char *view)
{
    uint64_t map[LEAVES]; unsigned present = 0, file = 0;
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = pread(fd, map, sizeof(map), (uintptr_t)base / LEAF * 8);
    close(fd);
    if (n != sizeof(map)) return false;
    for (size_t i = 0; i < LEAVES; i++) {
        present += !!(map[i] & PRESENT);
        file += !!(map[i] & PRESENT) && !!(map[i] & FILE_PAGE);
    }
    fprintf(stderr, "PREFLIGHT stage=%s view=%s present=%u file_present=%u leaves=%lu first_raw=%016llx\n",
            stage, view, present, file, LEAVES, (unsigned long long)map[0]);
    return true;
}
static bool token(int fd, char wanted)
{
    char got;
    return io_full(fd, &got, 1, false) && got == wanted;
}
#define CHILD_CHECK(test, name, code) do { stage = name; if (!(test)) { \
    fprintf(stderr, "CHILD_FAIL stage=%s exit=%d errno=%d\n", stage, code, errno); _exit(code); } } while (0)
static void child(unsigned char *a, unsigned char *b, int control, int status)
{
    struct message msg;
    uint64_t before[LEAVES], after[LEAVES];
    alarm(20);
    CHILD_CHECK(entry_summary(a, "A") && entry_summary(b, "B"), "before-child-prefault", 2);
    /* Fork may skip reconstructible file PTEs. Establish them in this mm. */
    CHILD_CHECK(verify(a, 0, SIZE_MAX), "child-prefault-A-original", 3);
    CHILD_CHECK(verify(b, 0, SIZE_MAX), "child-prefault-B-original", 3);
    CHILD_CHECK(entry_summary(a, "A") && entry_summary(b, "B"), "after-child-prefault", 2);
    CHILD_CHECK(entries(getpid(), a, before), "CPU-source-present", 3);
    CHILD_CHECK(quarter_file_state(before, 3, true), "CPU-source-file-PTE", 3);
    for (size_t window = 0; window < WINDOWS; window++) {
        size_t at = (window * 4 + 3) * LEAF;
        *(volatile unsigned char *)(a + at) = original(at) ^ 0x5a;
    }
    CHILD_CHECK(verify(a, 1, SIZE_MAX), "CPU-COW-full-bytes", 3);
    CHILD_CHECK(entries(getpid(), a, after), "CPU-destination-present", 3);
    CHILD_CHECK(quarter_file_state(after, 3, false), "CPU-destination-anon-PTE", 3);
    size_t first = SIZE_MAX;
    unsigned cpu_mismatch = mismatches(after, 3, &first);
    CHILD_CHECK(cpu_mismatch, "CPU-COW-actual-quarter-mismatch", 5);
    msg = (struct message){ .address = first, .phase = 1, .detail = cpu_mismatch };
    CHILD_CHECK(io_full(status, &msg, sizeof(msg), true), "CPU-result-write", 2);
    CHILD_CHECK(token(control, 'm'), "wait-mremap-command", 2);
    CHILD_CHECK(verify(a, 1, first), "post-GUP-A-full-bytes", 3);
    CHILD_CHECK(verify(b, 2, SIZE_MAX), "post-GUP-B-full-bytes", 3);
    CHILD_CHECK(entries(getpid(), b, before), "before-mremap-PFNs", 3);
    void *reserve = mmap(NULL, BYTES + 2 * NATIVE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHILD_CHECK(reserve != MAP_FAILED, "mremap-reservation", 2);
    unsigned long desired = ((uintptr_t)b + LEAF) % NATIVE;
    unsigned char *destination = (void *)((((uintptr_t)reserve + NATIVE - 1) & ~(NATIVE - 1)) + desired);
    unsigned char *moved = mremap(b, BYTES, BYTES, MREMAP_MAYMOVE | MREMAP_FIXED, destination);
    CHILD_CHECK(moved != MAP_FAILED && moved == destination &&
        (uintptr_t)moved % NATIVE != (uintptr_t)b % NATIVE, "mremap-shifted-VA", 3);
    CHILD_CHECK(verify(moved, 2, SIZE_MAX), "mremap-full-bytes", 3);
    CHILD_CHECK(entries(getpid(), moved, after), "after-mremap-PFNs", 3);
    for (size_t leaf = 0; leaf < LEAVES; leaf++) {
        if ((before[leaf] & PFN_MASK) != (after[leaf] & PFN_MASK)) {
            fprintf(stderr, "PFN_CHANGE stage=mremap leaf=%zu before=%016llx after=%016llx\n",
                    leaf, (unsigned long long)before[leaf], (unsigned long long)after[leaf]);
            CHILD_CHECK(false, "mremap-preserved-PFNs", 6);
        }
    }
    msg = (struct message){ .address = (uintptr_t)moved, .phase = 2, .detail = desired };
    CHILD_CHECK(io_full(status, &msg, sizeof(msg), true), "mremap-result-write", 2);
    CHILD_CHECK(token(control, 'x'), "wait-final-release", 2);
    CHILD_CHECK(verify(a, 1, first), "final-A-full-bytes", 3);
    CHILD_CHECK(verify(moved, 2, SIZE_MAX), "final-B-full-bytes", 3);
    munmap(reserve, BYTES + 2 * NATIVE); munmap(a, BYTES);
    close(control); close(status); _exit(0);
}
static bool remote_one(pid_t pid, void *local, void *remote, size_t bytes, bool write_it)
{
    struct iovec l = { .iov_base = local, .iov_len = bytes };
    struct iovec r = { .iov_base = remote, .iov_len = bytes };
    ssize_t n = write_it ? process_vm_writev(pid, &l, 1, &r, 1, 0) :
                           process_vm_readv(pid, &l, 1, &r, 1, 0);
    return n == (ssize_t)bytes;
}
static bool remote_quarters(pid_t pid, void *local, unsigned char *remote,
                            unsigned quarter, size_t offset, size_t bytes, bool write_it)
{
    struct iovec l = { .iov_base = local, .iov_len = WINDOWS * bytes };
    struct iovec r[WINDOWS];
    for (size_t window = 0; window < WINDOWS; window++)
        r[window] = (struct iovec){ .iov_base = remote + (window * 4 + quarter) * LEAF + offset, .iov_len = bytes };
    ssize_t n = write_it ? process_vm_writev(pid, &l, 1, r, WINDOWS, 0) :
                           process_vm_readv(pid, &l, 1, r, WINDOWS, 0);
    return n == (ssize_t)(WINDOWS * bytes);
}
static bool wait_ok(pid_t pid)
{
    int status; pid_t n;
    do { n = waitpid(pid, &status, 0); } while (n < 0 && errno == EINTR);
    if (n == pid) { child_reaped = true; child_status = status; report_wait(pid, status); }
    else fprintf(stderr, "CHILD_WAIT_ERROR pid=%ld errno=%d\n", (long)pid, errno);
    return n == pid && WIFEXITED(status) && !WEXITSTATUS(status);
}
#define CHECK(test, message) do { stage = message; if (!(test)) { fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno); goto cleanup; } } while (0)
int main(int argc, char **argv)
{
    unsigned char *buffer = NULL, *a = MAP_FAILED, *b = MAP_FAILED;
    int fd = -1, control[2] = {-1,-1}, status[2] = {-1,-1};
    pid_t pid = -1; bool ok = false;
    uint64_t map[LEAVES]; struct message msg;
    unsigned cpu_mismatch = 0, read_mismatch = 0, write_mismatch = 0;
    size_t first_a = SIZE_MAX, first_b = SIZE_MAX;
    if (argc != 2 || strcmp(argv[1], "--native-16k")) {
        fprintf(stderr, "usage: %s --native-16k\n", argv[0]); return 2;
    }
    if (getauxval(AT_PAGESZ) != LEAF || !visible_pfns()) {
        fprintf(stderr, "SKIP: established4K launcher/native16K kernel/visiblePFNs required\n"); return 4;
    }
    signal(SIGPIPE, SIG_IGN); signal(SIGALRM, stop); signal(SIGTERM, stop); signal(SIGINT, stop);
    alarm(30);
    buffer = malloc(BYTES);
    CHECK(buffer, "buffer allocation");
    for (size_t i = 0; i < BYTES; i++) buffer[i] = original(i);
    fd = memfd_create("own-file-cow-consumers", MFD_CLOEXEC);
    CHECK(fd >= 0 && !ftruncate(fd, BYTES), "owned memfd");
    CHECK(pwrite(fd, buffer, BYTES, 0) == BYTES, "initialize memfd");
    a = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    b = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED, "private file views");
    /* Full reads establish file PTEs before COW and typed pin operations. */
    CHECK(verify(a, 0, SIZE_MAX) && verify(b, 0, SIZE_MAX), "prefault originals");
    CHECK(!pipe(control) && !pipe(status), "control pipes");
    pid = fork(); CHECK(pid >= 0, "fork");
    if (!pid) {
        CHILD_CHECK(!setpgid(0, 0), "set-child-group", 2);
        close(control[1]); close(status[0]); close(fd);
        child(a, b, control[0], status[1]);
    }
    child_group = pid;
    if (setpgid(pid, pid) && errno != EACCES && errno != ESRCH) CHECK(false, "child group");
    close(control[0]); control[0] = -1; close(status[1]); status[1] = -1;
    bool received = io_full(status[0], &msg, sizeof(msg), false);
    if (!received) { fprintf(stderr, "RESULT_EOF stage=CPU-COW; waiting for exact child status\n"); (void)wait_ok(pid); }
    CHECK(received && msg.phase == 1, "CPU COW result");
    cpu_mismatch = msg.detail; first_a = msg.address;
    CHECK(cpu_mismatch && first_a < LEAVES && first_a % 4 == 3, "actual CPU COW offset mismatch");
    CHECK(remote_one(pid, buffer, a + first_a * LEAF, LEAF, false), "GUP read of mismatched anonymous slot");
    for (size_t i = 0; i < LEAF; i++)
        CHECK(buffer[i] == (unsigned char)(original(first_a * LEAF + i) ^ (i == 0 ? 0x5a : 0)), "GUP read bytes");
    memset(buffer, 0xe3, 32);
    CHECK(remote_one(pid, buffer, a + first_a * LEAF + 17, 32, true), "GUP write of mismatched anonymous slot");
    CHECK(entries(pid, b, map) && quarter_file_state(map, 3, true), "read-pin sources remain file PTEs");
    CHECK(remote_quarters(pid, buffer, b, 3, 0, LEAF, false), "typed read-pin file unshare");
    for (size_t w = 0; w < WINDOWS; w++)
        for (size_t i = 0; i < LEAF; i++)
            CHECK(buffer[w * LEAF + i] == original((w * 4 + 3) * LEAF + i), "read-pin copied bytes");
    CHECK(entries(pid, b, map) && quarter_file_state(map, 3, false), "read-pin produced anonymous slots");
    read_mismatch = mismatches(map, 3, &first_b);
    CHECK(read_mismatch && first_b < LEAVES, "actual read-pin file COW offset mismatch");
    CHECK(quarter_file_state(map, 2, true), "write-pin sources remain file PTEs");
    memset(buffer, 0xc7, WINDOWS * 32);
    CHECK(remote_quarters(pid, buffer, b, 2, 37, 32, true), "typed write-pin file COW");
    CHECK(entries(pid, b, map) && quarter_file_state(map, 2, false), "write-pin produced anonymous slots");
    write_mismatch = mismatches(map, 2, NULL);
    CHECK(write_mismatch, "actual write-pin file COW offset mismatch");
    char command = 'm'; CHECK(io_full(control[1], &command, 1, true), "mremap command");
    received = io_full(status[0], &msg, sizeof(msg), false);
    if (!received) { fprintf(stderr, "RESULT_EOF stage=mremap; waiting for exact child status\n"); (void)wait_ok(pid); }
    CHECK(received && msg.phase == 2, "mremap content/PFN result");
    unsigned char *moved = (void *)msg.address;
    CHECK(remote_one(pid, buffer, moved + first_b * LEAF, LEAF, false), "GUP read after shifted mremap");
    for (size_t i = 0; i < LEAF; i++) CHECK(buffer[i] == original(first_b * LEAF + i), "moved GUP bytes");
    CHECK(verify(a, 0, SIZE_MAX) && verify(b, 0, SIZE_MAX), "parent private views unchanged");
    CHECK(pread(fd, buffer, BYTES, 0) == BYTES && verify(buffer, 0, SIZE_MAX), "original memfd byte exact");
    command = 'x'; CHECK(io_full(control[1], &command, 1, true), "child release");
    CHECK(wait_ok(pid), "child final integrity"); pid = -1; child_group = 0;
    printf("{\"file_bytes\":%lu,\"cpu_cow_mismatches\":%u,\"read_pin_cow_mismatches\":%u,"
           "\"write_pin_cow_mismatches\":%u,\"targets_per_path\":%lu,\"mremap_shifted_va_quarter\":true,"
           "\"mremap_preserved_pfns\":true,\"gup_after_mremap\":true,\"parent_and_file_unchanged\":true,\"content_ok\":true}\n",
           BYTES, cpu_mismatch, read_mismatch, write_mismatch, WINDOWS);
    ok = true;
cleanup:
    if (pid > 0 && !child_reaped) {
        pid_t got = waitpid(pid, &child_status, WNOHANG);
        if (got == pid) { child_reaped = true; report_wait(pid, child_status); }
        else { kill(-pid, SIGKILL); (void)wait_ok(pid); }
    }
    child_group = 0; alarm(0);
    for (unsigned i = 0; i < 2; i++) { if (control[i] >= 0) close(control[i]); if (status[i] >= 0) close(status[i]); }
    if (a != MAP_FAILED) munmap(a, BYTES);
    if (b != MAP_FAILED) munmap(b, BYTES);
    if (fd >= 0) close(fd);
    free(buffer);
    return !ok;
}
