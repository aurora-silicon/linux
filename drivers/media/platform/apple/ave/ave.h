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
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#include "ave-sizes.h"
#include "ave-recovery.h"

struct debugfs_blob_wrapper;
struct dentry;
struct dev_pm_domain_list;
struct iommu_domain;
struct ave_vb2_mem;

/* Frames the firmware can hold at once per session (Process "slots"). */
#define AVE_MAX_SLOTS 7
/* Sessions one firmware boot serves at once (macOS was seen running two). */
#define AVE_MAX_SESSIONS 2

/*
 * Coprocessor memory every firmware boot uses, all inside one allocation.
 * Host commands live in three areas: one large buffer for Start, small
 * buffers for the scalar commands and one buffer per Process slot, so a
 * command stays untouched until the firmware acknowledges it.
 */
#define AVE_IPC_BYTES (20 * SZ_1M)
#define AVE_IPC_CHANNEL_OFF 0x4000
#define AVE_IPC_HANDSHAKE_OFF 0x24000
#define AVE_IPC_SHARED_OFF 0x28000
#define AVE_IPC_SHARED_BYTES SZ_64K
#define AVE_IPC_CMD_OFF 0x40000
#define AVE_IPC_CMD_BYTES 0x1c000
#define AVE_IPC_SCALAR_OFF 0x5c000
#define AVE_IPC_SCALAR_BYTES 0x100
#define AVE_IPC_SCALAR_BUFS 16
#define AVE_IPC_PROC_OFF 0x60000
#define AVE_IPC_PROC_BYTES 0x10000
#define AVE_IPC_PROC_BUFS (AVE_MAX_SESSIONS * AVE_MAX_SLOTS)
#define AVE_IPC_END (AVE_IPC_PROC_OFF + AVE_IPC_PROC_BUFS * AVE_IPC_PROC_BYTES)

/* Command buffer ids, one bit each in ave_dev.unacked. */
#define AVE_BUF_CMD 0
#define AVE_BUF_SCALAR(i) (1 + (i))
#define AVE_BUF_PROC(s) (1 + AVE_IPC_SCALAR_BUFS + (s))
#define AVE_BUF_COUNT AVE_BUF_PROC(AVE_IPC_PROC_BUFS)

#define AVE_MSG_MAX SZ_4K

enum ave_codec {
	AVE_CODEC_H264 = 1,
	AVE_CODEC_HEVC = 2,
};

struct ave_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size; /* what the firmware is told */
	size_t alloc; /* what is mapped, page rounded */
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
	AVE_FW_OFF, /* coprocessor in reset */
	AVE_FW_RUNNING,
	AVE_FW_HALTED, /* told to halt; restarts through the boot path */
	AVE_FW_BROKEN,
	AVE_FW_RECOVERING,	/* control off; awaiting a runtime power cycle */
};

/* The two large session commands, and their sizes per codec */
enum ave_image {
	AVE_IMAGE_START,
	AVE_IMAGE_PROCESS,
	AVE_IMAGE_COUNT,
};

/* Logical operations; wire IDs are selected by the firmware ABI. */
enum ave_opcode {
	AVE_OP_CONFIG = 1,
	AVE_OP_HALT,
	AVE_OP_OPEN,
	AVE_OP_CLOSE,
	AVE_OP_START = 6,
	AVE_OP_STOP,
	AVE_OP_PROCESS,
	AVE_OP_COMPLETE,
	AVE_OP_PRIORITY,
	AVE_OP_RESET,
	AVE_OP_FLUSH,
	AVE_OP_COUNT,
};

#include "ave-layout.h"

/* The firmware build whose command layouts the driver generates */
#define AVE_FW_TEXT_CRC_9003_78_0 0x69c54454
/* The M4 Pro (T6040) build of the same 25G83 firmware. */
#define AVE_FW_TEXT_CRC_T6040_25G83 0x68d5f9f6

struct ave_ctx;

/* Register offsets for the shared 0x40 coprocessor architecture. */
struct ave_coproc_regs {
	u32 vcbar;
	u32 timebase_freq;
	u32 timebase;
	u32 ctrl;
	u32 status;
	u32 boot_arg;
	u32 keep_mappings;
};

/*
 * One row of masked 32-bit read-modify-writes in the DPE, at codec resource
 * offsets: new = (old & ~mask) | value. A row covers count consecutive
 * registers 4 bytes apart; values, when set, gives each register its own
 * value in place of value.
 */
