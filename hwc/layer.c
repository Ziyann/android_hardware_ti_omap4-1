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

#include <stdbool.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "utils.h"
#include "layer.h"

bool is_dockable_layer(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    return (handle->usage & GRALLOC_USAGE_EXTERNAL_DISP) != 0;
}

bool is_protected_layer(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    return (handle->usage & GRALLOC_USAGE_PROTECTED) != 0;
}

bool is_blended_layer(const hwc_layer_1_t *layer)
{
    return layer->blending != HWC_BLENDING_NONE;
}

bool is_rgb_layer(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    return is_rgb_format(handle->iFormat);
}

bool is_bgr_layer(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    return is_bgr_format(handle->iFormat);
}

bool is_nv12_layer(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    return is_nv12_format(handle->iFormat);
}

bool is_scaled_layer(const hwc_layer_1_t *layer)
{
    int w = WIDTH(layer->sourceCrop);
    int h = HEIGHT(layer->sourceCrop);

    if (layer->transform & HWC_TRANSFORM_ROT_90)
        SWAP(w, h);

    bool res = WIDTH(layer->displayFrame) != w || HEIGHT(layer->displayFrame) != h;

    return res;
}

bool is_upscaled_nv12_layer(omap_hwc_device_t *hwc_dev, const hwc_layer_1_t *layer)
{
    if (!layer)
        return false;

    if (!is_nv12_layer(layer))
        return false;

    int w = WIDTH(layer->sourceCrop);
    int h = HEIGHT(layer->sourceCrop);

    if (layer->transform & HWC_TRANSFORM_ROT_90)
        SWAP(w, h);

    return (WIDTH(layer->displayFrame) >= w * hwc_dev->upscaled_nv12_limit ||
            HEIGHT(layer->displayFrame) >= h * hwc_dev->upscaled_nv12_limit);
}

uint32_t get_required_mem1d_size(const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    if (handle == NULL || is_nv12_layer(layer))
        return 0;

    int bpp = handle->iFormat == HAL_PIXEL_FORMAT_RGB_565 ? 2 : 4;
    int stride = ALIGN(handle->iWidth, HW_ALIGN) * bpp;
    return stride * handle->iHeight;
}

static bool can_scale_layer(omap_hwc_device_t *hwc_dev, const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    int src_w = WIDTH(layer->sourceCrop);
    int src_h = HEIGHT(layer->sourceCrop);
    int dst_w = WIDTH(layer->displayFrame);
    int dst_h = HEIGHT(layer->displayFrame);

    if (handle)
        if (get_format_bpp(handle->iFormat) == 32 && src_w > 1280 && dst_w * 3 < src_w)
            return false;

    /* account for 90-degree rotation */
    if (layer->transform & HWC_TRANSFORM_ROT_90)
        SWAP(src_w, src_h);

    /* NOTE: layers should be able to be scaled externally since
       framebuffer is able to be scaled on selected external resolution */
    return can_scale(src_w, src_h, dst_w, dst_h, is_nv12_layer(layer), &hwc_dev->fb_dis, &hwc_dev->platform_limits,
                     hwc_dev->fb_dis.timings.pixel_clock);
}

bool is_valid_layer(omap_hwc_device_t *hwc_dev, const hwc_layer_1_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

    /* Skip layers are handled by SF */
    if ((layer->flags & HWC_SKIP_LAYER) || !layer->handle)
        return false;

    if (layer->compositionType == HWC_FRAMEBUFFER_TARGET)
        return false;

    if (!is_valid_format(handle->iFormat))
        return false;

    /* 1D buffers: no transform, must fit in TILER slot */
    if (!is_nv12_layer(layer)) {
        if (layer->transform)
            return false;
        if (get_required_mem1d_size(layer) > hwc_dev->platform_limits.tiler1d_slot_size)
            return false;
    }

    return can_scale_layer(hwc_dev, layer);
}

void gather_layer_statistics(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *list, layer_statistics_t *layer_stats)
{
    uint32_t i;

    memset(layer_stats, 0, sizeof(*layer_stats));

    layer_stats->count = list ? list->numHwLayers : 0;

    /* Figure out how many layers we can support via DSS */
    for (i = 0; list && i < list->numHwLayers; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];

        if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            layer_stats->framebuffer++;
            layer_stats->count--;
        } else {
            layer->compositionType = HWC_FRAMEBUFFER;
        }

        if (is_valid_layer(hwc_dev, layer)) {
            layer_stats->composable++;

            /* NV12 layers can only be rendered on scaling overlays */
            if (is_scaled_layer(layer) || is_nv12_layer(layer) || hwc_dev->primary_transform)
                layer_stats->scaled++;

            if (is_bgr_layer(layer))
                layer_stats->bgr++;
            else if (is_rgb_layer(layer))
                layer_stats->rgb++;
            else if (is_nv12_layer(layer))
                layer_stats->nv12++;

            if (is_dockable_layer(layer))
                layer_stats->dockable++;

            if (is_protected_layer(layer))
                layer_stats->protected++;

            layer_stats->mem1d_total += get_required_mem1d_size(layer);
        }
    }
}
