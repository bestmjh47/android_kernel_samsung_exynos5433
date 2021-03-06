/* linux/drivers/media/video/exynos/gsc/gsc-output.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung EXYNOS5 SoC series G-scaler driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bug.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/exynos_iovmm.h>
#include <linux/fb.h>
#include <media/v4l2-ioctl.h>

#include "gsc-core.h"
DEFINE_SPINLOCK(size_lock);
int gsc_out_hw_reset_off(struct gsc_dev *gsc)
{
	gsc_hw_enable_localout(gsc->out.ctx, false);

	return 0;
}

void gsc_out_add_active_buf(struct gsc_dev *gsc)
{
	struct gsc_input_buf *buf;
	unsigned long flags;
	spin_lock_irqsave(&gsc->slock, flags);
	if (!list_empty(&gsc->out.pending_buf_q)) {
		buf = pending_q_pop(&gsc->out);
		active_q_push(&gsc->out, buf);
		gsc_hw_set_input_addr_fixed(gsc, &buf->addr);
	}
	spin_unlock_irqrestore(&gsc->slock, flags);
}

int gsc_out_hw_set(struct gsc_ctx *ctx)
{
	struct gsc_dev *gsc = ctx->gsc_dev;
	struct exynos_platform_gscaler *pdata = gsc->pdata;
	struct gsc_scaler *sc = &ctx->scaler;
	int ret = 0;

	if (gsc->out.ctx->out_path == GSC_FIMD) {
		gsc_hw_set_local_dst(gsc, GSC_FIMD, true);
	} else {
		gsc_hw_set_mixer(gsc->id);
		gsc_hw_set_local_dst(gsc, GSC_MIXER, true);
	}

	gsc_hw_set_frm_done_irq_mask(gsc, false);
	gsc_hw_set_overflow_irq_mask(gsc, false);
	gsc_hw_set_read_slave_error_mask(gsc, false);
	gsc_hw_set_write_slave_error_mask(gsc, false);
	gsc_hw_set_deadlock_irq_mask(gsc, true);
	gsc_hw_set_gsc_irq_enable(gsc, true);
	gsc_hw_set_one_frm_mode(gsc, false);
	gsc_hw_set_freerun_clock_mode(gsc, false);

	gsc_hw_set_input_path(ctx);
	gsc_hw_set_in_image_format(ctx);

	gsc_hw_set_output_path(ctx);
	gsc_hw_set_out_image_format(ctx);

	gsc_hw_set_input_rotation(ctx);

	if (ctx->gsc_ctrls.rotate->val &&
		((sc->main_hratio > GSC_SC_UP_MAX_RATIO) ||
		(sc->main_vratio > GSC_SC_UP_MAX_RATIO)))
		gsc_pm_qos_ctrl(gsc, GSC_QOS_ON, pdata->mif_min_otf_rot,
				pdata->int_min_otf);
	else
		gsc_pm_qos_ctrl(gsc, GSC_QOS_ON, pdata->mif_min,
				pdata->int_min_otf);

	gsc_hw_enable_localout(ctx, true);
	ret = gsc_wait_operating(gsc);
	if (ret < 0) {
		gsc_err("wait operation timeout");
		return -EINVAL;
	}

	return 0;
}

static int gsc_subdev_try_crop(struct gsc_dev *gsc, struct v4l2_rect *cr)
{
	struct gsc_variant *variant = gsc->variant;
	u32 pixel_rate = variant->pix_max->otf_w * variant->pix_max->otf_h;

	if (cr->width < variant->pix_min->otf_w ||
		cr->height < variant->pix_min->otf_h) {
		gsc_err("Min size error");
		goto err;
	}

	if (cr->width * cr->height > pixel_rate) {
		gsc_err("Max size error");
		goto err;
	}

	gsc_dbg("Aligned l:%d, t:%d, w:%d, h:%d", cr->left, cr->top,
		cr->width, cr->height);

	return 0;
err:
	gsc_err("l:%d, t:%d, w:%d, h:%d", cr->left, cr->top,
		cr->width, cr->height);
	return -EINVAL;
}

static int gsc_subdev_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_fh *fh,
			      struct v4l2_subdev_format *fmt)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct v4l2_mbus_framefmt *mf = &fmt->format;
	struct gsc_frame *f;

	if (fmt->pad == GSC_PAD_SINK) {
		gsc_err("Sink pad get_fmt is not supported");
		return 0;
	}

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_get_try_format(fh, fmt->pad);
		return 0;
	}

	f = &ctx->d_frame;
	mf->code = f->fmt->mbus_code;
	mf->width = f->f_width;
	mf->height = f->f_height;
	mf->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

static int gsc_subdev_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_fh *fh,
			       struct v4l2_subdev_format *fmt)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	struct v4l2_mbus_framefmt *mf;
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct gsc_frame *f;

	gsc_dbg("pad%d: code: 0x%x, %dx%d",
	    fmt->pad, fmt->format.code, fmt->format.width, fmt->format.height);

	if (fmt->pad == GSC_PAD_SINK) {
		gsc_err("Sink pad set_fmt is not supported");
		return 0;
	}

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		mf = v4l2_subdev_get_try_format(fh, fmt->pad);
		mf->width = fmt->format.width;
		mf->height = fmt->format.height;
		mf->code = fmt->format.code;
		mf->colorspace = V4L2_COLORSPACE_JPEG;
	} else {
		f = &ctx->d_frame;
		gsc_set_frame_size(f, fmt->format.width, fmt->format.height);
		f->fmt = find_format(NULL, &fmt->format.code, 0);
		ctx->state |= GSC_DST_FMT;
	}

	return 0;
}

static int gsc_subdev_get_crop(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_crop *crop)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct v4l2_rect *r = &crop->rect;
	struct gsc_frame *f;

	if (crop->pad == GSC_PAD_SINK) {
		gsc_err("Sink pad get_crop is not supported");
		return 0;
	}

	if (crop->which == V4L2_SUBDEV_FORMAT_TRY) {
		crop->rect = *v4l2_subdev_get_try_crop(fh, crop->pad);
		return 0;
	}

	f = &ctx->d_frame;
	r->left	  = f->crop.left;
	r->top	  = f->crop.top;
	r->width  = f->crop.width;
	r->height = f->crop.height;

	gsc_dbg("f:%p, pad%d: l:%d, t:%d, %dx%d, f_w: %d, f_h: %d",
	    f, crop->pad, r->left, r->top, r->width, r->height,
	    f->f_width, f->f_height);

	return 0;
}

static int gsc_subdev_set_crop(struct v4l2_subdev *sd,
				struct v4l2_subdev_fh *fh,
				struct v4l2_subdev_crop *crop)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct gsc_scaler *sc = &ctx->scaler;
	struct v4l2_rect *r;
	struct gsc_frame *f;

	gsc_dbg("id(%d), rot(%d)(%d,%d)/%dx%d", gsc->id,
		ctx->gsc_ctrls.rotate->val, crop->rect.left,
		crop->rect.top, crop->rect.width, crop->rect.height);

	if (crop->pad == GSC_PAD_SINK) {
		gsc_err("Sink pad set_fmt is not supported\n");
		return 0;
	}

	spin_lock(&size_lock);

	if (crop->which == V4L2_SUBDEV_FORMAT_TRY) {
		r = v4l2_subdev_get_try_crop(fh, crop->pad);
		r->left = crop->rect.left;
		r->top = crop->rect.top;
		r->width = crop->rect.width;
		r->height = crop->rect.height;
	} else {
		int ret = 0;
		f = &ctx->d_frame;
		f->crop.left = crop->rect.left;
		f->crop.top = crop->rect.top;
		f->crop.width = crop->rect.width;
		f->crop.height = crop->rect.height;
		ctx->state |= GSC_DST_CROP;

		if (test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
			ret = gsc_subdev_try_crop(gsc, &f->crop);
			if (ret) {
				gsc_err("Crop size error");
				spin_unlock(&size_lock);
				return ret;
			}
			ret = gsc_set_scaler_info(ctx);
			if (ret) {
				gsc_err("Scaler setup error");
				spin_unlock(&size_lock);
				return ret;
			}
			gsc_hw_set_in_size(ctx);
			gsc_hw_set_out_size(ctx);
			gsc_hw_set_prescaler(ctx);
			gsc_hw_set_mainscaler(ctx);

			if (sc->main_hratio_dirty)
				gsc_hw_set_h_coef(ctx);
			if (sc->main_vratio_dirty)
				gsc_hw_set_v_coef(ctx);

			gsc_hw_set_input_rotation(ctx);

			gsc_hw_set_smart_if_pix_num(ctx);
			gsc_hw_set_smart_if_con(gsc, true);
		}
	}

	spin_unlock(&size_lock);

	return 0;
}

static int gsc_subdev_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	int ret;

	if (enable) {
		if (gsc->out.ctx->out_path != GSC_FIMD) {
			gsc_err("output path(%d) is not DECON",
					gsc->out.ctx->out_path);
			return -EINVAL;
		}
		if (!test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
			gsc_err("GSC is not ready start");
			return -EBUSY;
		}
		ret = gsc_out_hw_set(gsc->out.ctx);
		if (ret) {
			gsc_err("GSC H/W setting is failed");
			return -EINVAL;
		}
	} else {
		if (test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
			gsc_out_hw_reset_off(gsc);
		}
	}

	return 0;
}

void gsc_otf_dump(struct gsc_dev *gsc)
{
	pr_info("Gscaler%d dump\n", gsc->id);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			gsc->regs, 0xD0, false);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			gsc->regs + 0xA78, 0x84, false);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			gsc->regs + 0xC00, 0x10, false);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			gsc->regs + 0xB00, 0x40, false);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			gsc->regs + 0xC0C, 0x10, false);
	pr_info("End of dump\n");
	pr_info("MIF : %d\n", pm_qos_request(PM_QOS_BUS_THROUGHPUT));
	pr_info("INT : %d\n", pm_qos_request(PM_QOS_DEVICE_THROUGHPUT));
	pr_info("DISP : %d\n", pm_qos_request(PM_QOS_DISPLAY_THROUGHPUT));
}

static long gsc_subdev_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	struct gsc_ctx *ctx = gsc->out.ctx;
	unsigned long flags;

	switch (cmd) {
	case GSC_SFR_UPDATE:
		gsc_out_add_active_buf(gsc);
		if ((bool)arg)
			gsc_hw_set_sfr_update(ctx);
		gsc->out.wq_time[gsc->out.wq_cnt % MAX_DEBUG_BUF_CNT] = sched_clock();
		gsc->out.wq_cnt++;
		break;

	case GSC_WAIT_STOP:
		gsc_wait_stop(gsc);
		gsc_pm_qos_ctrl(gsc, GSC_QOS_OFF, 0, 0);
		spin_lock_irqsave(&gsc->slock, flags);
		if ((int)arg & FBINFO_MISC_ESD_DETECTED)
			gsc_info("ST_OUTPUT_STREAMON is not cleared");
		else
			clear_bit(ST_OUTPUT_STREAMON, &gsc->state);
		spin_unlock_irqrestore(&gsc->slock, flags);
		wake_up(&gsc->irq_queue);
		break;

	case GSC_SFR_DUMP:
		gsc_otf_dump(gsc);
		break;
	default:
		gsc_err("unsupported gsc_subdev_ioctl");
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_subdev_core_ops gsc_subdev_core_ops = {
	.ioctl = gsc_subdev_ioctl,
};

static struct v4l2_subdev_pad_ops gsc_subdev_pad_ops = {
	.get_fmt = gsc_subdev_get_fmt,
	.set_fmt = gsc_subdev_set_fmt,
	.get_crop = gsc_subdev_get_crop,
	.set_crop = gsc_subdev_set_crop,
};

static struct v4l2_subdev_video_ops gsc_subdev_video_ops = {
	.s_stream = gsc_subdev_s_stream,
};

static struct v4l2_subdev_ops gsc_subdev_ops = {
	.pad = &gsc_subdev_pad_ops,
	.video = &gsc_subdev_video_ops,
	.core = &gsc_subdev_core_ops,
};

static int gsc_out_power_off(struct v4l2_subdev *sd)
{
	struct gsc_dev *gsc = entity_data_to_gsc(v4l2_get_subdevdata(sd));
	int ret;

	ret = gsc_out_hw_reset_off(gsc);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * The video node ioctl operations
 */
