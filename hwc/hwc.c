/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <cutils/native_handle.h>
#define HWC_REMOVE_DEPRECATED_VERSIONS 1
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware_legacy/uevent.h>
#include <system/graphics.h>
#include <utils/Timers.h>
#include <EGL/egl.h>
#include <edid_parser.h>
#include <linux/omapfb.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "display.h"
#include "sw_vsync.h"
#include "utils.h"

#define MAX_HW_OVERLAYS 4
#define NUM_NONSCALING_OVERLAYS 1
#define ASPECT_RATIO_TOLERANCE 0.02f

/* copied from: KK bionic/libc/kernel/common/linux/fb.h */
#ifndef FB_FLAG_RATIO_4_3
#define FB_FLAG_RATIO_4_3 64
#endif
#ifndef FB_FLAG_RATIO_16_9
#define FB_FLAG_RATIO_16_9 128
#endif

/* used by property settings */
enum {
    EXT_ROTATION    = 3,        /* rotation while mirroring */
    EXT_HFLIP       = (1 << 2), /* flip l-r on output (after rotation) */
};

//#define DUMP_LAYERS
//#define DUMP_DSSCOMPS

static bool debug = false;
static bool debugpost2 = false;

static void showfps(void)
{
    static int framecount = 0;
    static int lastframecount = 0;
    static nsecs_t lastfpstime = 0;
    static float fps = 0;
    char value[PROPERTY_VALUE_MAX];

    property_get("debug.hwc.showfps", value, "0");
    if (!atoi(value)) {
        return;
    }

    framecount++;
    if (!(framecount & 0x7)) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        nsecs_t diff = now - lastfpstime;
        fps = ((framecount - lastframecount) * (float)(s2ns(1))) / diff;
        lastfpstime = now;
        lastframecount = framecount;
        ALOGI("%d Frames, %f FPS", framecount, fps);
    }
}

#ifdef DUMP_LAYERS
static void dump_layer(hwc_layer_1_t const* l)
{
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
         l->compositionType, l->flags, l->handle, l->transform, l->blending,
         l->sourceCrop.left,
         l->sourceCrop.top,
         l->sourceCrop.right,
         l->sourceCrop.bottom,
         l->displayFrame.left,
         l->displayFrame.top,
         l->displayFrame.right,
         l->displayFrame.bottom);
}
#endif

#ifdef DUMP_DSSCOMPS
static void dump_dsscomp(struct dsscomp_setup_dispc_data *d)
{
    uint32_t i;

    ALOGD("[%08x] set: %c%c%c %d ovls\n",
         d->sync_id,
         (d->mode & DSSCOMP_SETUP_MODE_APPLY) ? 'A' : '-',
         (d->mode & DSSCOMP_SETUP_MODE_DISPLAY) ? 'D' : '-',
         (d->mode & DSSCOMP_SETUP_MODE_CAPTURE) ? 'C' : '-',
         d->num_ovls);

    for (i = 0; i < d->num_mgrs; i++) {
        struct dss2_mgr_info *mi = &d->mgrs[i];
        ALOGD(" (dis%d alpha=%d col=%08x ilace=%d)\n",
             mi->ix,
             mi->alpha_blending, mi->default_color,
             mi->interlaced);
    }

    for (i = 0; i < d->num_ovls; i++) {
        struct dss2_ovl_info *oi = &d->ovls[i];
        struct dss2_ovl_cfg *c = &oi->cfg;
        if (c->zonly)
            ALOGD("ovl%d(%s z%d)\n",
                 c->ix, c->enabled ? "ON" : "off", c->zorder);
        else
            ALOGD("ovl%d(%s z%d %s%s *%d%% %d*%d:%d,%d+%d,%d rot%d%s => %d,%d+%d,%d %p/%p|%d)\n",
                 c->ix, c->enabled ? "ON" : "off", c->zorder, DSS_FMT(c->color_mode),
                 c->pre_mult_alpha ? " premult" : "",
                 (c->global_alpha * 100 + 128) / 255,
                 c->width, c->height, c->crop.x, c->crop.y,
                 c->crop.w, c->crop.h,
                 c->rotation, c->mirror ? "+mir" : "",
                 c->win.x, c->win.y, c->win.w, c->win.h,
                 (void *) oi->ba, (void *) oi->uv, c->stride);
    }
}
#endif

struct dump_buf {
    char *buf;
    int buf_len;
    int len;
};

static void dump_printf(struct dump_buf *buf, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    buf->len += vsnprintf(buf->buf + buf->len, buf->buf_len - buf->len, fmt, ap);
    va_end(ap);
}

static void dump_set_info(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *list, int disp)
{
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;

    char logbuf[1024];
    struct dump_buf log = {
        .buf = logbuf,
        .buf_len = sizeof(logbuf),
    };
    uint32_t i;

    dump_printf(&log, "set H{");
    for (i = 0; list && i < list->numHwLayers; i++) {
        if (i)
            dump_printf(&log, " ");
        hwc_layer_1_t *layer = &list->hwLayers[i];
        IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
        const char *hw = "SGX";
        if (layer->compositionType == HWC_OVERLAY)
            hw = layer->hints & HWC_HINT_TRIPLE_BUFFER ? "DSS" : "BV2D";
        dump_printf(&log, "%p:%s,", handle, hw);
        if ((layer->flags & HWC_SKIP_LAYER) || !handle) {
            dump_printf(&log, "SKIP");
            continue;
        }
        if (layer->flags & HWC_HINT_CLEAR_FB)
            dump_printf(&log, "CLR,");
        dump_printf(&log, "%d*%d(%s)", handle->iWidth, handle->iHeight, HAL_FMT(handle->iFormat));
        if (layer->transform)
            dump_printf(&log, "~%d", layer->transform);
    }
    dump_printf(&log, "} D{");
    for (i = 0; i < dsscomp->num_ovls; i++) {
        if (i)
            dump_printf(&log, " ");
        dump_printf(&log, "%d=", dsscomp->ovls[i].cfg.ix);
        if (dsscomp->ovls[i].cfg.enabled)
            dump_printf(&log, "%08x:%d*%d,%s",
                        dsscomp->ovls[i].ba,
                        dsscomp->ovls[i].cfg.width,
                        dsscomp->ovls[i].cfg.height,
                        DSS_FMT(dsscomp->ovls[i].cfg.color_mode));
        else
            dump_printf(&log, "-");
    }
    dump_printf(&log, "} L{");
    for (i = 0; i < comp->num_buffers; i++) {
        if (i)
            dump_printf(&log, " ");
        dump_printf(&log, "%p", comp->buffers[i]);
    }
    if (comp->blitter.num_buffers) {
        dump_printf(&log, "} B{");
        for (i = 0; i < comp->blitter.num_buffers; i++) {
            if (i)
                dump_printf(&log, " ");
            dump_printf(&log, "%p", comp->buffers[comp->num_buffers + i]);
        }
    }
    dump_printf(&log, "}%s\n", comp->use_sgx ? " swap" : "");

    ALOGD("%s", log.buf);
}

static int sync_id = 0;

static void setup_overlay(int index, uint32_t format, bool blended,
                          int width, int height, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;
    /* YUV2RGB conversion */
    const struct omap_dss_cconv_coefs ctbl_bt601_5 = {
        298,  409,    0,  298, -208, -100,  298,    0,  517, 0,
    };

    /* convert color format */
    oc->color_mode = convert_hal_to_dss_format(format, blended);

    if (oc->color_mode == OMAP_DSS_COLOR_NV12)
        oc->cconv = ctbl_bt601_5;

    oc->width = width;
    oc->height = height;
    oc->stride = ALIGN(width, HW_ALIGN) * get_format_bpp(format) / 8;

    oc->enabled = 1;
    oc->global_alpha = 255;
    oc->zorder = index;
    oc->ix = 0;

    /* defaults for SGX framebuffer renders */
    oc->crop.w = oc->win.w = width;
    oc->crop.h = oc->win.h = height;

    /* for now interlacing and vc1 info is not supplied */
    oc->ilace = OMAP_DSS_ILACE_NONE;
    oc->vc1.enable = 0;
}

static void adjust_overlay_to_layer(omap_hwc_device_t *hwc_dev __unused, struct dss2_ovl_info *ovl,
                                    hwc_layer_1_t const *layer, int index)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
    struct dss2_ovl_cfg *oc = &ovl->cfg;

#ifdef DUMP_LAYERS
    dump_layer(layer);
#endif

    setup_overlay(index, handle->iFormat, is_blended_layer(layer), handle->iWidth, handle->iHeight, ovl);

    /* convert transformation - assuming 0-set config */
    if (layer->transform & HWC_TRANSFORM_FLIP_H)
        oc->mirror = 1;
    if (layer->transform & HWC_TRANSFORM_FLIP_V) {
        oc->rotation = 2;
        oc->mirror = !oc->mirror;
    }
    if (layer->transform & HWC_TRANSFORM_ROT_90) {
        oc->rotation += oc->mirror ? -1 : 1;
        oc->rotation &= 3;
    }

    oc->pre_mult_alpha = layer->blending == HWC_BLENDING_PREMULT;

    /* display position */
    oc->win.x = layer->displayFrame.left;
    oc->win.y = layer->displayFrame.top;
    oc->win.w = WIDTH(layer->displayFrame);
    oc->win.h = HEIGHT(layer->displayFrame);

    /* crop */
    oc->crop.x = layer->sourceCrop.left;
    oc->crop.y = layer->sourceCrop.top;
    oc->crop.w = WIDTH(layer->sourceCrop);
    oc->crop.h = HEIGHT(layer->sourceCrop);
}

