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

/* Injects one frame into the virtual pad addressed by `handle` (the /dev/hid ADD handle, which equals the DeviceId
 * libScePad uses). buttons are PS_PAD_* bits; sticks and triggers are 0..255, 128 centre. The timestamp and counter are
 * stamped here because the PS4 de-dups an unchanged sample. Returns scePadVirtualDeviceInsertData's result. */
int ps_scepad_inject(int32_t handle, uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2);

#ifdef __cplusplus
}
#endif
