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

#include <linux/fb.h>
#include <video/dsscomp.h>
#ifdef USE_TI_LIBION
#include <ion_ti/ion.h>
#else
#include <ion/ion.h>
#include "ion_ti_custom.h"
#endif


#include "hwc_dev.h"
#include "layer.h"
#include "display.h"
#include "utils.h"

#define LCD_DISPLAY_CONFIGS 1
#define LCD_DISPLAY_FPS 60
#define LCD_DISPLAY_DEFAULT_DPI 150

/* Currently SF cannot handle more than 1 config */
#define HDMI_DISPLAY_CONFIGS 1
#define HDMI_DISPLAY_FPS 60
#define HDMI_DISPLAY_DEFAULT_DPI 75

#define MAX_DISPLAY_ID (MAX_DISPLAYS - 1)
#define INCH_TO_MM 25.4f
#define MAX_HWC_LAYERS 32

static void free_display(display_t *display)
{
    if (display) {
        if (display->configs)
            free(display->configs);
        if (display->composition.buffers)
            free(display->composition.buffers);

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

    /* Allocate the maximum buffers that we can receive from HWC */
    display->composition.buffers = malloc(sizeof(buffer_handle_t) * MAX_HWC_LAYERS);
    if (!display->composition.buffers) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display->composition.buffers, 0, sizeof(buffer_handle_t) * MAX_HWC_LAYERS);

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

static void setup_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info,
                         int default_fps, int default_dpi)
{
    config->xres = xres;
    config->yres = yres;
    config->fps = default_fps;

    if (info->width_in_mm && info->height_in_mm) {
        config->xdpi = (int)(config->xres * INCH_TO_MM) / info->width_in_mm;
        config->ydpi = (int)(config->yres * INCH_TO_MM) / info->height_in_mm;
    } else {
        config->xdpi = default_dpi;
        config->ydpi = default_dpi;
    }
}

static void setup_lcd_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info)
{
    setup_config(config, xres, yres, info, LCD_DISPLAY_FPS, LCD_DISPLAY_DEFAULT_DPI);
}

static void setup_hdmi_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info)
{
    setup_config(config, xres, yres, info, HDMI_DISPLAY_FPS, HDMI_DISPLAY_DEFAULT_DPI);
}

static void set_primary_display_transform_matrix(omap_hwc_device_t *hwc_dev)
{
    /* Create primary display translation matrix */
    int lcd_w = hwc_dev->fb_dis.timings.x_res;
    int lcd_h = hwc_dev->fb_dis.timings.y_res;
    IMG_framebuffer_device_public_t* fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    int orig_w = fb_dev->base.width;
    int orig_h = fb_dev->base.height;
    hwc_rect_t region = {.left = 0, .top = 0, .right = orig_w, .bottom = orig_h};

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];

    display->transform.region = region;
    display->transform.rotation= ((lcd_w > lcd_h) ^ (orig_w > orig_h)) ? 1 : 0;
    display->transform.scaling= ((lcd_w != orig_w)||(lcd_h != orig_h)) ? 1 : 0;

    ALOGI("Transforming FB (%dx%d) => (%dx%d) rot%d", orig_w, orig_h, lcd_w, lcd_h, display->transform.rotation);

    /* Reorientation matrix is:
       m = (center-from-target-center) * (scale-to-target) * (mirror) * (rotate) * (center-to-original-center) */
    memcpy(display->transform.matrix, unit_matrix, sizeof(unit_matrix));
    translate_matrix(display->transform.matrix, -(orig_w >> 1), -(orig_h >> 1));
    rotate_matrix(display->transform.matrix, display->transform.rotation);

    if (display->transform.rotation & 1)
         SWAP(orig_w, orig_h);

    scale_matrix(display->transform.matrix, orig_w, lcd_w, orig_h, lcd_h);
    translate_matrix(display->transform.matrix, lcd_w >> 1, lcd_h >> 1);
}

static int free_tiler2d_buffers(external_hdmi_display_t *display)
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

