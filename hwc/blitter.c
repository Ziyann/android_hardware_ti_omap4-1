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
#include <fcntl.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <hardware/hwcomposer.h>

#include <linux/bltsville.h>
#include <video/dsscomp.h>
#include <video/omap_hwc.h>

#include "hwc_dev.h"
#include "blitter.h"
#include "display.h"

static rgz_t grgz;
#ifdef OMAP_ENHANCEMENT_HWC_EXTENDED_API
static rgz_ext_layer_list_t grgz_ext_layer_list;
#endif
static struct bvsurfgeom gscrngeom;

int init_blitter(omap_hwc_device_t *hwc_dev)
{
    blitter_config_t *blitter = &hwc_dev->blitter;
    int err = 0;

    blitter->debug = false;

    int gc2d_fd = open("/dev/gcioctl", O_RDWR);
    if (gc2d_fd < 0) {
        ALOGI("Unable to open gc-core device (%d), blits disabled", errno);
        blitter->policy = BLT_POLICY_DISABLED;
        blitter->debug = false;
    } else {
        char value[PROPERTY_VALUE_MAX];

        property_get("persist.hwc.bltmode", value, "1");
        blitter->mode = atoi(value);

        property_get("persist.hwc.bltpolicy", value, "1");
        blitter->policy = atoi(value);

        ALOGI("blitter present, blits mode %d, blits policy %d", blitter->mode, blitter->policy);

        close(gc2d_fd);

        if (rgz_get_screengeometry(hwc_dev->fb_fd, &gscrngeom,
                hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.format) != 0) {
            err = -EINVAL;
        }
    }

    return err;
}

void reset_blitter(omap_hwc_device_t *hwc_dev)
{
    /* Blitter is used only in primary display composition */
    composition_t *comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;

    comp->blitter.flags = 0;
    comp->blitter.num_blits = 0;
    comp->blitter.num_buffers = 0;
    comp->comp_data.blit_data.rgz_items = 0;
}

void release_blitter(void)
{
    rgz_release(&grgz);
}

bool blit_layers(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *contents, int buf_offset)
{
    /* Do not blit if this frame will be composed entirely by the GPU.
     * Currently blitter is supported only for single display scenarios.
     */
    if (!contents || hwc_dev->force_sgx || is_external_display_mirroring(hwc_dev))
        goto err_out;

    blitter_config_t *blitter = &hwc_dev->blitter;
    int rgz_in_op;
    int rgz_out_op;

    switch (blitter->mode) {
        case BLT_MODE_PAINT:
            rgz_in_op = RGZ_IN_HWCCHK;
            rgz_out_op = RGZ_OUT_BVCMD_PAINT;
            break;
        case BLT_MODE_REGION:
        default:
            rgz_in_op = RGZ_IN_HWC;
            rgz_out_op = RGZ_OUT_BVCMD_REGION;
            break;
    }

    size_t num_layers = contents->numHwLayers;
    if (hwc_dev->base.common.version > HWC_DEVICE_API_VERSION_1_0) {
        /* Ignore HWC_FRAMEBUFFER_TARGET layer at the end of the list */
        num_layers -= 1;
    }

#ifdef OMAP_ENHANCEMENT_HWC_EXTENDED_API
    /*
     * Request the layer identities to SurfaceFlinger, first figure out if the
     * operation is supported
     */
    if (!(contents->flags & HWC_EXTENDED_API) || !hwc_dev->procs ||
        hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERDATA, NULL, -1) != 0)
        goto err_out;

    /* Check if we have enough space in the extended layer list */
    if ((sizeof(hwc_layer_extended_t) * num_layers) > sizeof(grgz_ext_layer_list))
        goto err_out;
#endif
    uint32_t i;
#ifdef OMAP_ENHANCEMENT_HWC_EXTENDED_API
    for (i = 0; i < num_layers; i++) {
        hwc_layer_extended_t *ext_layer = &grgz_ext_layer_list.layers[i];
        ext_layer->idx = i;
        if (hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERDATA,
            (void **) &ext_layer, sizeof(hwc_layer_extended_t)) != 0)
            goto err_out;
    }
#endif

    rgz_in_params_t in = {
        .op = rgz_in_op,
        .data = {
            .hwc = {
                .dstgeom = &gscrngeom,
                .layers = contents->hwLayers,
#ifdef OMAP_ENHANCEMENT_HWC_EXTENDED_API
                .extlayers = grgz_ext_layer_list.layers,
#endif
                .layerno = num_layers
            }
        }
    };

    /*
     * This means if all the layers marked for the FRAMEBUFFER cannot be
     * blitted, do not blit, for e.g. SKIP layers
     */
    if (rgz_in(&in, &grgz) != RGZ_ALL)
        goto err_out;

    uint32_t count = 0;
    for (i = 0; i < num_layers; i++) {
        if (contents->hwLayers[i].compositionType != HWC_OVERLAY) {
            count++;
        }
    }

    rgz_out_params_t out = {
        .op = rgz_out_op,
        .data = {
            .bvc = {
                .dstgeom = &gscrngeom,
                .noblend = 0,
            }
        }
    };

    if (rgz_out(&grgz, &out) != 0) {
        ALOGE("Failed generating blits");
        goto err_out;
    }

    /* This is a special situation where the regionizer decided no blits are
     * needed for this frame but there are blit buffers to synchronize with. Can
     * happen only if the regionizer is enabled otherwise it's likely a bug
     */
    if (rgz_out_op != RGZ_OUT_BVCMD_REGION && out.data.bvc.out_blits == 0 && out.data.bvc.out_nhndls > 0) {
        ALOGE("Regionizer invalid output blit_num %d, post2_blit_buffers %d",
                out.data.bvc.out_blits, out.data.bvc.out_nhndls);
        goto err_out;
    }

    /* Blitter is used only in primary display composition */
    composition_t *comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;

    comp->blitter.flags |= HWC_BLT_FLAG_USE_FB;
    comp->blitter.num_blits = out.data.bvc.out_blits;
    comp->blitter.num_buffers = out.data.bvc.out_nhndls;

    for (i = 0; i < comp->blitter.num_buffers; i++) {
        //ALOGI("blit buffers[%d] = %p", bufoff, out.data.bvc.out_hndls[i]);
        comp->buffers[buf_offset++] = out.data.bvc.out_hndls[i];
    }

    struct rgz_blt_entry *res_blit_ops = (struct rgz_blt_entry *) out.data.bvc.cmdp;
    memcpy(comp->comp_data.blit_data.rgz_blts, res_blit_ops, sizeof(*res_blit_ops) * out.data.bvc.cmdlen);

    ALOGI_IF(blitter->debug, "blt struct sz %d", sizeof(*res_blit_ops) * out.data.bvc.cmdlen);
    ALOGE_IF(comp->blitter.num_blits != (uint32_t)out.data.bvc.cmdlen, "blit_num != out.data.bvc.cmdlen, %d != %d",
            comp->blitter.num_blits, out.data.bvc.cmdlen);

    /* all layers will be rendered without SGX help either via DSS or blitter */
    for (i = 0; i < num_layers; i++) {
        if (contents->hwLayers[i].compositionType != HWC_OVERLAY) {
            contents->hwLayers[i].compositionType = HWC_OVERLAY;
            //ALOGI("blitting layer %d", i);
            contents->hwLayers[i].hints &= ~HWC_HINT_TRIPLE_BUFFER;
        }

        contents->hwLayers[i].hints &= ~HWC_HINT_CLEAR_FB;
    }

    return true;

err_out:
    release_blitter();
    return false;
}