static int gsc_output_querycap(struct file *file, void *priv,
					struct v4l2_capability *cap)
{
	struct gsc_dev *gsc = video_drvdata(file);

	strncpy(cap->driver, gsc->pdev->name, sizeof(cap->driver) - 1);
	strncpy(cap->card, gsc->pdev->name, sizeof(cap->card) - 1);
	cap->bus_info[0] = 0;
	cap->capabilities = V4L2_CAP_STREAMING |
		V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OUTPUT_MPLANE;
	return 0;
}

static int gsc_output_enum_fmt_mplane(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	return gsc_enum_fmt_mplane(f);
}

static int gsc_output_try_fmt_mplane(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct gsc_dev *gsc = video_drvdata(file);

	if (!is_output(f->type)) {
		gsc_err("Not supported buffer type");
		return -EINVAL;
	}

	return gsc_try_fmt_mplane(gsc->out.ctx, f);
}

static int gsc_output_s_fmt_mplane(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct gsc_frame *frame;
	struct v4l2_pix_format_mplane *pix;
	int i, ret = 0;

	ret = gsc_output_try_fmt_mplane(file, fh, f);
	if (ret) {
		gsc_err("Invalid argument");
		return ret;
	}

	if (vb2_is_streaming(&gsc->out.vbq)) {
		gsc_err("queue (%d) busy", f->type);
		return -EBUSY;
	}

	frame = &ctx->s_frame;

	pix = &f->fmt.pix_mp;
	frame->fmt = find_format(&pix->pixelformat, NULL, 0);
	if (!frame->fmt) {
		gsc_err("Not supported pixel format");
		return -EINVAL;
	}

	for (i = 0; i < frame->fmt->num_planes; i++)
		frame->payload[i] = pix->plane_fmt[i].sizeimage;

	gsc_set_frame_size(frame, pix->width, pix->height);

	ctx->state |= GSC_SRC_FMT;

	gsc_dbg("f_w: %d, f_h: %d", frame->f_width, frame->f_height);

	return 0;
}

