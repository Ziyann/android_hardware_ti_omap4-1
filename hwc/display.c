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
#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>

#include <cutils/log.h>

#include <video/dsscomp.h>
#ifdef USE_TI_LIBION
#include <ion_ti/ion.h>
#else
#include <ion/ion.h>
#include "ion_ti_custom.h"
#endif


#include "hwc_dev.h"
#include "display.h"

#define LCD_DISPLAY_CONFIGS 1
#define LCD_DISPLAY_FPS 60
#define LCD_DISPLAY_DEFAULT_DPI 150

/* Currently SF cannot handle more than 1 config */
#define HDMI_DISPLAY_CONFIGS 1
#define HDMI_DISPLAY_FPS 60
#define HDMI_DISPLAY_DEFAULT_DPI 75

#define MAX_DISPLAY_ID (MAX_DISPLAYS - 1)
#define INCH_TO_MM 25.4f

static void free_display(display_t *display)
{
    if (display) {
        if (display->configs)
            free(display->configs);

        free(display);
    }
}

static int allocate_display(size_t display_data_size, uint32_t max_configs, display_t **new_display)
{
    int err = 0;

    display_t *display = (display_t *)malloc(display_data_size);
    if (display == NULL) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display, 0, display_data_size);

    display->num_configs = max_configs;
    size_t config_data_size = sizeof(*display->configs) * display->num_configs;
    display->configs = (display_config_t *)malloc(config_data_size);
    if (display->configs == NULL) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display->configs, 0, config_data_size);

err_out:

    if (err) {
        ALOGE("Failed to allocate display (configs = %d)", max_configs);
        free_display(display);
    } else {
        *new_display = display;
    }

    return err;
}

static int get_display_info(omap_hwc_device_t *hwc_dev, int disp, struct dsscomp_display_info *info)
{
    memset(info, 0, sizeof(*info));
    info->ix = disp;

    int ret = ioctl(hwc_dev->dsscomp_fd, DSSCIOC_QUERY_DISPLAY, info);
    if (ret) {
        ALOGE("Failed to get display info (%d): %m", errno);
        return -errno;
    }

    return 0;
}

static int free_tiler2d_buffers(external_display_t *display)
{
    int i;
    for (i = 0 ; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
#ifdef USE_TI_LIBION
        ion_free(display->ion_fd, display->ion_handles[i]);
#else
        ion_free(display->ion_fd, (ion_user_handle_t) display->ion_handles[i]);
#endif
        display->ion_handles[i] = NULL;
    }

    return 0;
}

static int allocate_tiler2d_buffers(omap_hwc_device_t *hwc_dev, external_display_t *display)
{
    int ret, i;
    size_t stride;

    if (display->ion_fd < 0) {
        ALOGE("No ion fd, hence can't allocate tiler2d buffers");
        return -ENOMEM;
    }

    for (i = 0; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        if (display->ion_handles[i])
            return 0;
    }

    for (i = 0 ; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        ret = ion_alloc_tiler(display->ion_fd, hwc_dev->fb_dev->base.width, hwc_dev->fb_dev->base.height,
                              TILER_PIXEL_FMT_32BIT, 0, &display->ion_handles[i], &stride);
        if (ret)
            goto handle_error;

        ALOGI("ion handle[%d][%p]", i, display->ion_handles[i]);
    }

    return 0;

handle_error:

    free_tiler2d_buffers(display);
    return -ENOMEM;
}

