/* SPDX-License-Identifier: MIT */
/*
 * mesa-m3-wlcheck: is a Wayland compositor alive and serving on SOCKET?
 * Used by mesa-m3-session-confirm before it marks a login with /opt/mesa-m3
 * as one that reached a running desktop.
 *
 *   mesa-m3-wlcheck [--timeout-ms N] SOCKET
 *
 * It connects to the Unix socket SOCKET (an absolute path), reads the
 * credentials of the process that listens on it (SO_PEERCRED: the compositor
 * that created the socket), asks for the registry and completes a
 * wl_display_sync round trip within the timeout (default 5000 ms). It passes
 * only when the round trip completes, the connection has no protocol error and
 * the compositor announced wl_compositor. No graphics library is loaded and
 * no surface is created.
 *
 * Prints key=value lines:
 *   peer_pid=  peer_uid=  globals=  wl_compositor=yes|no
 *   roundtrip=ok   or   roundtrip=fail <reason>
 * and exits 0 on success, 1 on failure, 2 on a usage error.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static int globals;
static int have_compositor;
static int done;

static void
global_add(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
   (void)data; (void)registry; (void)name; (void)version;
   globals++;
   if (strcmp(interface, "wl_compositor") == 0)
      have_compositor = 1;
}

static void
global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
   (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = { global_add, global_remove };

static void
sync_done(void *data, struct wl_callback *cb, uint32_t serial)
{
   (void)data; (void)serial;
   done = 1;
   wl_callback_destroy(cb);
}

static const struct wl_callback_listener sync_listener = { sync_done };

static long long
now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
fail(const char *why)
{
   printf("globals=%d\nwl_compositor=%s\nroundtrip=fail %s\n", globals, have_compositor ? "yes" : "no", why);
   return 1;
}

int
main(int argc, char **argv)
{
   long timeout_ms = 5000;
   const char *path = NULL;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
         char *end;
         timeout_ms = strtol(argv[++i], &end, 10);
         if (*end || timeout_ms <= 0 || timeout_ms > 600000) {
            fprintf(stderr, "mesa-m3-wlcheck: bad --timeout-ms\n");
            return 2;
         }
      } else if (!path && argv[i][0] == '/') {
         path = argv[i];
      } else {
         fprintf(stderr, "usage: mesa-m3-wlcheck [--timeout-ms N] /path/to/wayland-socket\n");
         return 2;
      }
   }
   if (!path) {
      fprintf(stderr, "usage: mesa-m3-wlcheck [--timeout-ms N] /path/to/wayland-socket\n");
      return 2;
   }
   long long deadline = now_ms() + timeout_ms;

   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   if (strlen(path) >= sizeof(addr.sun_path))
      return fail("socket path too long");
   strcpy(addr.sun_path, path);
   /* Non-blocking connect: a full backlog fails at once instead of hanging. */
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
   if (fd < 0)
      return fail("socket() failed");
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      char why[128];
      snprintf(why, sizeof(why), "connect: %s", strerror(errno));
      close(fd);
      return fail(why);
   }
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || len != sizeof(cred)) {
      close(fd);
      return fail("no peer credentials");
   }
   printf("peer_pid=%d\npeer_uid=%u\n", (int)cred.pid, (unsigned)cred.uid);

   struct wl_display *d = wl_display_connect_to_fd(fd);
   if (!d)
      return fail("wl_display_connect_to_fd failed");
   struct wl_registry *reg = wl_display_get_registry(d);
   wl_registry_add_listener(reg, &registry_listener, NULL);
   struct wl_callback *cb = wl_display_sync(d);
   wl_callback_add_listener(cb, &sync_listener, NULL);

   int rc = 0;
   const char *why = NULL;
   while (!done) {
      while (wl_display_prepare_read(d) != 0) {
         if (wl_display_dispatch_pending(d) < 0) {
            why = "protocol or connection error";
            goto out;
         }
      }
      if (done) {
         wl_display_cancel_read(d);
         break;
      }
      short events = POLLIN;
      if (wl_display_flush(d) < 0) {
         if (errno != EAGAIN) {
            wl_display_cancel_read(d);
            why = "write to the compositor failed";
            goto out;
         }
         events |= POLLOUT;
      }
      long long left = deadline - now_ms();
      if (left <= 0) {
         wl_display_cancel_read(d);
         why = "timeout";
         goto out;
      }
      struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = events };
      int n = poll(&pfd, 1, (int)left);
      if (n < 0 && errno == EINTR) {
         wl_display_cancel_read(d);
         continue;
      }
      if (n <= 0) {
         wl_display_cancel_read(d);
         why = n == 0 ? "timeout" : "poll failed";
         goto out;
      }
      if (pfd.revents & POLLIN) {
         if (wl_display_read_events(d) < 0) {
            why = "connection closed or read error";
            goto out;
         }
      } else {
         wl_display_cancel_read(d);
         if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            why = "connection closed";
            goto out;
         }
      }
      if (wl_display_dispatch_pending(d) < 0) {
         why = "protocol or connection error";
         goto out;
      }
   }
   if (wl_display_get_error(d))
      why = "protocol error";
   else if (!have_compositor)
      why = "no wl_compositor global";

out:
   if (why)
      rc = fail(why);
   else
      printf("globals=%d\nwl_compositor=yes\nroundtrip=ok\n", globals);
   wl_registry_destroy(reg);
   wl_display_disconnect(d);
   return rc;
}