/*
 * assuming xpy (xratio:yratio) original pixel ratio, calculate the adjusted width
 * and height for a screen of xres/yres and physical size of width/height.
 * The adjusted size is the largest that fits into the screen.
 */
static void get_max_dimensions(uint32_t orig_xres, uint32_t orig_yres,
                               float xpy,
                               uint32_t scr_xres, uint32_t scr_yres,
                               uint32_t scr_width, uint32_t scr_height,
                               uint32_t *adj_xres, uint32_t *adj_yres)
{
    /* assume full screen (largest size)*/
    *adj_xres = scr_xres;
    *adj_yres = scr_yres;

    /* assume 1:1 pixel ratios if none supplied */
    if (!scr_width || !scr_height) {
        scr_width = scr_xres;
        scr_height = scr_yres;
    }

    /* trim to keep aspect ratio */
    float x_factor = orig_xres * xpy * scr_height;
    float y_factor = orig_yres *       scr_width;

    /* allow for tolerance so we avoid scaling if framebuffer is standard size */
    if (x_factor < y_factor * (1.f - ASPECT_RATIO_TOLERANCE))
        *adj_xres = (uint32_t) (x_factor * *adj_xres / y_factor + 0.5);
    else if (x_factor * (1.f - ASPECT_RATIO_TOLERANCE) > y_factor)
        *adj_yres = (uint32_t) (y_factor * *adj_yres / x_factor + 0.5);
}

static void set_ext_matrix(omap_hwc_device_t *hwc_dev, struct hwc_rect region)
{
    int orig_w = WIDTH(region);
    int orig_h = HEIGHT(region);
    float xpy = hwc_dev->lcd_xpy;

    /* reorientation matrix is:
       m = (center-from-target-center) * (scale-to-target) * (mirror) * (rotate) * (center-to-original-center) */

    display_t *ext = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (!ext)
        return;

    memcpy(ext->transform.matrix, unit_matrix, sizeof(unit_matrix));
    translate_matrix(ext->transform.matrix, -(orig_w / 2.0f) - region.left, -(orig_h / 2.0f) - region.top);
    rotate_matrix(ext->transform.matrix, ext->transform.rotation);
    if (ext->transform.hflip)
        scale_matrix(ext->transform.matrix, 1, -1, 1, 1);

    if (ext->transform.rotation & 1) {
        SWAP(orig_w, orig_h);
        xpy = 1. / xpy;
    }

    /* get target size */
    uint32_t adj_xres, adj_yres;
    uint32_t width, height;
    int xres, yres;
    if (ext->type == DISP_TYPE_HDMI) {
        hdmi_display_t *hdmi = &((external_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_EXTERNAL])->hdmi;
        width = hdmi->width;
        height = hdmi->height;
        xres = hdmi->mode_db[~hdmi->current_mode].xres;
        yres = hdmi->mode_db[~hdmi->current_mode].yres;
        hdmi->base.transform.scaling = ((xres != orig_w) || (yres != orig_h)) ? 1 : 0;
    } else {
        width = 0;
        height = 0;
        xres = 0;
        yres = 0;
    }

    get_max_dimensions(orig_w, orig_h, xpy,
                       xres, yres, width, height,
                       &adj_xres, &adj_yres);

    scale_matrix(ext->transform.matrix, orig_w, adj_xres, orig_h, adj_yres);
    translate_matrix(ext->transform.matrix, xres >> 1, yres >> 1);
}

static int crop_overlay_to_rect(struct hwc_rect vis_rect, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;

    struct {
        int xy[2];
        int wh[2];
    } crop, win;
    struct {
        int lt[2];
        int rb[2];
    } vis;
    win.xy[0] = oc->win.x; win.xy[1] = oc->win.y;
    win.wh[0] = oc->win.w; win.wh[1] = oc->win.h;
    crop.xy[0] = oc->crop.x; crop.xy[1] = oc->crop.y;
    crop.wh[0] = oc->crop.w; crop.wh[1] = oc->crop.h;
    vis.lt[0] = vis_rect.left; vis.lt[1] = vis_rect.top;
    vis.rb[0] = vis_rect.right; vis.rb[1] = vis_rect.bottom;

    int c;
    bool swap = oc->rotation & 1;

    /* align crop window with display coordinates */
    if (swap)
        crop.xy[1] -= (crop.wh[1] = -crop.wh[1]);
    if (oc->rotation & 2)
        crop.xy[!swap] -= (crop.wh[!swap] = -crop.wh[!swap]);
    if ((!oc->mirror) ^ !(oc->rotation & 2))
        crop.xy[swap] -= (crop.wh[swap] = -crop.wh[swap]);

    for (c = 0; c < 2; c++) {
        /* see if complete buffer is outside the vis or it is
          fully cropped or scaled to 0 */
        if (win.wh[c] <= 0 || vis.rb[c] <= vis.lt[c] ||
            win.xy[c] + win.wh[c] <= vis.lt[c] ||
            win.xy[c] >= vis.rb[c] ||
            !crop.wh[c ^ swap])
            return -ENOENT;

        /* crop left/top */
        if (win.xy[c] < vis.lt[c]) {
            /* correction term */
            int a = (vis.lt[c] - win.xy[c]) * crop.wh[c ^ swap] / win.wh[c];
            crop.xy[c ^ swap] += a;
            crop.wh[c ^ swap] -= a;
            win.wh[c] -= vis.lt[c] - win.xy[c];
            win.xy[c] = vis.lt[c];
        }
        /* crop right/bottom */
        if (win.xy[c] + win.wh[c] > vis.rb[c]) {
            crop.wh[c ^ swap] = crop.wh[c ^ swap] * (vis.rb[c] - win.xy[c]) / win.wh[c];
            win.wh[c] = vis.rb[c] - win.xy[c];
        }

        if (!crop.wh[c ^ swap] || !win.wh[c])
            return -ENOENT;
    }

    /* realign crop window to buffer coordinates */
    if (oc->rotation & 2)
        crop.xy[!swap] -= (crop.wh[!swap] = -crop.wh[!swap]);
    if ((!oc->mirror) ^ !(oc->rotation & 2))
        crop.xy[swap] -= (crop.wh[swap] = -crop.wh[swap]);
    if (swap)
        crop.xy[1] -= (crop.wh[1] = -crop.wh[1]);

    oc->win.x = win.xy[0]; oc->win.y = win.xy[1];
    oc->win.w = win.wh[0]; oc->win.h = win.wh[1];
    oc->crop.x = crop.xy[0]; oc->crop.y = crop.xy[1];
    oc->crop.w = crop.wh[0]; oc->crop.h = crop.wh[1];

    return 0;
}

static void transform_overlay(transform_matrix transform, struct dss2_ovl_info *ovl)
{
    float x, y, w, h;
    struct dss2_ovl_cfg *oc = &ovl->cfg;

    /* display position */
    x = transform[0][0] * oc->win.x + transform[0][1] * oc->win.y + transform[0][2];
    y = transform[1][0] * oc->win.x + transform[1][1] * oc->win.y + transform[1][2];
    w = transform[0][0] * oc->win.w + transform[0][1] * oc->win.h;
    h = transform[1][0] * oc->win.w + transform[1][1] * oc->win.h;
    oc->win.x = round_float(w > 0 ? x : x + w);
    oc->win.y = round_float(h > 0 ? y : y + h);
    /* compensate position rounding error by adjusting layer size */
    w += w > 0 ? x - oc->win.x : oc->win.x - (x + w);
    h += h > 0 ? y - oc->win.y : oc->win.y - (y + h);
    oc->win.w = round_float(w > 0 ? w : -w);
    oc->win.h = round_float(h > 0 ? h : -h);
}

static void adjust_overlay_to_display(omap_hwc_device_t *hwc_dev, int disp, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;
    display_t *display = hwc_dev->displays[disp];
    if (!display)
        return;

    /* crop to clone region if mirroring */
    if (crop_overlay_to_rect(display->transform.region, ovl) != 0) {
        oc->enabled = 0;
        return;
    }

    transform_overlay(display->transform.matrix, ovl);

    /* combining transformations: F^a*R^b*F^i*R^j = F^(a+b)*R^(j+b*(-1)^i), because F*R = R^(-1)*F */
    oc->rotation += (oc->mirror ? -1 : 1) * display->transform.rotation;
    oc->rotation &= 3;
    if (display->transform.hflip)
        oc->mirror = !oc->mirror;
}