static int allocate_tiler2d_buffers(omap_hwc_device_t *hwc_dev, external_hdmi_display_t *display)
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

    IMG_framebuffer_device_public_t* fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    for (i = 0 ; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        ret = ion_alloc_tiler(display->ion_fd, fb_dev->base.width, fb_dev->base.height,
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

static int init_primary_lcd_display(omap_hwc_device_t *hwc_dev)
{
    int err;
    if (hwc_dev->displays[HWC_DISPLAY_PRIMARY])
        return 0;

    err = allocate_display(sizeof(primary_lcd_display_t), LCD_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_PRIMARY]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    display_config_t *config = &display->configs[0];
    IMG_framebuffer_device_public_t* fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    int xres = fb_dev->base.width;
    int yres = fb_dev->base.height;
    display->type = DISP_TYPE_LCD;
    display->role = DISP_ROLE_PRIMARY;

    setup_lcd_config(config, xres, yres, &hwc_dev->fb_dis);
    set_primary_display_transform_matrix(hwc_dev);

    return 0;
}

static int init_primary_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    int err;
    if (hwc_dev->displays[HWC_DISPLAY_PRIMARY])
        return 0;

    err = allocate_display(sizeof(primary_hdmi_display_t), HDMI_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_PRIMARY]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    display_config_t *config = &display->configs[0];
    IMG_framebuffer_device_public_t* fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    int xres = fb_dev->base.width;
    int yres = fb_dev->base.height;
    display->type = DISP_TYPE_HDMI;
    display->role = DISP_ROLE_PRIMARY;

    setup_hdmi_config(config, xres, yres, &hwc_dev->fb_dis);

    set_primary_display_transform_matrix(hwc_dev);

    return 0;
}

int init_primary_display(omap_hwc_device_t *hwc_dev)
{
    int err;

    err = get_display_info(hwc_dev, HWC_DISPLAY_PRIMARY, &hwc_dev->fb_dis);
    if (err)
        return -ENODEV;

    switch(hwc_dev->fb_dis.channel) {
        case OMAP_DSS_CHANNEL_LCD:
            err = init_primary_lcd_display(hwc_dev);
            break;
        case OMAP_DSS_CHANNEL_DIGIT:
            err = init_primary_hdmi_display(hwc_dev);
            break;
        default:
            return -ENODEV;
    }
    if (err)
        return -ENODEV;

    return err;
}

void reset_primary_display(omap_hwc_device_t *hwc_dev)
{
    int ret;
    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    if (!display)
        return;

    /* Remove bootloader image from the screen as blank/unblank does not change the composition */
    struct dsscomp_setup_dispc_data d = {
        .num_mgrs = 1,
    };

    ret = ioctl(hwc_dev->dsscomp_fd, DSSCIOC_SETUP_DISPC, &d);
    if (ret)
        ALOGW("failed to remove bootloader image");

    /* Blank and unblank fd to make sure display is properly programmed on boot.
     * This is needed because the bootloader can not be trusted.
     */
    blank_display(hwc_dev, HWC_DISPLAY_PRIMARY);
    unblank_display(hwc_dev, HWC_DISPLAY_PRIMARY);
}

int add_external_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    int err;
    struct dsscomp_display_info info;
    err = get_display_info(hwc_dev, HWC_DISPLAY_EXTERNAL, &info);
    if (err)
        return err;

    /* Currently SF cannot handle more than 1 config */
    err = allocate_display(sizeof(external_hdmi_display_t), HDMI_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    display_config_t *config = &display->configs[0];
    display->type = DISP_TYPE_HDMI;
    display->role = DISP_ROLE_EXTERNAL;

    int xres = WIDTH(display->transform.region);
    int yres = HEIGHT(display->transform.region);

    // TODO: Verify that HDMI supports xres x yres
    // TODO: Set HDMI resolution? What if we need to do docking of 1080p i.s.o. Presentation?

    setup_hdmi_config(config, xres, yres, &info);

    /* Allocate backup buffers for FB rotation. This is required only if the FB tranform is
     * different from that of the external display and the FB is not in TILER2D space.
     */
    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (display->transform.rotation && (hwc_dev->platform_limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
        ext_hdmi->ion_fd = ion_open();
        if (ext_hdmi->ion_fd >= 0) {
            allocate_tiler2d_buffers(hwc_dev, ext_hdmi);
        } else {
            ALOGE("Failed to open ion driver (%d)", errno);
        }
    }

    return 0;
}

void remove_external_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    display_t *display = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (!display)
        return;

    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (display->transform.rotation && (hwc_dev->platform_limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
        /* free tiler 2D buffer on detach */
        free_tiler2d_buffers(ext_hdmi);

        if (ext_hdmi->ion_fd >= 0)
            ion_close(ext_hdmi->ion_fd);
    }

    free_display(hwc_dev->displays[HWC_DISPLAY_EXTERNAL]);
    hwc_dev->displays[HWC_DISPLAY_EXTERNAL] = NULL;
}

struct ion_handle *get_external_display_ion_fb_handle(omap_hwc_device_t *hwc_dev)
{
    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t *)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];

    if (ext_hdmi) {
        struct dsscomp_setup_dispc_data *dsscomp = &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->composition.comp_data.dsscomp_data;

        return ext_hdmi->ion_handles[dsscomp->sync_id % EXTERNAL_DISPLAY_BACK_BUFFERS];
    } else {
        return NULL;
    }
}

