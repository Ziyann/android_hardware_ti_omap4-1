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

#ifndef __DISPLAY__
#define __DISPLAY__

#include <stdint.h>
#include <stdbool.h>

#include "blitter.h"
#include "layer.h"

#define MAX_DISPLAYS 3
#define MAX_DISPLAY_CONFIGS 32
#define EXTERNAL_DISPLAY_BACK_BUFFERS 2

struct ion_handle;
typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct omap_hwc_device omap_hwc_device_t;

struct display_transform {
    uint8_t rotation : 3;          /* 90-degree clockwise rotations */
    uint8_t hflip    : 1;          /* flip l-r (after rotation) */
    bool scaling;
    hwc_rect_t region;
    float matrix[2][3];
};
typedef struct display_transform display_transform_t;

struct display_config {
    int xres;
    int yres;
    int fps;
    int xdpi;
    int ydpi;
};
typedef struct display_config display_config_t;

enum disp_type {
    DISP_TYPE_UNKNOWN,
    DISP_TYPE_LCD,
    DISP_TYPE_HDMI,
    DISP_TYPE_WFD,
};

enum disp_mode {
    DISP_MODE_INVALID,
    DISP_MODE_LEGACY,
    DISP_MODE_PRESENTATION,
};

enum disp_role {
    DISP_ROLE_PRIMARY,
    DISP_ROLE_EXTERNAL,
};

struct composition {
    buffer_handle_t *buffers;
    uint32_t num_buffers;        /* # of buffers used in composition */

    bool use_sgx;
    bool swap_rb;

    uint32_t ovl_ix_base;        /* index of first overlay used in composition */
    uint32_t wanted_ovls;        /* # of overlays required for current composition */
    uint32_t avail_ovls;         /* # of overlays available for current composition */
    uint32_t scaling_ovls;       /* # of overlays available with scaling caps */
    uint32_t used_ovls;          /* # of overlays used in composition */

    blitter_composition_t blitter;

    /* This is a kernel data structure. comp_data and blit_ops should be defined
     * in consecutive memory.
     */
    struct omap_hwc_data comp_data;
    struct rgz_blt_entry blit_ops[RGZ_MAX_BLITS];
};
typedef struct composition composition_t;

struct display {
    uint32_t num_configs;
    display_config_t *configs;
    uint32_t active_config_ix;

    uint32_t type;
    uint32_t role;

    uint32_t mgr_ix;

    hwc_display_contents_1_t *contents;
    layer_statistics_t layer_stats;
    composition_t composition;
    display_transform_t transform;
};
typedef struct display display_t;

struct primary_display {
    //place holder for primary display specific controls
    //could be blitter,  vsync handling etc
    bool use_sw_vsync;
};
typedef struct primary_display primary_display_t;

struct external_display {
    bool is_mirroring; //mirroring or distinct mode
};
typedef struct external_display external_display_t;

struct primary_lcd_display {
    display_t lcd;
    primary_display_t primary;
};
typedef struct primary_lcd_display primary_lcd_display_t;

struct hdmi_display {
    display_t base;
    uint16_t width;         /* external screen dimensions */
    uint16_t height;
    uint32_t current_mode;
    uint32_t last_mode;
    struct dsscomp_videomode mode_db[MAX_DISPLAY_CONFIGS];
};
typedef struct hdmi_display hdmi_display_t;

struct primary_hdmi_display {
    hdmi_display_t hdmi;
    primary_display_t primary;
};
typedef struct primary_hdmi_display primary_hdmi_display_t;

struct external_hdmi_display {
    hdmi_display_t hdmi;
    external_display_t ext;

    /* attributes */
    bool avoid_mode_change;        /* use HDMI mode used for mirroring if possible */
    int ion_fd;
    struct ion_handle *ion_handles[EXTERNAL_DISPLAY_BACK_BUFFERS];
};
typedef struct external_hdmi_display external_hdmi_display_t;

int init_primary_display(omap_hwc_device_t *hwc_dev);
void reset_primary_display(omap_hwc_device_t *hwc_dev);

int add_external_hdmi_display(omap_hwc_device_t *hwc_dev);
void remove_external_hdmi_display(omap_hwc_device_t *hwc_dev);
struct ion_handle *get_external_display_ion_fb_handle(omap_hwc_device_t *hwc_dev);

void detect_virtual_displays(omap_hwc_device_t *hwc_dev, size_t num_displays, hwc_display_contents_1_t **displays);
void set_display_contents(omap_hwc_device_t *hwc_dev, size_t num_displays, hwc_display_contents_1_t **displays);

int get_external_display_id(omap_hwc_device_t *hwc_dev);
int get_display_configs(omap_hwc_device_t *hwc_dev, int disp, uint32_t *configs, size_t *numConfigs);
int get_display_attributes(omap_hwc_device_t *hwc_dev, int disp, uint32_t config, const uint32_t *attributes, int32_t *values);
uint32_t get_display_mode(omap_hwc_device_t *hwc_dev, int disp);

bool is_hdmi_display(omap_hwc_device_t *hwc_dev, int disp);
bool is_external_display_mirroring(omap_hwc_device_t *hwc_dev, int disp);

int blank_display(omap_hwc_device_t *hwc_dev, int disp);
int unblank_display(omap_hwc_device_t *hwc_dev, int disp);

void free_displays(omap_hwc_device_t *hwc_dev);

#endif