bool can_scale(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, bool is_2d,
               struct dsscomp_display_info *dis, struct dsscomp_platform_info *limits,
               uint32_t pclk)
{
    uint32_t fclk = limits->fclk / 1000;
    uint32_t min_src_w = DIV_ROUND_UP(src_w, is_2d ? limits->max_xdecim_2d : limits->max_xdecim_1d);
    uint32_t min_src_h = DIV_ROUND_UP(src_h, is_2d ? limits->max_ydecim_2d : limits->max_ydecim_1d);

    /* ERRATAs */
    /* cannot render 1-width layers on DSI video mode panels - we just disallow all 1-width LCD layers */
    if (dis->channel != OMAP_DSS_CHANNEL_DIGIT && dst_w < limits->min_width)
        return false;

    /* NOTE: no support for checking YUV422 layers that are tricky to scale */

    /* FIXME: limit vertical downscale well below theoretical limit as we saw display artifacts */
    if (dst_h < src_h / 4)
        return false;

    /* max downscale */
    if (dst_h * limits->max_downscale < min_src_h)
        return false;

    /* for manual panels pclk is 0, and there are no pclk based scaling limits */
    if (!pclk)
        return !(dst_w < src_w / limits->max_downscale / (is_2d ? limits->max_xdecim_2d : limits->max_xdecim_1d));

    /* :HACK: limit horizontal downscale well below theoretical limit as we saw display artifacts */
    if (dst_w * 4 < src_w)
        return false;

    /* max horizontal downscale is 4, or the fclk/pixclk */
    if (fclk > pclk * limits->max_downscale)
        fclk = pclk * limits->max_downscale;
    /* for small parts, we need to use integer fclk/pixclk */
    if (src_w < limits->integer_scale_ratio_limit)
        fclk = fclk / pclk * pclk;
    if ((uint32_t) dst_w * fclk < min_src_w * pclk)
        return false;

    return true;
}

static uint32_t add_scaling_score(uint32_t score,
                                  uint32_t xres, uint32_t yres, uint32_t refresh,
                                  uint32_t ext_xres, uint32_t ext_yres,
                                  uint32_t mode_xres, uint32_t mode_yres, uint32_t mode_refresh)
{
    uint32_t area = xres * yres;
    uint32_t ext_area = ext_xres * ext_yres;
    uint32_t mode_area = mode_xres * mode_yres;

    /* prefer to upscale (1% tolerance) [0..1] (insert after 1st bit) */
    int upscale = (ext_xres >= xres * 99 / 100 && ext_yres >= yres * 99 / 100);
    score = (((score & ~1) | upscale) << 1) | (score & 1);

    /* pick minimum scaling [0..16] */
    if (ext_area > area)
        score = (score << 5) | (16 * area / ext_area);
    else
        score = (score << 5) | (16 * ext_area / area);

    /* pick smallest leftover area [0..16] */
    score = (score << 5) | ((16 * ext_area + (mode_area >> 1)) / mode_area);

    /* adjust mode refresh rate */
    mode_refresh += mode_refresh % 6 == 5;

    /* prefer same or higher frame rate */
    upscale = (mode_refresh >= refresh);
    score = (score << 1) | upscale;

    /* pick closest frame rate */
    if (mode_refresh > refresh)
        score = (score << 8) | (240 * refresh / mode_refresh);
    else
        score = (score << 8) | (240 * mode_refresh / refresh);

    return score;
}

int set_best_hdmi_mode(omap_hwc_device_t *hwc_dev, uint32_t xres, uint32_t yres, float xpy)
{
    int dis_ix = is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY) ? 0 : 1;
    struct _qdis {
        struct dsscomp_videomode modedb[MAX_DISPLAY_CONFIGS];
        struct dsscomp_display_info dis; /* variable-sized type; should be at end of struct */
    } d = { .dis = { .ix = dis_ix } };

    hdmi_display_t *hdmi = NULL;
    bool avoid_mode_change = true;

    if (hwc_dev->displays[dis_ix]->role == DISP_ROLE_PRIMARY) {
        hdmi = &((primary_hdmi_display_t*)hwc_dev->displays[dis_ix])->hdmi;
    } else if (hwc_dev->displays[dis_ix]->role == DISP_ROLE_EXTERNAL) {
        hdmi = &((external_hdmi_display_t*)hwc_dev->displays[dis_ix])->hdmi;
        avoid_mode_change = ((external_hdmi_display_t*)hwc_dev->displays[dis_ix])->avoid_mode_change;
    } else
        return -ENODEV;

    d.dis.modedb_len = sizeof(d.modedb) / sizeof(*d.modedb);
    int ret = ioctl(hwc_dev->dsscomp_fd, DSSCIOC_QUERY_DISPLAY, &d);
    if (ret)
        return ret;

    if (d.dis.timings.x_res * d.dis.timings.y_res == 0 ||
        xres * yres == 0)
        return -EINVAL;

    uint32_t i, best = ~0, best_score = 0;
    hdmi->width = d.dis.width_in_mm;
    hdmi->height = d.dis.height_in_mm;

    uint32_t ext_fb_xres, ext_fb_yres;
    for (i = 0; i < d.dis.modedb_len; i++) {
        hdmi->mode_db[i] = d.modedb[i];

        uint32_t score = 0;
        uint32_t mode_xres = d.modedb[i].xres;
        uint32_t mode_yres = d.modedb[i].yres;
        uint32_t ext_width = d.dis.width_in_mm;
        uint32_t ext_height = d.dis.height_in_mm;

        if (d.modedb[i].vmode & FB_VMODE_INTERLACED)
            mode_yres /= 2;

        if (d.modedb[i].flag & FB_FLAG_RATIO_4_3) {
            ext_width = 4;
            ext_height = 3;
        } else if (d.modedb[i].flag & FB_FLAG_RATIO_16_9) {
            ext_width = 16;
            ext_height = 9;
        }

        if (!mode_xres || !mode_yres)
            continue;

        get_max_dimensions(xres, yres, xpy, mode_xres, mode_yres,
                           ext_width, ext_height, &ext_fb_xres, &ext_fb_yres);

        /* we need to ensure that even TILER2D buffers can be scaled */
        if (!d.modedb[i].pixclock ||
            (d.modedb[i].vmode & ~FB_VMODE_INTERLACED) ||
            !can_scale(xres, yres, ext_fb_xres, ext_fb_yres,
                       1, &d.dis, &hwc_dev->platform_limits,
                       1000000000 / d.modedb[i].pixclock))
            continue;

        /* prefer CEA modes */
        if (d.modedb[i].flag & (FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9))
            score = 1;

        /* prefer the same mode as we use for mirroring to avoid mode change */
        score = (score << 1) | (i == ~hdmi->current_mode && avoid_mode_change);

        score = add_scaling_score(score, xres, yres, 60, ext_fb_xres, ext_fb_yres,
                                  mode_xres, mode_yres, d.modedb[i].refresh ? : 1);

        ALOGD("#%d: %dx%d %dHz", i, mode_xres, mode_yres, d.modedb[i].refresh);
        if (debug)
            ALOGD("  score=0x%x adj.res=%dx%d", score, ext_fb_xres, ext_fb_yres);
        if (best_score < score) {
            hdmi->width = ext_width;
            hdmi->height = ext_height;
            best = i;
            best_score = score;
        }
    }
    if (~best) {
        struct dsscomp_setup_display_data sdis = { .ix = dis_ix };
        sdis.mode = d.dis.modedb[best];
        ALOGD("picking #%d", best);
        /* only reconfigure on change */
        if (hdmi->current_mode != ~best)
            ioctl(hwc_dev->dsscomp_fd, DSSCIOC_SETUP_DISPLAY, &sdis);
        hdmi->current_mode = ~best;
    } else {
        uint32_t ext_width = d.dis.width_in_mm;
        uint32_t ext_height = d.dis.height_in_mm;
        uint32_t ext_fb_xres, ext_fb_yres;

        get_max_dimensions(xres, yres, xpy, d.dis.timings.x_res, d.dis.timings.y_res,
                           ext_width, ext_height, &ext_fb_xres, &ext_fb_yres);
        if (!d.dis.timings.pixel_clock ||
            !can_scale(xres, yres, ext_fb_xres, ext_fb_yres,
                       1, &d.dis, &hwc_dev->platform_limits,
                       d.dis.timings.pixel_clock)) {
            ALOGW("DSS scaler cannot support HDMI cloning");
            return -1;
        }
    }
    return 0;
}

static void reserve_overlays_for_displays(omap_hwc_device_t *hwc_dev)
{
    display_t *primary_display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    uint32_t ovl_ix_base = OMAP_DSS_GFX;
    uint32_t max_overlays = MAX_HW_OVERLAYS;
    uint32_t num_nonscaling_overlays = NUM_NONSCALING_OVERLAYS;

    /* If FB is not same resolution as LCD don't use GFX overlay. */
    if (primary_display->transform.scaling) {
        ovl_ix_base = OMAP_DSS_VIDEO1;
        max_overlays -= num_nonscaling_overlays;
        num_nonscaling_overlays = 0;
    }

    /*
     * We cannot atomically switch overlays from one display to another. First, they
     * have to be disabled, and the disabling has to take effect on the current display.
     * We keep track of the available number of overlays here.
     */
    uint32_t max_primary_overlays = max_overlays - hwc_dev->last_ext_ovls;
    uint32_t max_external_overlays = max_overlays - hwc_dev->last_int_ovls;

    composition_t *primary_comp = &primary_display->composition;

    primary_comp->ovl_ix_base = ovl_ix_base;
    primary_comp->wanted_ovls = max_overlays;
    primary_comp->avail_ovls = max_primary_overlays;
    primary_comp->scaling_ovls = primary_comp->avail_ovls - num_nonscaling_overlays;
    primary_comp->used_ovls = 0;

    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) {
        /*
         * For primary display we must reserve at least one overlay for FB, plus an extra
         * overlay for each protected layer.
         */
        layer_statistics_t *primary_layer_stats = &primary_display->layer_stats;
        uint32_t min_primary_overlays = MIN(1 + primary_layer_stats->protected, max_overlays);

        /* Share available overlays between primary and external displays. */
        primary_comp->wanted_ovls = MAX(max_overlays / 2, min_primary_overlays);
        primary_comp->avail_ovls = MIN(max_primary_overlays, primary_comp->wanted_ovls);

        /*
         * We may not have enough overlays on the external display. We "reserve" them here but
         * may not do external composition for the first frame while the overlays required for
         * it are cleared.
         */
        composition_t *ext_comp = &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->composition;

        ext_comp->wanted_ovls = max_overlays - primary_comp->wanted_ovls;
        ext_comp->avail_ovls = MIN(max_external_overlays, ext_comp->wanted_ovls);
        ext_comp->scaling_ovls = ext_comp->avail_ovls;
        ext_comp->used_ovls = 0;
        ext_comp->ovl_ix_base = MAX_HW_OVERLAYS - ext_comp->avail_ovls;

        if (is_external_display_mirroring(hwc_dev)) {
            /*
             * If mirroring, we are limited on primary composition by number of available external
             * overlays. We should be able to clone all primary overlays to external. Still we
             * should not go below min_primary_overlays to sustain the primary composition. This
             * may result in some overlays not being cloned to external display.
             */
            if (ext_comp->avail_ovls && primary_comp->avail_ovls > ext_comp->avail_ovls)
                primary_comp->avail_ovls = MAX(min_primary_overlays, ext_comp->avail_ovls);
        }
    }
}

