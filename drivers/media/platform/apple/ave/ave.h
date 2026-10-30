/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE video encoder
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_H_
#define _APPLE_AVE_H_

#include <linux/atomic.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

struct dev_pm_domain_list;
struct firmware;
struct iommu_domain;

/* Frames the firmware can hold at once (Process "slots"). */
#define AVE_MAX_SLOTS		5

/*
 * Coprocessor memory every firmware boot uses, all inside one allocation.
 * Host commands live in three areas: one large buffer for Start, small
 * buffers for the scalar commands and one buffer per Process slot, so a
 * command stays untouched until the firmware acknowledges it.
 */
#define AVE_IPC_BYTES		(20 * SZ_1M)
#define AVE_IPC_CHANNEL_OFF	0x4000
#define AVE_IPC_HANDSHAKE_OFF	0x24000
#define AVE_IPC_SHARED_OFF	0x28000
#define AVE_IPC_SHARED_BYTES	SZ_64K
#define AVE_IPC_CMD_OFF		0x40000
#define AVE_IPC_CMD_BYTES	0x1c000
#define AVE_IPC_SCALAR_OFF	0x5c000
#define AVE_IPC_SCALAR_BYTES	0x100
#define AVE_IPC_SCALAR_BUFS	16
#define AVE_IPC_PROC_OFF	0x60000
#define AVE_IPC_PROC_BYTES	0x10000
#define AVE_IPC_END		(AVE_IPC_PROC_OFF + AVE_MAX_SLOTS * AVE_IPC_PROC_BYTES)

/* Command buffer ids, one bit each in ave_dev.unacked. */
#define AVE_BUF_CMD		0
#define AVE_BUF_SCALAR(i)	(1 + (i))
#define AVE_BUF_PROC(s)		(1 + AVE_IPC_SCALAR_BUFS + (s))
#define AVE_BUF_COUNT		AVE_BUF_PROC(AVE_MAX_SLOTS)

#define AVE_MSG_MAX		SZ_4K

enum ave_codec {
	AVE_CODEC_H264 = 1,
	AVE_CODEC_HEVC = 2,
};

struct ave_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	unsigned long attrs;
};

struct ave_fw_segment {
	phys_addr_t phys;
	u64 iova;
	size_t size;
};

struct ave_ring {
	char name[16];
	u32 mode;
	u32 doorbell;
	u32 slots;
	u32 offset;
	int consumer;
	int producer;
};

enum ave_fw_state {
	AVE_FW_OFF,		/* coprocessor in reset */
	AVE_FW_RUNNING,
	AVE_FW_HALTED,		/* told to halt; restarts through the boot path */
	AVE_FW_BROKEN,
};

/* Session command images that only differ from the listed fields. */
enum ave_tmpl {
	AVE_TMPL_START,
	AVE_TMPL_PROCESS,
	AVE_TMPL_COUNT,
};

struct ave_ctx;

struct ave_dev {
	struct device *dev;
	void __iomem *codec;
	void __iomem *mbox;
	void __iomem *coproc;
	struct dev_pm_domain_list *pd_list;
	struct iommu_domain *domain;

	struct ave_fw_segment text;
	struct ave_fw_segment data;
	u32 text_crc;
	void *data_pristine;
	bool data_dirty;

	/* Protects tmpl[]; the images stay loaded until the driver goes. */
	struct mutex tmpl_lock;
	const struct firmware *tmpl[2][AVE_TMPL_COUNT];

	/* Serialises every firmware and ring access. */
	struct mutex hw_lock;
	enum ave_fw_state fw_state;
	struct ave_buf ipc;
	struct ave_buf boot_cfg;
	struct ave_buf log;
	struct ave_buf heap;
	u64 ipc_fw_base;
	u32 channel_bytes;
	struct ave_ring ring[2];
	u64 next_token;
	u32 next_client;
	u32 unacked;
	u8 want[16];
	int reply_err;
	unsigned int scalar_next;
	struct ave_ctx *active;
	u8 msg[AVE_MSG_MAX];
	u32 msg_len;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;
	struct workqueue_struct *wq;
};

/* Encoding parameters, fixed for the lifetime of a session. */
struct ave_params {
	enum ave_codec codec;
	u32 width, height;		/* visible picture */
	u32 cw, ch;			/* coded size, multiples of 16 */
	u32 stride;			/* NV12 line stride, both planes */
	u32 mbs;			/* 16x16 macroblocks */
	u32 fps_num, fps_den;		/* frames per second = num / den */
	u32 fps_int;			/* the same, rounded */
	u32 slots;
	u32 refs;			/* max_num_ref_frames + 1 */
	bool bframes;
	bool intra_only;
	u32 gop;
	u32 rc_mode;
	u32 bitrate;
	u32 limit_bytes;		/* 0: no data-rate limit */
	s32 min_qp;
	u32 max_qp;
	u32 profile;			/* H.264: 2, 4, 6; HEVC: 1 */
	u32 level;			/* firmware level enum, 1-based */
	bool high;
	u32 coded_cap;
};

