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

#ifndef __HWC_DEV__
#define __HWC_DEV__

#include <stdint.h>
#include <stdbool.h>

#include <pthread.h>

#include <hardware/hwcomposer.h>
#include <linux/bltsville.h>
#include <video/dsscomp.h>
#include <video/omap_hwc.h>

#include "hal_public.h"
#include "rgz_2d.h"
#include "display.h"

struct ext_transform {
    uint8_t rotation : 3;          /* 90-degree clockwise rotations */
    uint8_t hflip    : 1;          /* flip l-r (after rotation) */
    uint8_t enabled  : 1;          /* cloning enabled */
};
typedef struct ext_transform ext_transform_t;

/* cloning support and state */
struct omap_hwc_ext {
    /* support */
    ext_transform_t mirror;             /* mirroring settings */
    float lcd_xpy;                      /* pixel ratio for UI */
    bool avoid_mode_change;             /* use HDMI mode used for mirroring if possible */

    /* state */
    bool hdmi_state;                    /* whether HDMI is connected */
    ext_transform_t current;            /* current settings */
    ext_transform_t last;               /* last-used settings */

    /* configuration */
    uint32_t last_xres_used;            /* resolution and pixel ratio used for mode selection */
    uint32_t last_yres_used;
    uint32_t last_mode;                 /* 2-s complement of last HDMI mode set, 0 if none */
    uint32_t mirror_mode;               /* 2-s complement of mode used when mirroring */
    float last_xpy;
    uint16_t width;                     /* external screen dimensions */
    uint16_t height;
    uint32_t xres;                      /* external screen resolution */
    uint32_t yres;
    float m[2][3];                      /* external transformation matrix */
    hwc_rect_t mirror_region;           /* region of screen to mirror */
};
typedef struct omap_hwc_ext omap_hwc_ext_t;

enum bltpolicy {
    BLTPOLICY_DISABLED = 0,
    BLTPOLICY_DEFAULT = 1,    /* Default blit policy */
    BLTPOLICY_ALL,            /* Test mode to attempt to blit all */
};

enum bltmode {
    BLTMODE_PAINT = 0,    /* Attempt to blit layer by layer */
    BLTMODE_REGION = 1,   /* Attempt to blit layers via regions */
};

struct omap_hwc_module {
    hwc_module_t base;

    IMG_framebuffer_device_public_t *fb_dev;
};
typedef struct omap_hwc_module omap_hwc_module_t;

struct counts {
    uint32_t max_hw_overlays;
    uint32_t max_scaling_overlays;
};
typedef struct counts counts_t;

struct omap_hwc_device {
    /* static data */
    hwc_composer_device_1_t base;
    hwc_procs_t *procs;
    pthread_t hdmi_thread;
    pthread_mutex_t lock;

    struct dsscomp_platform_info platform_limits;
    IMG_framebuffer_device_public_t *fb_dev;
    int fb_fd;                   /* file descriptor for /dev/fb0 */
    int dsscomp_fd;              /* file descriptor for /dev/dsscomp */
    int hdmi_fb_fd;              /* file descriptor for /dev/fb1 */
    int pipe_fds[2];             /* pipe to event thread */

    int img_mem_size;           /* size of fb for hdmi */
    void *img_mem_ptr;          /* start of fb for hdmi */

    int flags_rgb_order;
    int flags_nv12_only;
    float upscaled_nv12_limit;

    int force_sgx;
    omap_hwc_ext_t ext;         /* external mirroring data */
    int idle;

    int primary_transform;
    int primary_rotation;
    hwc_rect_t primary_region;

    buffer_handle_t *buffers;
    bool use_sgx;
    bool swap_rb;
    uint32_t post2_layers;       /* buffers used with DSS pipes*/
    uint32_t post2_blit_buffers; /* buffers used with blit */
    int ext_ovls;                /* # of overlays on external display for current composition */
    int ext_ovls_wanted;         /* # of overlays that should be on external display for current composition */
    int last_ext_ovls;           /* # of overlays on external/internal display for last composition */
    int last_int_ovls;

    enum bltmode blt_mode;
    enum bltpolicy blt_policy;

    uint32_t blit_flags;
    int blit_num;
    struct omap_hwc_data comp_data; /* This is a kernel data structure */
    struct rgz_blt_entry blit_ops[RGZ_MAX_BLITS];

    counts_t counts;

    bool use_sw_vsync;

    display_t *displays[MAX_DISPLAYS];

    struct dsscomp_display_info fb_dis; /* variable-sized type; should be at end of struct */
};

bool can_scale(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, bool is_2d,
               struct dsscomp_display_info *dis, struct dsscomp_platform_info *limits,
               uint32_t pclk);

#endif
