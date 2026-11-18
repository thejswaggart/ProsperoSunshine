// SPDX-License-Identifier: GPL-3.0-only
#include "capture/replay_backend.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace prospero {
namespace {
// One NAL unit: where its start code begins, where its header byte is, and where it ends (the next start code).
struct Nal {
    std::size_t start, header, end;
};

std::vector<Nal> find_nals(std::span<const uint8_t> s) {
    std::vector<Nal> nals;
    for (std::size_t i = 0; i + 2 < s.size();) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            // A 4-byte start code counts its leading zero as part of this unit.
            const std::size_t start = i > 0 && s[i - 1] == 0 ? i - 1 : i;
            if (!nals.empty()) nals.back().end = start;
            nals.push_back({start, i + 3, s.size()});
            i += 3;
        } else {
            ++i;
        }
    }
    // A unit with no header byte (a start code at the very end) is dropped.
    std::erase_if(nals, [&](const Nal &n) { return n.header >= n.end; });
    return nals;
}

int nal_type(std::span<const uint8_t> s, const Nal &n) { return s[n.header] & 0x1f; }
bool is_slice(int type) { return type == 1 || type == 5; }
// first_mb_in_slice is ue(v), and 0 codes as a single 1 bit: the slice starts a picture when the top bit is set.
bool starts_picture(std::span<const uint8_t> s, const Nal &n) { return n.header + 1 < n.end && (s[n.header + 1] & 0x80); }
}

std::vector<AccessUnit> split_access_units(std::span<const uint8_t> stream) {
    std::vector<AccessUnit> units;
    AccessUnit current;
    bool open = false, has_slice = false;
    const auto finish = [&](std::size_t end) {
        if (open && has_slice) {
            current.size = end - current.offset;
            units.push_back(current);
        }
        open = has_slice = false;
    };
    for (const Nal &n : find_nals(stream)) {
        const int type = nal_type(stream, n);
        const bool begins = (type >= 6 && type <= 9) || (is_slice(type) && starts_picture(stream, n));
        // Parameter sets and SEI before a picture belong to it; they only start a unit once the current one has a slice.
        if (begins && has_slice) finish(n.start);
        if (!open) {
            current = {n.start, 0, false};
            open = true;
        }
        if (is_slice(type)) {
            has_slice = true;
            if (type == 5) current.idr = true;
        }
    }
    finish(stream.size());
    return units;
}

std::vector<uint8_t> h264_parameter_sets(std::span<const uint8_t> stream) {
    std::span<const uint8_t> sps, pps;
    for (const Nal &n : find_nals(stream)) {
        const int type = nal_type(stream, n);
        if (type == 7 && sps.empty()) sps = stream.subspan(n.header, n.end - n.header);
        if (type == 8 && pps.empty()) pps = stream.subspan(n.header, n.end - n.header);
        if (!sps.empty() && !pps.empty()) break;
    }
    if (sps.empty() || pps.empty()) return {};
    // Trailing zero bytes before the next start code are not part of either unit.
    while (sps.size() > 1 && sps.back() == 0) sps = sps.first(sps.size() - 1);
    while (pps.size() > 1 && pps.back() == 0) pps = pps.first(pps.size() - 1);
    std::vector<uint8_t> out;
    for (const auto unit : {sps, pps}) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

ReplayBackend::ReplayBackend(std::filesystem::path file, int fps)
    : file_(std::move(file)), period_(std::chrono::nanoseconds(1'000'000'000LL / std::clamp(fps, 1, 240))) {}

ReplayBackend::ReplayBackend(std::vector<uint8_t> stream, int fps)
    : stream_(std::move(stream)), period_(std::chrono::nanoseconds(1'000'000'000LL / std::clamp(fps, 1, 240))) {}

int ReplayBackend::open() {
    if (!file_.empty()) {
        std::ifstream in(file_, std::ios::binary);
        if (!in) return -1;
        stream_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    units_ = split_access_units(stream_);
    headers_ = h264_parameter_sets(stream_);
    if (headers_.empty() || std::none_of(units_.begin(), units_.end(), [](const AccessUnit &u) { return u.idr; })) return -1;
    // Every stream starts at an IDR.
    next_ = next_idr(0);
    due_ = now_();
    keyframe_wanted_ = false;
    open_ = true;
    return 0;
}

int ReplayBackend::headers(void *destination, size_t capacity) {
    if (!open_ || headers_.size() > capacity) return -1;
    std::memcpy(destination, headers_.data(), headers_.size());
    return static_cast<int>(headers_.size());
}

int ReplayBackend::read(void *destination, size_t capacity, uint8_t descriptor[0x88]) {
    if (!open_) return -1;
    const auto now = now_();
    if (now < due_) return 0;
    // Behind by more than a frame (the reader stalled): don't hand out a burst, start the pace again from now.
    due_ = now - due_ > period_ ? now + period_ : due_ + period_;
    if (keyframe_wanted_) {
        next_ = next_idr(next_);
        keyframe_wanted_ = false;
    }
    const AccessUnit &unit = units_[next_];
    if (unit.size > capacity) return -1;
    std::memcpy(destination, stream_.data() + unit.offset, unit.size);
    if (descriptor) std::memset(descriptor, 0, 0x88);
    // At the end of the file, back to its first IDR: the loop starts with a decodable picture.
    next_ = next_ + 1 < units_.size() ? next_ + 1 : next_idr(0);
    return static_cast<int>(unit.size);
}

int ReplayBackend::close() {
    open_ = false;
    return 0;
}

int ReplayBackend::request_keyframe() {
    if (!open_) return -1;
    keyframe_wanted_ = true;
    return 0;
}

std::size_t ReplayBackend::next_idr(std::size_t from) const {
    for (std::size_t i = 0; i < units_.size(); ++i) {
        const std::size_t at = (from + i) % units_.size();
        if (units_[at].idr) return at;
    }
    return 0;
}

}
