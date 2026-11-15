// SPDX-License-Identifier: GPL-2.0-only
/* V4L2 delivery stress test. Does not infer sensor FPS from a driver counter. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_MODES 256
#define MAX_BUFFERS 16

struct mode {
	uint32_t fourcc, width, height;
};

struct mapping {
	void *address;
	size_t size;
};

static int xioctl(int fd, unsigned long cmd, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static double monotonic(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static int capture(int fd, struct mode mode, unsigned int seconds,
		   unsigned int fps, unsigned int cycle)
{
	struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers req = {
		.count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
	};
	struct mapping maps[MAX_BUFFERS] = {};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned int frames = 0, bad = 0, gaps = 0, sequence_gaps = 0, mapped = 0;
	unsigned int last_sequence = 0;
	double start, end, first = 0, last = 0, max_gap = 0, measured_fps = 0;
	bool streaming = false;
	int ret = -1;

	format.fmt.pix.width = mode.width;
	format.fmt.pix.height = mode.height;
	format.fmt.pix.pixelformat = mode.fourcc;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0)
		goto error;
	/* Fail instead of counting a silently substituted mode as tested. */
	if (format.fmt.pix.width != mode.width || format.fmt.pix.height != mode.height ||
	    format.fmt.pix.pixelformat != mode.fourcc) {
		fprintf(stderr, "requested mode was substituted\n");
		goto out;
	}
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
		goto error;
	if (parm.parm.capture.timeperframe.numerator != 1 ||
	    parm.parm.capture.timeperframe.denominator != fps) {
		fprintf(stderr, "requested rate was substituted\n");
		goto out;
	}
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		goto error;
	if (req.count < 2 || req.count > MAX_BUFFERS)
		goto out;
	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_buffer buf = {
			.type = type, .memory = V4L2_MEMORY_MMAP, .index = i,
		};

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
			goto error;
		maps[i].size = buf.length;
		maps[i].address = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				       MAP_SHARED, fd, buf.m.offset);
		if (maps[i].address == MAP_FAILED)
			goto error;
		mapped++;
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
			goto error;
	}
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
		goto error;
	streaming = true;
	start = monotonic();
	end = start + seconds;
	while (monotonic() < end) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct v4l2_buffer buf = { .type = type, .memory = V4L2_MEMORY_MMAP };
		double timestamp, gap;
		int ready = poll(&pfd, 1, 2000);

		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "stream stalled or queue failed\n");
			goto out;
		}
		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0)
			goto error;
		if (buf.index >= req.count || buf.bytesused > maps[buf.index].size)
			goto out;
		if (buf.flags & V4L2_BUF_FLAG_ERROR)
			bad++;
		timestamp = buf.timestamp.tv_sec + buf.timestamp.tv_usec / 1000000.0;
		if (frames) {
			gap = timestamp - last;
			if (gap > max_gap)
				max_gap = gap;
			if (gap <= 0 || gap > 1.5 / fps)
				gaps++;
			if ((uint32_t)(buf.sequence - last_sequence) != 1)
				sequence_gaps++;
		} else {
			first = timestamp;
		}
		last = timestamp;
		last_sequence = buf.sequence;
		frames++;
		/* Requeue immediately; image copies/conversion do not limit the test. */
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
			goto error;
	}
	if (frames > 1 && last > first)
		measured_fps = (frames - 1) / (last - first);
	/* Allow clock/29.97-vs-30 rounding; gaps and buffer errors must be zero. */
	ret = bad || gaps || sequence_gaps || measured_fps < fps * 0.998 ? 1 : 0;
	goto out;
error:
	perror("V4L2 ioctl");
out:
	if (streaming && xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
		perror("STREAMOFF");
		ret = -1;
	}
	for (unsigned int i = 0; i < mapped; i++)
		munmap(maps[i].address, maps[i].size);
	req.count = 0;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		ret = -1;
	printf("{\"cycle\":%u,\"fourcc\":\"%.4s\",\"width\":%u,\"height\":%u,",
	       cycle, (char *)&mode.fourcc, mode.width, mode.height);
	printf("\"stride\":%u,\"sizeimage\":%u,\"requested_fps\":%u,\"frames\":%u,",
	       format.fmt.pix.bytesperline, format.fmt.pix.sizeimage, fps, frames);
	printf("\"delivered_fps\":%.6f,\"max_gap_ms\":%.3f,\"timing_gaps\":%u,",
	       measured_fps, max_gap * 1000, gaps);
	printf("\"buffer_errors\":%u,\"sequence_gaps\":%u,\"pass\":%s}\n",
	       bad, sequence_gaps, ret ? "false" : "true");
	fflush(stdout);
	return ret;
}

int main(int argc, char **argv)
{
	struct mode modes[MAX_MODES];
	unsigned int count = 0, seconds, cycles, fps;
	int fd, failed = 0;
	char *end;
	unsigned long values[3];

	if (argc != 5 && argc != 8) {
		fprintf(stderr, "usage: %s device seconds cycles fps [FOURCC width height]\n",
			argv[0]);
		return 2;
	}
	for (int i = 0; i < 3; i++) {
		errno = 0;
		values[i] = strtoul(argv[i + 2], &end, 10);
		if (errno || *end || !values[i] || values[i] > 86400)
			return 2;
	}
	seconds = values[0];
	cycles = values[1];
	fps = values[2];
	if (fps > 240 || cycles > 1000 || seconds < 5)
		return 2;
	fd = open(argv[1], O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(argv[1]);
		return 2;
	}
	for (unsigned int i = 0; ; i++) {
		struct v4l2_fmtdesc format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i };

		if (xioctl(fd, VIDIOC_ENUM_FMT, &format) < 0) {
			if (errno != EINVAL)
				failed = 1;
			break;
		}
		/* This harness exercises the driver's contiguous, single-planar API. */
		if (format.pixelformat == V4L2_PIX_FMT_NV12M)
			continue;
		for (unsigned int j = 0; ; j++) {
			struct v4l2_frmsizeenum size = {
				.pixel_format = format.pixelformat, .index = j,
			};

			if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
				if (errno != EINVAL)
					failed = 1;
				break;
			}
			if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE || count == MAX_MODES) {
				failed = 1;
				break;
			}
			modes[count++] = (struct mode){format.pixelformat, size.discrete.width,
						       size.discrete.height};
		}
	}
	if (argc == 8) {
		uint32_t fourcc;
		unsigned long width, height;
		bool found = false;

		if (strlen(argv[5]) != 4) {
			close(fd);
			return 2;
		}
		memcpy(&fourcc, argv[5], sizeof(fourcc));
		errno = 0;
		width = strtoul(argv[6], &end, 10);
		if (errno || *end || !width || width > UINT32_MAX) {
			close(fd);
			return 2;
		}
		errno = 0;
		height = strtoul(argv[7], &end, 10);
		if (errno || *end || !height || height > UINT32_MAX) {
			close(fd);
			return 2;
		}
		for (unsigned int i = 0; i < count; i++) {
			if (modes[i].fourcc == fourcc && modes[i].width == width &&
			    modes[i].height == height) {
				modes[0] = modes[i];
				found = true;
				break;
			}
		}
		count = found ? 1 : 0;
	}
	if (!count)
		failed = 1;
	/* Long runs first, then repeated stops/restarts and format/size switches. */
	for (unsigned int cycle = 0; cycle <= cycles && !failed; cycle++) {
		for (unsigned int i = 0; i < count; i++) {
			if (capture(fd, modes[i], cycle ? 5 : seconds, fps, cycle)) {
				failed = 1;
				break;
			}
		}
	}
	close(fd);
	return failed ? 1 : 0;
}
