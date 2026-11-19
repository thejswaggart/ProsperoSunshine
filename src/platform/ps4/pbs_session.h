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
// own log for this session, replaced each time. On a reconnect in the same host process the SceRemotePlay daemon PBS
// injects into may have respawned, so the whole open (after a pbs_ps4_capture_reset) is retried for up to a few seconds to
// ride out that respawn; a prior stream no longer poisons the next, so reconnecting needs no payload reload.
int ps_pbs_open(const char *log_path, unsigned bitrate_kbps, uint8_t *headers, size_t capacity);

// The next encoded frame (Annex B) into destination: its length, 0 when none is ready, negative when the capture is lost
// (ReadVideo failed, or no frame for 3 s). A ring overrun asks for an IDR and returns 0.
int ps_pbs_read(void *destination, size_t capacity);

// Asks the encoder for an IDR. 0 or negative.
int ps_pbs_keyframe(void);

// Ends the current Moonlight stream. The capture daemon is kept warm (held, not torn down) so the next stream reuses it,
// unless the capture was lost mid-stream; only the per-stream audio and session log are closed. Returns 0, or negative if a
// release step failed on the not-warm path. Safe to call repeatedly.
int ps_pbs_close(void);

// Releases the warm capture daemon for good. Call once when the host is shutting down, so the capture is handed back to the
// system (native Remote Play can run again and the next host load starts clean). Safe to call with nothing open.
void ps_pbs_shutdown(void);

// Game audio on the running session (call after ps_pbs_open). Opens the Avcap audio channel (sceVshAvcapOpenAudioEx) with
// the 16-byte config Remote Play itself uses (see ps_pbs_audio_open); 0 or a negative error (logged). ps_pbs_close closes it.
int ps_pbs_audio_open(void);
// The next audio frame into destination: its length, 0 when none ready, negative on error. Raw PCM. Not thread-safe with
// ps_pbs_read: both drive the one injection context, so call them from the same thread or serialise them.
int ps_pbs_audio_read(void *destination, size_t capacity);
void ps_pbs_audio_close(void);

#ifdef __cplusplus
}
#endif
