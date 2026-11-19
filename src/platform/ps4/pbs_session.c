// SPDX-License-Identifier: GPL-3.0-only
// pbs_session.h. Built with PBS's own stream defines (cmake/ps4.cmake), against its sources in .cache/deps/src.
#include "platform/ps4/pbs_session.h"
#include "platform/ps4/capture/capture.h"
#include "platform/ps4/capture/request.h"
#include "platform/ps4/system/firmware.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps4/kernel.h>

#define PS_PBS_HEIGHT 720
#define PS_PBS_READ_LIMIT 0x200000  // PBS reads into 2 MiB
#define PS_PBS_AVCAP_BUSY 0x80a70008u
#define PS_PBS_RING_OVERRUN 0x80a70004u

static FILE *session_log;
static void *handle;
static void *audio_handle;
static int began, initialized, started;
static uint64_t last_frame_ms;
static uint64_t packets;

static uint64_t milliseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void note(const char *format, const char *what, int value) {
    if (!session_log) return;
    fprintf(session_log, format, what, value);
    fflush(session_log);
}

int ps_pbs_close(void) {
    int failed = 0;
    ps_pbs_audio_close();  // release the audio channel before the video session
    if (started) {
        const int rc = pbs_ps4_stop(handle);
        note("%s -> %08x\n", "Stop", rc);
        if (rc < 0) failed = -1;
        started = 0;
    }
    if (handle) {
        const int rc = pbs_ps4_close(handle);
        note("%s -> %08x\n", "Close", rc);
        if (rc < 0) failed = -1;
        handle = NULL;
    }
    if (initialized) {
        const int rc = pbs_ps4_term();
        note("%s -> %08x\n", "Term", rc);
        if (rc < 0) failed = -1;
        initialized = 0;
    }
    if (began) {
        pbs_ps4_capture_end();
        began = 0;
    }
    if (session_log) {
        fprintf(session_log, "session end: packets=%llu released=%s\n", (unsigned long long)packets,
                failed ? "WITH ERRORS (next begin refused)" : "ok");
        fclose(session_log);
        session_log = NULL;
    }
    return failed;
}

