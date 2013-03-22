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
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <cutils/log.h>

#include <video/dsscomp.h>

#include "hwc_dev.h"
#include "display.h"
#include "dsscomp.h"
#include "layer.h"
#include "utils.h"

#define MAX_MODE_DB_LENGTH 32

#define WB_CAPTURE_MAX_UPSCALE 1.0f
#define WB_CAPTURE_MAX_DOWNSCALE 0.5f

/*
 * This tolerance threshold controls decision of whether to use WB in CAPTURE or in MEM2MEM mode
 * when setting up primary display mirroring.
 */
#define WB_ASPECT_RATIO_TOLERANCE 0.15f

static void append_manager(struct dsscomp_setup_dispc_data *dsscomp, int mgr_ix, bool swap_rb)
{
    dsscomp->mgrs[dsscomp->num_mgrs].ix = mgr_ix;
    dsscomp->mgrs[dsscomp->num_mgrs].alpha_blending = 1;
    dsscomp->mgrs[dsscomp->num_mgrs].swap_rb = swap_rb;
    dsscomp->num_mgrs++;
}

int init_dsscomp(omap_hwc_device_t *hwc_dev)
{
    dsscomp_state_t *dsscomp = &hwc_dev->dsscomp;
    int err = 0;

    dsscomp->fd = open("/dev/dsscomp", O_RDWR);
    if (dsscomp->fd < 0) {
        ALOGE("Failed to open dsscomp (%d)", errno);
        return -errno;
    }

    int ret = ioctl(dsscomp->fd, DSSCIOC_QUERY_PLATFORM, &dsscomp->limits);
    if (ret) {
        ALOGE("Failed to get platform limits (%d)", errno);
        close_dsscomp(hwc_dev);
        return -errno;
    }

    return err;
}

void close_dsscomp(omap_hwc_device_t *hwc_dev)
{
    if (hwc_dev->dsscomp.fd >= 0) {
        close(hwc_dev->dsscomp.fd);
        hwc_dev->dsscomp.fd = 0;
    }
}

int get_dsscomp_display_info(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_display_info *info)
{
    memset(info, 0, sizeof(*info));

    info->ix = mgr_ix;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_QUERY_DISPLAY, info);
    if (ret) {
        ALOGE("Failed to get display %d info (%d)", mgr_ix, errno);
        return -errno;
    }

    return 0;
}

int get_dsscomp_display_mode_db(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode_db, uint32_t *mode_db_len)
{
    struct query {
        struct dsscomp_videomode modedb[MAX_MODE_DB_LENGTH];
        struct dsscomp_display_info info; /* variable-sized type; should be at end of struct */
    } query;

    memset(&query, 0, sizeof(query));

    query.info.ix = mgr_ix;
    query.info.modedb_len = *mode_db_len;

    if (query.info.modedb_len > MAX_MODE_DB_LENGTH)
        query.info.modedb_len = MAX_MODE_DB_LENGTH;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_QUERY_DISPLAY, &query.info);
    if (ret) {
        ALOGE("Failed to get display %d mode database (%d)", mgr_ix, errno);
        return -errno;
    }

    memcpy(mode_db, query.modedb, sizeof(query.modedb[0]) * query.info.modedb_len);

    *mode_db_len = query.info.modedb_len;

    return 0;
}

int setup_dsscomp_display(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode)
{
    struct dsscomp_setup_display_data data;

    memset(&data, 0, sizeof(data));

    data.ix = mgr_ix;
    data.mode = *mode;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_SETUP_DISPLAY, &data);
    if (ret) {
        ALOGE("Failed to setup display %d (%d)", mgr_ix, errno);
        return -errno;
    }

    return 0;
}

void setup_dsscomp_manager(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp;

    if (is_external_display_mirroring(hwc_dev, disp))
        /* When mirroring add second manager to the primary display composition */
        dsscomp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition.comp_data.dsscomp_data;
    else
        dsscomp = &comp->comp_data.dsscomp_data;

    append_manager(dsscomp, display->mgr_ix, comp->swap_rb);

    if (hwc_dev->dsscomp.last_ext_ovls && get_external_display_id(hwc_dev) < 0) {
        append_manager(dsscomp, 1, false);
        hwc_dev->dsscomp.last_ext_ovls = 0;
    }
}