struct ave_slot {
	struct ave_buf coded;
	struct ave_buf meta;
	struct ave_buf aux;		/* H.264 per-frame buffer, HEVC slice headers */
	struct vb2_v4l2_buffer *src;
	u32 status;
	bool busy;			/* Process sent, reply pending */
	bool done;			/* reply in, output not delivered yet */
};

#define AVE_MAX_REFS		5
#define AVE_TILE_BUFS		8

struct ave_session {
	struct ave_params p;
	struct ave_buf hdr;
	struct ave_buf ref_a[AVE_MAX_REFS];
	struct ave_buf ref_b[AVE_MAX_REFS];
	struct ave_buf ref_c[AVE_MAX_REFS];
	struct ave_buf ref_d[AVE_MAX_REFS];
	struct ave_buf mb;
	struct ave_buf row_a, row_b, fixed;
	struct ave_buf tile[AVE_TILE_BUFS];
	struct ave_buf tile_aux[AVE_TILE_BUFS];
	struct ave_buf hevc_extra[2];
	struct ave_slot slot[AVE_MAX_SLOTS];
	u8 done_fifo[AVE_MAX_SLOTS];
	unsigned int done_head;
	unsigned int done_count;
	unsigned int busy;
	u32 frames;			/* Process commands sent */
	u32 client;
	bool fw_open;			/* Open sent, Close not yet */
	bool dead;			/* firmware gone; buffers only */
	bool warned;
	bool checked[2];		/* Start/Process images checked for stale addresses */
	unsigned long progress;		/* jiffies of the last frame sent or answered */
};

struct ave_ctx {
	struct v4l2_fh fh;
	struct ave_dev *ave;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct v4l2_fract timeperframe;
	struct work_struct run_work;
	struct delayed_work poll_work;

	u32 bitrate_mode;
	u32 bitrate;
	u32 bitrate_peak;
	u32 gop_size;
	u32 b_frames;
	u32 h264_profile;
	u32 h264_level;
	u32 h264_min_qp;
	u32 h264_max_qp;
	u32 hevc_level;
	u32 hevc_min_qp;
	u32 hevc_max_qp;
	bool force_key_frame;

	struct ave_session *sess;
	atomic_t ready;			/* outputs waiting for a CAPTURE buffer */
	bool draining;
	bool flush_pending;
	bool stopped;
	struct vb2_v4l2_buffer *drain_src;
	u32 src_sequence;
	u32 dst_sequence;
};

static inline struct ave_ctx *ave_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct ave_ctx, fh);
}

/* ave-fw.c */
int ave_fw_probe(struct ave_dev *ave);
void ave_fw_remove(struct ave_dev *ave);
int ave_fw_ensure_running(struct ave_dev *ave);
void ave_fw_halt(struct ave_dev *ave);
int ave_fw_shutdown(struct ave_dev *ave);
void ave_fw_lost(struct ave_dev *ave);
int ave_fw_templates(struct ave_dev *ave, enum ave_codec codec);
int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size, unsigned long attrs);
void ave_free(struct ave_dev *ave, struct ave_buf *b);

/* ave-ipc.c */
int ave_ipc_negotiate(struct ave_dev *ave, const u32 hello[4]);
void *ave_ipc_cmd(struct ave_dev *ave, unsigned int buf, size_t len);
int ave_ipc_scalar_buf(struct ave_dev *ave);
int ave_ipc_wait_ack(struct ave_dev *ave, unsigned int buf);
int ave_ipc_submit(struct ave_dev *ave, unsigned int buf, size_t len, u64 timeout_us,
		   bool reply);
int ave_ipc_pump(struct ave_dev *ave, unsigned int wait_ms,
		 bool (*cond)(struct ave_dev *ave, void *arg), void *arg);
int ave_ipc_settle(struct ave_dev *ave, unsigned int wait_ms);

/* ave-cmd.c */
int ave_cmd_config(struct ave_dev *ave);
int ave_cmd_halt(struct ave_dev *ave);
int ave_cmd_open(struct ave_ctx *ctx);
int ave_cmd_start(struct ave_ctx *ctx);
int ave_cmd_process(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *src,
		    u32 bitrate, bool force_key);
int ave_cmd_complete(struct ave_ctx *ctx, unsigned int count);
int ave_cmd_close(struct ave_ctx *ctx);

/* ave-sess.c */
struct ave_session *ave_sess_create(struct ave_ctx *ctx);
void ave_sess_destroy(struct ave_ctx *ctx);
int ave_sess_free_slot(struct ave_session *sess);
void ave_sess_reply(struct ave_ctx *ctx, const u8 *msg, u32 len);
int ave_sess_output(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *dst,
		    bool *keyframe);
void ave_sess_prepare_slot(struct ave_session *sess, unsigned int slot);
void ave_sess_check_addrs(struct ave_ctx *ctx, const u8 *cmd, size_t len, unsigned int kind,
			  dma_addr_t input);
bool ave_sess_stalled(struct ave_session *sess);

/* ave-v4l2.c */
int ave_v4l2_register(struct ave_dev *ave);
void ave_v4l2_unregister(struct ave_dev *ave);
enum ave_codec ave_ctx_codec(struct ave_ctx *ctx);

#endif