int init_primary_display(omap_hwc_device_t *hwc_dev)
{
    int err;

    err = get_display_info(hwc_dev, HWC_DISPLAY_PRIMARY, &hwc_dev->fb_dis);
    if (err)
        return err;

    uint32_t disp_type = (hwc_dev->fb_dis.channel == OMAP_DSS_CHANNEL_DIGIT) ? DISP_TYPE_HDMI : DISP_TYPE_LCD;
    uint32_t max_configs = (disp_type == DISP_TYPE_HDMI) ? HDMI_DISPLAY_CONFIGS : LCD_DISPLAY_CONFIGS;

    if (disp_type == DISP_TYPE_HDMI)
        ALOGI("Primary display is HDMI");

    err = allocate_display(sizeof(display_t), max_configs, &hwc_dev->displays[HWC_DISPLAY_PRIMARY]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    display_config_t *config = &display->configs[0];

    config->xres = hwc_dev->fb_dis.timings.x_res;
    config->yres = hwc_dev->fb_dis.timings.y_res;
    config->fps = (disp_type == DISP_TYPE_HDMI) ? HDMI_DISPLAY_FPS : LCD_DISPLAY_FPS;

    if (hwc_dev->fb_dis.width_in_mm && hwc_dev->fb_dis.height_in_mm) {
        config->xdpi = (int)(config->xres * INCH_TO_MM) / hwc_dev->fb_dis.width_in_mm;
        config->ydpi = (int)(config->yres * INCH_TO_MM) / hwc_dev->fb_dis.height_in_mm;
    } else {
        config->xdpi = (disp_type == DISP_TYPE_HDMI) ? HDMI_DISPLAY_DEFAULT_DPI : LCD_DISPLAY_DEFAULT_DPI;
        config->ydpi = (disp_type == DISP_TYPE_HDMI) ? HDMI_DISPLAY_DEFAULT_DPI : LCD_DISPLAY_DEFAULT_DPI;
    }

    display->type = disp_type;

    return 0;
}

int add_external_display(omap_hwc_device_t *hwc_dev)
{
    int err;

    struct dsscomp_display_info info;
    err = get_display_info(hwc_dev, HWC_DISPLAY_EXTERNAL, &info);
    if (err)
        return err;

    /* Currently SF cannot handle more than 1 config */
    err = allocate_display(sizeof(external_display_t), HDMI_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]);
    if (err)
        return err;

    external_display_t *display = (external_display_t *)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    display_config_t *config = &display->base.configs[0];
    omap_hwc_ext_t *ext = &hwc_dev->ext;

    config->xres = ext->mirror_region.right - ext->mirror_region.left;
    config->yres = ext->mirror_region.bottom - ext->mirror_region.top;
    config->fps = HDMI_DISPLAY_FPS;

    if (info.width_in_mm && info.height_in_mm) {
        config->xdpi = (int)(config->xres * INCH_TO_MM) / info.width_in_mm;
        config->ydpi = (int)(config->yres * INCH_TO_MM) / info.height_in_mm;
    } else {
        config->xdpi = HDMI_DISPLAY_DEFAULT_DPI;
        config->ydpi = HDMI_DISPLAY_DEFAULT_DPI;
    }

    if (info.channel == OMAP_DSS_CHANNEL_DIGIT) {
        display->base.type = DISP_TYPE_HDMI;
    } else {
        ALOGW("Unknown type of external display is connected");
    }

    /* Allocate backup buffers for FB rotation. This is required only if the FB tranform is
     * different from that of the external display and the FB is not in TILER2D space.
     */
    if (ext->mirror.rotation && (hwc_dev->platform_limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
        display->ion_fd = ion_open();
        if (display->ion_fd >= 0) {
            allocate_tiler2d_buffers(hwc_dev, display);
        } else {
            ALOGE("Failed to open ion driver (%d)", errno);
        }
    }

    return 0;
}

void remove_external_display(omap_hwc_device_t *hwc_dev)
{
    external_display_t *display = (external_display_t *)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (!display)
        return;

    omap_hwc_ext_t *ext = &hwc_dev->ext;

    if (ext->mirror.rotation && (hwc_dev->platform_limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
        /* free tiler 2D buffer on detach */
        free_tiler2d_buffers(display);

        if (display->ion_fd >= 0)
            ion_close(display->ion_fd);
    }

    free_display(hwc_dev->displays[HWC_DISPLAY_EXTERNAL]);
    hwc_dev->displays[HWC_DISPLAY_EXTERNAL] = NULL;
}

struct ion_handle *get_external_display_ion_fb_handle(omap_hwc_device_t *hwc_dev)
{
    external_display_t *display = (external_display_t *)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];

    if (display) {
        struct dsscomp_setup_dispc_data *dsscomp = &hwc_dev->comp_data.dsscomp_data;

        return display->ion_handles[dsscomp->sync_id % EXTERNAL_DISPLAY_BACK_BUFFERS];
    } else {
        return NULL;
    }
}

int get_display_configs(omap_hwc_device_t *hwc_dev, int disp, uint32_t *configs, size_t *numConfigs)
{
    if (!numConfigs)
        return -EINVAL;

    if (*numConfigs == 0)
        return 0;

    if (!configs || disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return -EINVAL;

    display_t *display = hwc_dev->displays[disp];
    size_t num = display->num_configs;
    uint32_t c;

    if (num > *numConfigs)
        num = *numConfigs;

    for (c = 0; c < num; c++)
        configs[c] = c;

    *numConfigs = num;

    return 0;
}

int get_display_attributes(omap_hwc_device_t *hwc_dev, int disp, uint32_t cfg, const uint32_t *attributes, int32_t *values)
{
    if (!attributes || !values)
        return 0;

    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return -EINVAL;

    display_t *display = hwc_dev->displays[disp];

    if (cfg >= display->num_configs)
        return -EINVAL;

    const uint32_t* attribute = attributes;
    int32_t* value = values;
    display_config_t *config = &display->configs[cfg];

    while (*attribute != HWC_DISPLAY_NO_ATTRIBUTE) {
        switch (*attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            *value = 1000000000 / config->fps;
            break;
        case HWC_DISPLAY_WIDTH:
            *value = config->xres;
            break;
        case HWC_DISPLAY_HEIGHT:
            *value = config->yres;
            break;
        case HWC_DISPLAY_DPI_X:
            *value = 1000 * config->xdpi;
            break;
        case HWC_DISPLAY_DPI_Y:
            *value = 1000 * config->ydpi;
            break;
        }

        attribute++;
        value++;
    }

    return 0;
}

bool is_hdmi_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return false;

    return hwc_dev->displays[disp]->type == DISP_TYPE_HDMI;
}

void free_displays(omap_hwc_device_t *hwc_dev)
{
    /* Make sure that we don't leak ION memory that might be allocated by external display */
    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL])
        remove_external_display(hwc_dev);

    int i;
    for (i = 0; i < MAX_DISPLAYS; i++)
        free_display(hwc_dev->displays[i]);
}
