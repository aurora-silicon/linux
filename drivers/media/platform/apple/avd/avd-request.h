/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AVD_REQUEST_H_
#define AVD_REQUEST_H_

#include <linux/types.h>

/* All transitions are serialized by avd_dev.job_lock. */
enum avd_job_state {
	AVD_JOB_IDLE,
	AVD_JOB_RUNNING,
	AVD_JOB_SUBMITTED,
	AVD_JOB_FINISHING,
};

struct avd_request {
	enum avd_job_state state;
	u64 seq;
	unsigned int vp;
	bool end_sent;
	bool pending;
	bool vp_done;
	bool failed;
	bool reset_needed;
};

enum avd_request_event {
	AVD_EVENT_NONE,
	AVD_EVENT_KICK_PP,
	AVD_EVENT_FINISH,
};

static inline void avd_request_begin(struct avd_request *req, u64 seq)
{
	*req = (struct avd_request) {
		.state = AVD_JOB_RUNNING,
		.seq = seq,
	};
}

static inline enum avd_request_event
avd_request_irq(struct avd_request *req, u32 status)
{
	if (req->state != AVD_JOB_RUNNING && req->state != AVD_JOB_SUBMITTED)
		return AVD_EVENT_NONE;

	if (!(status & 0x11000) && (status & 0x100)) {
		if (!req->end_sent || (status & 0xff) != req->vp || req->vp_done)
			return AVD_EVENT_NONE;
		req->vp_done = true;
		return AVD_EVENT_KICK_PP;
	}

	if (status == 0x1000) {
		if (!req->end_sent || !req->vp_done)
			return AVD_EVENT_NONE;
	} else {
		req->failed = true;
		req->reset_needed = true;
	}
	if (req->state == AVD_JOB_RUNNING) {
		req->pending = true;
		return AVD_EVENT_NONE;
	}
	req->state = AVD_JOB_FINISHING;
	return AVD_EVENT_FINISH;
}

/* Called only after the CPU has stopped touching the codec's run state. */
static inline bool avd_request_submitted(struct avd_request *req, bool failed,
					 bool owns_hardware)
{
	if (failed) {
		req->failed = true;
		req->reset_needed = owns_hardware;
	}
	if (failed || req->pending) {
		req->state = AVD_JOB_FINISHING;
		return true;
	}
	req->state = AVD_JOB_SUBMITTED;
	return false;
}

static inline bool avd_request_timeout(struct avd_request *req, u64 seq)
{
	if (req->seq != seq || req->state != AVD_JOB_SUBMITTED)
		return false;
	req->state = AVD_JOB_FINISHING;
	req->failed = true;
	req->reset_needed = true;
	return true;
}

#endif /* AVD_REQUEST_H_ */