static int gsc_output_g_fmt_mplane(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_ctx *ctx = gsc->out.ctx;

	if (!is_output(f->type)) {
		gsc_err("Not supported buffer type");
		return -EINVAL;
	}

	return gsc_g_fmt_mplane(ctx, f);
}

static int gsc_output_reqbufs(struct file *file, void *priv,
			    struct v4l2_requestbuffers *reqbufs)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_output_device *out = &gsc->out;
	struct gsc_frame *frame;
	int ret;

	if (reqbufs->count > gsc->variant->in_buf_cnt) {
		gsc_err("Requested count exceeds maximun count of input buffer");
		return -EINVAL;
	} else if (reqbufs->count == 0)
		gsc_ctx_state_lock_clear(GSC_SRC_FMT | GSC_DST_FMT |
				GSC_DST_CROP, out->ctx);

	gsc_set_protected_content(gsc, out->ctx->gsc_ctrls.drm_en->cur.val);

	frame = ctx_get_frame(out->ctx, reqbufs->type);
	frame->cacheable = out->ctx->gsc_ctrls.cacheable->val;
	gsc->vb2->set_cacheable(gsc->alloc_ctx, frame->cacheable);


	if (gsc->protected_content && reqbufs->count) {
		int id = gsc->id + 3;
		exynos_smc(SMC_PROTECTION_SET, 0, id, 1);
		gsc_dbg("DRM enable");
	} else if (gsc->protected_content && !reqbufs->count){
		int id = gsc->id + 3;
		exynos_smc(SMC_PROTECTION_SET, 0, id, 0);
		gsc_dbg("DRM disable");
		gsc_set_protected_content(gsc, false);
	}

	ret = vb2_reqbufs(&out->vbq, reqbufs);
	if (ret)
		return ret;
	out->req_cnt = reqbufs->count;

	return ret;
}