static bool can_dss_render_all_for_display(omap_hwc_device_t *hwc_dev, int disp)
{
    layer_statistics_t *layer_stats = &hwc_dev->displays[disp]->layer_stats;
    bool cloning = is_external_display_mirroring(hwc_dev);
    bool on_tv = is_hdmi_display(hwc_dev, disp);
    if (!on_tv && cloning) {
        int clone = (disp == HWC_DISPLAY_PRIMARY) ? HWC_DISPLAY_EXTERNAL : HWC_DISPLAY_PRIMARY;
        on_tv = is_hdmi_display(hwc_dev, clone);
    }
    bool tform = cloning && (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.rotation ||
                            hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.hflip);

    composition_t *comp = &hwc_dev->displays[disp]->composition;

    return  !hwc_dev->force_sgx &&
            /* must have at least one layer if using composition bypass to get sync object */
            layer_stats->composable &&
            layer_stats->composable <= comp->avail_ovls &&
            layer_stats->composable == layer_stats->count &&
            layer_stats->scaled <= comp->scaling_ovls &&
            layer_stats->nv12 <= comp->scaling_ovls &&
            /* fits into TILER slot */
            layer_stats->mem1d_total <= hwc_dev->platform_limits.tiler1d_slot_size &&
            /* we cannot clone non-NV12 transformed layers */
            (!tform || (layer_stats->nv12 == layer_stats->composable)) &&
            /* HDMI cannot display BGR */
            (layer_stats->bgr == 0 || (layer_stats->rgb == 0 && !on_tv) || !hwc_dev->flags_rgb_order) &&
            /* If nv12_only flag is set DSS should only render NV12 */
            (!hwc_dev->flags_nv12_only || (layer_stats->bgr == 0 && layer_stats->rgb == 0));
}

static inline bool can_dss_render_layer_for_display(omap_hwc_device_t *hwc_dev, int disp, hwc_layer_1_t *layer)
{
    bool cloning = is_external_display_mirroring(hwc_dev);
    bool on_tv = is_hdmi_display(hwc_dev, disp);
    if (!on_tv && cloning) {
        int clone = (disp == HWC_DISPLAY_PRIMARY) ? HWC_DISPLAY_EXTERNAL : HWC_DISPLAY_PRIMARY;
        on_tv = is_hdmi_display(hwc_dev, clone);
    }
    bool tform = cloning && (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.rotation ||
                            hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.hflip);
    composition_t *comp = &hwc_dev->displays[disp]->composition;

    return is_valid_layer(hwc_dev, layer) &&
           /* cannot rotate non-NV12 layers on external display */
           (!tform || is_nv12_layer(layer)) &&
           /* skip non-NV12 layers if also using SGX (if nv12_only flag is set) */
           (!hwc_dev->flags_nv12_only || (!comp->use_sgx || is_nv12_layer(layer))) &&
           /* make sure RGB ordering is consistent (if rgb_order flag is set) */
           (!(comp->swap_rb ? is_rgb_layer(layer) : is_bgr_layer(layer)) ||
            !hwc_dev->flags_rgb_order) &&
           /* TV can only render RGB */
           !(on_tv && is_bgr_layer(layer));
}

static inline int get_overlay_display_area(struct dss2_ovl_info const *ovl)
{
    return ovl->cfg.win.w * ovl->cfg.win.h;
}

static int clone_overlay(omap_hwc_device_t *hwc_dev, int ix)
{
    composition_t *primary_comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &primary_comp->comp_data.dsscomp_data;
    int ext_ovl_ix = dsscomp->num_ovls - primary_comp->used_ovls;
    struct dss2_ovl_info *o = &dsscomp->ovls[dsscomp->num_ovls];

    if (dsscomp->num_ovls >= MAX_HW_OVERLAYS) {
        ALOGE("**** cannot clone overlay #%d. using all %d overlays.", ix, dsscomp->num_ovls);
        return -EBUSY;
    }

    memcpy(o, dsscomp->ovls + ix, sizeof(*o));

    /* reserve overlays at end for other display */
    o->cfg.ix = MAX_HW_OVERLAYS - 1 - ext_ovl_ix;
    o->cfg.mgr_ix = 1;
    /*
     * Here the assumption is that overlay0 is the one attached to FB.
     * Hence this clone_overlay call is for FB cloning (provided use_sgx is true).
     */
    /* For the external displays whose transform is the same as
     * that of primary display, ion_handles would be NULL hence
     * the below logic doesn't execute.
     */
    struct ion_handle *ion_handle = get_external_display_ion_fb_handle(hwc_dev);
    if (ix == 0 && ion_handle && primary_comp->use_sgx) {
        o->addressing = OMAP_DSS_BUFADDR_ION;
        o->ba = (int)ion_handle;
    } else {
        o->addressing = OMAP_DSS_BUFADDR_OVL_IX;
        o->ba = ix;
    }

    /* use distinct z values (to simplify z-order checking) */
    o->cfg.zorder += primary_comp->used_ovls;

    adjust_overlay_to_display(hwc_dev, HWC_DISPLAY_EXTERNAL, o);
    dsscomp->num_ovls++;
    return 0;
}

static int setup_mirroring(omap_hwc_device_t *hwc_dev)
{
    display_t *ext = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (!ext)
        return -ENODEV;

    uint32_t xres = WIDTH(ext->transform.region);
    uint32_t yres = HEIGHT(ext->transform.region);

    if (ext->transform.rotation & 1)
        SWAP(xres, yres);

    if (set_best_hdmi_mode(hwc_dev, xres, yres, hwc_dev->lcd_xpy))
        return -ENODEV;

    set_ext_matrix(hwc_dev, ext->transform.region);
    return 0;
}

static void setup_framebuffer(omap_hwc_device_t *hwc_dev, int disp, int ovl_ix, int zorder)
{
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    struct dss2_ovl_info *fb_ovl = &dsscomp->ovls[0];
    uint32_t i;

    setup_overlay(zorder,
                  hwc_dev->fb_dev[disp]->base.format,
                  true,   /* FB is always premultiplied */
                  hwc_dev->fb_dev[disp]->base.width,
                  hwc_dev->fb_dev[disp]->base.height,
                  fb_ovl);

    fb_ovl->cfg.mgr_ix = disp;
    fb_ovl->cfg.ix = ovl_ix;
    fb_ovl->cfg.pre_mult_alpha = 1;
    fb_ovl->addressing = OMAP_DSS_BUFADDR_LAYER_IX;

    if (comp->use_sgx) {
        /* Add an empty buffer list entry for SGX FB */
        fb_ovl->ba = comp->num_buffers;
        comp->buffers[comp->num_buffers] = NULL;
        comp->num_buffers++;
    } else {
        /*
         * Blitter FB will be inserted in OMAPLFB at position 0. All buffer references in
         * dss2_ovl_info have to be updated to accommodate for that.
         */
        fb_ovl->ba = 0;
        for (i = 1; i < dsscomp->num_ovls; i++)
            dsscomp->ovls[i].ba += 1;
    }
}

/*
 * We're using "implicit" synchronization, so make sure we aren't passing any
 * sync object descriptors around.
 */
static void check_sync_fds_for_display(int disp, hwc_display_contents_1_t *list)
{
    //ALOGD("checking sync FDs");
    if (disp < 0 || disp >= MAX_DISPLAYS || !list)
        return;

    if (list->retireFenceFd >= 0) {
        ALOGW("retireFenceFd[%u] was %d", disp, list->retireFenceFd);
        list->retireFenceFd = -1;
    }

    unsigned int j;
    for (j = 0; j < list->numHwLayers; j++) {
        hwc_layer_1_t* layer = &list->hwLayers[j];
        if (layer->acquireFenceFd >= 0) {
            ALOGW("acquireFenceFd[%u][%u] was %d, closing", disp, j, layer->acquireFenceFd);
            close(layer->acquireFenceFd);
            layer->acquireFenceFd = -1;
        }
        if (layer->releaseFenceFd >= 0) {
            ALOGW("releaseFenceFd[%u][%u] was %d", disp, j, layer->releaseFenceFd);
            layer->releaseFenceFd = -1;
        }
    }
}

