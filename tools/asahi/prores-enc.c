// SPDX-License-Identifier: GPL-2.0-only
/*
 * prores-enc: encode raw frames with a V4L2 ProRes encoder.
 *
 *   prores-enc -d /dev/videoN -f nv16|p216|nv24|p416 -s WxH -p 0..5
 *              -i input.yuv [-n frames] [-o outdir] [-l loops]
 *
 * The input holds tightly packed frames: Y rows, then CbCr rows. Each frame
 * is copied into a driver buffer once; encoding then loops over them, so
 * the reported rate excludes file I/O. With -o, frame K of the first loop is
 * written to outdir/frame-K.prores.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef V4L2_PIX_FMT_PRORES
#define V4L2_PIX_FMT_PRORES v4l2_fourcc('P', 'R', 'E', 'S')
#endif
#ifndef V4L2_PIX_FMT_P216
#define V4L2_PIX_FMT_P216 v4l2_fourcc('P', '2', '1', '6')
#define V4L2_PIX_FMT_P416 v4l2_fourcc('P', '4', '1', '6')
#endif
#ifndef V4L2_CID_MPEG_VIDEO_PRORES_PROFILE
#define V4L2_CID_MPEG_VIDEO_PRORES_PROFILE (V4L2_CID_CODEC_BASE + 658)
#endif

#define MAX_BUFS 8

struct buf {
	void *ptr;
	size_t len;
};

static int fd;

static void die(const char *what)
{
	fprintf(stderr, "prores-enc: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void xioctl(unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg))
		die(what);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int setup_queue(enum v4l2_buf_type type, unsigned int count, struct buf *bufs)
{
	struct v4l2_requestbuffers req = { .count = count, .type = type,
					   .memory = V4L2_MEMORY_MMAP };
	unsigned int i;

	xioctl(VIDIOC_REQBUFS, &req, "REQBUFS");
	for (i = 0; i < req.count; i++) {
		struct v4l2_plane plane = { 0 };
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
					 .index = i, .length = 1, .m.planes = &plane };

		xioctl(VIDIOC_QUERYBUF, &b, "QUERYBUF");
		bufs[i].len = plane.length;
		bufs[i].ptr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, plane.m.mem_offset);
		if (bufs[i].ptr == MAP_FAILED)
			die("mmap");
	}
	return req.count;
}

static void qbuf(enum v4l2_buf_type type, unsigned int index, size_t used)
{
	struct v4l2_plane plane = { .bytesused = used };
	struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
				 .index = index, .length = 1, .m.planes = &plane };

	xioctl(VIDIOC_QBUF, &b, "QBUF");
}

static int dqbuf(enum v4l2_buf_type type, size_t *used, uint32_t *flags)
{
	struct v4l2_plane plane = { 0 };
	struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
				 .length = 1, .m.planes = &plane };

	xioctl(VIDIOC_DQBUF, &b, "DQBUF");
	if (used)
		*used = plane.bytesused;
	if (flags)
		*flags = b.flags;
	return b.index;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0", *in = NULL, *outdir = NULL, *fmtname = "nv16";
	unsigned int w = 1920, h = 1080, frames = 0, loops = 1, profile = 2, i;
	enum v4l2_buf_type out_t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	enum v4l2_buf_type cap_t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct buf src[MAX_BUFS], dst[MAX_BUFS];
	unsigned int nsrc, ndst, bpp, chroma_mul, total, done = 0, errors = 0;
	size_t frame_bytes, coded_total = 0;
	uint32_t fourcc;
	struct v4l2_format f = { .type = out_t };
	struct v4l2_control ctrl;
	unsigned char *data;
	double t0, t1;
	struct stat st;
	int c, infd;

	while ((c = getopt(argc, argv, "d:f:s:p:i:n:o:l:")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'f': fmtname = optarg; break;
		case 's': sscanf(optarg, "%ux%u", &w, &h); break;
		case 'p': profile = atoi(optarg); break;
		case 'i': in = optarg; break;
		case 'n': frames = atoi(optarg); break;
		case 'o': outdir = optarg; break;
		case 'l': loops = atoi(optarg); break;
		default: return 2;
		}
	}
	if (!in)
		return fprintf(stderr, "usage: see source\n"), 2;
	if (!strcmp(fmtname, "nv16")) { fourcc = V4L2_PIX_FMT_NV16; bpp = 1; chroma_mul = 1; }
	else if (!strcmp(fmtname, "p216")) { fourcc = V4L2_PIX_FMT_P216; bpp = 2; chroma_mul = 1; }
	else if (!strcmp(fmtname, "nv24")) { fourcc = V4L2_PIX_FMT_NV24; bpp = 1; chroma_mul = 2; }
	else if (!strcmp(fmtname, "p416")) { fourcc = V4L2_PIX_FMT_P416; bpp = 2; chroma_mul = 2; }
	else return fprintf(stderr, "bad format\n"), 2;

	frame_bytes = (size_t)w * h * bpp * (1 + chroma_mul);
	infd = open(in, O_RDONLY);
	if (infd < 0 || fstat(infd, &st))
		die(in);
	if (!frames || frames > st.st_size / frame_bytes)
		frames = st.st_size / frame_bytes;
	if (!frames)
		return fprintf(stderr, "input shorter than one frame\n"), 1;
	data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, infd, 0);
	if (data == MAP_FAILED)
		die("mmap input");

	fd = open(dev, O_RDWR);
	if (fd < 0)
		die(dev);
	f.fmt.pix_mp.pixelformat = fourcc;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	xioctl(VIDIOC_S_FMT, &f, "S_FMT out");
	ctrl = (struct v4l2_control){ V4L2_CID_MPEG_VIDEO_PRORES_PROFILE, profile };
	xioctl(VIDIOC_S_CTRL, &ctrl, "S_CTRL profile");
	{
		struct v4l2_format cf = { .type = cap_t };

		cf.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_PRORES;
		xioctl(VIDIOC_S_FMT, &cf, "S_FMT cap");
	}

	nsrc = setup_queue(out_t, frames < MAX_BUFS ? frames : MAX_BUFS, src);
	ndst = setup_queue(cap_t, MAX_BUFS, dst);
	if (frames > nsrc)
		frames = nsrc;	/* frames stay resident in driver buffers */
	{
		unsigned int stride = f.fmt.pix_mp.plane_fmt[0].bytesperline, r, k;

		for (k = 0; k < frames; k++) {
			unsigned char *s = data + k * frame_bytes, *d = src[k].ptr;

			memset(d, 0, src[k].len);
			for (r = 0; r < h; r++)
				memcpy(d + (size_t)r * stride, s + (size_t)r * w * bpp, w * bpp);
			for (r = 0; r < h; r++)
				memcpy(d + (size_t)stride * h + (size_t)r * stride * chroma_mul,
				       s + (size_t)w * h * bpp + (size_t)r * w * bpp * chroma_mul,
				       w * bpp * chroma_mul);
		}
	}

	for (i = 0; i < ndst; i++)
		qbuf(cap_t, i, 0);
	xioctl(VIDIOC_STREAMON, &cap_t, "STREAMON cap");
	xioctl(VIDIOC_STREAMON, &out_t, "STREAMON out");

	total = frames * loops;
	t0 = now();
	for (i = 0; i < frames && i < total; i++)
		qbuf(out_t, i, src[i].len);
	while (done < total) {
		size_t used;
		uint32_t flags;
		int di = dqbuf(cap_t, &used, &flags);
		int si = dqbuf(out_t, NULL, NULL);

		if ((flags & V4L2_BUF_FLAG_ERROR) || !used) {
			errors++;
		} else if (outdir && done < frames) {
			char name[512];
			FILE *o;

			snprintf(name, sizeof(name), "%s/frame-%04u.prores", outdir, done);
			o = fopen(name, "wb");
			if (!o || fwrite(dst[di].ptr, 1, used, o) != used)
				die(name);
			fclose(o);
		}
		coded_total += used;
		done++;
		qbuf(cap_t, di, 0);
		if (i < total) {
			qbuf(out_t, si, src[si].len);
			i++;
		}
	}
	t1 = now();
	printf("frames %u errors %u time %.3f s fps %.1f avg_bytes %zu\n",
	       done, errors, t1 - t0, done / (t1 - t0), coded_total / done);
	xioctl(VIDIOC_STREAMOFF, &out_t, "STREAMOFF out");
	xioctl(VIDIOC_STREAMOFF, &cap_t, "STREAMOFF cap");
	return errors ? 1 : 0;
}