static int gsc_output_querybuf(struct file *file, void *priv,
			   struct v4l2_buffer *buf)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_output_device *out = &gsc->out;

	return vb2_querybuf(&out->vbq, buf);
}

static int gsc_output_streamon(struct file *file, void *priv,
			     enum v4l2_buf_type type)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_output_device *out = &gsc->out;
	struct media_pad *sink_pad;
	int ret;

	if (test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
		gsc_err("gsc didn't stop complete");
		return -EBUSY;
	}

	sink_pad = media_entity_remote_source(&out->sd_pads[GSC_PAD_SOURCE]);
	if (IS_ERR(sink_pad)) {
		gsc_err("No sink pad conncted with a gscaler source pad");
		return PTR_ERR(sink_pad);
	}

	ret = vb2_streamon(&gsc->out.vbq, type);

	return ret;
}

static int gsc_output_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type type)
{
	struct gsc_dev *gsc = video_drvdata(file);
	int ret = 0;

	ret = vb2_streamoff(&gsc->out.vbq, type);

	return ret;
}

static int gsc_output_qbuf(struct file *file, void *priv,
			  struct v4l2_buffer *buf)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_output_device *out = &gsc->out;
	int ret;

	ret = vb2_qbuf(&out->vbq, buf);
	gsc->out.q_time[gsc->out.q_cnt % MAX_DEBUG_BUF_CNT] = sched_clock();
	gsc->out.q_cnt++;

	return ret;
}

static int gsc_output_dqbuf(struct file *file, void *priv,
			   struct v4l2_buffer *buf)
{
	struct gsc_dev *gsc = video_drvdata(file);
	int ret = 0;
	struct vb2_queue *vbq = &gsc->out.vbq;
	unsigned long flags;

	spin_lock_irqsave(&gsc->slock, flags);
	if (list_empty(&vbq->done_list))
		gsc_info("Done list empty");
	spin_unlock_irqrestore(&gsc->slock, flags);

	ret = vb2_dqbuf(vbq, buf, file->f_flags & O_NONBLOCK);

	gsc->out.dq_time[gsc->out.dq_cnt % MAX_DEBUG_BUF_CNT] = sched_clock();
	gsc->out.dq_cnt++;

	return ret;
}

static int gsc_output_cropcap(struct file *file, void *fh,
				struct v4l2_cropcap *cr)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_ctx *ctx = gsc->out.ctx;

	if (!is_output(cr->type)) {
		gsc_err("Not supported buffer type");
		return -EINVAL;
	}

	cr->bounds.left		= 0;
	cr->bounds.top		= 0;
	cr->bounds.width	= ctx->s_frame.f_width;
	cr->bounds.height	= ctx->s_frame.f_height;
	cr->defrect		= cr->bounds;

	return 0;

}

static int gsc_output_g_crop(struct file *file, void *fh,
			     struct v4l2_crop *cr)
{
	struct gsc_dev *gsc = video_drvdata(file);

	if (!is_output(cr->type)) {
		gsc_err("Not supported buffer type");
		return -EINVAL;
	}

	return gsc_g_crop(gsc->out.ctx, cr);
}

static int gsc_output_s_crop(struct file *file, void *fh, struct v4l2_crop *cr)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct gsc_ctx *ctx = gsc->out.ctx;
	struct gsc_frame *f;
	int ret;

	if (!is_output(cr->type)) {
		gsc_err("Not supported buffer type");
		return -EINVAL;
	}

	spin_lock(&size_lock);

	ret = gsc_try_crop(ctx, cr);
	if (ret) {
		spin_unlock(&size_lock);
		return ret;
	}

	f = &ctx->s_frame;

	ctx->scaler.is_scaled_down = false;

	f->crop.left = cr->c.left;
	f->crop.top = cr->c.top;
	f->crop.width  = cr->c.width;
	f->crop.height = cr->c.height;

	spin_unlock(&size_lock);

	return 0;
}

static const struct v4l2_ioctl_ops gsc_output_ioctl_ops = {
	.vidioc_querycap		= gsc_output_querycap,
	.vidioc_enum_fmt_vid_out_mplane	= gsc_output_enum_fmt_mplane,

	.vidioc_try_fmt_vid_out_mplane	= gsc_output_try_fmt_mplane,
	.vidioc_s_fmt_vid_out_mplane	= gsc_output_s_fmt_mplane,
	.vidioc_g_fmt_vid_out_mplane	= gsc_output_g_fmt_mplane,

	.vidioc_reqbufs			= gsc_output_reqbufs,
	.vidioc_querybuf		= gsc_output_querybuf,

	.vidioc_qbuf			= gsc_output_qbuf,
	.vidioc_dqbuf			= gsc_output_dqbuf,

	.vidioc_streamon		= gsc_output_streamon,
	.vidioc_streamoff		= gsc_output_streamoff,

	.vidioc_g_crop			= gsc_output_g_crop,
	.vidioc_s_crop			= gsc_output_s_crop,
	.vidioc_cropcap			= gsc_output_cropcap,
};