void debug_post2(omap_hwc_device_t *hwc_dev, int nbufs, int disp)
{
    if (!debugpost2)
        return;

    struct dsscomp_setup_dispc_data *dsscomp = &hwc_dev->displays[disp]->composition.comp_data.dsscomp_data;
    int i;
    for (i=0; i<nbufs; i++) {
        ALOGI("buf[%d] hndl %p", i, hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition.buffers[i]);
    }
    for (i=0; i < dsscomp->num_ovls; i++) {
        ALOGI("ovl[%d] ba %d", i, dsscomp->ovls[i].ba);
    }
}

static int hwc_prepare_for_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp >= MAX_DISPLAYS || !hwc_dev->displays[disp])
        return -ENODEV;

    hwc_display_contents_1_t *list = hwc_dev->displays[disp]->contents;
    if (!list)
        return 0;

    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    layer_statistics_t *layer_stats = &hwc_dev->displays[disp]->layer_stats;
    blitter_config_t *blitter = &hwc_dev->blitter;

    if (get_display_mode(hwc_dev, disp) == DISP_MODE_LEGACY) {
        /* mirror the layers from primary display composition */
        composition_t *primary_comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
        struct dsscomp_setup_dispc_data *dsscomp_primary = &primary_comp->comp_data.dsscomp_data;
        uint32_t i, ix;

        /* mirror all layers */
        for (ix = 0; ix < primary_comp->used_ovls; ix++) {
            if (clone_overlay(hwc_dev, ix))
                break;
        }

        for (i = 0; list && i < list->numHwLayers; i++) {
            hwc_layer_1_t *layer = &list->hwLayers[i];
            if (layer->compositionType == HWC_FRAMEBUFFER_TARGET)
                continue;

            layer->compositionType = HWC_OVERLAY;
        }

        dsscomp_primary->mgrs[1] = dsscomp_primary->mgrs[0];
        dsscomp_primary->mgrs[1].ix = 1;
        dsscomp_primary->num_mgrs++;
        hwc_dev->last_ext_ovls = primary_comp->used_ovls;
        return 0;
    }

    memset(dsscomp, 0x0, sizeof(*dsscomp));
    dsscomp->sync_id = sync_id++;

    /*
     * The following priorities are used for different compositing HW:
     * 1 - BLITTER (policy = ALL)
     * 2 - DSSCOMP
     * 3 - BLITTER (policy = DEFAULT)
     * 4 - SGX
     */

    /* Check if we can blit everything */
    bool blit_all = (blitter->policy == BLT_POLICY_ALL) && (disp == HWC_DISPLAY_PRIMARY) &&
            blit_layers(hwc_dev, list, 0);

    if (blit_all) {
        comp->use_sgx = false;
        comp->swap_rb = false;
    } else  {
        if (can_dss_render_all_for_display(hwc_dev, disp)) {
            /* All layers can be handled by the DSS -- don't use SGX for composition */
            comp->use_sgx = false;
            comp->swap_rb = layer_stats->bgr != 0;
        } else {
            /* Use SGX for composition plus first 3 layers that are DSS renderable */
            comp->use_sgx = true;
            comp->swap_rb = is_bgr_format(hwc_dev->fb_dev[disp]->base.format);
        }
    }

    if (is_hdmi_display(hwc_dev, disp))
        comp->swap_rb = 0; /* hdmi manager doesn't support R&B swap */

    /* setup DSS overlays */
    int z = 0;
    int fb_z = -1;
    bool scaled_gfx = false;
    uint32_t ovl_ix = comp->ovl_ix_base;
    uint32_t mem_used = 0;
    uint32_t i;

    dsscomp->num_ovls = 0;
    comp->num_buffers = 0;

    /*
     * If the SGX is used or we are going to blit something we need a framebuffer and an overlay
     * for it. Reserve GFX for FB and begin using VID1 for DSS overlay layers.
     */
    bool needs_fb = comp->use_sgx || blit_all;
    if (needs_fb) {
        dsscomp->num_ovls++;
        ovl_ix++;
    }

    uint32_t tiler1d_slot_size = hwc_dev->platform_limits.tiler1d_slot_size;
    if ((hwc_dev->last_ext_ovls || hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) &&
        (get_display_mode(hwc_dev, HWC_DISPLAY_EXTERNAL) != DISP_MODE_LEGACY)) {
        tiler1d_slot_size = tiler1d_slot_size >> 1;
    }

    for (i = 0; list && i < list->numHwLayers && !blit_all; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];

        if (dsscomp->num_ovls < comp->avail_ovls &&
            can_dss_render_layer_for_display(hwc_dev, disp, layer) &&
            (!hwc_dev->force_sgx ||
            /* render protected layers via DSS */
            is_protected_layer(layer) ||
            is_upscaled_nv12_layer(hwc_dev, layer)) &&
            mem_used + get_required_mem1d_size(layer) <= tiler1d_slot_size &&
            /* can't have a transparent overlay in the middle of the framebuffer stack */
            !(is_blended_layer(layer) && fb_z >= 0)) {

            /* render via DSS overlay */
            mem_used += get_required_mem1d_size(layer);
            layer->compositionType = HWC_OVERLAY;
            /*
             * This hint will not be used in vanilla ICS, but maybe in
             * JellyBean, it is useful to distinguish between blts and true
             * overlays
             */
            layer->hints |= HWC_HINT_TRIPLE_BUFFER;

            /* clear FB above all opaque layers if rendering via SGX */
            if (comp->use_sgx && !is_blended_layer(layer))
                layer->hints |= HWC_HINT_CLEAR_FB;

            comp->buffers[comp->num_buffers] = layer->handle;
            //ALOGI("dss buffers[%d] = %p", comp->num_buffers, comp->buffers[comp->num_buffers]);

            adjust_overlay_to_layer(hwc_dev, &dsscomp->ovls[dsscomp->num_ovls], layer, z);

            dsscomp->ovls[dsscomp->num_ovls].cfg.ix = ovl_ix;
            dsscomp->ovls[dsscomp->num_ovls].cfg.mgr_ix = disp;
            dsscomp->ovls[dsscomp->num_ovls].addressing = OMAP_DSS_BUFADDR_LAYER_IX;
            dsscomp->ovls[dsscomp->num_ovls].ba = comp->num_buffers;

            if (disp == HWC_DISPLAY_PRIMARY) {
                /* ensure GFX layer is never scaled */
                if ((dsscomp->num_ovls == 0) && (!hwc_dev->displays[HWC_DISPLAY_PRIMARY]->transform.scaling)) {
                    scaled_gfx = is_scaled_layer(layer) || is_nv12_layer(layer);
                } else if (scaled_gfx && !is_scaled_layer(layer) && !is_nv12_layer(layer)) {
                    /* swap GFX layer with this one */
                    uint32_t temp = dsscomp->ovls[dsscomp->num_ovls].cfg.ix;
                    dsscomp->ovls[dsscomp->num_ovls].cfg.ix = dsscomp->ovls[0].cfg.ix;
                    dsscomp->ovls[0].cfg.ix = temp;
                    scaled_gfx = 0;
                }
            }

            dsscomp->num_ovls++;
            comp->num_buffers++;
            ovl_ix++;
            z++;
        } else if (comp->use_sgx) {
            if (fb_z < 0) {
                /* NOTE: we are not handling transparent cutout for now */
                fb_z = z;
                z++;
            } else {
                /* move fb z-order up (by lowering dss layers) */
                while (fb_z < z - 1)
                    dsscomp->ovls[1 + fb_z++].cfg.zorder--;
            }
        }
    }

    /* if scaling GFX (e.g. only 1 scaled surface) use a VID pipe */
    if (scaled_gfx && disp == HWC_DISPLAY_PRIMARY)
        dsscomp->ovls[0].cfg.ix = ovl_ix;

    if ((blitter->policy == BLT_POLICY_DEFAULT) && (disp == HWC_DISPLAY_PRIMARY)) {
        /*
         * As long as we keep blitting on consecutive frames keep the regionizer
         * state, if this is not possible the regionizer state is unreliable and
         * we need to reset its state.
         */
        if (comp->use_sgx) {
            if (blit_layers(hwc_dev, list, comp->num_buffers)) {
                comp->use_sgx = 0;
            }
        } else
            release_blitter();
    }

    /* If the SGX is not used and there is blit data we need a framebuffer and
     * a DSS pipe well configured for it
     */
    if (needs_fb) {
        /* assign a z-layer for fb */
        if (fb_z < 0) {
            if ((!blitter->policy) != BLT_POLICY_DISABLED && layer_stats->count)
                ALOGE("**** should have assigned z-layer for fb");
            fb_z = z++;
        }

        setup_framebuffer(hwc_dev, disp, comp->ovl_ix_base, fb_z);
    }

    comp->used_ovls = dsscomp->num_ovls;
    if (disp == HWC_DISPLAY_PRIMARY)
        hwc_dev->last_int_ovls = comp->used_ovls;
    else if (disp == HWC_DISPLAY_EXTERNAL)
        hwc_dev->last_ext_ovls = comp->used_ovls;

    /* Apply transform for display */
    if (hwc_dev->displays[disp]->transform.scaling)
        for (i = 0; i < dsscomp->num_ovls; i++) {
            adjust_overlay_to_display(hwc_dev, disp, &dsscomp->ovls[i]);
        }

    if (z != dsscomp->num_ovls || dsscomp->num_ovls > MAX_HW_OVERLAYS)
        ALOGE("**** used %d z-layers for %d overlays\n", z, dsscomp->num_ovls);

    /* verify all z-orders and overlay indices are distinct */
    uint32_t ix;
    for (i = z = ix = 0; i < dsscomp->num_ovls; i++) {
        struct dss2_ovl_cfg *c = &dsscomp->ovls[i].cfg;

        if (z & (1 << c->zorder))
            ALOGE("**** used z-order #%d multiple times", c->zorder);
        if (ix & (1 << c->ix))
            ALOGE("**** used ovl index #%d multiple times", c->ix);

        z |= 1 << c->zorder;
        ix |= 1 << c->ix;
    }
    dsscomp->mode = DSSCOMP_SETUP_DISPLAY;
    dsscomp->mgrs[0].ix = disp;
    dsscomp->mgrs[0].alpha_blending = 1;
    dsscomp->mgrs[0].swap_rb = comp->swap_rb;
    dsscomp->num_mgrs = 1;

    if (hwc_dev->last_ext_ovls && !hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) {
        dsscomp->mgrs[1] = dsscomp->mgrs[0];
        dsscomp->mgrs[1].ix = 1;
        dsscomp->mgrs[1].swap_rb = 0;
        dsscomp->num_mgrs++;
        hwc_dev->last_ext_ovls = 0;
    }

    /*
     * Whilst the mode of the display is being changed drop compositions to the
     * display
     */
    if (is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY)) {
        hdmi_display_t *hdmi = &((primary_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_PRIMARY])->hdmi;
        if (hdmi->current_mode == 0)
            dsscomp->num_ovls = 0;
        hdmi->last_mode = hdmi->current_mode;
    }

    if (is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL)) {
        hdmi_display_t *hdmi = &((external_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_EXTERNAL])->hdmi;
        //mode tracking is required to switch modes during mirror and distinct mode switch
        hdmi->last_mode = hdmi->current_mode;
    }

    if (debug) {
        ALOGD("prepare (%d) - %s (layers=%d, comp=%d/%d scaled, RGB=%d,BGR=%d,NV12=%d) (ext=%s%s%ddeg%s %dex/%dmx (last %dex,%din)\n",
        dsscomp->sync_id,
        comp->use_sgx ? "SGX+OVL" : "all-OVL",
        layer_stats->count,
        layer_stats->composable, layer_stats->scaled,
        layer_stats->rgb, layer_stats->bgr, layer_stats->nv12,
        is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL) ? "tv+" : "",
        is_external_display_mirroring(hwc_dev)? "mirror+" : "OFF+",
        hwc_dev->displays[HWC_DISPLAY_EXTERNAL] ? hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.rotation * 90 : 0,
        hwc_dev->displays[HWC_DISPLAY_EXTERNAL] ? hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->transform.hflip ? "+hflip" : "" : "",
        comp->avail_ovls, comp->avail_ovls, hwc_dev->last_ext_ovls, hwc_dev->last_int_ovls);
    }
    return 0;
}