struct ave_dpe_rmw {
	u32 offset;
	u32 mask;
	u32 value;
	u32 count;
	const u16 *values;
};

struct ave_dpe_table {
	const struct ave_dpe_rmw *rows;
	unsigned int count;
	/* CAT and CAC block bases, for the read-only hello-timeout snapshot. */
	u32 cat;
	u32 cac;
};

struct ave_variant {
	const struct ave_coproc_regs *regs;
	u32 fw_text_size;
	u32 fw_data_size;
	u32 boot_device_id;
	u32 device_type;
	u32 lfs_ref_scale;
	u32 text_crc;
	u32 boot_arg_value;
	u32 timebase_multiplier;
	u64 vcbar_mask;
	u64 vcbar_flags;
	/*
	 * TEXT offset of the 64-bit cell the loader fills with the DATA
	 * address; it is zero in the firmware image. 0 when there is none.
	 */
	u32 text_link_cell;
	/* DPE defaults applied before every cold firmware start, or NULL. */
	const struct ave_dpe_table *dpe;

	size_t extra0_size;
	/* Register sequence and boot/hello configuration have been qualified. */
	bool boot_qualified;
};

struct ave_dev {
	struct device *dev;
	const struct ave_variant *variant;
	const struct ave_fw_layout *layout;
	void __iomem *codec;
	void __iomem *mbox;
	void __iomem *coproc;
	struct dev_pm_domain_list *pd_list;
	struct iommu_domain *domain;

	struct ave_fw_segment text;
	struct ave_fw_segment data;
	u32 text_crc;
	u32 data_crc;
	void *data_pristine;
	bool data_dirty;

	/* hw_lock protects recovery history and quarantined DMA allocations. */
	struct ave_recovery_history recovery;
	unsigned int recovery_count;
	struct ave_session *failed_sessions;
	bool inject_wedge;

	/* debugfs: the last Start and first Process built, per codec */
	struct dentry *debugfs;
	void *dump[2][AVE_IMAGE_COUNT];

	/* Serialises every firmware and ring access. */
	struct mutex hw_lock;
	enum ave_fw_state fw_state;
	struct ave_buf ipc;
	struct ave_buf boot_cfg;
	struct ave_buf log;
	struct ave_buf heap;
	dma_addr_t extra0_dma;
	size_t extra0_map_size;
	bool extra0_mapped;
	bool module_pinned;
	/* A 6070 lifecycle command is serialized per logical operation. */
	struct ave_reply expected[AVE_OP_COUNT];
	u64 ipc_fw_base;
	u32 channel_bytes;
	struct ave_ring ring[2];
	u32 next_client;
	u32 unacked;
	u8 want[AVE_OP_COUNT];
	int reply_err;
	unsigned int scalar_next;
	struct ave_ctx *sessions[AVE_MAX_SESSIONS];
	u8 msg[AVE_MSG_MAX];
	u32 msg_len;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	/* The video device's ioctl and queue lock. */
	struct mutex vdev_lock;
	struct workqueue_struct *wq;
};

/* Encoding parameters, fixed for the lifetime of a session. */
struct ave_params {
	enum ave_codec codec;
	u32 width, height; /* visible picture */
	u32 cw, ch; /* coded size, multiples of 16 */
	u32 stride; /* NV12 line stride, both planes */
	u32 mbs; /* 16x16 macroblocks */
	u32 fps_num, fps_den; /* frames per second = num / den */
	u32 fps_int; /* the same, rounded */
	u32 slots;
	u32 refs; /* max_num_ref_frames + 1 */
	bool bframes;
	bool intra_only;
	bool reorder_off; /* the client asked for no B-frames */
	u32 gop;
	u32 rc_mode;
	u32 bitrate;
	u32 limit_bytes; /* 0: no data-rate limit */
	s32 min_qp;
	u32 max_qp;
	u32 profile; /* H.264: 2, 4, 6; HEVC: 1 */
	u32 level; /* firmware level enum, 1-based */
	bool high;
	u32 coded_cap;
	u32 bit_depth; /* zero also means the existing 8-bit API */
	u32 initial_qp; /* CQ only; bit-rate modes use 26 */
};

