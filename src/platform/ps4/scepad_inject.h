/* SPDX-License-Identifier: GPL-3.0-or-later
 * PS4 controller injection through libScePad's virtual-device API. The pad is created and its handle obtained through
 * /dev/hid (platform/ps5/pad.c), which works on PS4 and needs no klog; the raw /dev/hid INSERT ioctl, however, is
 * rejected on PS4 (EIO), so the per-frame input goes through scePadVirtualDeviceInsertData instead -- the path the
 * ps5-gamepad-bridge payload uses on PS4. The ScePadData layout and the rising timestamp/counter are from that project
 * (client/gc_types.h, client/gc_main.c, GPL-3.0-or-later). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* scePadInit once (on PS4 it does not hang, unlike the PS5 payload note in pad.c). 0 or an "already initialized" code. */
int ps_scepad_init(void);

/* Deletes libScePad's virtual-device entry for `handle` (the /dev/hid ADD handle / DeviceId). ps_pad_close removes the HID
 * side, but libScePad keeps its own entry (fed by InsertData), which the console shows as a leftover controller; call this
 * alongside ps_pad_close to clear it. Returns scePadVirtualDeviceDeleteDevice's result. */
int ps_scepad_remove(int32_t handle);

/* Injects one frame into the virtual pad addressed by `handle` (the /dev/hid ADD handle, which equals the DeviceId
 * libScePad uses). buttons are PS_PAD_* bits; sticks and triggers are 0..255, 128 centre. `gyro` is angular velocity in
 * rad/s and `accel` acceleration in g (a level pad at rest is gyro {0,0,0}, accel {0,1,0}); orientation is left level.
 * The timestamp and counter are stamped here because the PS4 de-dups an unchanged sample. Returns
 * scePadVirtualDeviceInsertData's result. */
int ps_scepad_inject(int32_t handle, uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2,
                     const float gyro[3], const float accel[3]);

#ifdef __cplusplus
}
#endif