static int hwc_prepare(struct hwc_composer_device_1 *dev, size_t numDisplays,
        hwc_display_contents_1_t** displays)
{
    if (!numDisplays || displays == NULL) {
        return 0;
    }

    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *)dev;
    pthread_mutex_lock(&hwc_dev->lock);

    set_display_contents(hwc_dev, numDisplays, displays);

    external_display_t *ext = NULL;
    if (is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL))
        ext = &((external_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_EXTERNAL])->ext;

    if (ext) {
        if (get_display_mode(hwc_dev, HWC_DISPLAY_EXTERNAL) == DISP_MODE_LEGACY)
            ext->is_mirroring = true;
        else
            ext->is_mirroring = false;
    }

    reserve_overlays_for_displays(hwc_dev);
    reset_blitter(hwc_dev);

    uint32_t i;
    for (i = 0; i < numDisplays; i++) {
        hwc_prepare_for_display(hwc_dev, i);
    }

    pthread_mutex_unlock(&hwc_dev->lock);
    return 0;
}

static int hwc_set_for_display(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *list)
{
    hwc_display_t dpy = NULL;
    hwc_surface_t sur = NULL;
    if (list != NULL) {
        dpy = list->dpy;
        sur = list->sur;
    }

    layer_statistics_t *layer_stats = &hwc_dev->displays[disp]->layer_stats;
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    blitter_config_t *blitter = &hwc_dev->blitter;

    int err = 0;
    bool invalidate = false;

    pthread_mutex_lock(&hwc_dev->lock);

    static bool first_set = true;
    if (first_set) {
        reset_primary_display(hwc_dev);
        first_set = false;
    }

    if (disp == HWC_DISPLAY_EXTERNAL) {
        invalidate = comp->wanted_ovls && (comp->avail_ovls < comp->wanted_ovls) &&
                    (layer_stats->protected || !comp->avail_ovls);
    }

    if (debug)
        dump_set_info(hwc_dev, list, disp);

    if (dpy && sur) {
        // list can be NULL which means hwc is temporarily disabled.
        // however, if dpy and sur are null it means we're turning the
        // screen off. no shall not call eglSwapBuffers() in that case.

        if (comp->use_sgx) {
            if (hwc_dev->base.common.version <= HWC_DEVICE_API_VERSION_1_0) {
                if (!eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)sur)) {
                    ALOGE("eglSwapBuffers error");
                    err = HWC_EGL_ERROR;
                    goto err_out;
                }
            } else {
                if (list) {
                    if (layer_stats->framebuffer) {
                        /* Layer with HWC_FRAMEBUFFER_TARGET should be last in the list. The buffer handle
                         * is updated by SurfaceFlinger after prepare() call, so FB slot has to be updated
                         * in set().
                         */
                        uint32_t list_fb_ix = list->numHwLayers - 1;
                        uint32_t comp_fb_ix = dsscomp->ovls[0].ba;
                        comp->buffers[comp_fb_ix] = list->hwLayers[list_fb_ix].handle;
                    } else {
                        ALOGE("No buffer is provided for GL composition");
                        err = -EFAULT;
                        goto err_out;
                    }
                }
            }
        }

#ifdef DUMP_DSSCOMPS
        dump_dsscomp(dsscomp);
#endif

        // signal the event thread that a post has happened
        write(hwc_dev->pipe_fds[1], "s", 1);
        if (hwc_dev->force_sgx > 0)
            hwc_dev->force_sgx--;

        comp->comp_data.blit_data.rgz_flags = comp->blitter.flags;
        comp->comp_data.blit_data.rgz_items = comp->blitter.num_blits;
        int omaplfb_comp_data_sz = sizeof(comp->comp_data) +
            (comp->comp_data.blit_data.rgz_items * sizeof(struct rgz_blt_entry));

        uint32_t num_buffers = comp->num_buffers + comp->blitter.num_buffers;

        ALOGI_IF(blitter->debug, "Post2, blits %d, ovl_buffers %d, blit_buffers %d sgx %d",
            comp->blitter.num_blits, comp->num_buffers, comp->blitter.num_buffers, comp->use_sgx);

        debug_post2(hwc_dev, num_buffers, disp);
        err = hwc_dev->fb_dev[disp]->Post2((framebuffer_device_t *)hwc_dev->fb_dev[disp],
                             comp->buffers, num_buffers,
                             dsscomp, omaplfb_comp_data_sz);

        showfps();
    }

    if (err)
        ALOGE("Post2 error[%d]", disp);

        check_sync_fds_for_display(disp, list);

err_out:
    pthread_mutex_unlock(&hwc_dev->lock);

    if (invalidate)
        hwc_dev->procs->invalidate(hwc_dev->procs);

    return err;
}


static int hwc_set(struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || displays == NULL) {
        ALOGD("set: empty display list");
        return 0;
    }
    uint32_t i;
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t*)dev;
    int err = -1;
    for (i = 0; i < numDisplays && hwc_dev->displays[i] && displays[i]; i++) {
        if (get_display_mode(hwc_dev,i) != DISP_MODE_LEGACY)
            err = hwc_set_for_display(hwc_dev, i, displays[i]);
    }

    return err;
}

static void hwc_dump(struct hwc_composer_device_1 *dev, char *buff, int buff_len)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *)dev;
    composition_t *primary_comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &primary_comp->comp_data.dsscomp_data;
    struct dump_buf log = {
        .buf = buff,
        .buf_len = buff_len,
    };
    int i;

    dump_printf(&log, "omap_hwc %d:\n", dsscomp->num_ovls);
    dump_printf(&log, "  idle timeout: %dms\n", hwc_dev->idle);

    for (i = 0; i < dsscomp->num_ovls; i++) {
        struct dss2_ovl_cfg *cfg = &dsscomp->ovls[i].cfg;

        dump_printf(&log, "  layer %d:\n", i);
        dump_printf(&log, "     enabled:%s buff:%p %dx%d stride:%d\n",
                          cfg->enabled ? "true" : "false", primary_comp->buffers[i],
                          cfg->width, cfg->height, cfg->stride);
        dump_printf(&log, "     src:(%d,%d) %dx%d dst:(%d,%d) %dx%d ix:%d zorder:%d\n",
                          cfg->crop.x, cfg->crop.y, cfg->crop.w, cfg->crop.h,
                          cfg->win.x, cfg->win.y, cfg->win.w, cfg->win.h,
                          cfg->ix, cfg->zorder);
    }

    blitter_config_t *blitter = &hwc_dev->blitter;

    if (blitter->policy != BLT_POLICY_DISABLED) {
        dump_printf(&log, "  bltpolicy: %s, bltmode: %s\n",
            blitter->policy == BLT_POLICY_DEFAULT ? "default" :
                blitter->policy == BLT_POLICY_ALL ? "all" : "unknown",
                    blitter->mode == BLT_MODE_PAINT ? "paint" : "regionize");
    }
    dump_printf(&log, "\n");
}

