// SPDX-License-Identifier: GPL-3.0-only
// One PS4 screen capture through PBS-Payload (pinned in deps.lock): its Avcap calls run inside SceRemotePlay, and H.264
// comes back to this process. pbs_session.c follows the session PBS's own PS4 stream payload runs (payload/src/platform/
// ps4/main.c) step for step: begin, Init, OpenVideoEx with the native retry on 0x80a70008, the pre-Start wait for
// SPS/PPS, Start, nonblocking ReadVideo, then Stop, Close, Term and end. One session at a time per process.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens a 720p60 capture at bitrate_kbps (clamped to PBS's 1000..15000) and writes its SPS and PPS, each behind a 4-byte
// start code, to headers. Returns their length, or a negative step code (logged): -1 a session is open already, -2
// firmware unsupported, -3 begin refused, -4 Init, -5 request, -6 OpenVideoEx, -7 no SPS/PPS, -8 Start. log_path: PBS's
// own log for this session, replaced each time.
int ps_pbs_open(const char *log_path, unsigned bitrate_kbps, uint8_t *headers, size_t capacity);

// The next encoded frame (Annex B) into destination: its length, 0 when none is ready, negative when the capture is lost
// (ReadVideo failed, or no frame for 3 s). A ring overrun asks for an IDR and returns 0.
int ps_pbs_read(void *destination, size_t capacity);

// Asks the encoder for an IDR. 0 or negative.
int ps_pbs_keyframe(void);

// Stops and releases whatever ps_pbs_open set up; safe to call repeatedly. 0, or negative if a release step failed (PBS
// then refuses the next begin until the host restarts: it won't risk two owners of the encoder).
int ps_pbs_close(void);

#ifdef __cplusplus
}
#endif
