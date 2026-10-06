// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/tls.h>
#include <linux/sockios.h>
#include <netinet/udp.h>
#include "../../kselftest.h"

static bool use_tcp, use_ipv6, use_udp;

static int connected_pair(int sockets[2])
{
	struct sockaddr_in a4 = { .sin_family = AF_INET,
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct sockaddr_in6 a6 = { .sin6_family = AF_INET6,
				   .sin6_addr = IN6ADDR_LOOPBACK_INIT };
	struct sockaddr_storage peer;
	struct sockaddr *address = use_ipv6 ? (void *)&a6 : (void *)&a4;
	socklen_t size = use_ipv6 ? sizeof(a6) : sizeof(a4),
		  peer_size = sizeof(peer);
	int domain = use_ipv6 ? AF_INET6 : AF_INET;
	int listener, saved;

	if (!use_tcp && !use_udp)
		return socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	listener = socket(domain, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	sockets[0] = -1;
	if (bind(listener, address, size) ||
	    getsockname(listener, address, &size))
		goto error;
	if (!use_udp && listen(listener, 1))
		goto error;
	sockets[0] = socket(domain, use_udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	if (sockets[0] < 0 || connect(sockets[0], address, size))
		goto error;
	if (use_udp) {
		if (getsockname(sockets[0], (void *)&peer, &peer_size) ||
		    connect(listener, (void *)&peer, peer_size))
			goto error;
		sockets[1] = listener;
		return 0;
	}
	sockets[1] = accept(listener, NULL, NULL);
	if (sockets[1] < 0)
		goto error;
	close(listener);
	return 0;
error:
	saved = errno;
	if (sockets[0] >= 0)
		close(sockets[0]);
	close(listener);
	errno = saved;
	return -1;
}

static unsigned char value(size_t off)
{
	return (off * 13 + (off >> 12) * 31) & 255;
}
static void fill(unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = value(i);
}
static bool check(const unsigned char *p, size_t n, size_t off)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != value(i + off)) {
			ksft_print_msg(
				"data mismatch at %zu: got %02x expected %02x\n",
				i + off, p[i], value(i + off));
			return false;
		}
	return true;
}
static bool read_exact(int fd, unsigned char *p, size_t n)
{
	while (n) {
		ssize_t got = read(fd, p, n);

		if (got <= 0) {
			ksft_print_msg(
				"read fd=%d got=%zd remaining=%zu errno=%d\n",
				fd, got, n, errno);
			return false;
		}
		p += got;
		n -= got;
	}
	return true;
}
static bool replace(unsigned char *p, size_t n)
{
	if (munmap(p, n) ||
	    mmap(p, n, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != p)
		return false;
	memset(p, 0xec, n);
	return true;
}
static bool cow(unsigned char *p, size_t n)
{
	pid_t child = fork();
	int status = 0;

	if (!child)
		_exit(0);
	if (child < 0 || waitpid(child, &status, 0) != child || status)
		return false;
	memset(p, 0xab, n);
	return true;
}
static void report(bool ok, size_t ps, const char *name)
{
	ksft_test_result(ok, "%zuK %s\n", ps / 1024, name);
}
static bool tls_pair(int sockets[2], unsigned int version)
{
	struct tls12_crypto_info_aes_gcm_128 crypto = {
		.info.version = version,
		.info.cipher_type = TLS_CIPHER_AES_GCM_128,
	};
	struct timeval timeout = { .tv_sec = 5 };
	int no_padding = 1;

	if (connected_pair(sockets))
		return false;
	return !setsockopt(sockets[0], IPPROTO_TCP, TCP_ULP, "tls", 4) &&
	       !setsockopt(sockets[1], IPPROTO_TCP, TCP_ULP, "tls", 4) &&
	       !setsockopt(sockets[0], SOL_TLS, TLS_TX, &crypto,
			   sizeof(crypto)) &&
	       !setsockopt(sockets[1], SOL_TLS, TLS_RX, &crypto,
			   sizeof(crypto)) &&
	       (version != TLS_1_3_VERSION ||
		!setsockopt(sockets[1], SOL_TLS, TLS_RX_EXPECT_NO_PAD,
			    &no_padding, sizeof(no_padding))) &&
	       !setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
			   sizeof(timeout));
}

static void tls_checks(size_t ps, unsigned char *p, size_t len,
		       unsigned char *back)
{
	int sockets[2], fds[2];
	char name[160];

	if (pipe(fds))
		ksft_exit_fail_msg("TLS pipe errno=%d\n", errno);
	for (unsigned int v = 0; v < 2; v++) {
		unsigned int version = v ? TLS_1_3_VERSION : TLS_1_2_VERSION;
		struct iovec vec;
		bool ok;

		if (!tls_pair(sockets, version))
			ksft_exit_fail_msg("TLS %x setup errno=%d\n", version,
					   errno);
		for (unsigned int mode = 0; mode < 2; mode++) {
			const size_t count = mode ? 4096 : 4096 - 29;
			const size_t offset = ps + (mode ? 0 : 29);

			fill(p, len);
			vec = (struct iovec){ p + offset, count };
			if (vmsplice(fds[1], &vec, 1, 0) != (ssize_t)count ||
			    splice(fds[0], NULL, sockets[0], NULL, count,
				   SPLICE_F_MORE) != (ssize_t)count)
				ksft_exit_fail_msg("TLS MORE splice errno=%d\n",
						   errno);
			errno = 0;
			if (recv(sockets[1], back, len, MSG_DONTWAIT) != -1 ||
			    errno != EAGAIN)
				ksft_exit_fail_msg(
					"TLS pending plaintext precondition errno=%d\n",
					errno);
			ok = mode ? cow(p + ps, 4096) : replace(p + ps, ps);
			ok &= send(sockets[0], "!", 1, 0) == 1;
			ok &= read_exact(sockets[1], back, count + 1) &&
			      check(back, count, offset) && back[count] == '!';
			snprintf(name, sizeof(name),
				 "TLS 1.%u pending plaintext survives %s",
				 v + 2,
				 mode ? "fork/COW" : "unmap/replacement");
			report(ok, ps, name);
		}
		{
			const size_t bytes = 3 * 16384 + 123, prefix = 137;
			const size_t mapping = (bytes + ps - 1) & ~(ps - 1);
			unsigned char *source =
				mmap(NULL, mapping, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			unsigned char *received = malloc(bytes + prefix);

			if (source == MAP_FAILED || !received)
				ksft_exit_fail_msg(
					"TLS multi-record allocation\n");
			fill(source, mapping);
			ok = send(sockets[0], source, prefix, 0) ==
			     (ssize_t)prefix;
			vec = (struct iovec){ source, bytes };
			if (vmsplice(fds[1], &vec, 1, 0) != (ssize_t)bytes ||
			    splice(fds[0], NULL, sockets[0], NULL, bytes, 0) !=
				    (ssize_t)bytes)
				ksft_exit_fail_msg(
					"TLS multi-record splice errno=%d\n",
					errno);
			ok &= replace(source, mapping);
			ok &= read_exact(sockets[1], received,
					 bytes + prefix) &&
			      check(received, prefix, 0) &&
			      check(received + prefix, bytes, 0);
			snprintf(
				name, sizeof(name),
				"TLS 1.%u ordinary send and multiple spliced records",
				v + 2);
			report(ok, ps, name);
			munmap(source, mapping);
			free(received);
		}
		{
			const size_t bytes = 5003;
			int file = memfd_create("tls-receive", 0);
			unsigned char *mapped;

			if (file < 0 || ftruncate(file, 3 * ps))
				ksft_exit_fail_msg("TLS receive file setup\n");
			mapped = mmap(NULL, 3 * ps, PROT_READ | PROT_WRITE,
				      MAP_SHARED, file, 0);
			if (mapped == MAP_FAILED)
				ksft_exit_fail_msg(
					"TLS receive file mapping\n");
			memset(mapped, 0x5c, 3 * ps);
			fill(p, len);
			ok = send(sockets[0], p, bytes, 0) == (ssize_t)bytes &&
			     read_exact(sockets[1], mapped + 17, bytes);
			ok &= !msync(mapped, 3 * ps, MS_SYNC) &&
			      pread(file, back, bytes + 34, 0) ==
				      (ssize_t)(bytes + 34) &&
			      check(back + 17, bytes, 0);
			for (unsigned int i = 0; i < 17; i++)
				ok &= back[i] == 0x5c &&
				      back[bytes + 17 + i] == 0x5c;
			snprintf(
				name, sizeof(name),
				"TLS 1.%u decrypts into shared file with neighboring bytes intact",
				v + 2);
			report(ok, ps, name);
			munmap(mapped, 3 * ps);
			close(file);
		}
		{
			unsigned char *mapped =
				mmap(NULL, 6 * ps, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			struct iovec vectors[4];
			struct msghdr message = { .msg_iov = vectors,
						  .msg_iovlen = 4 };

			if (mapped == MAP_FAILED)
				ksft_exit_fail_msg(
					"TLS receive iovec mapping\n");
			memset(mapped, 0x81, 6 * ps);
			vectors[0] = (struct iovec){ mapped + 13, 5003 };
			vectors[1] = (struct iovec){ NULL, 0 };
			vectors[2] =
				(struct iovec){ mapped + 2 * ps + 7, 2222 };
			vectors[3] =
				(struct iovec){ mapped + 4 * ps + 5, 1010 };
			fill(p, len);
			ok = send(sockets[0], p, 8235, 0) == 8235 &&
			     recvmsg(sockets[1], &message, MSG_WAITALL) ==
				     8235 &&
			     check(mapped + 13, 5003, 0) &&
			     check(mapped + 2 * ps + 7, 2222, 5003) &&
			     check(mapped + 4 * ps + 5, 1010, 7225);
			ok &= mapped[12] == 0x81 && mapped[5016] == 0x81 &&
			      mapped[2 * ps + 6] == 0x81 &&
			      mapped[2 * ps + 2229] == 0x81 &&
			      mapped[4 * ps + 4] == 0x81 &&
			      mapped[4 * ps + 1015] == 0x81;
			snprintf(
				name, sizeof(name),
				"TLS 1.%u scattered receive, empty iovec and subpage boundaries",
				v + 2);
			report(ok, ps, name);
			munmap(mapped, 6 * ps);
		}
		close(sockets[0]);
		close(sockets[1]);
		ok = true;
		for (unsigned int round = 0; round < 64 && ok; round++) {
			if (!tls_pair(sockets, version))
				ksft_exit_fail_msg(
					"TLS teardown setup errno=%d\n", errno);
			fill(p, len);
			vec = (struct iovec){ p + ps, 4096 };
			ok = vmsplice(fds[1], &vec, 1, 0) == 4096 &&
			     splice(fds[0], NULL, sockets[0], NULL, 4096,
				    SPLICE_F_MORE) == 4096;
			close(sockets[1]);
			close(sockets[0]);
			ok &= !madvise(p + ps, ps, MADV_DONTNEED);
			memset(p + ps, 0x75, ps);
		}
		snprintf(name, sizeof(name),
			 "TLS 1.%u repeated pending-record teardown and reuse",
			 v + 2);
		report(ok, ps, name);
	}
	close(fds[0]);
	close(fds[1]);
}

static int exercise(void)
{
	const size_t ps = getauxval(AT_PAGESZ), len = 8 * ps;
	unsigned char *p, *back = malloc(len);
	int fds[2], dup[2], file, sockets[2];
	struct iovec v[3];
	struct stat self, init;
	struct timeval timeout = { .tv_sec = 5 };
	bool ok;
	ssize_t result;

	if (stat("/proc/self/exe", &self) || stat("/proc/1/exe", &init) ||
	    self.st_dev != init.st_dev || self.st_ino != init.st_ino)
		return 2;
	p = mmap((void *)0x300000000UL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED || !back || pipe(fds) || pipe(dup))
		ksft_exit_fail_msg("setup\n");
	ksft_print_header();
	ksft_set_plan(use_tcp ? 22 : 10);
	ksft_print_msg("transport=%s family=%s\n",
		       use_tcp ? "TCP" :
		       use_udp ? "UDP" :
				 "Unix stream",
		       use_ipv6 ? "IPv6" : "IPv4/Unix");
	fill(p, len);
	v[0] = (struct iovec){ NULL, 0 };
	v[1] = (struct iovec){ p + 17, ps - 17 };
	v[2] = (struct iovec){ p + 2 * ps + 31, 2 * ps - 31 };
	result = vmsplice(fds[1], v, 3, 0);
	if (result != (ssize_t)(3 * ps - 48))
		ksft_exit_fail_msg("vmsplice returned %zd errno=%d\n", result,
				   errno);
	ok = read_exact(fds[0], back, result) && check(back, ps - 17, 17) &&
	     check(back + ps - 17, 2 * ps - 31, 2 * ps + 31);
	report(ok, ps, "unaligned fragmented iovecs and empty segment");
	v[0] = (struct iovec){ p, 16384 };
	ok = fcntl(fds[1], F_SETPIPE_SZ, 16384) == 16384;
	result = vmsplice(fds[1], v, 1, SPLICE_F_NONBLOCK);
	{
		int pending = 0;

		ok &= result == 16384 && !ioctl(fds[0], FIONREAD, &pending) &&
		      pending == 16384;
	}
	if (result > 0)
		ok &= read_exact(fds[0], back, result) &&
		      check(back, result, 0);
	report(ok, ps,
	       "contiguous quarters retain one-native-page pipe capacity");
	fcntl(fds[1], F_SETPIPE_SZ, 16 * 16384);
	v[0] = (struct iovec){ p + 11, 3 * ps - 11 };
	result = vmsplice(fds[1], v, 1, 0);
	if (result != (ssize_t)(3 * ps - 11) ||
	    tee(fds[0], dup[1], result, 0) != result)
		ksft_exit_fail_msg("tee setup\n");
	ok = replace(p, len);
	ok &= read_exact(fds[0], back, 19) && check(back, 19, 11);
	ok &= read_exact(fds[0], back + 19, result - 19) &&
	      check(back, result, 11);
	ok &= read_exact(dup[0], back, result) && check(back, result, 11);
	report(ok, ps,
	       "tee and partial reads retain data across unmap/replacement");
	fill(p, len);
	v[0] = (struct iovec){ p, ps };
	ok = vmsplice(fds[1], v, 1, 0) == (ssize_t)ps && cow(p, ps);
	ok &= read_exact(fds[0], back, ps) && check(back, ps, 0);
	report(ok, ps, "pipe GET retains pre-COW data after fork sharer exit");
	fill(p, len);
	file = memfd_create("splice-target", 0);
	v[0] = (struct iovec){ p + ps + 29, 2 * ps - 29 };
	result = vmsplice(fds[1], v, 1, SPLICE_F_GIFT);
	ok = file >= 0 && result == (ssize_t)(2 * ps - 29) &&
	     splice(fds[0], NULL, file, NULL, result, SPLICE_F_MOVE) == result;
	ok &= pread(file, back, result, 0) == result &&
	      check(back, result, ps + 29);
	report(ok, ps, "gifted fragments splice to buffered file");
	close(file);
	v[0] = (struct iovec){ p, ps };
	v[1] = (struct iovec){ (void *)0x1000, ps };
	result = vmsplice(fds[1], v, 2, 0);
	ok = result == (ssize_t)ps && read_exact(fds[0], back, ps) &&
	     check(back, ps, 0);
	report(ok, ps, "partial vmsplice before inaccessible iovec");
	if (connected_pair(sockets))
		ksft_exit_fail_msg("socketpair errno=%d\n", errno);
	setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
		   sizeof(timeout));
	for (unsigned int mode = 0; mode < 3; mode++) {
		const size_t bytes = mode == 1 ? ps :
						 (use_udp && ps == 4096 ?
							  6 * ps - 256 :
							  2 * ps - 256);
		const size_t offset = mode == 1 ? 0 : ps + 29;
		int cork = 1;

		if ((use_tcp || use_udp) && mode < 2 &&
		    setsockopt(sockets[0], use_tcp ? IPPROTO_TCP : IPPROTO_UDP,
			       use_tcp ? TCP_CORK : UDP_CORK, &cork,
			       sizeof(cork)))
			ksft_exit_fail_msg("TCP_CORK setup errno=%d\n", errno);
		fill(p, len);
		v[0] = (struct iovec){ p + offset, bytes };
		ok = vmsplice(fds[1], v, 1, 0) == (ssize_t)bytes &&
		     splice(fds[0], NULL, sockets[0], NULL, bytes,
			    SPLICE_F_NONBLOCK) == (ssize_t)bytes;
		if (!ok)
			ksft_exit_fail_msg("socket splice setup errno=%d\n",
					   errno);
		if (mode == 2) {
			ok = splice(sockets[1], NULL, fds[1], NULL, bytes, 0) ==
				     (ssize_t)bytes &&
			     tee(fds[0], dup[1], bytes, 0) == (ssize_t)bytes;
			if (!ok)
				ksft_exit_fail_msg(
					"socket-to-pipe setup errno=%d\n",
					errno);
		}
		if (use_tcp && mode < 2) {
			int unsent = 0;

			if (ioctl(sockets[0], SIOCOUTQNSD, &unsent) ||
			    unsent != (int)bytes)
				ksft_exit_fail_msg(
					"TCP unsent precondition: %d expected %zu errno=%d\n",
					unsent, bytes, errno);
		}
		if (use_udp && mode < 2) {
			errno = 0;
			if (recv(sockets[1], back, len, MSG_DONTWAIT) != -1 ||
			    errno != EAGAIN)
				ksft_exit_fail_msg(
					"UDP corked precondition errno=%d\n",
					errno);
		}
		ok &= mode == 1 ? cow(p, ps) : replace(p + ps, 2 * ps);
		if ((use_tcp || use_udp) && mode < 2) {
			cork = 0;
			if (setsockopt(sockets[0],
				       use_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				       use_tcp ? TCP_CORK : UDP_CORK, &cork,
				       sizeof(cork))) {
				ksft_print_msg("uncork errno=%d\n", errno);
				ok = false;
			}
		}
		ok &= read_exact(mode == 2 ? fds[0] : sockets[1], back,
				 bytes) &&
		      check(back, bytes, offset);
		if (mode == 2)
			ok &= read_exact(dup[0], back, bytes) &&
			      check(back, bytes, offset);
		report(ok, ps,
		       mode == 0 ?
			       "socket retains quarters across partial unmap/replacement" :
		       mode == 1 ?
			       "socket GET retains pre-COW data after fork sharer exit" :
			       "socket-to-pipe and tee retain independent quarter ownership");
	}
	close(sockets[0]);
	close(sockets[1]);
	ok = true;
	for (unsigned int round = 0; round < 128 && ok; round++) {
		fill(p, len);
		ok = !connected_pair(sockets);
		v[0] = (struct iovec){ p + ps, ps };
		ok &= vmsplice(fds[1], v, 1, 0) == (ssize_t)ps &&
		      splice(fds[0], NULL, sockets[0], NULL, ps, 0) ==
			      (ssize_t)ps;
		close(sockets[1]);
		close(sockets[0]);
		ok &= !madvise(p + ps, ps, MADV_DONTNEED);
		memset(p + ps, 0x43, ps);
	}
	report(ok, ps, "repeated unread socket teardown and quarter reuse");
	if (use_tcp)
		tls_checks(ps, p, len, back);
	close(fds[0]);
	close(fds[1]);
	close(dup[0]);
	close(dup[1]);
	munmap(p, len);
	free(back);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}
static bool async_cleanup_balanced(void)
{
	const char *names[] = { "user4k_rx_deferred", "user4k_rx_released" };
	unsigned long counts[2];
	char path[128];
	FILE *file = fopen("/sys/module/tls/parameters/user4k_test_async", "r");
	int enabled;

	if (!file)
		return false;
	enabled = fgetc(file);
	fclose(file);
	for (unsigned int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "/sys/module/tls/parameters/%s",
			 names[i]);
		file = fopen(path, "r");
		if (!file)
			return false;
		if (fscanf(file, "%lu", &counts[i]) != 1) {
			fclose(file);
			return false;
		}
		fclose(file);
	}
	printf("# TLS async=%c deferred=%lu released=%lu\n", enabled, counts[0],
	       counts[1]);
	return counts[0] == counts[1] && (enabled != 'Y' || counts[0] > 0);
}

int main(int argc, char **argv)
{
	bool ok = true;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 &&
	    (!strcmp(argv[1], "--test") || !strcmp(argv[1], "--tcp") ||
	     !strcmp(argv[1], "--tcp6") || !strcmp(argv[1], "--udp") ||
	     !strcmp(argv[1], "--udp6")) &&
	    getppid() == 1) {
		use_tcp = !strncmp(argv[1], "--tcp", 5);
		use_udp = !strncmp(argv[1], "--udp", 5);
		use_ipv6 = strchr(argv[1], '6') != NULL;
		return exercise();
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Run only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		return 3;
	{
		struct ifreq request = { .ifr_name = "lo" };
		int fd = socket(AF_INET, SOCK_DGRAM, 0);

		if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &request))
			return 4;
		request.ifr_flags |= IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &request))
			return 4;
		close(fd);
	}
	for (unsigned int mode = 0; mode < 10; mode++) {
		unsigned int small = mode & 1;
		pid_t pid = fork();
		int status = 0;

		if (!pid) {
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
				  small ? 4096UL : 0UL, 0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init",
			      mode >= 8 ? "--udp6" :
			      mode >= 6 ? "--udp" :
			      mode >= 4 ? "--tcp6" :
			      mode >= 2 ? "--tcp" :
					  "--test",
			      NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
	}
	ok &= async_cleanup_balanced();
	printf("%s - splice fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