static int gsc_out_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct gsc_ctx *ctx = q->drv_priv;
	struct gsc_dev *gsc = ctx->gsc_dev;
	unsigned long flags;

	spin_lock_irqsave(&gsc->slock, flags);
	if (!list_empty(&gsc->out.pending_buf_q)) {
		/*
		 * Gscaler starts standalone in case of first frame, So
		 * before starting gscaer, base address should be set.
		 * Unless, page-fault will be occured.
		 */
		struct gsc_input_buf *buf =
			list_first_entry(&gsc->out.pending_buf_q,
					struct gsc_input_buf, list);
		gsc_hw_set_input_addr_fixed(gsc, &buf->addr);
	} else {
		gsc_err("Before StreamON, 1 Qbuf should be called at least");
		return -EINVAL;
	}

	set_bit(DEBUG_STREAMON, &gsc->state);
	spin_unlock_irqrestore(&gsc->slock, flags);

	return 0;
}

struct gsc_deferred_stop_data {
	struct completion okay;
	struct gsc_dev *gsc;
};

/*
 * gsc_out_deferred_stop - OTF completion waiter
 *
 * Invoked by gsc_out_stop_streaming if wait_event_timeout() in it is timed out.
 * Occasionally DECON does not stop GSC OTF in 2 seconds that is the limit of
 * the patience of GSC driver for waiting OTF to be ended because the driver
 * should respond in a finite time to the user that wanted to finish working
 * of the GScaler driver.
 *
 * Gscaler driver should release every reference to buffers queued from the
 * user on reqbufs(0) and close(). However releasing references causes GScaler
 * to access freed buffer if OTF is not stoped by DECON driver.
 * gsc_out_deferred_stop() is the final and desperate try to prevent the invalid
 * buffer access by GScaler.
 *
 * This logic relies on ION's lazy unmapping that unmaps all mappings of a
 * buffer from System MMU page tables when the buffer is finally freed.
 */
static int gsc_out_deferred_stop(void *p)
{
	struct gsc_deferred_stop_data *data = p;
	struct gsc_dev *gsc = data->gsc;
	struct gsc_input_buf *active_buf;
	/*
	 * allocate buffers for YUV semiplanar images. It is okay to allocate
	 * 32 * 2 array because the kernel stack is much larger and the call
	 * depth is not such deep.
	 */
	struct dma_buf *dmabufs[VIDEO_MAX_FRAME * 2];
	int cnt = 0;

	list_for_each_entry(active_buf, &gsc->out.active_buf_q, list) {
		int i;

		for (i = 0; i < active_buf->vb.num_planes; i++) {
			if (active_buf->vb.planes[i].dbuf) {
				dmabufs[cnt] = active_buf->vb.planes[i].dbuf;
				get_dma_buf(dmabufs[cnt++]);
			} else {
				/* all valid output buffers should be dmabuf */
				BUG();
			}
		}
	}

	/*
	 * After the call to complete() below, this function should not reference
	 * @data because it is a local variable of gsc_out_stop_streaming() that
	 * invokes this kthread function. gsc_out_stop_streaming() returns after
	 * the call to complete() below, and @data becomes invalid.
	 */
	complete(&data->okay);

	/*
	 * Wait until the irq_queue to be signaled undefinitely. DECON driver
	 * therefore should call s_stream(false) to disable OTF of GScaler
	 * someday. Otherwise, the buffers referenced here are not freed and
	 * no furthre OTF will start until DECON driver dsiables OTF.
	 */
	wait_event(gsc->irq_queue, !test_bit(ST_OUTPUT_STREAMON, &gsc->state));

	while (cnt-- > 0)
		dma_buf_put(dmabufs[cnt]);

	gsc_info("Completed deferred OTF stop");

	return 0;
}

static int gsc_out_stop_streaming(struct vb2_queue *q)
{
	struct gsc_ctx *ctx = q->drv_priv;
	struct gsc_dev *gsc = ctx->gsc_dev;
	int ret = 0;

	unsigned long flags;
	ret = wait_event_timeout(gsc->irq_queue,
		!test_bit(ST_OUTPUT_STREAMON, &gsc->state),
		msecs_to_jiffies(2000));
	if (ret == 0) {
		struct gsc_deferred_stop_data data;

		gsc_warn("wait timeout: deferring OTF wait..");
		init_completion(&data.okay);
		data.gsc = gsc;

		kthread_run(gsc_out_deferred_stop, &data,
				"gsc_out_deferred_stop");

		wait_for_completion(&data.okay);
	}

	spin_lock_irqsave(&gsc->slock, flags);
	while(!list_empty(&gsc->out.active_buf_q)) {
		struct gsc_input_buf *done_buf;
		done_buf = active_q_pop(&gsc->out);
		vb2_buffer_done(&done_buf->vb, VB2_BUF_STATE_ERROR);
	}

	while(!list_empty(&gsc->out.pending_buf_q)) {
		struct gsc_input_buf *buf;
		buf = pending_q_pop(&gsc->out);
	}

	INIT_LIST_HEAD(&gsc->out.pending_buf_q);
	INIT_LIST_HEAD(&gsc->out.active_buf_q);

	clear_bit(DEBUG_STREAMON, &gsc->state);
	spin_unlock_irqrestore(&gsc->slock, flags);

	return 0;
}