int set_display_contents(omap_hwc_device_t *hwc_dev, size_t num_displays, hwc_display_contents_1_t **displays) {
    size_t i;

    if (num_displays > MAX_DISPLAYS)
        num_displays = MAX_DISPLAYS;

    for (i = 0; i < num_displays; i++) {
        if (hwc_dev->displays[i]) {
            display_t *display = hwc_dev->displays[i];
            display->contents = displays[i];

            gather_layer_statistics(hwc_dev, display->contents, &display->layer_stats);
        }

    }

    for ( ; i < MAX_DISPLAYS; i++) {
        if (hwc_dev->displays[i])
            hwc_dev->displays[i]->contents = NULL;
    }

    return 0;
}

int get_display_configs(omap_hwc_device_t *hwc_dev, int disp, uint32_t *configs, size_t *numConfigs)
{
    // HACK: to enable HDMI till hotplug propagation is supported
    if (disp != HWC_DISPLAY_PRIMARY)
        return -EINVAL;

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

static int get_layer_stack(omap_hwc_device_t *hwc_dev, int disp, uint32_t *stack)
{
    hwc_layer_stack_t stackInfo = {.dpy = disp};
    void *param = &stackInfo;
    int err = hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERSTACK, &param, sizeof(stackInfo));
    if (err)
        return err;

    *stack = stackInfo.stack;

    return 0;
}

uint32_t get_display_mode(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return DISP_MODE_INVALID;

    if (disp == HWC_DISPLAY_PRIMARY)
        return DISP_MODE_PRESENTATION;

    display_t *display = hwc_dev->displays[disp];

    //HACK: to enable HDMI till the hotplug propagation is supported
    //since display object is created, we can assume legacy mode
    return DISP_MODE_LEGACY;

    if (!display->contents)
        return DISP_MODE_INVALID;

    if (!(display->contents->flags & HWC_EXTENDED_API) || !hwc_dev->procs || !hwc_dev->procs->extension_cb)
        return DISP_MODE_LEGACY;

    uint32_t primaryStack, stack;
    int err;

    err = get_layer_stack(hwc_dev, HWC_DISPLAY_PRIMARY, &primaryStack);
    if (err)
        return DISP_MODE_INVALID;

    err = get_layer_stack(hwc_dev, disp, &stack);
    if (err)
        return DISP_MODE_INVALID;

    if (stack != primaryStack)
        return DISP_MODE_PRESENTATION;

    return DISP_MODE_LEGACY;
}

bool is_hdmi_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return false;

    return hwc_dev->displays[disp]->type == DISP_TYPE_HDMI;
}

bool is_external_display_mirroring(omap_hwc_device_t *hwc_dev)
{
    if (!hwc_dev->displays[HWC_DISPLAY_EXTERNAL])
        return false;

    external_display_t *ext = NULL;
    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->type == DISP_TYPE_HDMI)
        ext = &((external_hdmi_display_t*) hwc_dev->displays[HWC_DISPLAY_EXTERNAL])->ext;

    if (ext && ext->is_mirroring)
        return true;

    return false;
}

int blank_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return -EINVAL;

    int err = 0;

    switch (disp) {
    case HWC_DISPLAY_PRIMARY:
        err = ioctl(hwc_dev->fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN);
        break;
    case HWC_DISPLAY_EXTERNAL:
        if (is_hdmi_display(hwc_dev, disp))
            err = ioctl(hwc_dev->hdmi_fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN);
        break;
    default:
        err = -EINVAL;
        break;
    }

    if (err)
        ALOGW("Failed to blank display %d (%d)", disp, err);

    return err;
}

int unblank_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return -EINVAL;

    int err = 0;

    switch (disp) {
    case HWC_DISPLAY_PRIMARY:
        err = ioctl(hwc_dev->fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
        break;
    case HWC_DISPLAY_EXTERNAL:
        if (is_hdmi_display(hwc_dev, disp))
            err = ioctl(hwc_dev->hdmi_fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
        break;
    default:
        err = -EINVAL;
        break;
    }

    if (err)
        ALOGW("Failed to unblank display %d (%d)", disp, err);

    return err;
}

void free_displays(omap_hwc_device_t *hwc_dev)
{
    /* Make sure that we don't leak ION memory that might be allocated by external display */
    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) {
        if (is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL))
            remove_external_hdmi_display(hwc_dev);
    }

    int i;
    for (i = 0; i < MAX_DISPLAYS; i++)
        free_display(hwc_dev->displays[i]);
}
