/* SPDX-License-Identifier: GPL-3.0-or-later
 * scepad_inject.h. ScePadData and the inject stamping are ported from ps5-gamepad-bridge (client/gc_types.h,
 * client/gc_main.c, GPL-3.0-or-later). */
#include "platform/ps4/scepad_inject.h"
#include <string.h>

extern int32_t scePadInit(void);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *pad_data);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);

/* The PS5 ScePadData layout, used verbatim on PS4 (ps5-gamepad-bridge gc_types.h). Buttons, both sticks, the analog
 * triggers and the motion block (angular velocity + acceleration; orientation left level) are populated; the touchpad
 * data is still zeroed. Whether libScePad surfaces the injected motion to a game's scePadReadState is unverified on the
 * PS4 and needs a console check. */
typedef struct {
    uint16_t x, y;
    uint8_t finger;
    uint8_t pad[3];
} sce_pad_touch;
typedef struct {
    uint8_t fingers;
    uint8_t pad1[3];
    uint32_t pad2;
    sce_pad_touch touch[2];
} sce_pad_touch_data;
typedef struct {
    uint32_t buttons;
    struct { uint8_t x, y; } left_stick;
    struct { uint8_t x, y; } right_stick;
    struct { uint8_t l2, r2; } analog;
    uint16_t padding;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } vel;
    struct { float x, y, z; } accel;
    sce_pad_touch_data touch_data;
    uint8_t connected;
    uint8_t align_[3];
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
} sce_pad_data;

static int init_done;

int ps_scepad_init(void) {
    if (init_done) return 0;
    const int rc = scePadInit();
    init_done = 1;
    return rc;
}

int ps_scepad_remove(int32_t handle) {
    // The /dev/hid DELETE (ps_pad_close) removes the HID device, but libScePad keeps its own virtual-device entry for the
    // handle it has been fed through scePadVirtualDeviceInsertData; the console then still lists a leftover controller.
    // scePadVirtualDeviceDeleteDevice clears that entry.
    return scePadVirtualDeviceDeleteDevice(handle);
}

int ps_scepad_inject(int32_t handle, uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2,
                     uint8_t r2, const float gyro[3], const float accel[3]) {
    static uint64_t stamp;
    static uint8_t count;
    sce_pad_data data;
    memset(&data, 0, sizeof data);
    data.buttons = buttons;
    data.left_stick.x = lx;
    data.left_stick.y = ly;
    data.right_stick.x = rx;
    data.right_stick.y = ry;
    data.analog.l2 = l2;
    data.analog.r2 = r2;
    // Orientation is left level (identity quaternion); only the raw sensors are forwarded from Moonlight.
    data.quat.x = 0.0f;
    data.quat.y = 0.0f;
    data.quat.z = 0.0f;
    data.quat.w = 1.0f;
    data.vel.x = gyro[0];
    data.vel.y = gyro[1];
    data.vel.z = gyro[2];
    data.accel.x = accel[0];
    data.accel.y = accel[1];
    data.accel.z = accel[2];
    data.connected = 1;
    // The PS4 treats an unchanged timestamp/counter as "no new data", so both advance each frame.
    stamp += 33000u;  // ~one 60 Hz frame in microseconds
    count += 1;
    data.timestamp = stamp;
    data.count = count;
    return scePadVirtualDeviceInsertData(handle, &data);
}
