/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
#include <linux/vhost.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
#ifndef VHOST_SET_FORK_FROM_OWNER
#define VHOST_SET_FORK_FROM_OWNER _IOW(VHOST_VIRTIO, 0x84, __u8)
#define VHOST_GET_FORK_FROM_OWNER _IOR(VHOST_VIRTIO, 0x85, __u8)
#endif
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL line %d: %s errno=%d\n", __LINE__, #c, \
			       errno);                                      \
			return 1;                                           \
		}                                                           \
	} while (0)
#define N 8
#define ROUNDS 32
#define PACKET_SIZE 96
#define PROTOCOL 0x88b5
#define DATA_GPA (51ULL * 4096)
#define TX_USED_GPA (71ULL * 4096)
#define RX_USED_GPA (70ULL * 4096)

struct queue {
	struct vring_desc desc[N];
	struct {
		uint16_t flags, idx, ring[N + 1];
	} avail;
	struct {
		uint16_t flags, idx;
		struct vring_used_elem ring[N];
		uint16_t event;
	} used;
	int kick, call;
};

static int queue_setup(int fd, struct queue *q, unsigned index)
{
	struct vhost_vring_state state = { .index = index, .num = N };
	struct vhost_vring_addr addr = {
		.index = index,
		.flags = 1U << VHOST_VRING_F_LOG,
		.desc_user_addr = (uintptr_t)q->desc,
		.avail_user_addr = (uintptr_t)&q->avail,
		.used_user_addr = (uintptr_t)&q->used,
		.log_guest_addr = index ? TX_USED_GPA : RX_USED_GPA,
	};
	q->kick = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	q->call = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	CHECK(q->kick >= 0 && q->call >= 0);
	CHECK(!ioctl(fd, VHOST_SET_VRING_NUM, &state));
	state.num = 0;
	CHECK(!ioctl(fd, VHOST_SET_VRING_BASE, &state));
	CHECK(!ioctl(fd, VHOST_SET_VRING_ADDR, &addr));
	struct vhost_vring_file file = { .index = index, .fd = q->kick };
	CHECK(!ioctl(fd, VHOST_SET_VRING_KICK, &file));
	file.fd = q->call;
	CHECK(!ioctl(fd, VHOST_SET_VRING_CALL, &file));
	return 0;
}
static int submit(struct queue *q, unsigned round)
{
	uint64_t one = 1;
	q->avail.ring[round % N] = 0;
	__atomic_store_n(&q->avail.idx, round + 1, __ATOMIC_RELEASE);
	return write(q->kick, &one, sizeof(one)) == sizeof(one);
}
static int complete(struct queue *q, unsigned round)
{
	for (unsigned i = 0; i < 2000; i++) {
		if (__atomic_load_n(&q->used.idx, __ATOMIC_ACQUIRE) ==
		    round + 1)
			return 1;
		usleep(1000);
	}
	printf("queue timeout round=%u used=%u avail=%u\n", round, q->used.idx,
	       q->avail.idx);
	return 0;
}
static int bitmap_ok(unsigned char *leaf, size_t ps, int active, int missing)
{
	for (size_t i = 0; i < ps; i++) {
		unsigned char expected = i < ps - 9 ? (missing ? 0 : 0x64) : 0;
		if (active && i == ps - 3)
			expected = 8; /* RX buffer, GPA page 51 */
		if (active && i == ps - 1)
			expected = 0xc0; /* RX/TX rings, pages 70/71 */
		if (__atomic_load_n(leaf + i, __ATOMIC_ACQUIRE) != expected) {
			printf("bitmap byte %zu got=%#x expected=%#x\n", i,
			       leaf[i], expected);
			return 0;
		}
	}
	return 1;
}
static int filled(unsigned char *p, size_t len, unsigned char byte)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != byte)
			return 0;
	return 1;
}
static int exercise_case(size_t ps, unsigned shared, unsigned forked,
			 uint8_t mode)
{
	int backing = -1;
	if (shared) {
		backing = memfd_create("vhost-log", 0);
		CHECK(backing >= 0 && !ftruncate(backing, 4 * ps));
	}
	unsigned char *src = mmap(
		(void *)0x20000000, 4 * ps, PROT_READ | PROT_WRITE,
		MAP_FIXED_NOREPLACE |
			(shared ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS),
		backing, 0);
	unsigned char *dst =
		mmap((void *)0x30000000, 4 * ps, PROT_READ | PROT_WRITE,
		     MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(src != MAP_FAILED && dst != MAP_FAILED);
	for (unsigned i = 0; i < 4; i++)
		memset(src + i * ps, 0x61 + i, ps);
	memset(dst, 0x91, 4 * ps);
	CHECK(mremap(src + 3 * ps, ps, ps, MREMAP_MAYMOVE | MREMAP_FIXED,
		     dst + ps) == dst + ps);
	unsigned char *leaf = dst + ps, *bitmap = leaf + ps - 9;
	memset(bitmap, 0, 9);
	int barrier[2];
	pid_t child = -1;
	if (forked) {
		CHECK(!pipe(barrier));
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			close(barrier[1]);
			char token;
			int ok = read(barrier[0], &token, 1) == 1 &&
				 bitmap_ok(leaf, ps, shared, 0);
			_exit(ok ? 0 : 8);
		}
		close(barrier[0]);
	}
	int tap = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	CHECK(tap >= 0);
	struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI };
	strcpy(ifr.ifr_name, "abi%d");
	CHECK(!ioctl(tap, TUNSETIFF, &ifr));
	int raw = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(PROTOCOL));
	CHECK(raw >= 0);
	CHECK(!ioctl(raw, SIOCGIFINDEX, &ifr));
	int ifindex = ifr.ifr_ifindex;
	CHECK(!ioctl(raw, SIOCGIFFLAGS, &ifr));
	ifr.ifr_flags |= IFF_UP;
	CHECK(!ioctl(raw, SIOCSIFFLAGS, &ifr));
	int fd = open("/dev/vhost-net", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	CHECK(!ioctl(fd, VHOST_SET_FORK_FROM_OWNER, &mode));
	uint8_t gotmode = 9;
	CHECK(!ioctl(fd, VHOST_GET_FORK_FROM_OWNER, &gotmode) &&
	      gotmode == mode);
	CHECK(!ioctl(fd, VHOST_SET_OWNER, NULL));
	uint64_t log_base = (uintptr_t)bitmap;
	CHECK(!ioctl(fd, VHOST_SET_LOG_BASE, &log_base));
	unsigned char *data = mmap(NULL, ps, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(data != MAP_FAILED);
	struct {
		struct vhost_memory mem;
		struct vhost_memory_region region;
	} mem = {
		.mem.nregions = 1,
		.region = { .guest_phys_addr = DATA_GPA,
			    .memory_size = 4096,
			    .userspace_addr = (uintptr_t)data },
	};
	CHECK(!ioctl(fd, VHOST_SET_MEM_TABLE, &mem));
	uint64_t features = (1ULL << VHOST_F_LOG_ALL) |
			    (1ULL << VHOST_NET_F_VIRTIO_NET_HDR);
	uint64_t available;
	CHECK(!ioctl(fd, VHOST_GET_FEATURES, &available) &&
	      (available & features) == features);
	CHECK(!ioctl(fd, VHOST_SET_FEATURES, &features));
	struct queue *q = mmap(NULL, ps, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(q != MAP_FAILED && sizeof(*q) * 2 < ps);
	CHECK(!queue_setup(fd, q, 0) && !queue_setup(fd, q + 1, 1));
	for (unsigned index = 0; index < 2; index++) {
		struct vhost_vring_file backend = { .index = index, .fd = tap };
		CHECK(!ioctl(fd, VHOST_NET_SET_BACKEND, &backend));
	}
	struct sockaddr_ll dest = { .sll_family = AF_PACKET,
				    .sll_protocol = htons(PROTOCOL),
				    .sll_ifindex = ifindex,
				    .sll_halen = 6 };
	unsigned char packet[PACKET_SIZE];
	memset(packet, 0x5c, sizeof(packet));
	memset(packet, 0xff, 6);
	packet[6] = 2;
	packet[12] = PROTOCOL >> 8;
	packet[13] = PROTOCOL & 0xff;
	q[0].desc[0] = (struct vring_desc){ .addr = DATA_GPA,
					    .len = 512,
					    .flags = VRING_DESC_F_WRITE };
	q[1].desc[0] =
		(struct vring_desc){ .addr = DATA_GPA + 1024,
				     .len = sizeof(struct virtio_net_hdr) +
					    PACKET_SIZE };
	memcpy(data + 1024 + sizeof(struct virtio_net_hdr), packet,
	       PACKET_SIZE);
	for (unsigned phase = 0; phase < 2; phase++) {
		if (phase) {
			memset(bitmap, 0, 9);
			CHECK(!madvise(leaf, ps, MADV_DONTNEED));
			int pagemap = open("/proc/self/pagemap", O_RDONLY);
			uint64_t entry;
			CHECK(pagemap >= 0 &&
			      pread(pagemap, &entry, sizeof(entry),
				    (uintptr_t)leaf / ps * sizeof(entry)) ==
				      sizeof(entry));
			CHECK(!(entry & (1ULL << 63)));
			close(pagemap);
			for (unsigned index = 0; index < 2; index++) {
				struct vhost_vring_file backend = {
					.index = index, .fd = tap
				};
				CHECK(!ioctl(fd, VHOST_NET_SET_BACKEND,
					     &backend));
			}
		}
		for (unsigned round = phase * ROUNDS;
		     round < (phase + 1) * ROUNDS; round++) {
			packet[20] = round;
			memset(data, 0xaa, 512);
			CHECK(submit(q, round));
			CHECK(sendto(raw, packet, sizeof(packet), 0,
				     (void *)&dest,
				     sizeof(dest)) == sizeof(packet));
			CHECK(submit(q + 1, round));
			CHECK(complete(q, round) && complete(q + 1, round));
			CHECK(q[0].used.ring[round % N].id == 0);
			if (q[0].used.ring[round % N].len !=
			    sizeof(struct virtio_net_hdr) + PACKET_SIZE)
				printf("RX length=%u expected=%zu ethertype=%02x%02x\n",
				       q[0].used.ring[round % N].len,
				       sizeof(struct virtio_net_hdr) +
					       PACKET_SIZE,
				       data[sizeof(struct virtio_net_hdr) + 12],
				       data[sizeof(struct virtio_net_hdr) + 13]);
			CHECK(q[0].used.ring[round % N].len ==
			      sizeof(struct virtio_net_hdr) + PACKET_SIZE);
			CHECK(!memcmp(data + sizeof(struct virtio_net_hdr),
				      packet, PACKET_SIZE));
			CHECK(filled(data + sizeof(struct virtio_net_hdr) +
					     PACKET_SIZE,
				     512 - sizeof(struct virtio_net_hdr) -
					     PACKET_SIZE,
				     0xaa));
		}
		/* Stop and flush workers before checking the bitmap or discarding backing. */
		for (unsigned index = 0; index < 2; index++) {
			struct vhost_vring_file backend = { .index = index,
							    .fd = -1 };
			CHECK(!ioctl(fd, VHOST_NET_SET_BACKEND, &backend));
		}
		CHECK(bitmap_ok(leaf, ps, 1, phase && !shared));
		if (forked && !phase) {
			CHECK(write(barrier[1], "x", 1) == 1);
			close(barrier[1]);
			int status;
			CHECK(waitpid(child, &status, 0) == child &&
			      WIFEXITED(status) && !WEXITSTATUS(status));
		}
	}
	close(fd);
	close(raw);
	close(tap);
	for (unsigned i = 0; i < 2; i++) {
		close(q[i].kick);
		close(q[i].call);
	}
	CHECK(filled(dst, ps, 0x91) && filled(dst + 2 * ps, 2 * ps, 0x91));
	for (unsigned i = 0; i < 3; i++)
		CHECK(filled(src + i * ps, ps, 0x61 + i));
	CHECK(!munmap(src, 3 * ps) && !munmap(dst, 4 * ps));
	CHECK(!munmap(data, ps) && !munmap(q, ps));
	if (backing >= 0)
		close(backing);
	printf("ok - %zuK vhost %s fork=%u worker=%u: %u RX/TX packets, relocated dirty bits, missing-PTE repair and neighbors\n",
	       ps / 1024, shared ? "shared" : "private", forked, mode,
	       2 * ROUNDS);
	return 0;
}
static int exercise(size_t expected)
{
	CHECK(getauxval(AT_PAGESZ) == expected);
	alarm(45);
	for (uint8_t mode = 0; mode < 2; mode++)
		for (unsigned shared = 0; shared < 2; shared++)
			for (unsigned forked = 0; forked < 2; forked++)
				CHECK(!exercise_case(expected, shared, forked,
						     mode));
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	int fail = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
		perror("mount");
		fail = 1;
		goto done;
	}
	/* Keep unsolicited IPv6 DAD packets out of the isolated TAP queues. */
	int ipv6 =
		open("/proc/sys/net/ipv6/conf/default/disable_ipv6", O_WRONLY);
	if (ipv6 < 0 || write(ipv6, "1", 1) != 1) {
		perror("disable IPv6");
		fail = 1;
		goto done;
	}
	close(ipv6);
	size_t sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	for (unsigned i = 0; i < (sizes[0] > 16384 ? 3U : 2U); i++) {
		pid_t c = fork();
		int status = 0;
		if (!c) {
			char expected[24];
			snprintf(expected, sizeof(expected), "%zu", sizes[i]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", expected, NULL);
			_exit(5);
		}
		int ok = 0;
		for (unsigned ticks = 0; c > 0 && ticks < 600; ticks++) {
			pid_t got = waitpid(c, &status, WNOHANG);
			if (got == c) {
				ok = WIFEXITED(status) && !WEXITSTATUS(status);
				break;
			}
			if (ticks == 599) {
				kill(c, SIGKILL);
				waitpid(c, &status, 0);
				break;
			}
			usleep(100000);
		}
		printf("%s - ABI %zuK vhost log status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		fail += !ok;
	}
done:
	printf("VHOST LOG %s\n", fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
