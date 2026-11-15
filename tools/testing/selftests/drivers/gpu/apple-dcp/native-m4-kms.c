/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Run only through run-native-m4-kms.py with the fresh-boot rollback guard.
 * This test temporarily owns DRM master and changes the internal display.
 * The desktop must already have been stopped by the qualification operator.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		exit(1);
	}
}

static void complete(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data)
{
	(void)fd; (void)sequence; (void)sec; (void)usec;
	unsigned *seen = data;
	require(seen && !*seen, "no duplicate or foreign flip completion");
	++*seen;
}

static void wait_flip(int fd, unsigned *seen)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	drmEventContext events = { .version = 2, .page_flip_handler = complete };
	require(poll(&p, 1, 7000) == 1 && p.revents == POLLIN, "bounded flip event");
	require(!drmHandleEvent(fd, &events) && *seen == 1, "one matching flip completion");
}

static uint32_t internal_crtc(int fd, const drmModeRes *res,
			      const drmModeConnector *con)
{
	uint32_t possible = 0, current = 0;
	for (int i = 0; i < con->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(fd, con->encoders[i]);
		require(encoder != NULL, "eDP encoder");
		possible |= encoder->possible_crtcs;
		if (encoder->encoder_id == con->encoder_id)
			current = encoder->crtc_id;
		drmModeFreeEncoder(encoder);
	}
	for (int i = 0; i < res->count_crtcs && i < 32; i++)
		if ((possible & (1U << i)) && res->crtcs[i] == current)
			return current;
	for (int i = 0; i < res->count_crtcs && i < 32; i++)
		if (possible & (1U << i))
			return res->crtcs[i];
	require(0, "eDP encoder has an available CRTC");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "--execute")) {
		puts("Use run-native-m4-kms.py; hardware execution requires --execute.");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	int fd = drmOpen("apple", NULL);
	require(fd >= 0, "open native DCP DRM node");
	drmVersionPtr version = drmGetVersion(fd);
	require(version && version->name_len == 5 &&
		!memcmp(version->name, "apple", 5), "shared appledrm driver identity");
	drmFreeVersion(version);
	require(!drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1), "atomic capability");
	require(!drmSetMaster(fd), "DRM master (desktop must be stopped)");
	drmModeRes *res = drmModeGetResources(fd);
	/*
	 * Connector ordering is not an ABI. Resolve an internal CRTC from the
	 * eDP encoder's possible_crtcs mask; leave external pipelines alone.
	 */
	require(res && res->count_crtcs >= 1 && res->count_connectors >= 1, "internal pipeline");
	drmModeConnector *con = NULL;
	for (int i = 0; i < res->count_connectors; ++i) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		require(c != NULL, "connector resources");
		if (c->connector_type == DRM_MODE_CONNECTOR_eDP) {
			require(!con, "single eDP connector");
			con = c;
		} else {
			drmModeFreeConnector(c);
		}
	}
	require(con && con->connector_type == DRM_MODE_CONNECTOR_eDP &&
		con->connection == DRM_MODE_CONNECTED && con->count_modes == 1, "connected fixed-mode eDP");
	drmModeModeInfo mode = con->modes[0];
	require(mode.hdisplay == 2560 && mode.vdisplay == 1664, "qualified panel dimensions");
	uint32_t crtc = internal_crtc(fd, res, con), connector = con->connector_id;
	printf("KMS shared-driver=apple connector=%u crtc=%u mode=%ux%u\n",
		connector, crtc, mode.hdisplay, mode.vdisplay);
	uint32_t fbs[2], handles[2];
	void *pixels[2];
	size_t sizes[2];
	for (unsigned i = 0; i < 2; ++i) {
		struct drm_mode_create_dumb create = { .width = mode.hdisplay,
			.height = mode.vdisplay, .bpp = 32 };
		require(!drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create), "create dumb framebuffer");
		handles[i] = create.handle;
		sizes[i] = create.size;
		uint32_t hs[4] = { create.handle }, ps[4] = { create.pitch }, offsets[4] = {0};
		require(!drmModeAddFB2(fd, create.width, create.height, DRM_FORMAT_XRGB8888,
			hs, ps, offsets, &fbs[i], 0), "linear XRGB framebuffer");
		struct drm_mode_map_dumb map = { .handle = create.handle };
		require(!drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map), "map dumb framebuffer");
		pixels[i] = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
		require(pixels[i] != MAP_FAILED, "mmap dumb framebuffer");
		for (unsigned y = 0; y < create.height; ++y) {
			uint32_t *row = (uint32_t *)((char *)pixels[i] + y * create.pitch);
			for (unsigned x = 0; x < create.width; ++x)
				row[x] = ((x / 160 + y / 104) & 1) ? 0xffffff : (i ? 0x204080 : 0x408020);
		}
	}
	require(!drmModeSetCrtc(fd, crtc, fbs[0], 0, 0, &connector, 1, &mode), "initial native modeset");
	/* Distinct cookies remain alive for every request, including late replies. */
	unsigned seen[61] = {0};
	for (unsigned i = 0; i < 60; ++i) {
		require(!drmModePageFlip(fd, crtc, fbs[(i + 1) & 1], DRM_MODE_PAGE_FLIP_EVENT, &seen[i]), "page flip");
		wait_flip(fd, &seen[i]);
		if (i % 10 == 9)
			printf("KMS completed flips=%u\n", i + 1);
	}
	/* No new plane state: this legal no-op must not poison the firmware owner. */
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	require(props != NULL, "CRTC properties");
	uint32_t active = 0;
	for (unsigned i = 0; i < props->count_props; ++i) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, "ACTIVE"))
			active = p->prop_id;
		drmModeFreeProperty(p);
	}
	require(active != 0, "ACTIVE property");
	drmModeAtomicReq *request = drmModeAtomicAlloc();
	require(request && drmModeAtomicAddProperty(request, crtc, active, 1) >= 0, "CRTC-only request");
	require(!drmModeAtomicCommit(fd, request, DRM_MODE_PAGE_FLIP_EVENT, &seen[60]), "CRTC-only atomic commit");
	wait_flip(fd, &seen[60]);
	drmModeAtomicFree(request);
	drmModeFreeObjectProperties(props);
	for (unsigned i = 0; i < 20; i++) {
		require(!drmModeSetCrtc(fd, crtc, 0, 0, 0, NULL, 0, NULL), "logical blank");
		require(!drmModeSetCrtc(fd, crtc, fbs[0], 0, 0, &connector, 1, &mode), "unblank and restore frame");
	}
	puts("PASS: native eDP, 60 matching page flips, CRTC-only commit, 20 blank/unblank cycles");
	/*
	 * File close drops the userspace handles and framebuffer ownership.
	 * The common driver must keep the active direct-DMA framebuffer alive
	 * until the firmware's exact successor receipt or the next blank.
	 */
	for (unsigned i = 0; i < 2; ++i) {
		munmap(pixels[i], sizes[i]);
		struct drm_mode_destroy_dumb destroy = { .handle = handles[i] };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	drmModeFreeConnector(con);
	drmModeFreeResources(res);
	close(fd);
	return 0;
}