struct ave_slot {
	struct ave_buf coded;
	struct ave_buf meta;
	struct ave_buf aux; /* H.264 per-frame buffer, HEVC slice headers */
	struct ave_buf extra; /* counted layout: per-slot 16 KiB buffer */
	struct ave_buf bounce; /* input copied to the encoder's stride */
	struct vb2_v4l2_buffer *src;
	struct ave_vb2_mem *input_mem; /* independent firmware mapping reference */
	u32 status;
	bool busy; /* Process sent, reply pending */
	bool done; /* reply in, output not delivered yet */
};

#define AVE_MAX_REFS 5
#define AVE_TILE_BUFS 8

struct ave_session {
	struct ave_session *failed_next;
	struct ave_params p;
	struct ave_buf hdr;
	struct ave_buf ref_a[AVE_MAX_REFS];
	struct ave_buf ref_b[AVE_MAX_REFS];
	struct ave_buf ref_c[AVE_MAX_REFS];
	struct ave_buf ref_d[AVE_MAX_REFS];
	struct ave_buf ref_e[AVE_MAX_REFS];
	struct ave_buf ref_f[AVE_MAX_REFS];
	struct ave_buf mb;
	struct ave_buf mb_extra[2];
	struct ave_buf row_a, row_b, fixed;
	struct ave_buf tile[AVE_TILE_BUFS];
	struct ave_buf tile_aux[AVE_TILE_BUFS];
	struct ave_buf hevc_extra[2];
	struct ave_slot slot[AVE_MAX_SLOTS];
	u8 done_fifo[AVE_MAX_SLOTS];
	unsigned int done_head;
	unsigned int done_count;
	unsigned int busy;
	u32 frames; /* Process commands sent */
	u32 client;
	u64 token; /* command order token, per session */
	unsigned int index; /* in ave_dev.sessions[] */
	bool fw_open; /* Open sent, Close not yet */
	bool resume; /* closed for system sleep, reopen on next frame */
	bool copy_noted;
	/* CBR: bytes under budget so far, and who pads (see ave_sess_cbr_fill()) */
	s64 cbr_surplus;
	bool fw_fills;
	bool host_fill_noted;
	bool warned;
	bool checked[2]; /* Start/Process images checked for stale addresses */
	bool handed; /* Start sent: the firmware knows the buffers */
	bool nudged; /* Complete sent because input stopped */
	unsigned long progress; /* jiffies of the last frame sent or answered */
	unsigned long last_sent; /* jiffies of the last frame sent */
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
	atomic_t ready; /* outputs waiting for a CAPTURE buffer */
	bool draining;
	bool flush_pending;
	bool stopped;
	/* Serialises QBUF with fatal-error publication and ready-queue draining. */
	spinlock_t queue_lock;
	bool failed;
	struct vb2_v4l2_buffer *drain_src;
	u32 src_sequence;
	u32 dst_sequence;
};

static inline struct ave_ctx *ave_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct ave_ctx, fh);
}

extern const struct ave_variant ave_t6000, ave_t8112;

/* ave-fw.c */
u64 ave_fw_vcbar(const struct ave_variant *variant, u64 text_iova);
int ave_fw_text_id_crc(const void *text, size_t size, u32 cell_off,
		       u64 data_iova, u32 *crc);
void ave_dpe_apply(void __iomem *codec, const struct ave_dpe_table *t);
unsigned int ave_dpe_check(void __iomem *codec, const struct ave_dpe_table *t,
			   struct device *dev);
int ave_fw_probe(struct ave_dev *ave);
void ave_fw_remove(struct ave_dev *ave);
int ave_fw_ensure_running(struct ave_dev *ave);
void ave_fw_halt(struct ave_dev *ave);
int ave_fw_shutdown(struct ave_dev *ave);
int ave_fw_runtime_suspend(struct ave_dev *ave);
void ave_fw_mark_broken(struct ave_dev *ave, int err, const char *what);
void ave_fw_lost(struct ave_dev *ave);
int ave_fw_check_build(struct ave_dev *ave);
int ave_fw_check_boot(struct ave_dev *ave);
void ave_fw_release_boot(struct ave_dev *ave);
int ave_fw_lifecycle_test(struct ave_dev *ave, enum ave_codec codec);
int ave_alloc(struct ave_dev *ave, struct ave_buf *b, size_t size,
	      unsigned long attrs);
int ave_alloc_padded(struct ave_dev *ave, struct ave_buf *b, size_t size,
		     size_t alloc, unsigned long attrs);
void ave_free(struct ave_dev *ave, struct ave_buf *b);

