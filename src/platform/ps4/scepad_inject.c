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

/* SDL/Moonlight sensor frame -> DualShock 4 native frame.
 *
 * Moonlight sends accelerometer and gyro in SDL's controller convention (X right, Y up, Z out of the
 * face toward the player). libScePad expects the DS4's own sensor frame, which differs by a signed axis
 * permutation. Injecting one frame's numbers into the other field-for-field makes a game read motion on
 * the wrong axes: gyro-aim tilts the wrong way, roll drives pitch, and the gravity vector points wrong so
 * the aim drifts and only settles at certain orientations.
 *
 * gyro is a pseudovector: a frame change that flips handedness (a reflection, det -1) would need the gyro
 * negated relative to the accel. This frame is a proper rotation (det +1), so the two tables are equal.
 *
 * Each table maps a destination axis (0:x 1:y 2:z of the DS4 vector) to {source axis in the SDL vector,
 * sign}. On-console tests showed changing the gyro routing alone did nothing to the aim, while changing
 * the accel frame rotated it and moved the drift: this game derives aim from the fused ORIENTATION, which
 * is accel(gravity)-dominated for the slow tilts tested. So the aim is set by the accel frame, and both
 * sensors must share one frame for the fusion to be consistent.
 *
 * The frame is a rotation that (a) keeps rest gravity on the DS4 up-axis (SDL +Y at rest -> DS4 +Z) so
 * there is no drift, and (b) routes pitch-tilt -> vertical aim and roll-tilt -> horizontal aim. That is
 * the cyclic rotation x<-z, y<-x, z<-y (a proper rotation, det +1, so gyro keeps no extra pseudovector
 * sign). Signs give nose-up -> aim up (before the game's own "Inverted Y", which the player keeps on) and
 * left-side-down -> aim left.
 *
 * Verify by re-test: if an aim axis is mirrored, negate that one entry in BOTH tables (keep them equal).
 * If the drift returns, the gravity (up-axis) sign is wrong - negate ACCEL_MAP[2] (and GYRO_MAP[2]).
 *
 * To flip one axis, negate its sign; to re-route one, change its source index. */
typedef struct { int src; float sign; } axis_map;
static const axis_map GYRO_MAP[3]  = { {2, +1.0f}, {0, +1.0f}, {1, +1.0f} };  /* x<-z, y<-x, z<-y */
static const axis_map ACCEL_MAP[3] = { {2, +1.0f}, {0, +1.0f}, {1, +1.0f} };  /* x<-z, y<-x, z<-y */

static void remap(const axis_map m[3], const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = m[i].sign * in[m[i].src];
}

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
    // Re-map from Moonlight's SDL sensor frame into the DS4's native frame (GYRO_MAP / ACCEL_MAP above).
    float g[3], a[3];
    remap(GYRO_MAP, gyro, g);
    remap(ACCEL_MAP, accel, a);
    data.vel.x = g[0];
    data.vel.y = g[1];
    data.vel.z = g[2];
    data.accel.x = a[0];
    data.accel.y = a[1];
    data.accel.z = a[2];
    data.connected = 1;
    // The PS4 treats an unchanged timestamp/counter as "no new data", so both advance each frame.
    stamp += 33000u;  // ~one 60 Hz frame in microseconds
    count += 1;
    data.timestamp = stamp;
    data.count = count;
    return scePadVirtualDeviceInsertData(handle, &data);
}