static int gsc_out_queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
				unsigned int *num_buffers, unsigned int *num_planes,
				unsigned int sizes[], void *allocators[])
{
	struct gsc_ctx *ctx = vq->drv_priv;
	struct gsc_fmt *ffmt = ctx->s_frame.fmt;
	int i, ret = 0;

	if (IS_ERR(ffmt)) {
		gsc_err("Invalid source format");
		return PTR_ERR(ffmt);
	}

	*num_planes = ffmt->num_planes;

	for (i = 0; i < ffmt->num_planes; i++) {
		sizes[i] = get_plane_size(&ctx->s_frame, i);
		allocators[i] = ctx->gsc_dev->alloc_ctx;
	}

	ret = vb2_queue_init(vq);
	if (ret) {
		gsc_err("failed to init vb2_queue");
		return ret;
	}

	return 0;
}

int gsc_out_add_pending_buf(struct gsc_dev *gsc, struct gsc_ctx *ctx,
 			struct gsc_input_buf *buf, int index)
{
	int ret;

	ret = gsc_prepare_addr(ctx, &buf->vb, &ctx->s_frame,
			&ctx->s_frame.addr);
	if (ret) {
		gsc_err("Fail to prepare G-Scaler address");
		return -EINVAL;
	}
	if (!ctx->s_frame.addr.y) {
		gsc_err("source address is null");
		return -EINVAL;
	}
	buf->addr = ctx->s_frame.addr;
	buf->idx = index;
	pending_q_push(&gsc->out, buf);

	return 0;
}

static void gsc_out_buffer_queue(struct vb2_buffer *vb)
{
	struct gsc_input_buf *buf
		= container_of(vb, struct gsc_input_buf, vb);
	struct vb2_queue *q = vb->vb2_queue;
	struct gsc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct gsc_dev *gsc = ctx->gsc_dev;
	unsigned long flags;
	int ret;

	if (!test_bit(ST_OUTPUT_STREAMON, &gsc->state) && q->streaming) {
		gsc_info("Already s_stream(0) is called");
		return;
	}

	if (!q->streaming) {
		gsc_hw_set_sw_reset(gsc);
		ret = gsc_wait_reset(gsc);
		if (ret < 0) {
			gsc_err("gscaler s/w reset timeout");
			return;
		}
		gsc_hw_set_input_buf_fixed(gsc);
		set_bit(ST_OUTPUT_STREAMON, &gsc->state);
	}

	spin_lock_irqsave(&gsc->slock, flags);
	if (gsc->out.req_cnt >= atomic_read(&q->queued_count)) {
		ret = gsc_out_add_pending_buf(gsc, ctx, buf,
				vb->v4l2_buf.index);
		if (ret) {
			gsc_err("Failed to prepare G-Scaler address");
		}
	} else {
		gsc_err("All requested buffers have been queued already");
	}
	spin_unlock_irqrestore(&gsc->slock, flags);
}

static struct vb2_ops gsc_output_qops = {
	.queue_setup		= gsc_out_queue_setup,
	.buf_prepare		= vb2_ion_buf_prepare,
	.buf_queue		= gsc_out_buffer_queue,
	.wait_prepare		= gsc_unlock,
	.wait_finish		= gsc_lock,
	.start_streaming	= gsc_out_start_streaming,
	.stop_streaming		= gsc_out_stop_streaming,
};

static int gsc_out_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	if (media_entity_type(entity) != MEDIA_ENT_T_V4L2_SUBDEV)
		return 0;

	if (local->flags == MEDIA_PAD_FL_SOURCE) {
		struct gsc_dev *gsc = entity_to_gsc(entity);
		struct v4l2_subdev *sd;
		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (gsc->pipeline.disp == NULL) {
				sd = media_entity_to_v4l2_subdev(remote->entity);
				gsc->pipeline.disp = sd;
				gsc->out.ctx->out_path = GSC_FIMD;
				gsc_dbg("LINK Enable(%d)", gsc->id);
			}
		} else if (!(flags & ~MEDIA_LNK_FL_ENABLED)) {
			if (gsc->pipeline.disp != NULL) {
				gsc_dbg("LINK Disable(%d)", gsc->id);
				gsc->pipeline.disp = NULL;
				gsc->out.ctx->out_path = 0;
			}
		}
	}

	return 0;
}

static const struct media_entity_operations gsc_out_media_ops = {
	.link_setup = gsc_out_link_setup,
};

int gsc_output_ctrls_create(struct gsc_dev *gsc)
{
	int ret;

	ret = gsc_ctrls_create(gsc->out.ctx);
	if (ret) {
		gsc_err("Failed to create controls of G-Scaler");
		return ret;
	}

	return 0;
}

static int gsc_sysmmu_output_fault_handler(struct iommu_domain *domain,
				struct device *dev, unsigned long fault_addr,
				int fault_flags, void *p)
{
	struct gsc_output_device *out = p;

	dev_crit(dev, "System MMU fault while OUTPUT ---------\n");
	gsc_hw_dump_regs(out->ctx->gsc_dev->regs);

	return 0;
}

