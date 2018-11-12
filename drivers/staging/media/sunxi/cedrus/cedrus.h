/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#ifndef _CEDRUS_H_
#define _CEDRUS_H_

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include <linux/platform_device.h>

#define CEDRUS_NAME			"cedrus"

#define CEDRUS_CAPABILITY_UNTILED	BIT(0)

enum cedrus_codec {
	CEDRUS_CODEC_MPEG2,

	CEDRUS_CODEC_LAST,
};

enum cedrus_irq_status {
	CEDRUS_IRQ_NONE,
	CEDRUS_IRQ_ERROR,
	CEDRUS_IRQ_OK,
};

struct cedrus_control {
	u32			id;
	u32			elem_size;
	enum cedrus_codec	codec;
	unsigned char		required:1;
};

struct cedrus_mpeg2_run {
	const struct v4l2_ctrl_mpeg2_slice_params	*slice_params;
	const struct v4l2_ctrl_mpeg2_quantization	*quantization;
};

struct cedrus_run {
	struct vb2_v4l2_buffer	*src;
	struct vb2_v4l2_buffer	*dst;

	union {
		struct cedrus_mpeg2_run	mpeg2;
	};
};

struct cedrus_buffer {
	struct v4l2_m2m_buffer          m2m_buf;
};

struct cedrus_ctx {
	struct v4l2_fh			fh;
	struct cedrus_dev		*dev;

	struct v4l2_pix_format		src_fmt;
	struct v4l2_pix_format		dst_fmt;
	enum cedrus_codec		current_codec;

	struct v4l2_ctrl_handler	hdl;
	struct v4l2_ctrl		**ctrls;

	struct vb2_buffer		*dst_bufs[VIDEO_MAX_FRAME];
};

struct cedrus_dec_ops {
	void (*irq_clear)(struct cedrus_ctx *ctx);
	void (*irq_disable)(struct cedrus_ctx *ctx);
	enum cedrus_irq_status (*irq_status)(struct cedrus_ctx *ctx);
	void (*setup)(struct cedrus_ctx *ctx, struct cedrus_run *run);
	int (*start)(struct cedrus_ctx *ctx);
	void (*stop)(struct cedrus_ctx *ctx);
	void (*trigger)(struct cedrus_ctx *ctx);
};

struct cedrus_variant {
	unsigned int	capabilities;
};

struct cedrus_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct media_device	mdev;
	struct media_pad	pad[2];
	struct platform_device	*pdev;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;
	struct cedrus_dec_ops	*dec_ops[CEDRUS_CODEC_LAST];

	/* Device file mutex */
	struct mutex		dev_mutex;
	/* Interrupt spinlock */
	spinlock_t		irq_lock;

	void __iomem		*base;

	struct clk		*mod_clk;
	struct clk		*ahb_clk;
	struct clk		*ram_clk;

	struct reset_control	*rstc;

	unsigned int		capabilities;
};

extern struct cedrus_dec_ops cedrus_dec_ops_mpeg2;

static inline void cedrus_write(struct cedrus_dev *dev, u32 reg, u32 val)
{
	writel(val, dev->base + reg);
}

static inline u32 cedrus_read(struct cedrus_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

static inline dma_addr_t cedrus_buf_addr(struct vb2_buffer *buf,
					 struct v4l2_pix_format *pix_fmt,
					 unsigned int plane)
{
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(buf, 0);

	return addr + (pix_fmt ? (dma_addr_t)pix_fmt->bytesperline *
	       pix_fmt->height * plane : 0);
}

static inline dma_addr_t cedrus_dst_buf_addr(struct cedrus_ctx *ctx,
					     unsigned int index,
					     unsigned int plane)
{
	struct vb2_buffer *buf = ctx->dst_bufs[index];

	return buf ? cedrus_buf_addr(buf, &ctx->dst_fmt, plane) : 0;
}

static inline struct cedrus_buffer *
vb2_v4l2_to_cedrus_buffer(const struct vb2_v4l2_buffer *p)
{
	return container_of(p, struct cedrus_buffer, m2m_buf.vb);
}

static inline struct cedrus_buffer *
vb2_to_cedrus_buffer(const struct vb2_buffer *p)
{
	return vb2_v4l2_to_cedrus_buffer(to_vb2_v4l2_buffer(p));
}

void *cedrus_find_control_data(struct cedrus_ctx *ctx, u32 id);

#endif
