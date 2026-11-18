// SPDX-License-Identifier: GPL-3.0-only
// Replaces Sunshine's video.cpp on the PS4. Like the PS5 host there is no encoder in the host: a capture backend delivers
// H.264 and its frames go straight into the stream's packet queue. For now the backend replays a recorded file
// (capture/replay_backend.hpp); the console's own encoder takes its place later. One stream at a time, H.264 and SDR only.
#include "capture/capture_session.hpp"
#include "capture/replay_backend.hpp"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/globals.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/stream.h"
#include "src/video.h"
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

using namespace std::literals;

namespace video {
// 1 is "disabled" for HEVC and AV1 in Sunshine: only H.264 is offered.
int active_hevc_mode = 1;
int active_av1_mode = 1;
bool last_encoder_probe_supported_ref_frames_invalidation = false;
std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {false, false, false};
std::array<bool, 3> last_encoder_probe_supported_dynamic_range_for_codec = {false, false, false};
std::array<bool, 3> last_encoder_probe_supported_dynamic_range_yuv444_for_codec = {false, false, false};

namespace {
// What the replay backend plays: replay_file and replay_fps in sunshine.conf, else replay.h264 in the data folder at 60 fps.
struct replay_settings {
    std::filesystem::path file;
    int fps = 60;
};

replay_settings read_replay_settings() {
    replay_settings settings{platf::appdata() / "replay.h264", 60};
    const auto conf = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    if (const auto it = conf.find("replay_file"); it != conf.end() && !it->second.empty()) settings.file = it->second;
    if (const auto it = conf.find("replay_fps"); it != conf.end()) {
        try {
            settings.fps = std::clamp(std::stoi(it->second), 1, 240);
        } catch (...) {
            BOOST_LOG(warning) << "replay_fps in sunshine.conf is not a number, using 60"sv;
        }
    }
    return settings;
}

// A frame for one stream's session. The session counts it until the video broadcast thread sent or dropped it, and
// stream::session::join() waits for that (patch 0026).
struct counted_packet final : packet_raw_generic {
    counted_packet(std::vector<uint8_t> &&bytes, int64_t index, bool idr, stream::session_t *session)
        : packet_raw_generic(std::move(bytes), index, idr), ref(session) {
        channel_data = session;
    }
    stream::session::video_frame_ref_t ref;
};

// The one stream that captures. A new stream ends the running one and waits for it to let go.
struct {
    std::mutex mutex;
    std::condition_variable released;
    safe::mail_raw_t::event_t<bool> shutdown;  // of the running stream, empty while none runs
} current;
}

std::optional<bool> hdr_conflict(bool) { return std::nullopt; }

int probe_encoders() {
    const auto settings = read_replay_settings();
    std::error_code ec;
    if (!std::filesystem::exists(settings.file, ec))
        BOOST_LOG(warning) << "PS4 video: "sv << settings.file.string() << " does not exist yet; streams fail until it does"sv;
    BOOST_LOG(info) << "PS4 video: replaying "sv << settings.file.string() << " at "sv << settings.fps << " fps, H.264 only"sv;
    return 0;
}

void capture(safe::mail_t mail, config_t config, stream::session_t *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);

    BOOST_LOG(info) << "PS4 capture requested "sv << config.width << 'x' << config.height << '@' << config.framerate
                    << (config.videoFormat ? ", but only H.264 is offered"sv : ""sv);
    input::touch_port_t port{};
    port.width = port.logical_width = config.width;
    port.height = port.logical_height = config.height;
    touch_port_event->raise(port);
    hdr_event->raise(std::make_unique<hdr_info_raw_t>(false));

    {
        std::unique_lock lock(current.mutex);
        if (current.shutdown) {
            BOOST_LOG(info) << "PS4 capture: a new stream started, ending the running one"sv;
            current.shutdown->raise(true);
            if (!current.released.wait_for(lock, 5s, [] { return !current.shutdown; })) {
                BOOST_LOG(warning) << "PS4 capture: the running stream did not end; ending the new one"sv;
                shutdown_event->raise(true);
                return;
            }
        }
        current.shutdown = shutdown_event;
    }
    const auto release = util::fail_guard([] {
        {
            std::lock_guard lock(current.mutex);
            current.shutdown.reset();
        }
        current.released.notify_all();
    });

    const auto settings = read_replay_settings();
    prospero::CaptureSession session(std::make_unique<prospero::ReplayBackend>(settings.file, settings.fps));
    // The session's frame buffer is 4 MB, and a payload's heap is small: out of memory ends this stream, not the host.
    int start_rc = -1;
    try {
        start_rc = session.start();
    } catch (const std::bad_alloc &) {
        BOOST_LOG(error) << "PS4 capture: out of memory opening the capture, ending the stream"sv;
        shutdown_event->raise(true);
        return;
    }
    if (const int rc = start_rc) {
        BOOST_LOG(error) << "PS4 capture: "sv << settings.file.string()
                         << " can't be replayed (missing, or no SPS, PPS or IDR in it): "sv << rc;
        shutdown_event->raise(true);
        return;
    }
    BOOST_LOG(info) << "PS4 capture: replaying "sv << settings.file.string() << " at "sv << settings.fps << " fps"sv;

    int64_t next_index = 0;
    prospero::EncodedFrame frame;
    while (!shutdown_event->peek()) {
        if (idr_events->peek()) {
            idr_events->pop();
            if (session.request_keyframe()) BOOST_LOG(warning) << "PS4 capture: keyframe request failed"sv;
        }
        const auto status = session.read(frame, 100ms);
        if (status == prospero::CaptureStatus::timeout) continue;
        if (status != prospero::CaptureStatus::frame) {
            BOOST_LOG(error) << "PS4 capture: the backend stopped ("sv << static_cast<int>(status) << "), ending the stream"sv;
            shutdown_event->raise(true);
            break;
        }
        auto packet = std::make_unique<counted_packet>(std::move(frame.bytes), next_index++, frame.idr, channel_data);
        packet->frame_timestamp = frame.read_time;
        packets->raise(std::move(packet));
        frame.bytes = {};
    }
    session.stop();
}
}