static int gsc_output_open(struct file *file)
{
	struct gsc_dev *gsc = video_drvdata(file);
	int ret = v4l2_fh_open(file);

	if (ret)
		return ret;

	if (test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
		gsc_err("Previous OTF is not ended.");
		ret = -EBUSY;
		goto err_otf;
	}

	gsc_dbg("pid: %d, state: 0x%lx", task_pid_nr(current), gsc->state);
	ret = pm_runtime_get_sync(&gsc->pdev->dev);
	if (ret < 0) {
		gsc_err("fail to pm_runtime_get_sync()");
		goto err_pm;
	}

	/* Return if the corresponding mem2mem/output/capture video node
	   is already opened. */
	if (gsc_m2m_opened(gsc) || gsc_cap_opened(gsc) || gsc_out_opened(gsc)) {
		gsc_err("G-Scaler%d has been opened already, state : 0x%lx",
				gsc->id, gsc->state);
		ret = -EBUSY;
		goto err_exist;
	}

	if (WARN_ON(gsc->out.ctx == NULL)) {
		gsc_err("G-Scaler output context is NULL");
		ret = -ENXIO;
		goto err_exist;
	}

	set_bit(ST_OUTPUT_OPEN, &gsc->state);

	gsc->out.ctx->out_path = GSC_FIMD;

	ret = gsc_ctrls_create(gsc->out.ctx);
	if (ret < 0)
		goto err_ctrl;

	iovmm_set_fault_handler(&gsc->pdev->dev,
			gsc_sysmmu_output_fault_handler, &gsc->out);

	bts_otf_initialize(gsc->id, true);

	gsc->out.q_cnt = gsc->out.dq_cnt = gsc->out.isr_cnt = gsc->out.wq_cnt = 0;
	gsc->out.real_isr_cnt = 0;
	gsc->out.ctx->scaler.main_hratio = 0;
	gsc->out.ctx->scaler.main_vratio = 0;

	return ret;
err_ctrl:
	clear_bit(ST_OUTPUT_OPEN, &gsc->state);
err_exist:
	pm_runtime_put(&gsc->pdev->dev);
err_pm:
err_otf:
	v4l2_fh_release(file);

	return ret;
}

static int gsc_output_close(struct file *file)
{
	struct gsc_dev *gsc = video_drvdata(file);
	struct vb2_queue *q = &gsc->out.vbq;
	int ret = 0;

	gsc_dbg("pid: %d, state: 0x%lx", task_pid_nr(current), gsc->state);
	/* This is unnormal case */
	if (gsc->protected_content) {
		int id = gsc->id + 3;
		gsc_err("DRM should be disabled before device close");
		exynos_smc(SMC_PROTECTION_SET, 0, id, 0);
		gsc_set_protected_content(gsc, false);
	}

	if (test_bit(ST_OUTPUT_STREAMON, &gsc->state)) {
		gsc_info("driver is closed by force");
		gsc_ctx_state_lock_clear(GSC_SRC_FMT | GSC_DST_FMT |
				GSC_DST_CROP, gsc->out.ctx);

		ret = v4l2_subdev_call(gsc->mdev[MDEV_OUTPUT]->decon_sd, core, ioctl,
				S3CFB_FLUSH_WORKQUEUE, NULL);
		if (ret) {
			gsc_err("Decon subdev ioctl failed");
			return ret;
		}
	}

	if (q->streaming)
		gsc_out_stop_streaming(q);

	vb2_queue_release(q);
	gsc_ctrls_delete(gsc->out.ctx);
	v4l2_fh_release(file);

	bts_otf_initialize(gsc->id, false);
	pm_runtime_put_sync(&gsc->pdev->dev);

	clear_bit(ST_OUTPUT_OPEN, &gsc->state);

	gsc->out.ctx->out_path = 0;

	return 0;
}

static unsigned int gsc_output_poll(struct file *file,
				      struct poll_table_struct *wait)
{
	struct gsc_dev *gsc = video_drvdata(file);

	return vb2_poll(&gsc->out.vbq, file, wait);
}

static int gsc_output_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct gsc_dev *gsc = video_drvdata(file);

	return vb2_mmap(&gsc->out.vbq, vma);
}

static const struct v4l2_file_operations gsc_output_fops = {
	.owner		= THIS_MODULE,
	.open		= gsc_output_open,
	.release	= gsc_output_close,
	.poll		= gsc_output_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= gsc_output_mmap,
};

static int gsc_create_link(struct gsc_dev *gsc)
{
	struct media_entity *source, *sink;
	int ret;

	source = &gsc->out.vfd->entity;
	sink = &gsc->out.sd->entity;
	ret = media_entity_create_link(source, 0, sink, GSC_PAD_SINK,
				       MEDIA_LNK_FL_IMMUTABLE |
				       MEDIA_LNK_FL_ENABLED);
	if (ret) {
		gsc_err("Failed to create link between G-Scaler vfd and subdev");
		return ret;
	}

	return 0;
}


