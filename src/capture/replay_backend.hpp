// SPDX-License-Identifier: GPL-3.0-only
// A capture backend that replays a recorded Annex B H.264 file at a fixed frame rate, looping from its first IDR.
// Portable: it stands in for the console's encoder while the rest of the host is brought up on a new platform (the PS4
// port's first milestone), and lets the stream path be tested without a capture.
//
// The file is never held in memory: open() indexes its access units in one pass over 1 MiB chunks, and each read() copies
// one unit from the file straight into the caller's buffer. A payload's heap on the console is small (a 7.5 MB clip read
// whole ran it out on the PS4).
#pragma once
#include "capture/capture_session.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace prospero {

// One access unit of the stream: where its bytes are (start codes included) and whether it holds an IDR slice.
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

    // The file is indexed at open() and read a unit at a time. fps: the rate the units are handed out at, 1 to 240.
    ReplayBackend(std::filesystem::path file, int fps);
    // For tests: replays `stream` from memory instead of a file.
    ReplayBackend(std::vector<uint8_t> stream, int fps);

    // 0, or -1 when the file can't be read, holds no IDR, has no SPS/PPS, or memory for the index runs out.
    int open() override;
    int headers(void *destination, size_t capacity) override;
    // The next unit once it is due, else 0. -1 when the unit doesn't fit, the file can't be read, or the backend isn't open.
    int read(void *destination, size_t capacity, uint8_t descriptor[0x88]) override;
    int close() override;
    // The next unit handed out is the next IDR in the file (from the start after the last one).
    int request_keyframe() override;

    // For tests: the time read() takes as now. Defaults to clock::now().
    void set_clock(clock::time_point (*now)()) { now_ = now; }
    std::size_t unit_count() const { return units_.size(); }

private:
    std::size_t next_idr(std::size_t from) const;
    bool copy_bytes(std::size_t offset, std::size_t size, void *destination);

    std::filesystem::path path_;
    std::ifstream file_;
    std::vector<uint8_t> memory_;  // the test stream; empty for a file
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
