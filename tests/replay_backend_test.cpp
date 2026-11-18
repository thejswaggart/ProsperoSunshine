// SPDX-License-Identifier: GPL-3.0-only
#include "capture/replay_backend.hpp"
#include "capture/capture_session.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace std::chrono_literals;
using prospero::ReplayBackend;

namespace {
using bytes = std::vector<uint8_t>;

// NAL units with 4-byte start codes. A slice's second byte has its top bit set when first_mb_in_slice is 0.
bytes nal(uint8_t header, std::initializer_list<uint8_t> payload) {
    bytes out{0, 0, 0, 1, header};
    out.insert(out.end(), payload);
    return out;
}
const bytes aud = nal(0x09, {0xf0});
const bytes sps = nal(0x67, {0x64, 0x00, 0x1f});
const bytes pps = nal(0x68, {0xee, 0x3c, 0x80});
const bytes idr = nal(0x65, {0x88, 0x11});
const bytes idr_second_slice = nal(0x65, {0x40, 0x22});  // first_mb_in_slice != 0: same picture
const bytes p = nal(0x41, {0x9a, 0x33});

bytes cat(std::initializer_list<bytes> parts) {
    bytes out;
    for (const auto &part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

// IDR (two slices), P, P, IDR, P: five pictures, the first with SPS and PPS in front.
bytes sample_stream() { return cat({aud, sps, pps, idr, idr_second_slice, aud, p, aud, p, aud, idr, aud, p}); }

ReplayBackend::clock::time_point fake_now;
ReplayBackend::clock::time_point now() { return fake_now; }

int read_one(ReplayBackend &backend, bytes &out) {
    out.assign(1 << 16, 0);
    uint8_t descriptor[0x88];
    const int n = backend.read(out.data(), out.size(), descriptor);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return n;
}

void splits_pictures() {
    const auto stream = sample_stream();
    const auto units = prospero::split_access_units(stream);
    assert(units.size() == 5);
    assert(units[0].idr && !units[1].idr && !units[2].idr && units[3].idr && !units[4].idr);
    // The first unit holds AUD, SPS, PPS and both IDR slices.
    assert(units[0].offset == 0);
    assert(units[0].size == aud.size() + sps.size() + pps.size() + idr.size() + idr_second_slice.size());
    // Units cover the stream without gaps.
    for (size_t i = 1; i < units.size(); ++i) assert(units[i].offset == units[i - 1].offset + units[i - 1].size);
    assert(units.back().offset + units.back().size == stream.size());
}

void parameter_sets() {
    const auto headers = prospero::h264_parameter_sets(sample_stream());
    assert(headers == cat({sps, pps}));
    assert(prospero::h264_parameter_sets(cat({aud, idr})).empty());
}

void paces_and_loops() {
    fake_now = {};
    ReplayBackend backend(sample_stream(), 50);  // 20 ms a frame
    backend.set_clock(now);
    assert(backend.open() == 0);
    assert(backend.unit_count() == 5);
    bytes frame;
    // The first frame is due at once and is the first IDR.
    assert(read_one(backend, frame) > 0 && prospero::has_idr(frame));
    // Nothing more until 20 ms later.
    assert(read_one(backend, frame) == 0);
    fake_now += 19ms;
    assert(read_one(backend, frame) == 0);
    fake_now += 1ms;
    assert(read_one(backend, frame) > 0 && !prospero::has_idr(frame));
    // Units 2, 3 (IDR), 4, then back to the first IDR: the loop starts decodable.
    for (bool want_idr : {false, true, false, true}) {
        fake_now += 20ms;
        assert(read_one(backend, frame) > 0);
        assert(prospero::has_idr(frame) == want_idr);
    }
    // A stall longer than a frame doesn't release a burst.
    fake_now += 1s;
    assert(read_one(backend, frame) > 0);
    assert(read_one(backend, frame) == 0);
}

void keyframe_request_skips_ahead() {
    fake_now = {};
    ReplayBackend backend(sample_stream(), 50);
    backend.set_clock(now);
    assert(backend.open() == 0);
    bytes frame;
    assert(read_one(backend, frame) > 0);  // unit 0
    assert(backend.request_keyframe() == 0);
    fake_now += 20ms;
    assert(read_one(backend, frame) > 0 && prospero::has_idr(frame));  // unit 3, not unit 1
    fake_now += 20ms;
    assert(read_one(backend, frame) > 0 && !prospero::has_idr(frame));  // unit 4
}

void refuses_unplayable() {
    ReplayBackend no_idr(cat({aud, sps, pps, aud, p}), 60);
    assert(no_idr.open() == -1);
    ReplayBackend no_sps(cat({aud, idr, aud, p}), 60);
    assert(no_sps.open() == -1);
    ReplayBackend missing(std::filesystem::path("/nonexistent/replay.h264"), 60);
    assert(missing.open() == -1);
    bytes frame;
    assert(read_one(missing, frame) == -1);
}

// Through CaptureSession, as video_ps4.cpp uses it: headers cached, frames delivered and IDRs flagged.
void through_capture_session() {
    prospero::CaptureSession session(std::make_unique<ReplayBackend>(sample_stream(), 240));
    assert(session.start() == 0);
    assert(session.parameter_sets() == cat({sps, pps}));
    prospero::EncodedFrame frame;
    assert(session.read(frame, 100ms) == prospero::CaptureStatus::frame);
    assert(frame.idr);
    assert(session.read(frame, 100ms) == prospero::CaptureStatus::frame);
    assert(!frame.idr);
    session.stop();
}
}

int main() {
    splits_pictures();
    parameter_sets();
    paces_and_loops();
    keyframe_request_skips_ahead();
    refuses_unplayable();
    through_capture_session();
    std::cout << "replay_backend_test: ok\n";
}