static int gsc_create_subdev(struct gsc_dev *gsc)
{
	struct v4l2_subdev *sd;
	struct exynos_media_ops *gsc_out_link_callback;
	int ret;

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd)
	       return -ENOMEM;

	v4l2_subdev_init(sd, &gsc_subdev_ops);
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(sd->name, sizeof(sd->name), "%s.%d", GSC_SUBDEV_NAME, gsc->id);

	gsc->out.sd_pads[GSC_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	gsc->out.sd_pads[GSC_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	sd->grp_id = gsc->id;
	ret = media_entity_init(&sd->entity, GSC_PADS_NUM,
				gsc->out.sd_pads, 0);
	if (ret) {
		gsc_err("Failed to initialize the G-Scaler media entity");
		goto error;
	}

	sd->entity.ops = &gsc_out_media_ops;
	ret = v4l2_device_register_subdev(&gsc->mdev[MDEV_OUTPUT]->v4l2_dev, sd);
	if (ret) {
		media_entity_cleanup(&sd->entity);
		goto error;
	}
	gsc->mdev[MDEV_OUTPUT]->gsc_sd[gsc->id] = sd;
	gsc_dbg("gsc_sd[%d] = 0x%08x\n", gsc->id,
			(u32)gsc->mdev[MDEV_OUTPUT]->gsc_sd[gsc->id]);
	gsc->out.sd = sd;
	gsc_out_link_callback = kzalloc(sizeof(*gsc_out_link_callback), GFP_KERNEL);
	if (!gsc_out_link_callback)
	       goto error;

	gsc_out_link_callback->power_off = gsc_out_power_off;
	gsc->md_data.media_ops = gsc_out_link_callback;
	v4l2_set_subdevdata(sd, &gsc->md_data);

	return 0;
error:
	kfree(sd);
	return ret;
}

int gsc_get_sysreg_addr(struct gsc_dev *gsc)
{
	if (of_have_populated_dt()) {
		struct device_node *nd;

		nd = of_find_compatible_node(NULL, NULL,
				"samsung,exynos5-sysreg_localout");
		if (!nd) {
			gsc_err("failed find compatible node");
			return -ENODEV;
		}

		gsc->sysreg_disp = of_iomap(nd, 0);
		if (!gsc->sysreg_disp) {
			gsc_err("Failed to get address.");
			return -ENOMEM;
		}

		gsc->sysreg_gscl = of_iomap(nd, 1);
		if (!gsc->sysreg_gscl) {
			gsc_err("Failed to get address.");
			return -ENOMEM;
		}
	} else {
		gsc_err("failed have populated device tree");
		return -EIO;
	}

	return 0;
}

int gsc_register_output_device(struct gsc_dev *gsc)
{
	struct video_device *vfd;
	struct gsc_output_device *gsc_out;
	struct gsc_ctx *ctx;
	struct vb2_queue *q;
	int ret = -ENOMEM;

	ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->gsc_dev	 = gsc;
	ctx->s_frame.fmt = get_format(GSC_OUT_DEF_SRC);
	ctx->d_frame.fmt = get_format(GSC_OUT_DEF_DST);
	ctx->in_path	 = GSC_DMA;
	ctx->state	 = GSC_CTX_OUTPUT;

	vfd = video_device_alloc();
	if (!vfd) {
		gsc_err("Failed to allocate video device");
		goto err_ctx_alloc;
	}

	snprintf(vfd->name, sizeof(vfd->name), "%s.output",
		 dev_name(&gsc->pdev->dev));


	vfd->fops	= &gsc_output_fops;
	vfd->ioctl_ops	= &gsc_output_ioctl_ops;
	vfd->v4l2_dev	= &gsc->mdev[MDEV_OUTPUT]->v4l2_dev;
	vfd->release	= video_device_release;
	vfd->lock	= &gsc->lock;
	vfd->vfl_dir	= VFL_DIR_TX;
	video_set_drvdata(vfd, gsc);

	gsc_out	= &gsc->out;
	gsc_out->vfd = vfd;

	INIT_LIST_HEAD(&gsc_out->active_buf_q);
	INIT_LIST_HEAD(&gsc_out->pending_buf_q);
	spin_lock_init(&ctx->slock);
	gsc_out->ctx = ctx;

	q = &gsc->out.vbq;
	memset(q, 0, sizeof(*q));
	q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = gsc->out.ctx;
	q->ops = &gsc_output_qops;
	q->mem_ops = gsc->vb2->ops;
	q->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->buf_struct_size = sizeof(struct gsc_input_buf);
	q->name = "gsc-output.queue";

	ret = vb2_queue_init(q);
	if (ret) {
		gsc_err("failed to init vb2_queue");
		goto err_ctx_alloc;
	}

	ret = video_register_device(vfd, VFL_TYPE_GRABBER,
				    EXYNOS_VIDEONODE_GSC_OUT(gsc->id));
	if (ret) {
		gsc_err("Failed to register video device");
		goto err_ent;
	}

	gsc->out.vd_pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_init(&vfd->entity, 1, &gsc->out.vd_pad, 0);
	if (ret)
		goto err_ent;

	ret = gsc_create_subdev(gsc);
	if (ret)
		goto err_sd_reg;

	ret = gsc_create_link(gsc);
	if (ret)
		goto err_sd_reg;

	vfd->ctrl_handler = &ctx->ctrl_handler;

	ret = gsc_get_sysreg_addr(gsc);
	if (ret)
		goto err_sd_reg;

	gsc_dbg("gsc output driver registered as /dev/video%d, ctx(0x%08x)", vfd->num, (u32)ctx);
	return 0;

err_sd_reg:
	media_entity_cleanup(&vfd->entity);
err_ent:
	video_device_release(vfd);
err_ctx_alloc:
	kfree(ctx);
	return ret;
}

static void gsc_destroy_subdev(struct gsc_dev *gsc)
{
	struct v4l2_subdev *sd = gsc->out.sd;

	if (!sd)
		return;
	media_entity_cleanup(&sd->entity);
	v4l2_device_unregister_subdev(sd);
	kfree(sd);
	sd = NULL;
}

void gsc_unregister_output_device(struct gsc_dev *gsc)
{
	struct video_device *vfd = gsc->out.vfd;

	if (vfd) {
		media_entity_cleanup(&vfd->entity);
		/* Can also be called if video device was
		   not registered */
		video_unregister_device(vfd);
	}
	gsc_destroy_subdev(gsc);
	kfree(gsc->out.ctx);
	gsc->out.ctx = NULL;
}