static int hwc_device_close(hw_device_t* device)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) device;;

    if (hwc_dev) {
        if (hwc_dev->dsscomp_fd >= 0)
            close(hwc_dev->dsscomp_fd);
        if (hwc_dev->hdmi_fb_fd >= 0)
            close(hwc_dev->hdmi_fb_fd);
        if (hwc_dev->fb_fd >= 0)
            close(hwc_dev->fb_fd);

        /* pthread will get killed when parent process exits */
        pthread_mutex_destroy(&hwc_dev->lock);
        free_displays(hwc_dev);
        free(hwc_dev);
    }

    return 0;
}

static int open_fb_hal(IMG_framebuffer_device_public_t *fb_dev[MAX_DISPLAYS])
{
    const struct hw_module_t *psModule;
    IMG_gralloc_module_public_t *psGrallocModule;
    int err, i;

    err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &psModule);
    psGrallocModule = (IMG_gralloc_module_public_t *) psModule;

    if(err)
        goto err_out;

    if (strcmp(psGrallocModule->base.common.author, "Imagination Technologies")) {
        err = -EINVAL;
        goto err_out;
    }

    for (i = 0; i < MAX_DISPLAYS; i++)
        fb_dev[i] = NULL;

    fb_dev[0] = psGrallocModule->psFbDevice[0];
    fb_dev[1] = psGrallocModule->psFbDevice[1];

    return 0;

err_out:
    ALOGE("Composer HAL failed to load compatible Graphics HAL");
    return err;
}

static void handle_hotplug(omap_hwc_device_t *hwc_dev)
{
    bool state = hwc_dev->ext_disp_state;

    /* Ignore external HDMI logic if the primary display is HDMI */
    if (is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY)) {
        ALOGI("Primary display is HDMI - skip clone logic");

        if (state) {
            uint32_t xres = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.width;
            uint32_t yres = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.height;
            if (set_best_hdmi_mode(hwc_dev, xres, yres, hwc_dev->lcd_xpy)) {
                ALOGE("Failed to set HDMI mode");
            }
        } else {
            hdmi_display_t *hdmi = &((primary_hdmi_display_t*) hwc_dev->displays[HWC_DISPLAY_PRIMARY])->hdmi;
            if (hdmi)
                hdmi->current_mode = 0;
        }
        return;
    }

    pthread_mutex_lock(&hwc_dev->lock);
    if (state) {
        int err = -1;
        err = add_external_hdmi_display(hwc_dev);
        if (err) {
            pthread_mutex_unlock(&hwc_dev->lock);
            return;
        }

        external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*) hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
        /* check whether we can clone */
        char value[PROPERTY_VALUE_MAX];
        property_get("persist.hwc.mirroring.enabled", value, "1");
        ext_hdmi->ext.is_mirroring = atoi(value) > 0;
        property_get("persist.hwc.avoid_mode_change", value, "1");
        ext_hdmi->avoid_mode_change = atoi(value) > 0;

        /* get cloning transformation */
        property_get("persist.hwc.mirroring.transform", value, hwc_dev->fb_dis.timings.y_res > hwc_dev->fb_dis.timings.x_res ? "3" : "0");
        ext_hdmi->hdmi.base.transform.rotation = atoi(value) & EXT_ROTATION;
        ext_hdmi->hdmi.base.transform.hflip = (atoi(value) & EXT_HFLIP) > 0;

        /* get the board specific clone properties */
        /* 0:0:1280:720 */
        if (property_get("persist.hwc.mirroring.region", value, "") <= 0 ||
            sscanf(value, "%d:%d:%d:%d",
               &ext_hdmi->hdmi.base.transform.region.left, &ext_hdmi->hdmi.base.transform.region.top,
               &ext_hdmi->hdmi.base.transform.region.right, &ext_hdmi->hdmi.base.transform.region.bottom) != 4 ||
               ext_hdmi->hdmi.base.transform.region.left >= ext_hdmi->hdmi.base.transform.region.right ||
               ext_hdmi->hdmi.base.transform.region.top >= ext_hdmi->hdmi.base.transform.region.bottom) {
            struct hwc_rect fb_region = { .right = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.width,
                                          .bottom = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.height };
            ext_hdmi->hdmi.base.transform.region = fb_region;
        }
        ALOGI("clone region is set to (%d,%d) to (%d,%d)",
                ext_hdmi->hdmi.base.transform.region.left, ext_hdmi->hdmi.base.transform.region.top,
                ext_hdmi->hdmi.base.transform.region.right, ext_hdmi->hdmi.base.transform.region.bottom);

        /* select best mode for mirroring */
        if (ext_hdmi->ext.is_mirroring) {
            if (setup_mirroring(hwc_dev) == 0) {
                unblank_display(hwc_dev, HWC_DISPLAY_EXTERNAL);
            } else
            ext_hdmi->ext.is_mirroring = 0;
        }
    } else
        remove_external_hdmi_display(hwc_dev);

    display_t *ext = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    ALOGI("external display changed (state=%d, mirror={%s tform=%ddeg%s}, tv=%d", state,
         is_external_display_mirroring(hwc_dev) ? "mirror enabled" : "mirror disabled",
         ext ? ext->transform.rotation * 90 : -1,
         ext ? ext->transform.hflip ? "+hflip" : "" : "",
         is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL));

    pthread_mutex_unlock(&hwc_dev->lock);

    /* hwc_dev->procs is set right after the device is opened, but there is
     * still a race condition where a hotplug event might occur after the open
     * but before the procs are registered. */
    if (hwc_dev->procs) {
        if (hwc_dev->procs->hotplug) {
            hwc_dev->procs->hotplug(hwc_dev->procs, HWC_DISPLAY_EXTERNAL, state);
        } else {
            if (hwc_dev->procs->invalidate)
                hwc_dev->procs->invalidate(hwc_dev->procs);
        }
    }
}

static void handle_uevents(omap_hwc_device_t *hwc_dev, const char *buff, int len)
{
    int hdmi;
    int vsync;
    int state = 0;
    uint64_t timestamp = 0;
    const char *s = buff;

    hdmi = !strcmp(s, "change@/devices/virtual/switch/hdmi");
    vsync = !strcmp(s, "change@/devices/platform/omapfb") ||
        !strcmp(s, "change@/devices/virtual/switch/omapfb-vsync");

    if (!vsync && !hdmi)
       return;

    s += strlen(s) + 1;

    while(*s) {
        if (!strncmp(s, "SWITCH_STATE=", strlen("SWITCH_STATE=")))
            state = atoi(s + strlen("SWITCH_STATE="));
        else if (!strncmp(s, "SWITCH_TIME=", strlen("SWITCH_TIME=")))
            timestamp = strtoull(s + strlen("SWITCH_TIME="), NULL, 0);
        else if (!strncmp(s, "VSYNC=", strlen("VSYNC=")))
            timestamp = strtoull(s + strlen("VSYNC="), NULL, 0);

        s += strlen(s) + 1;
        if (s - buff >= len)
            break;
    }

    if (vsync) {
        if (hwc_dev->procs)
            hwc_dev->procs->vsync(hwc_dev->procs, 0, timestamp);
    } else {
        hwc_dev->ext_disp_state = state == 1;
        handle_hotplug(hwc_dev);
    }
}

static void *hdmi_thread(void *data)
{
    omap_hwc_device_t *hwc_dev = data;
    static char uevent_desc[4096];
    struct pollfd fds[2];
    bool invalidate = false;
    int timeout;
    int err;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    uevent_init();

    fds[0].fd = uevent_get_fd();
    fds[0].events = POLLIN;
    fds[1].fd = hwc_dev->pipe_fds[0];
    fds[1].events = POLLIN;

    timeout = hwc_dev->idle ? hwc_dev->idle : -1;

    memset(uevent_desc, 0, sizeof(uevent_desc));

    do {
        err = poll(fds, hwc_dev->idle ? 2 : 1, timeout);

        if (err == 0) {
            if (hwc_dev->idle) {
                if (hwc_dev->procs) {
                    pthread_mutex_lock(&hwc_dev->lock);
                    invalidate = hwc_dev->last_int_ovls > 1 && !hwc_dev->force_sgx;
                    if (invalidate) {
                        hwc_dev->force_sgx = 2;
                    }
                    pthread_mutex_unlock(&hwc_dev->lock);

                    if (invalidate) {
                        hwc_dev->procs->invalidate(hwc_dev->procs);
                        timeout = -1;
                    }
                }

                continue;
            }
        }

        if (err == -1) {
            if (errno != EINTR)
                ALOGE("event error: %m");
            continue;
        }

        if (hwc_dev->idle && fds[1].revents & POLLIN) {
            char c;
            read(hwc_dev->pipe_fds[0], &c, 1);
            if (!hwc_dev->force_sgx)
                timeout = hwc_dev->idle ? hwc_dev->idle : -1;
        }

        if (fds[0].revents & POLLIN) {
            /* keep last 2 zeroes to ensure double 0 termination */
            int len = uevent_next_event(uevent_desc, sizeof(uevent_desc) - 2);
            handle_uevents(hwc_dev, uevent_desc, len);
        }
    } while (1);

    return NULL;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                                    hwc_procs_t const* procs)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    hwc_dev->procs = (typeof(hwc_dev->procs)) procs;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we don't support the background layer yet
        value[0] = 0;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = 1000000000.0 / hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.fps;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev,
        int dpy __unused, int event, int enabled)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
    {
        int val = !!enabled;
        int err;
        primary_display_t *primary = NULL;
        if (hwc_dev->displays[HWC_DISPLAY_PRIMARY]->type == DISP_TYPE_LCD)
            primary = &((primary_lcd_display_t*)hwc_dev->displays[HWC_DISPLAY_PRIMARY])->primary;
        else if (hwc_dev->displays[HWC_DISPLAY_PRIMARY]->type == DISP_TYPE_HDMI)
            primary = &((primary_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_PRIMARY])->primary;
        else
            return -ENODEV;

        if (primary->use_sw_vsync) {
            if (enabled)
                start_sw_vsync(hwc_dev);
            else
                stop_sw_vsync();
            return 0;
        }

        err = ioctl(hwc_dev->fb_fd, OMAPFB_ENABLEVSYNC, &val);
        if (err < 0)
            return -errno;

        return 0;
    }
    default:
        return -EINVAL;
    }
}

