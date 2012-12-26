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

#ifndef DSSWB_HAL_H
#define DSSWB_HAL_H

#ifdef __cplusplus

#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Vector.h>
#include <utils/threads.h>

#include <hardware/gralloc.h>
#include <IDSSWBHal.h>

namespace android {

class DSSWBHal : public BnDSSWBHal {
private:

    DSSWBHal();

    status_t initialize();

public:

    virtual ~DSSWBHal();

    static status_t instantiate();

    buffer_handle_t processQueue();
    void captureStarted(buffer_handle_t handle);
    bool capturePending();
    void getConfig(wb_capture_config_t *config);

    // IDSSWBHal interface
    virtual status_t acquireWB(int *wbHandlePtr);
    virtual status_t releaseWB(int wbHandle);
    virtual status_t registerBuffers(int wbHandle, int numBuffers, buffer_handle_t handles[]);
    virtual status_t queue(int wbHandle, int bufIndex);
    virtual status_t dequeue(int wbHandle, int *bufIndex);
    virtual status_t cancelBuffer(int wbHandle, int *bufIndex);
    virtual status_t setConfig(int wbHandle, const wb_capture_config_t &config);
    virtual status_t getConfig(int wbHandle, wb_capture_config_t *config);

private:

    struct BufferSlot {
        enum BufferState {
            QUEUED = 0,
            WRITEBACK = 1,
            DEQUEUED = 2,
        };

        BufferState state;
        native_handle_t *handle;
    };

    void getConfigLocked(wb_capture_config_t *config);

    int mWBHandle;
    Mutex mLock;
    wb_capture_config_t mConfig;

    Vector<BufferSlot> mBufferSlots;

    List<int> mQueueList;
    List<int> mWritebackList;
    List<int> mDequeueList;

    // mDequeueCondition condition used for dequeueBuffer in synchronous mode
    mutable Condition mDequeueCondition;

    gralloc_module_t *mGrallocModule;
};

/* the declaration of functions being used from hwc.c need to be
 * "C" or else the name gets mangled and hwc library will not build
 * against these functions. So two declarations of this function. One in
 * C++ which will create the library with right "C" name and other in
 * C which is for hwc.c when it includes this file.
 */
extern "C" int wb_open();
extern "C" int wb_capture_layer(hwc_layer_1_t *wb_layer);
extern "C" void wb_capture_started(buffer_handle_t handle);
extern "C" int wb_capture_pending();
};
#else
#include <hardware/hwcomposer.h>

extern int wb_open();
extern int wb_capture_layer(hwc_layer_1_t *wb_layer);
extern void wb_capture_started(buffer_handle_t handle);
extern int wb_capture_pending();
#endif // __cplusplus

#endif // DSSWB_HAL_H