int ps_pbs_open(const char *log_path, unsigned bitrate_kbps, uint8_t *headers, size_t capacity) {
    if (began || session_log) return -1;
    // Sessions append, so a failure isn't lost to the client's retries that follow it; past 1 MB the file starts anew.
    struct stat st;
    session_log = fopen(log_path, stat(log_path, &st) == 0 && st.st_size > 1024 * 1024 ? "w" : "a");
    packets = 0;
    const uint32_t firmware_version = kernel_get_fw_version();
    if (bitrate_kbps < 1000) bitrate_kbps = 1000;
    if (bitrate_kbps > 15000) bitrate_kbps = 15000;
    if (session_log) {
        char when[32] = "";
        const time_t now = time(NULL);
        struct tm local;
        if (localtime_r(&now, &local)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
        fprintf(session_log, "\n=== %s ProsperoSunshine PBS session pid=%d fw=%08x %dp60 bitrate=%u kbps\n", when, (int)getpid(),
                firmware_version, PS_PBS_HEIGHT, bitrate_kbps);
        fflush(session_log);
    }
    // PBS finds its kernel layout at run time; without one it makes no kernel patches and no capture.
    if (!pbs_ps4_firmware_find(firmware_version)) {
        note("%s (fw=%08x)\n", "PS4 kernel layout unavailable or unsupported", (int)firmware_version);
        ps_pbs_close();
        return -2;
    }
    FILE *pbs_log = session_log ? session_log : stderr;
    if (pbs_ps4_capture_begin(pbs_log)) {
        note("%s\n", "capture begin refused: unresolved previous ownership/teardown", 0);
        ps_pbs_close();
        return -3;
    }
    began = 1;
    // Right after a previous host process let the capture go, its SceRemotePlay worker and gates can still be winding
    // down, and the first Init collides (seen once as Init -> error, which then poisoned that whole host load). PBS's
    // remote Init rolls its own control/attach back on failure, so a bounded retry is safe and mirrors PBS's own
    // transient-busy retry on OpenVideoEx. Give it up to ~2 s before giving up for good.
    int rc = -1;
    for (unsigned attempt = 1; attempt <= 5; ++attempt) {
        rc = pbs_ps4_init();
        note("%s -> %08x\n", attempt == 1 ? "Init" : "Init (retry)", rc);
        if (rc >= 0) break;
        if (attempt < 5) usleep(150000);
    }
    if (rc < 0) {
        ps_pbs_close();
        return -4;
    }
    initialized = 1;
    uint32_t params[PBS_PS4_VIDEO_REQUEST_WORDS];
    if (pbs_ps4_make_video_request_at_bitrate(params, PS_PBS_HEIGHT, 0, 0, bitrate_kbps)) {
        note("%s\n", "video request rejected by PBS's ABI guard", 0);
        ps_pbs_close();
        return -5;
    }
    // As native RemotePlay and PBS: retry only Avcap's "temporarily unavailable" with no handle returned, within ~2 s.
    // Any other failure, or a returned handle, is not retried: a blind reopen could create a second owner.
    const uint64_t open_began = milliseconds();
    unsigned busy = 0;
    for (;;) {
        rc = pbs_ps4_open(&handle, params);
        if ((uint32_t)rc != PS_PBS_AVCAP_BUSY || handle || milliseconds() - open_began >= 2016u) break;
        ++busy;
        usleep(16000);
    }
    if (busy) note("%s %d times\n", "OpenVideoEx busy, retried", (int)busy);
    note("%s -> %08x\n", "OpenVideoEx", rc);
    if (rc < 0 || !handle) {
        handle = NULL;  // a failed Open is quarantined, never closed or reopened blindly (PBS main.c)
        ps_pbs_close();
        return -6;
    }
    // Before Start, wait for the encoder's headers as native RemotePlay does.
    unsigned char sps[128], pps[128];
    int sps_len = 0, pps_len = 0;
    const uint64_t until = milliseconds() + 2000;
    do {
        if (sps_len <= 0) sps_len = pbs_ps4_info(handle, 1, sps, sizeof sps);
        if (pps_len <= 0) pps_len = pbs_ps4_info(handle, 2, pps, sizeof pps);
        if (sps_len > 0 && pps_len > 0) break;
        usleep(5000);
    } while (milliseconds() < until);
    if (session_log) fprintf(session_log, "Pre-Start GetInfo SPS=%d PPS=%d\n", sps_len, pps_len);
    const size_t needed = 8 + (size_t)(sps_len > 0 ? sps_len : 0) + (size_t)(pps_len > 0 ? pps_len : 0);
    if (sps_len <= 0 || sps_len > 128 || pps_len <= 0 || pps_len > 128 || (sps[0] & 31) != 7 || (pps[0] & 31) != 8 ||
        needed > capacity) {
        ps_pbs_close();
        return -7;
    }
    static const uint8_t start_code[4] = {0, 0, 0, 1};
    memcpy(headers, start_code, 4);
    memcpy(headers + 4, sps, (size_t)sps_len);
    memcpy(headers + 4 + sps_len, start_code, 4);
    memcpy(headers + 8 + sps_len, pps, (size_t)pps_len);
    rc = pbs_ps4_start(handle);
    note("%s -> %08x\n", "Start", rc);
    if (rc < 0) {
        ps_pbs_close();
        return -8;
    }
    started = 1;
    last_frame_ms = milliseconds();
    return (int)needed;
}

int ps_pbs_read(void *destination, size_t capacity) {
    if (!started) return -1;
    unsigned char info[0x40] = {0};
    // flags=1: the nonblocking ring read PBS uses; the native wait can't be cancelled.
    const int rc = pbs_ps4_read(handle, destination, capacity < PS_PBS_READ_LIMIT ? capacity : PS_PBS_READ_LIMIT, info, 1);
    if ((uint32_t)rc == PS_PBS_RING_OVERRUN) {
        // The reader resynchronized; the frames after it need a new IDR.
        note("%s at packet %d, requesting IDR\n", "Avcap ring overrun", (int)packets);
        return pbs_ps4_keyframe(handle) < 0 ? -1 : 0;
    }
    if (rc < 0) {
        note("%s rc=%08x\n", "ReadVideo failed", rc);
        return -1;
    }
    if (rc > 0) {
        if (session_log && (packets == 0 || packets == 59)) {
            fprintf(session_log, "packet[%llu] len=%d prefix=", (unsigned long long)packets, rc);
            for (int i = 0; i < 16 && i < rc; ++i) fprintf(session_log, "%02x", ((const uint8_t *)destination)[i]);
            fputc('\n', session_log);
            fflush(session_log);
        }
        ++packets;
        last_frame_ms = milliseconds();
        return rc;
    }
    if (milliseconds() - last_frame_ms > 3000) {
        note("%s after %d packets\n", "ReadVideo stalled for 3 s", (int)packets);
        return -1;
    }
    return 0;
}

int ps_pbs_keyframe(void) {
    if (!started) return -1;
    const int rc = pbs_ps4_keyframe(handle);
    if (rc < 0) note("%s -> %08x\n", "Keyframe", rc);
    return rc < 0 ? -1 : 0;
}

int ps_pbs_audio_open(void) {
    if (!started || audio_handle) return -1;
    // 16-byte OpenAudioEx config, taken verbatim from a live Remote Play session (ps4debug on SceRemotePlay pid 47: found
    // remoteplay's call site OpenAudioEx(&handle, &obj+0x204) and read the exact bytes it passes — three live buffers all
    // agreed). RemotePlay uses OpenAudioEx, NOT the plain OpenAudio, and this is the config the AV service accepts:
    //   [0x00]=0  [0x04]=0  [0x08]=1  [0x0C]=0x1f400 (128000)
    // +0x04 is 0 here, so the libSceAvcap config[+4]==2 branch (which needs extra caller setup we don't have — the source
    // of our earlier 0x80a90005) is skipped; this is the normal full game-audio session. pbs_ps4_audio_open resolves
    // sceVshAvcapOpenAudioEx and Starts the returned handle, as RemotePlay does.
    static const uint8_t config[16] = {
        0x00, 0x00, 0x00, 0x00,  // [0x00] = 0
        0x00, 0x00, 0x00, 0x00,  // [0x04] = 0
        0x01, 0x00, 0x00, 0x00,  // [0x08] = 1
        0x00, 0xf4, 0x01, 0x00,  // [0x0C] = 128000 (0x1f400)
    };
    const int rc = pbs_ps4_audio_open(&audio_handle, config, sizeof config);
    note("%s -> %08x\n", "audio open", rc);  // PBS logs the handle and the OpenAudioEx/Start return codes
    return rc < 0 ? rc : 0;
}

int ps_pbs_audio_read(void *destination, size_t capacity) {
    if (!audio_handle) return -1;
    unsigned char info[0x40] = {0};
    return pbs_ps4_audio_read(audio_handle, destination, capacity, info);
}

void ps_pbs_audio_close(void) {
    if (!audio_handle) return;
    pbs_ps4_audio_close(audio_handle);
    audio_handle = NULL;
}
