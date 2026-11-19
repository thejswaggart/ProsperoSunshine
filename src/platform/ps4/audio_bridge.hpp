// SPDX-License-Identifier: GPL-3.0-only
// A tiny lock-protected float ring between the PS4 capture thread and Sunshine's audio mic. PBS delivers game audio as
// interleaved float32 stereo at 48 kHz in [-1,1] (confirmed on fw 14.00 from a live ReadAudio dump) — exactly the format
// Sunshine's Opus float encoder wants, so no conversion, scaling or resampling: the capture thread pushes the raw floats
// here and the mic pulls fixed-size frames out. The capture thread owns every pbs_session call (audio and video reads share
// one injection context and must not run concurrently); this bridge is the only thing the mic touches, so the two threads
// never race on the console. One stream at a time, so a single global instance is enough.
#pragma once
#include <cstddef>
#include <mutex>
#include <vector>

namespace prospero::audio_bridge {

struct State {
    std::mutex mutex;
    std::vector<float> ring;  // interleaved float samples in [-1,1], oldest first
    size_t cap = 0;           // max samples retained; beyond this the oldest are dropped to bound latency
    bool active = false;
};

inline State &state() {
    static State s;
    return s;
}

// The capture thread calls this when the audio channel opens. cap_samples bounds buffered latency (oldest dropped past it).
inline void open(size_t cap_samples) {
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.ring.clear();
    s.ring.reserve(cap_samples);
    s.cap = cap_samples;
    s.active = true;
}

inline void close() {
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.active = false;
    s.ring.clear();
    s.ring.shrink_to_fit();
}

// Producer (capture thread): append interleaved floats; drop the oldest beyond cap so latency can't grow without bound.
inline void push(const float *samples, size_t count) {
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.active || count == 0) return;
    s.ring.insert(s.ring.end(), samples, samples + count);
    if (s.cap && s.ring.size() > s.cap) s.ring.erase(s.ring.begin(), s.ring.begin() + (s.ring.size() - s.cap));
}

// Consumer (mic): take exactly count interleaved floats into out, padding the tail with silence on underflow. Returns the
// number of real (non-padded) samples delivered, so the caller can tell a live frame from a filled-in one.
inline size_t pull(float *out, size_t count) {
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    const size_t have = s.ring.size() < count ? s.ring.size() : count;
    for (size_t i = 0; i < have; ++i) out[i] = s.ring[i];
    for (size_t i = have; i < count; ++i) out[i] = 0.0f;
    if (have) s.ring.erase(s.ring.begin(), s.ring.begin() + have);
    return have;
}

}  // namespace prospero::audio_bridge
