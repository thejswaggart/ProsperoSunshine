// SPDX-License-Identifier: GPL-3.0-only
// A capture backend that replays a recorded Annex B H.264 file at a fixed frame rate, looping from its first IDR.
// Portable: it stands in for the console's encoder while the rest of the host is brought up on a new platform (the PS4
// port's first milestone), and lets the stream path be tested without a capture.
#pragma once
#include "capture/capture_session.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace prospero {

// One access unit of the file: its bytes (start codes included) and whether it holds an IDR slice.
struct AccessUnit {
    std::size_t offset = 0, size = 0;
    bool idr = false;
};

// Splits an Annex B H.264 stream into access units. A new unit starts at an access unit delimiter, SEI, SPS or PPS, or at
// a slice whose first_mb_in_slice is 0, once the current unit holds a slice. Bytes before the first NAL unit are skipped.
std::vector<AccessUnit> split_access_units(std::span<const uint8_t> stream);

// The first SPS and the first PPS of the stream, each with a 4-byte start code; empty when either is missing.
std::vector<uint8_t> h264_parameter_sets(std::span<const uint8_t> stream);

class ReplayBackend final : public CaptureBackend {
public:
    using clock = std::chrono::steady_clock;

    // The file is read at open(). fps: the rate the units are handed out at, 1 to 240.
    ReplayBackend(std::filesystem::path file, int fps);
    // For tests: replays `stream` instead of a file.
    ReplayBackend(std::vector<uint8_t> stream, int fps);

    // 0, or -1 when the file can't be read, holds no IDR, or has no SPS/PPS.
    int open() override;
    int headers(void *destination, size_t capacity) override;
    // The next unit once it is due, else 0. -1 when the unit doesn't fit or the backend isn't open.
    int read(void *destination, size_t capacity, uint8_t descriptor[0x88]) override;
    int close() override;
    // The next unit handed out is the next IDR in the file (from the start after the last one).
    int request_keyframe() override;

    // For tests: the time read() takes as now. Defaults to clock::now().
    void set_clock(clock::time_point (*now)()) { now_ = now; }
    std::size_t unit_count() const { return units_.size(); }

private:
    std::size_t next_idr(std::size_t from) const;

    std::filesystem::path file_;
    std::vector<uint8_t> stream_;
    std::vector<AccessUnit> units_;
    std::vector<uint8_t> headers_;
    std::chrono::nanoseconds period_;
    clock::time_point (*now_)() = &clock::now;
    clock::time_point due_{};
    std::size_t next_ = 0;
    bool keyframe_wanted_ = false;
    bool open_ = false;
};

}
