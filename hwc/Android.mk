LOCAL_PATH := $(call my-dir)

# HAL module implementation, not prelinked and stored in
# hw/<HWCOMPOSE_HARDWARE_MODULE_ID>.<ro.product.board>.so
include $(CLEAR_VARS)
LOCAL_ARM_MODE := arm
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/../vendor/lib/hw
LOCAL_SHARED_LIBRARIES := liblog libEGL libcutils libutils libhardware libhardware_legacy libz \
                          libedid libdsswbhal

LOCAL_SRC_FILES := \
    blitter.c \
    color_fmt.c \
    display.c \
    dsscomp.c \
    dump.c \
    hwc.c \
    layer.c \
    rgz_2d.c \
    sw_vsync.c \
    utils.c

LOCAL_CFLAGS := -Wall -Werror

ifeq ($(BOARD_USE_TI_LIBION),true)
LOCAL_SHARED_LIBRARIES += libion_ti
LOCAL_CFLAGS += -DUSE_TI_LIBION
else
LOCAL_SHARED_LIBRARIES += libion
LOCAL_SRC_FILES += ../libion/ion_ti_custom.c
LOCAL_C_INCLUDES += $(HARDWARE_TI_OMAP4_BASE)/libion
endif

ifdef OMAP_ENHANCEMENT_S3D
LOCAL_CFLAGS += -DOMAP_ENHANCEMENT_S3D
endif

ifdef OMAP_ENHANCEMENT_HWC_EXTENDED_API
LOCAL_CFLAGS += -DOMAP_ENHANCEMENT_HWC_EXTENDED_API
endif

LOCAL_STATIC_LIBRARIES := libpng

LOCAL_MODULE_TAGS := optional

LOCAL_MODULE := hwcomposer.$(TARGET_BOARD_PLATFORM)
LOCAL_CFLAGS += -DLOG_TAG=\"ti_hwc\"
LOCAL_C_INCLUDES += external/libpng external/zlib

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/../edid/inc \
    $(LOCAL_PATH)/../include \
    $(LOCAL_PATH)/../libdsswb

# LOG_NDEBUG=0 means verbose logging enabled
# LOCAL_CFLAGS += -DLOG_NDEBUG=0
include $(BUILD_SHARED_LIBRARY)