bool can_dss_scale(omap_hwc_device_t *hwc_dev, uint32_t src_w, uint32_t src_h,
                   uint32_t dst_w, uint32_t dst_h, bool is_2d,
                   struct dsscomp_display_info *dis, uint32_t pclk)
{
    struct dsscomp_platform_info *limits = &hwc_dev->dsscomp.limits;
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

bool can_dss_render_all_layers(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    layer_statistics_t *layer_stats = &display->layer_stats;
    composition_t *comp = &display->composition;
    bool on_tv = is_hdmi_display(hwc_dev, disp);
    bool tform = false;

    int ext_disp = (disp == HWC_DISPLAY_PRIMARY) ? get_external_display_id(hwc_dev) : disp;
    if (is_external_display_mirroring(hwc_dev, ext_disp)) {
        display_t *ext_display = hwc_dev->displays[ext_disp];
        uint32_t ext_composable_mask = ext_display->layer_stats.composable_mask;

        /*
         * Make sure that all layers that are composable on the primary display are also
         * composable on the external.
         */
        if ((layer_stats->composable_mask & ext_composable_mask) != layer_stats->composable_mask)
            return false;

        if (!on_tv) {
            int clone = (disp == HWC_DISPLAY_PRIMARY) ? ext_disp : HWC_DISPLAY_PRIMARY;
            on_tv = is_hdmi_display(hwc_dev, clone);
        }

        tform = ext_display->transform.rotation || ext_display->transform.hflip;
    }

    return  !hwc_dev->force_sgx &&
            /* must have at least one layer if using composition bypass to get sync object */
            layer_stats->composable &&
            layer_stats->composable <= comp->avail_ovls &&
            layer_stats->composable == layer_stats->count &&
            layer_stats->scaled <= comp->scaling_ovls &&
            layer_stats->nv12 <= comp->scaling_ovls &&
            /* fits into TILER slot */
            layer_stats->mem1d_total <= comp->tiler1d_slot_size &&
            /* we cannot clone non-NV12 transformed layers */
            (!tform || (layer_stats->nv12 == layer_stats->composable)) &&
            /* HDMI cannot display BGR */
            (layer_stats->bgr == 0 || (layer_stats->rgb == 0 && !on_tv) || !hwc_dev->flags_rgb_order) &&
            /* If nv12_only flag is set DSS should only render NV12 */
            (!hwc_dev->flags_nv12_only || (layer_stats->bgr == 0 && layer_stats->rgb == 0));
}

bool can_dss_render_layer(omap_hwc_device_t *hwc_dev, int disp, hwc_layer_1_t *layer)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    bool on_tv = is_hdmi_display(hwc_dev, disp);
    bool tform = false;

    int ext_disp = (disp == HWC_DISPLAY_PRIMARY) ? get_external_display_id(hwc_dev) : disp;
    if (is_external_display_mirroring(hwc_dev, ext_disp)) {
        display_t *ext_display = hwc_dev->displays[ext_disp];

        if (!is_composable_layer(hwc_dev, ext_disp, layer))
            return false;

        if (!on_tv) {
            int clone = (disp == HWC_DISPLAY_PRIMARY) ? ext_disp : HWC_DISPLAY_PRIMARY;
            on_tv = is_hdmi_display(hwc_dev, clone);
        }

        tform = ext_display->transform.rotation || ext_display->transform.hflip;
    }

    return is_composable_layer(hwc_dev, disp, layer) &&
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

uint32_t decide_dss_wb_capture_mode(uint32_t src_xres, uint32_t src_yres, uint32_t dst_xres, uint32_t dst_yres)
{
    uint32_t wb_mode = OMAP_WB_CAPTURE_MODE;
    float x_scale_factor = (float)src_xres / dst_xres;
    float y_scale_factor = (float)src_yres / dst_yres;

    if (x_scale_factor > WB_CAPTURE_MAX_UPSCALE || y_scale_factor > WB_CAPTURE_MAX_UPSCALE ||
        x_scale_factor < WB_CAPTURE_MAX_DOWNSCALE || y_scale_factor < WB_CAPTURE_MAX_DOWNSCALE ||
        x_scale_factor < y_scale_factor * (1.f - WB_ASPECT_RATIO_TOLERANCE) ||
        x_scale_factor * (1.f - WB_ASPECT_RATIO_TOLERANCE) > y_scale_factor) {
        wb_mode = OMAP_WB_MEM2MEM_MODE;
    }

    // HACK: Force MEM2MEM mode until switching between MEM2MEM and CAPTURE is properly supported
    wb_mode = OMAP_WB_MEM2MEM_MODE;

    return wb_mode;
}
