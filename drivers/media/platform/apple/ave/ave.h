/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE video encoder
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_H_
#define _APPLE_AVE_H_

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
struct iommu_domain;

/* Coprocessor memory every firmware boot uses, all inside one allocation. */
#define AVE_IPC_BYTES		(20 * SZ_1M)
#define AVE_IPC_CHANNEL_OFF	0x4000
#define AVE_IPC_HANDSHAKE_OFF	0x24000
#define AVE_IPC_SHARED_OFF	0x28000
#define AVE_IPC_SHARED_BYTES	SZ_64K
#define AVE_IPC_CMD_OFF		0x40000
#define AVE_IPC_CMD_BYTES	SZ_128K
#define AVE_IPC_END		(AVE_IPC_CMD_OFF + AVE_IPC_CMD_BYTES)

#define AVE_MSG_MAX		SZ_4K

enum ave_codec {
	AVE_CODEC_H264 = 1,
	AVE_CODEC_HEVC = 2,
};

struct ave_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
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
	AVE_FW_OFF,
	AVE_FW_RUNNING,
	AVE_FW_BROKEN,
};

struct ave_dev {
	struct device *dev;
	void __iomem *codec;
	void __iomem *mbox;
	void __iomem *coproc;
	struct dev_pm_domain_list *pd_list;
	struct iommu_domain *domain;

	struct ave_fw_segment text;
	struct ave_fw_segment data;
	void *data_pristine;
	bool data_dirty;

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
	u8 msg[AVE_MSG_MAX];
	u32 msg_len;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;
	struct workqueue_struct *wq;
};

struct ave_ctx {
	struct v4l2_fh fh;
	struct ave_dev *ave;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct v4l2_fract timeperframe;
	struct work_struct run_work;

	u32 bitrate_mode;
	u32 bitrate;
	u32 bitrate_peak;
	u32 gop_size;
	u32 h264_profile;
	u32 h264_level;
	u32 h264_i_period;
	u32 h264_min_qp;
	u32 h264_max_qp;
	u32 hevc_profile;
	u32 hevc_level;
	u32 hevc_min_qp;
	u32 hevc_max_qp;
	bool force_key_frame;

	u32 client;
	bool session_open;
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
int ave_fw_shutdown(struct ave_dev *ave);
void ave_fw_lost(struct ave_dev *ave);
void ave_fw_resume(struct ave_dev *ave);

/* ave-ipc.c */
int ave_ipc_negotiate(struct ave_dev *ave, const u32 hello[4]);
int ave_ipc_call(struct ave_dev *ave, const void *cmd, size_t len);
int ave_ipc_wait_msg(struct ave_dev *ave, unsigned int timeout_ms);

/* ave-cmd.c */
int ave_cmd_config(struct ave_dev *ave);
int ave_cmd_halt(struct ave_dev *ave);
int ave_cmd_open(struct ave_ctx *ctx, enum ave_codec codec);
int ave_cmd_start(struct ave_ctx *ctx, enum ave_codec codec);
int ave_cmd_process(struct ave_ctx *ctx, enum ave_codec codec,
		    struct vb2_v4l2_buffer *src, struct vb2_v4l2_buffer *dst,
		    u32 *bytesused, bool *keyframe);
int ave_cmd_finish(struct ave_ctx *ctx, enum ave_codec codec);

/* ave-v4l2.c */
int ave_v4l2_register(struct ave_dev *ave);
void ave_v4l2_unregister(struct ave_dev *ave);
enum ave_codec ave_ctx_codec(struct ave_ctx *ctx);

#endif
