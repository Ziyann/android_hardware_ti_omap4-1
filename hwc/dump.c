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
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <hardware/hwcomposer.h>

#include <video/dsscomp.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "blitter.h"
#include "display.h"
#include "dump.h"

static void dump_printf(dump_buf_t *buf, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    buf->len += vsnprintf(buf->buf + buf->len, buf->buf_len - buf->len, fmt, ap);
    va_end(ap);
}

void dump_hwc_info(omap_hwc_device_t *hwc_dev, dump_buf_t *log)
{
    dump_printf(log, "OMAP HWC %d.%d:\n",
                     (hwc_dev->base.common.version >> 24) & 0xff,
                     (hwc_dev->base.common.version >> 16) & 0xff);
    dump_printf(log, "  idle timeout: %dms\n", hwc_dev->idle);

    blitter_config_t *blitter = &hwc_dev->blitter;

    if (blitter->policy != BLT_POLICY_DISABLED) {
        dump_printf(log, "  blitter:\n");
        dump_printf(log, "    policy: %s, mode: %s\n",
                         blitter->policy == BLT_POLICY_DEFAULT ? "default" :
                         blitter->policy == BLT_POLICY_ALL ? "all" : "unknown",
                         blitter->mode == BLT_MODE_PAINT ? "paint" : "regionize");
    }
}

void dump_display(omap_hwc_device_t *hwc_dev, dump_buf_t *log, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    display_config_t *config = &display->configs[display->active_config_ix];

    dump_printf(log, "  display[%d]: %s %dx%d\n",
                     disp,
                     display->type == DISP_TYPE_LCD ? "LCD" :
                     display->type == DISP_TYPE_HDMI ? "HDMI" :
                     display->type == DISP_TYPE_WFD ? "WFD" : "unknown",
                     config->xres, config->yres);

    if (get_display_mode(hwc_dev, disp) == DISP_MODE_LEGACY) {
        dump_printf(log, "    legacy mode\n");
        return;
    }

    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    int i;

    for (i = 0; i < dsscomp->num_ovls; i++) {
        struct dss2_ovl_cfg *cfg = &dsscomp->ovls[i].cfg;

        dump_printf(log, "    layer[%d]:\n", i);
        dump_printf(log, "      enabled:%s buff:%p %dx%d stride:%d\n",
                         cfg->enabled ? "true" : "false", comp->buffers[i],
                         cfg->width, cfg->height, cfg->stride);
        dump_printf(log, "      src:(%d,%d) %dx%d dst:(%d,%d) %dx%d ix:%d@%d z:%d\n",
                         cfg->crop.x, cfg->crop.y, cfg->crop.w, cfg->crop.h,
                         cfg->win.x, cfg->win.y, cfg->win.w, cfg->win.h,
                         cfg->ix, cfg->mgr_ix, cfg->zorder);
    }
}

void dump_layer(const hwc_layer_1_t *l)
{
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
         l->compositionType, l->flags, l->handle, l->transform, l->blending,
         l->sourceCrop.left, l->sourceCrop.top, l->sourceCrop.right, l->sourceCrop.bottom,
         l->displayFrame.left, l->displayFrame.top, l->displayFrame.right, l->displayFrame.bottom);
}

void dump_set_info(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *list, int disp)
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

        dump_printf(&log, "%d@%d=", dsscomp->ovls[i].cfg.ix, dsscomp->ovls[i].cfg.mgr_ix);

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

void dump_dsscomp(struct dsscomp_setup_dispc_data *d)
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
        char writeback[20] = {'\0'};
        if (c->ix == OMAP_DSS_WB)
            sprintf(writeback, "wb(%s@%s%d) => ",
                    c->wb_mode == OMAP_WB_MEM2MEM_MODE ? "m2m" : "cap",
                    c->wb_source < OMAP_WB_GFX ? "mgr" : "ovl",
                    c->wb_source < OMAP_WB_GFX ? c->wb_source : c->wb_source - OMAP_WB_GFX);
        if (c->zonly)
            ALOGD("ovl%d@%d(%s z%d)\n",
                 c->ix, c->mgr_ix, c->enabled ? "ON" : "off", c->zorder);
        else
            ALOGD("ovl%d@%d(%s z%d %s%s *%d%% %s%d*%d:%d,%d+%d,%d rot%d%s => %d,%d+%d,%d %p/%p|%d)\n",
                 c->ix, c->mgr_ix, c->enabled ? "ON" : "off", c->zorder, DSS_FMT(c->color_mode),
                 c->pre_mult_alpha ? " premult" : "",
                 (c->global_alpha * 100 + 128) / 255,
                 writeback,
                 c->width, c->height, c->crop.x, c->crop.y,
                 c->crop.w, c->crop.h,
                 c->rotation, c->mirror ? "+mir" : "",
                 c->win.x, c->win.y, c->win.w, c->win.h,
                 (void *) oi->ba, (void *) oi->uv, c->stride);
    }
}