/* ave-ipc.c */
int ave_ipc_check_hello(const struct ave_fw_transport *t, const u32 hello[4]);
int ave_ipc_parse_channels(struct ave_dev *ave);
int ave_ipc_negotiate(struct ave_dev *ave, const u32 hello[4]);
void *ave_ipc_cmd(struct ave_dev *ave, unsigned int buf, size_t len);
int ave_ipc_scalar_buf(struct ave_dev *ave);
int ave_ipc_wait_ack(struct ave_dev *ave, unsigned int buf);
int ave_ipc_submit(struct ave_dev *ave, unsigned int buf, size_t len,
		   u64 timeout_us, bool reply);
int ave_ipc_pump(struct ave_dev *ave, unsigned int wait_ms,
		 bool (*cond)(struct ave_dev *ave, void *arg), void *arg);
int ave_ipc_settle(struct ave_dev *ave, unsigned int wait_ms);

/* ave-writer.c: pure command builders, also exercised by KUnit. */
u32 ave_session_arg2(struct ave_ctx *ctx);
u32 ave_session_setup_arg2(struct ave_ctx *ctx);
void ave_write_header(struct ave_ctx *ctx, u8 *cmd, enum ave_opcode op,
		      u32 echo, u32 arg0, u32 arg2);
void ave_write_global(struct ave_dev *ave, u8 *cmd, enum ave_opcode op);
void ave_stamp_command(const struct ave_fw_abi *abi, u8 *cmd, u64 timeout,
		       u64 created);
void ave_write_surface(const struct ave_fw_layout *fw, u8 *cmd,
		       const struct ave_surface_layout *l, unsigned int index,
		       dma_addr_t addr, u32 size);
int ave_build_start(struct ave_ctx *ctx, u8 *cmd, size_t len);
int ave_build_process(struct ave_ctx *ctx, u8 *cmd, size_t len,
		      unsigned int slot, dma_addr_t input, u32 bitrate,
		      bool force_key);

/* ave-cmd.c */
int ave_cmd_config(struct ave_dev *ave);
int ave_cmd_halt(struct ave_dev *ave);
int ave_cmd_open(struct ave_ctx *ctx);
int ave_cmd_start(struct ave_ctx *ctx);
int ave_cmd_process(struct ave_ctx *ctx, unsigned int slot,
		    struct vb2_v4l2_buffer *src, dma_addr_t input, u32 bitrate,
		    bool force_key);
int ave_cmd_complete(struct ave_ctx *ctx, unsigned int count);
int ave_cmd_close(struct ave_ctx *ctx);

/* ave-vb2.c */
extern const struct vb2_mem_ops ave_vb2_memops;
struct ave_vb2_mem *ave_vb2_get(struct vb2_buffer *vb);
void ave_vb2_put(void *buf_priv);

/* ave-sess.c */
struct ave_session *ave_sess_create(struct ave_ctx *ctx);
void ave_sess_destroy(struct ave_ctx *ctx);
void ave_sess_reap(struct ave_dev *ave);
int ave_sess_free_slot(struct ave_session *sess);
void ave_sess_reply(struct ave_dev *ave, const struct ave_reply *reply);
int ave_sess_input(struct ave_ctx *ctx, unsigned int slot,
		   struct vb2_v4l2_buffer *src, dma_addr_t *input);
int ave_sess_output(struct ave_ctx *ctx, unsigned int slot,
		    struct vb2_v4l2_buffer *dst, bool *keyframe);
void ave_sess_prepare_slot(struct ave_session *sess, unsigned int slot);
bool ave_sess_stalled(struct ave_session *sess);
bool ave_sess_starved(struct ave_session *sess);

/* ave-image.c */
void ave_image_start(u8 *cmd, size_t len, const struct ave_fw_layout *fw,
		     enum ave_codec codec, bool reorder_off,
		     bool hevc_low_delay);
void ave_image_process(u8 *cmd, size_t len, const struct ave_fw_layout *fw,
		       enum ave_codec codec);

/* ave-drv.c */
void ave_debugfs_save(struct ave_dev *ave, enum ave_codec codec,
		      enum ave_image kind, const void *cmd, size_t len);

/* ave-v4l2.c */
void ave_v4l2_suspend(struct ave_dev *ave);
int ave_v4l2_register(struct ave_dev *ave);
void ave_v4l2_unregister(struct ave_dev *ave);
enum ave_codec ave_ctx_codec(struct ave_ctx *ctx);

#endif