static int hwc_blank(struct hwc_composer_device_1 *dev __unused, int dpy __unused, int blank __unused)
{
    // We're using an older method of screen blanking based on
    // early_suspend in the kernel.  No need to do anything here.
    return 0;
}

static int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp, uint32_t* configs, size_t* numConfigs)
{
    return get_display_configs((omap_hwc_device_t *)dev, disp, configs, numConfigs);
}

static int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
                                    uint32_t config, const uint32_t* attributes, int32_t* values)
{
    return get_display_attributes((omap_hwc_device_t *)dev, disp, config, attributes, values);
}

static int hwc_device_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    omap_hwc_module_t *hwc_mod = (omap_hwc_module_t *)module;
    omap_hwc_device_t *hwc_dev;
    int err = 0, i;

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        return -EINVAL;
    }

    if (!hwc_mod->fb_dev[HWC_DISPLAY_PRIMARY]) {
        err = open_fb_hal(hwc_mod->fb_dev);
        if (err)
            return err;

        if (!hwc_mod->fb_dev) {
            ALOGE("Framebuffer HAL not opened before HWC");
            return -EFAULT;
        }
        hwc_mod->fb_dev[HWC_DISPLAY_PRIMARY]->bBypassPost = 1;
    }

    hwc_dev = (omap_hwc_device_t *)malloc(sizeof(*hwc_dev));
    if (hwc_dev == NULL)
        return -ENOMEM;

    memset(hwc_dev, 0, sizeof(*hwc_dev));

    hwc_dev->base.common.tag = HARDWARE_DEVICE_TAG;
    hwc_dev->base.common.version = HWC_DEVICE_API_VERSION_1_1;

    hwc_dev->base.common.module = (hw_module_t *)module;
    hwc_dev->base.common.close = hwc_device_close;
    hwc_dev->base.prepare = hwc_prepare;
    hwc_dev->base.set = hwc_set;
    hwc_dev->base.eventControl = hwc_eventControl;
    hwc_dev->base.blank = hwc_blank;
    hwc_dev->base.dump = hwc_dump;
    hwc_dev->base.registerProcs = hwc_registerProcs;
    hwc_dev->base.getDisplayConfigs = hwc_getDisplayConfigs;
    hwc_dev->base.getDisplayAttributes = hwc_getDisplayAttributes;
    hwc_dev->base.query = hwc_query;

    for (i = 0; i < MAX_DISPLAYS; i++)
        hwc_dev->fb_dev[i] = hwc_mod->fb_dev[i];

    *device = &hwc_dev->base.common;

    hwc_dev->dsscomp_fd = open("/dev/dsscomp", O_RDWR);
    if (hwc_dev->dsscomp_fd < 0) {
        ALOGE("failed to open dsscomp (%d)", errno);
        err = -errno;
        goto done;
    }

    int ret = ioctl(hwc_dev->dsscomp_fd, DSSCIOC_QUERY_PLATFORM, &hwc_dev->platform_limits);
    if (ret) {
        ALOGE("failed to get platform limits (%d): %m", errno);
        err = -errno;
        goto done;
    }

    hwc_dev->fb_fd = open("/dev/graphics/fb0", O_RDWR);
    if (hwc_dev->fb_fd < 0) {
        ALOGE("failed to open fb (%d)", errno);
        err = -errno;
        goto done;
    }

    err = init_primary_display(hwc_dev);
    if (err)
        goto done;

    primary_display_t *primary = NULL;
    if (use_sw_vsync()) {
        if(hwc_dev->displays[HWC_DISPLAY_PRIMARY]->type == DISP_TYPE_LCD)
            primary = &((primary_lcd_display_t*)hwc_dev->displays[HWC_DISPLAY_PRIMARY])->primary;
        else if (hwc_dev->displays[HWC_DISPLAY_PRIMARY]->type == DISP_TYPE_HDMI)
            primary = &((primary_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_PRIMARY])->primary;
        else {
            err = -ENODEV;
            goto done;
        }
        primary->use_sw_vsync = true;
        init_sw_vsync(hwc_dev);
    }

    /* use default value in case some of requested display parameters missing */
    hwc_dev->lcd_xpy = 1.0;
    if (hwc_dev->fb_dis.timings.x_res && hwc_dev->fb_dis.height_in_mm) {
        hwc_dev->lcd_xpy = (float)
            hwc_dev->fb_dis.width_in_mm / hwc_dev->fb_dis.timings.x_res /
            hwc_dev->fb_dis.height_in_mm * hwc_dev->fb_dis.timings.y_res;
    }

    if (!is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY)) {
        hwc_dev->hdmi_fb_fd = open("/dev/graphics/fb1", O_RDWR);
        if (hwc_dev->hdmi_fb_fd < 0) {
            ALOGE("failed to open hdmi fb (%d)", errno);
            err = -errno;
            goto done;
        }
    }

    if (pipe(hwc_dev->pipe_fds) == -1) {
            ALOGE("failed to event pipe (%d): %m", errno);
            err = -errno;
            goto done;
    }

    if (pthread_mutex_init(&hwc_dev->lock, NULL)) {
        ALOGE("failed to create mutex (%d): %m", errno);
        err = -errno;
        goto done;
    }
    if (pthread_create(&hwc_dev->hdmi_thread, NULL, hdmi_thread, hwc_dev))
    {
        ALOGE("failed to create HDMI listening thread (%d): %m", errno);
        err = -errno;
        goto done;
    }

    /* get debug properties */

    /* see if hwc is enabled at all */
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.hwc.rgb_order", value, "1");
    hwc_dev->flags_rgb_order = atoi(value);
    property_get("debug.hwc.nv12_only", value, "0");
    hwc_dev->flags_nv12_only = atoi(value);
    property_get("debug.hwc.idle", value, "250");
    hwc_dev->idle = atoi(value);

    /* read switch state */
    int sw_fd = open("/sys/class/switch/hdmi/state", O_RDONLY);
    if (sw_fd >= 0) {
        char value;
        if (read(sw_fd, &value, 1) == 1)
            hwc_dev->ext_disp_state = value == '1';
        close(sw_fd);
    }

    handle_hotplug(hwc_dev);

    ALOGI("open_device(rgb_order=%d nv12_only=%d)",
        hwc_dev->flags_rgb_order, hwc_dev->flags_nv12_only);

    err = init_blitter(hwc_dev);
    if (err)
        goto done;

    property_get("persist.hwc.upscaled_nv12_limit", value, "2.");
    sscanf(value, "%f", &hwc_dev->upscaled_nv12_limit);
    if (hwc_dev->upscaled_nv12_limit < 0. || hwc_dev->upscaled_nv12_limit > 2048.) {
        ALOGW("Invalid upscaled_nv12_limit (%s), setting to 2.", value);
        hwc_dev->upscaled_nv12_limit = 2.;
    }

done:
    if (err && hwc_dev) {
        if (hwc_dev->dsscomp_fd >= 0)
            close(hwc_dev->dsscomp_fd);
        if (hwc_dev->hdmi_fb_fd >= 0)
            close(hwc_dev->hdmi_fb_fd);
        if (hwc_dev->fb_fd >= 0)
            close(hwc_dev->fb_fd);
        pthread_mutex_destroy(&hwc_dev->lock);
        free(hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition.buffers);
        free_displays(hwc_dev);
        free(hwc_dev);
    }

    return err;
}

static struct hw_module_methods_t module_methods = {
    .open = hwc_device_open,
};

omap_hwc_module_t HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag =                  HARDWARE_MODULE_TAG,
            .module_api_version =   HWC_MODULE_API_VERSION_0_1,
            .hal_api_version =      HARDWARE_HAL_API_VERSION,
            .id =                   HWC_HARDWARE_MODULE_ID,
            .name =                 "OMAP 44xx Hardware Composer HAL",
            .author =               "Texas Instruments",
            .methods =              &module_methods,
        },
    },
};
