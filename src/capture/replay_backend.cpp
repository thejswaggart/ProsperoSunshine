// SPDX-License-Identifier: GPL-3.0-only
#include "capture/replay_backend.hpp"
#include <algorithm>
#include <cstring>
#include <new>

namespace prospero {
namespace {
// One NAL unit: where its start code begins, where its header byte is, where it ends (the next start code), and the two
// bytes the splitter looks at: the header and the one after it (0 when the unit has none).
struct Nal {
    std::size_t start, header, end;
    uint8_t first = 0, second = 0;
};

int nal_type(const Nal &n) { return n.first & 0x1f; }
bool is_slice(int type) { return type == 1 || type == 5; }
// first_mb_in_slice is ue(v), and 0 codes as a single 1 bit: the slice starts a picture when the top bit is set.
bool starts_picture(const Nal &n) { return n.header + 1 < n.end && (n.second & 0x80); }

// Finds start codes in a stream that arrives in pieces. feed() takes each piece in order; finish() closes the last unit.
// Header bytes are filled in by the caller once the positions are known.
class NalScanner {
public:
    void feed(std::span<const uint8_t> piece) {
        for (const uint8_t b : piece) {
            // 00 00 01 at position-2..position.
            if (b == 1 && zeros_ >= 2) {
                const std::size_t three = position_ - 2;
                // A 4-byte start code counts its leading zero as part of this unit.
                const std::size_t start = zeros_ >= 3 ? three - 1 : three;
                if (!nals_.empty()) nals_.back().end = start;
                nals_.push_back({start, position_ + 1, 0});
            }
            zeros_ = b == 0 ? zeros_ + 1 : 0;
            ++position_;
        }
    }
    std::vector<Nal> finish() {
        if (!nals_.empty()) nals_.back().end = position_;
        // A unit with no header byte (a start code at the very end) is dropped.
        std::erase_if(nals_, [](const Nal &n) { return n.header >= n.end; });
        return std::move(nals_);
    }
    std::size_t size() const { return position_; }

private:
    std::vector<Nal> nals_;
    std::size_t position_ = 0, zeros_ = 0;
};

std::vector<Nal> find_nals(std::span<const uint8_t> s) {
    NalScanner scanner;
    scanner.feed(s);
    auto nals = scanner.finish();
    for (Nal &n : nals) {
        n.first = s[n.header];
        n.second = n.header + 1 < n.end ? s[n.header + 1] : 0;
    }
    return nals;
}

std::vector<AccessUnit> group_units(const std::vector<Nal> &nals, std::size_t stream_size) {
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
    for (const Nal &n : nals) {
        const int type = nal_type(n);
        const bool begins = (type >= 6 && type <= 9) || (is_slice(type) && starts_picture(n));
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
    finish(stream_size);
    return units;
}

// The first SPS and PPS as [header, end) ranges, with trailing zero bytes trimmed by the caller.
struct ParameterSetRanges {
    const Nal *sps = nullptr, *pps = nullptr;
};
ParameterSetRanges find_parameter_sets(const std::vector<Nal> &nals) {
    ParameterSetRanges r;
    for (const Nal &n : nals) {
        if (nal_type(n) == 7 && !r.sps) r.sps = &n;
        if (nal_type(n) == 8 && !r.pps) r.pps = &n;
        if (r.sps && r.pps) break;
    }
    return r;
}

// SPS then PPS, each behind a 4-byte start code. `bytes_of` hands out a NAL unit's bytes from its header on.
template<class BytesOf>
std::vector<uint8_t> join_parameter_sets(const ParameterSetRanges &r, BytesOf bytes_of) {
    if (!r.sps || !r.pps) return {};
    std::vector<uint8_t> out;
    for (const Nal *n : {r.sps, r.pps}) {
        std::vector<uint8_t> unit = bytes_of(*n);
        // Trailing zero bytes before the next start code are not part of the unit.
        while (unit.size() > 1 && unit.back() == 0) unit.pop_back();
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

constexpr std::size_t chunk_bytes = 1 << 20;
}

std::vector<AccessUnit> split_access_units(std::span<const uint8_t> stream) { return group_units(find_nals(stream), stream.size()); }

std::vector<uint8_t> h264_parameter_sets(std::span<const uint8_t> stream) {
    const auto nals = find_nals(stream);
    return join_parameter_sets(find_parameter_sets(nals), [&](const Nal &n) {
        return std::vector<uint8_t>(stream.begin() + n.header, stream.begin() + n.end);
    });
}

ReplayBackend::ReplayBackend(std::filesystem::path file, int fps)
    : path_(std::move(file)), period_(std::chrono::nanoseconds(1'000'000'000LL / std::clamp(fps, 1, 240))) {}

ReplayBackend::ReplayBackend(std::vector<uint8_t> stream, int fps)
    : memory_(std::move(stream)), period_(std::chrono::nanoseconds(1'000'000'000LL / std::clamp(fps, 1, 240))) {}

bool ReplayBackend::copy_bytes(std::size_t offset, std::size_t size, void *destination) {
    if (path_.empty()) {
        if (offset + size > memory_.size()) return false;
        std::memcpy(destination, memory_.data() + offset, size);
        return true;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
    return file_.gcount() == static_cast<std::streamsize>(size);
}

int ReplayBackend::open() {
    open_ = false;
    try {
        std::vector<Nal> nals;
        std::size_t size = 0;
        if (path_.empty()) {
            nals = find_nals(memory_);
            size = memory_.size();
        } else {
            file_.close();
            file_.open(path_, std::ios::binary);
            if (!file_) return -1;
            // One pass in chunks: only the start code positions are kept.
            NalScanner scanner;
            std::vector<uint8_t> chunk(chunk_bytes);
            while (file_) {
                file_.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                const auto got = static_cast<std::size_t>(file_.gcount());
                if (!got) break;
                scanner.feed(std::span<const uint8_t>(chunk.data(), got));
            }
            size = scanner.size();
            nals = scanner.finish();
            // The two bytes the splitter needs from each unit.
            for (Nal &n : nals) {
                uint8_t two[2] = {};
                const std::size_t want = n.header + 1 < n.end ? 2 : 1;
                if (!copy_bytes(n.header, want, two)) return -1;
                n.first = two[0];
                n.second = want == 2 ? two[1] : 0;
            }
        }
        units_ = group_units(nals, size);
        headers_ = join_parameter_sets(find_parameter_sets(nals), [&](const Nal &n) {
            std::vector<uint8_t> unit(n.end - n.header);
            if (!copy_bytes(n.header, unit.size(), unit.data())) unit.clear();
            return unit;
        });
    } catch (const std::bad_alloc &) {
        return -1;
    }
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
    if (unit.size > capacity || !copy_bytes(unit.offset, unit.size, destination)) return -1;
    if (descriptor) std::memset(descriptor, 0, 0x88);
    // At the end of the file, back to its first IDR: the loop starts with a decodable picture.
    next_ = next_ + 1 < units_.size() ? next_ + 1 : next_idr(0);
    return static_cast<int>(unit.size);
}

int ReplayBackend::close() {
    open_ = false;
    file_.close();
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
