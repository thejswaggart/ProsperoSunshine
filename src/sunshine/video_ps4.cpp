// SPDX-License-Identifier: GPL-3.0-only
// Replaces Sunshine's video.cpp on the PS4. Like the PS5 host there is no encoder in the host: a capture backend delivers
// H.264 and its frames go straight into the stream's packet queue. The backend is the console's screen through PBS-Payload
// (platform/ps4/pbs_backend.hpp), 720p60; ps4_capture = replay in sunshine.conf replays a recorded file instead
// (capture/replay_backend.hpp). One stream at a time, H.264 and SDR only.
#include "capture/capture_session.hpp"
#include "capture/replay_backend.hpp"
#include "platform/ps4/pbs_backend.hpp"
#include "platform/ps4/audio_bridge.hpp"
#include "platform/ps4/pbs_session.h"
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
// Where the picture comes from, from sunshine.conf. ps4_capture: pbs (the screen, default) or replay. The replay plays
// replay_file at replay_fps, else replay.h264 in the data folder at 60 fps.
struct capture_settings {
    bool replay = false;
    std::filesystem::path file;
    int fps = 60;
};

capture_settings read_capture_settings() {
    capture_settings settings{false, platf::appdata() / "replay.h264", 60};
    const auto conf = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    if (const auto it = conf.find("ps4_capture"); it != conf.end()) {
        if (it->second == "replay") settings.replay = true;
        else if (it->second != "pbs") BOOST_LOG(warning) << "ps4_capture in sunshine.conf is pbs or replay; using pbs"sv;
    }
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
    const auto settings = read_capture_settings();
    if (!settings.replay) {
        BOOST_LOG(info) << "PS4 video: the console's screen through PBS, 720p60 H.264"sv;
        return 0;
    }
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

    const auto settings = read_capture_settings();
    BOOST_LOG(info) << "PS4 capture requested "sv << config.width << 'x' << config.height << '@' << config.framerate
                    << (config.videoFormat ? ", but only H.264 is offered"sv : ""sv)
                    << (settings.replay ? ""sv : ", delivering 1280x720@60"sv);
    input::touch_port_t port{};
    port.width = port.logical_width = settings.replay ? config.width : 1280;
    port.height = port.logical_height = settings.replay ? config.height : 720;
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

    // config.bitrate is in kbit/s; PBS takes 1000..15000.
    const unsigned kbps = config.bitrate > 0 ? static_cast<unsigned>(std::clamp(config.bitrate, 1000, 15000)) : 10000;
    std::unique_ptr<prospero::CaptureBackend> backend;
    if (settings.replay) backend = std::make_unique<prospero::ReplayBackend>(settings.file, settings.fps);
    else backend = prospero::make_pbs_capture_backend(kbps, (platf::appdata() / "pbs_capture.log").string());
    prospero::CaptureSession session(std::move(backend));
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
        if (settings.replay)
            BOOST_LOG(error) << "PS4 capture: "sv << settings.file.string()
                             << " can't be replayed (missing, or no SPS, PPS or IDR in it): "sv << rc;
        else
            BOOST_LOG(error) << "PS4 capture: the screen capture didn't open: "sv << prospero::pbs_open_error(rc) << " ("sv << rc
                             << "); pbs_capture.log has PBS's steps"sv;
        shutdown_event->raise(true);
        return;
    }
    if (settings.replay)
        BOOST_LOG(info) << "PS4 capture: replaying "sv << settings.file.string() << " at "sv << settings.fps << " fps"sv;
    else
        BOOST_LOG(info) << "PS4 capture: screen capture open, 1280x720@60, "sv << kbps << " kbit/s"sv;

    // Game audio: open the PBS audio channel and feed each ReadAudio frame into the audio bridge, all on this capture
    // thread — audio and video reads share one injection context and must not run concurrently (pbs_session.h). The mic
    // (platform_ps4.cpp) drains the bridge. Avcap delivers interleaved float32 stereo at 48 kHz in [-1,1], exactly what
    // Sunshine's Opus float encoder consumes, so the samples pass through unconverted.
    bool audio_open = false;
    std::vector<uint8_t> audio_buf;
    if (!settings.replay && ps_pbs_audio_open() == 0) {
        audio_open = true;
        audio_buf.resize(64 * 1024);
        prospero::audio_bridge::open(48000u * 2u / 4u);  // ~250 ms of 48 kHz stereo float, to bound latency
        BOOST_LOG(info) << "PS4 audio: capturing game audio (float32 stereo 48 kHz)"sv;
    }

    // Number frames from 1: Moonlight drops frame 0, so starting at 0 would waste the opening IDR and the client would
    // wait for a later keyframe before it had a picture (the same reason upstream numbers the PS5 stream from 1).
    int64_t next_index = 1;
    prospero::EncodedFrame frame;
    while (!shutdown_event->peek()) {
        if (audio_open) {
            const int a = ps_pbs_audio_read(audio_buf.data(), audio_buf.size());
            if (a > 0)
                prospero::audio_bridge::push(reinterpret_cast<const float *>(audio_buf.data()),
                                             static_cast<size_t>(a) / sizeof(float));
        }
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
    if (audio_open) prospero::audio_bridge::close();
    session.stop();
}
}
