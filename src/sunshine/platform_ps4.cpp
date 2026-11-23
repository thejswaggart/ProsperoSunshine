// SPDX-License-Identifier: GPL-3.0-only
// Sunshine's platf:: layer for the PS4: networking as on the PS5 (platform_ps5.cpp), silent audio, and no input yet. The
// first milestone streams video only; Moonlight's audio stream still gets correctly paced silence so it stays up, and
// controller, keyboard and mouse input is accepted and dropped.
#include "net/send_retry.hpp"
#include "platform/ps4/audio_bridge.hpp"  // float ring filled by the capture thread, drained by the game-audio mic
#include "platform/ps5/pad.h"  // native /dev/hid virtual controller, shared with the PS5 host
#include "platform/ps4/scepad_inject.h"  // per-frame input via libScePad (the /dev/hid INSERT ioctl is rejected on PS4)
#include "input/pad_touch.hpp"  // motion_accel/motion_gyro and the m/s²→g, deg/s→rad/s conversions (shared with the PS5 host)
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio/ip/address.hpp>

#include <signal.h>

using namespace std::literals;

extern "C" int sceNetGetMacAddress(uint8_t address[6], int flags);  // libSceNet, linked by default

extern "C" {
// Boost.Process v1, which process.h includes, signals a process group with killpg; the PS4's libc has no killpg, and the
// host starts no processes. Same meaning as BSD's: the group's id, negated.
int killpg(pid_t group, int signal) { return kill(-group, signal); }

// The title in front (platform/ps5/title_launch.h), which the patched input.cpp asks for the keyboard pad's profile. Not
// known on the PS4 yet: the home screen, as far as the host can tell.
int ps_title_running(char title_id[16]) {
    title_id[0] = '\0';
    return 0;
}
}

namespace platf {

std::unique_ptr<deinit_t> init() { return std::make_unique<deinit_t>(); }

namespace publish {
// Upstream calls this from main.cpp, which main_ps4.cpp replaces; mDNS runs from there.
std::unique_ptr<deinit_t> start() { return nullptr; }
}

std::filesystem::path appdata() { return "/data/prosperosunshine"; }

std::string get_host_name() {
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) == 0 && name[0]) return name;
    return "PS4";
}

std::string get_mac_address(const std::string_view &address) {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return "00:00:00:00:00:00";
    std::string interface_name, mac = "00:00:00:00:00:00";
    for (auto *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6)) continue;
        if (from_sockaddr(a->ifa_addr) == address) interface_name = a->ifa_name;
    }
    for (auto *a = list; a && !interface_name.empty(); a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || interface_name != a->ifa_name) continue;
        const auto *dl = reinterpret_cast<const sockaddr_dl *>(a->ifa_addr);
        if (dl->sdl_alen != 6) continue;
        const auto *b = reinterpret_cast<const unsigned char *>(LLADDR(dl));
        char buf[18];
        std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
        mac = buf;
    }
    freeifaddrs(list);
    // Without a match above, ask the network library.
    if (mac == "00:00:00:00:00:00") {
        uint8_t b[6] = {};
        if (sceNetGetMacAddress(b, 0) == 0) {
            char buf[18];
            std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
            mac = buf;
        }
    }
    return mac;
}

std::string from_sockaddr(const sockaddr *const addr) { return from_sockaddr_ex(addr).second; }

std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const addr) {
    char text[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (addr->sa_family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
        port = ntohs(in6->sin6_port);
    } else if (addr->sa_family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(addr);
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
        port = ntohs(in->sin_port);
    }
    return {port, text};
}

void set_thread_name(std::string_view name) {
    const std::string copy(name.substr(0, 15));
    pthread_set_name_np(pthread_self(), copy.c_str());
}

void adjust_thread_priority(thread_priority_e) {}
void streaming_will_start() {}
void streaming_will_stop() {}
void open_url(const std::string &) {}

std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t, boost::asio::ip::address &, uint16_t, qos_data_type_e, bool) {
    return nullptr;
}

namespace {
socklen_t to_sockaddr(const boost::asio::ip::address &address, uint16_t port, sockaddr_storage &out) {
    std::memset(&out, 0, sizeof out);
    if (address.is_v6()) {
        auto *in6 = reinterpret_cast<sockaddr_in6 *>(&out);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        const auto bytes = address.to_v6().to_bytes();
        std::memcpy(&in6->sin6_addr, bytes.data(), bytes.size());
        return sizeof(sockaddr_in6);
    }
    auto *in = reinterpret_cast<sockaddr_in *>(&out);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    const auto bytes = address.to_v4().to_bytes();
    std::memcpy(&in->sin_addr, bytes.data(), bytes.size());
    return sizeof(sockaddr_in);
}

// One try: 0 when the datagram left, else the errno.
int send_once(int fd, const sockaddr_storage &to, socklen_t to_len, const char *header, size_t header_size,
              const char *payload, size_t payload_size) {
    iovec iov[2];
    int count = 0;
    if (header_size) iov[count++] = {const_cast<char *>(header), header_size};
    iov[count++] = {const_cast<char *>(payload), payload_size};
    msghdr msg{};
    msg.msg_name = const_cast<sockaddr_storage *>(&to);
    msg.msg_namelen = to_len;
    msg.msg_iov = iov;
    msg.msg_iovlen = count;
    return sendmsg(fd, &msg, 0) >= 0 ? 0 : errno;
}

// A full send buffer on a nonblocking socket: give the NIC a moment instead of dropping, 100 steps of 100 us per batch
// (net/send_retry.hpp).
constexpr int send_max_waits = 100;
void send_wait() { std::this_thread::sleep_for(100us); }

// What was dropped, in one line at most every 10 s.
void note_dropped(const prospero::SendBatchResult &result) {
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point logged{};
    static std::size_t dropped = 0, batches = 0;
    static int error = 0;
    std::lock_guard lock(mutex);
    dropped += result.dropped;
    ++batches;
    if (result.error) error = result.error;
    const auto now = std::chrono::steady_clock::now();
    if (now - logged < 10s) return;
    logged = now;
    BOOST_LOG(warning) << "PS4 network: "sv << dropped << " packets in "sv << batches << " sends dropped"sv
                       << (error ? ", last error: "s + std::strerror(error) : ", the send buffer stayed full"s);
    dropped = batches = 0;
    error = 0;
}
}

// Always true: Sunshine reads false as "no batched send here" and would send the whole batch again packet by packet.
bool send_batch(const batched_send_info_t &info) {
    sockaddr_storage to;
    const socklen_t to_len = to_sockaddr(info.target_address, info.target_port, to);
    const auto result = prospero::send_with_retries(
        info.block_count, send_max_waits,
        [&](size_t i) {
            const auto block = info.buffer_for_payload_offset((info.block_offset + i) * info.payload_size);
            const size_t size = std::min(info.payload_size, block.size);
            return send_once(static_cast<int>(info.native_socket), to, to_len,
                             info.headers ? info.headers + (info.block_offset + i) * info.header_size : nullptr,
                             info.headers ? info.header_size : 0, block.buffer, size);
        },
        send_wait);
    if (result.dropped) note_dropped(result);
    return true;
}

bool send(send_info_t &info) {
    sockaddr_storage to;
    const socklen_t to_len = to_sockaddr(info.target_address, info.target_port, to);
    const auto result = prospero::send_with_retries(
        1, send_max_waits,
        [&](size_t) {
            return send_once(static_cast<int>(info.native_socket), to, to_len, info.header, info.header_size, info.payload,
                             info.payload_size);
        },
        send_wait);
    if (result.dropped) note_dropped(result);
    return result.sent == 1;
}

namespace {
class sleep_timer final : public high_precision_timer {
public:
    void sleep_for(const std::chrono::nanoseconds &duration) override { std::this_thread::sleep_for(duration); }
    explicit operator bool() override { return true; }
};
}

std::unique_ptr<high_precision_timer> create_high_precision_timer() { return std::make_unique<sleep_timer>(); }

// Audio: correctly paced zero frames, so Moonlight's audio stream stays up while there is no game audio yet.
namespace {
class silent_mic final : public mic_t {
public:
    silent_mic(int channels, std::uint32_t rate, std::uint32_t frame_size)
        : samples_(static_cast<size_t>(channels) * frame_size),
          period_(std::chrono::nanoseconds(1'000'000'000LL * frame_size / rate)),
          next_(std::chrono::steady_clock::now()) {}
    capture_e sample(std::vector<float> &frame_buffer) override {
        next_ += period_;
        std::this_thread::sleep_until(next_);
        frame_buffer.assign(samples_, 0.0f);
        return capture_e::ok;
    }

private:
    size_t samples_;
    std::chrono::nanoseconds period_;
    std::chrono::steady_clock::time_point next_;
};

// Game audio from PBS. The capture thread fills the bridge with interleaved float32 stereo 48 kHz samples; this mic self-
// paces one Opus frame at a time (as silent_mic does) and drains that many samples, padding with silence on underflow so
// the audio stream never stalls or spins. Only used when the stream matches Avcap's native layout (2 ch, 48 kHz); any other
// layout falls back to silence rather than mangle the channel mapping.
class pbs_mic final : public mic_t {
public:
    pbs_mic(int channels, std::uint32_t rate, std::uint32_t frame_size)
        : samples_(static_cast<size_t>(channels) * frame_size),
          period_(std::chrono::nanoseconds(1'000'000'000LL * frame_size / rate)),
          next_(std::chrono::steady_clock::now()) {}
    capture_e sample(std::vector<float> &frame_buffer) override {
        next_ += period_;
        std::this_thread::sleep_until(next_);
        frame_buffer.resize(samples_);
        prospero::audio_bridge::pull(frame_buffer.data(), samples_);
        return capture_e::ok;
    }

private:
    size_t samples_;
    std::chrono::nanoseconds period_;
    std::chrono::steady_clock::time_point next_;
};

class silent_audio_control final : public audio_control_t {
public:
    int set_sink(const std::string &) override { return 0; }
    std::unique_ptr<mic_t> microphone(const std::uint8_t *, int channels, std::uint32_t sample_rate, std::uint32_t frame_size,
                                      bool, bool) override {
        if (channels == 2 && sample_rate == 48000) {
            BOOST_LOG(info) << "PS4 audio: streaming game audio, 2 channels at 48000 Hz"sv;
            return std::make_unique<pbs_mic>(channels, sample_rate, frame_size);
        }
        BOOST_LOG(info) << "PS4 audio: sending silence (no native capture for "sv << channels << " channels at "sv
                        << sample_rate << " Hz), "sv << channels << " channels at "sv << sample_rate << " Hz"sv;
        return std::make_unique<silent_mic>(channels, sample_rate, frame_size);
    }
    bool is_sink_available(const std::string &) override { return true; }
    std::optional<sink_t> sink_info() override {
        sink_t sink;
        sink.host = "ps4";
        return sink;
    }
};
}

std::unique_ptr<audio_control_t> audio_control() { return std::make_unique<silent_audio_control>(); }

// Input: one native virtual DualShock 4 on /dev/hid (platform/ps5/pad.c, shared with the PS5 host), fed by Moonlight's
// gamepad state, including the DualShock 4 motion sensors (accelerometer and gyro; orientation is left level and the
// touchpad is not forwarded yet). Keyboard and mouse are not forwarded. One controller at a time: a newly announced
// controller replaces the open one (newest wins), so a reconnect — or a swap after a controller was powered off without
// Moonlight sending a removal — takes over the slot instead of being refused. Every arrival/rebind/free is logged with its
// Moonlight indices so we can see exactly what the client signals on a controller power-off.
struct input_raw_t {
    std::mutex mutex;
    ps_pad *pad = nullptr;
    int pad_nr = -1;  // the Moonlight global index that owns the pad
    // Motion: a client only sends the sensors the host asks for, so the owning session's control-stream queue is kept to
    // make that request, with the device-relative index the client echoes back in each motion packet. The latest sample
    // is stamped into every injected frame (the PS4 input path is scePadVirtualDeviceInsertData, not the /dev/hid block,
    // so the pad.c motion setters don't reach the game here). Rest is a level pad: no turn, gravity down.
    feedback_queue_t feedback;
    uint8_t client_index = 0;
    float gyro[3] = {0, 0, 0};   // rad/s
    float accel[3] = {0, 1, 0};  // g
    // The last button/stick frame. Motion arrives in its own packets (gamepad_motion) between button/stick packets
    // (gamepad_update), and each injected frame must carry a full state, so a motion-only update re-injects these.
    ps_pad_state last{0, 128, 128, 128, 128, 0, 0};  // neutral: no buttons, centred sticks, no triggers
};

// Fully remove the virtual controller: libScePad's virtual-device entry first (scePadVirtualDeviceDeleteDevice), then the
// /dev/hid device (ps_pad_close, which also sends a neutral state and closes the fd). Doing only the /dev/hid delete left
// libScePad's entry behind, so the console still listed a leftover controller after the stream pad went away.
static void close_virtual_pad(ps_pad *pad) {
    if (!pad) return;
    const int del_rc = ps_scepad_remove(static_cast<int32_t>(ps_pad_device_id(pad)));
    const int rc = ps_pad_close(pad);
    BOOST_LOG(info) << "PS4 input: virtual controller removed (scePadVirtualDeviceDeleteDevice rc="sv << del_rc
                    << ", /dev/hid rc="sv << rc << ')';
}

// Ask the client to start sending accelerometer and gyro at 100 Hz (as the PS5 host does). A client with no sensors
// ignores it; a new or reconnected client must be asked anew, so this runs on every arrival and rebind. The caller holds
// input_raw_t::mutex.
static void request_motion(const input_raw_t &raw) {
    if (!raw.feedback) return;
    for (const uint8_t type : {prospero::moonlight::motion_accel, prospero::moonlight::motion_gyro})
        raw.feedback->raise(gamepad_feedback_msg_t::make_motion_event_state(raw.client_index, type, 100));
}

void freeInput(input_raw_t *input) {
    if (input->pad) close_virtual_pad(input->pad);
    delete input;
}
input_t input() { return input_t{new input_raw_t}; }

void move_mouse(input_t &, int, int) {}
void move_mouse(input_t &, const touch_port_t &, int, int) {}
void abs_mouse(input_t &, const touch_port_t &, float, float) {}
void button_mouse(input_t &, int, bool) {}
void scroll(input_t &, int) {}
void hscroll(input_t &, int) {}
void keyboard_update(input_t &, uint16_t, bool, uint8_t, bool) {}
void unicode(input_t &, const char *, int) {}

namespace {
// A Moonlight stick axis (int16, +up/+right) to a DualShock axis (0..255, 128 centre).
uint8_t to_axis(int value) {
    value = 128 + (value >> 8);
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

uint32_t to_ds4_buttons(uint32_t m) {
    uint32_t b = 0;
    if (m & DPAD_UP) b |= PS_PAD_UP;
    if (m & DPAD_DOWN) b |= PS_PAD_DOWN;
    if (m & DPAD_LEFT) b |= PS_PAD_LEFT;
    if (m & DPAD_RIGHT) b |= PS_PAD_RIGHT;
    if (m & A) b |= PS_PAD_CROSS;
    if (m & B) b |= PS_PAD_CIRCLE;
    if (m & X) b |= PS_PAD_SQUARE;
    if (m & Y) b |= PS_PAD_TRIANGLE;
    if (m & LEFT_BUTTON) b |= PS_PAD_L1;
    if (m & RIGHT_BUTTON) b |= PS_PAD_R1;
    if (m & LEFT_STICK) b |= PS_PAD_L3;
    if (m & RIGHT_STICK) b |= PS_PAD_R3;
    if (m & START) b |= PS_PAD_OPTIONS;
    if (m & BACK) b |= PS_PAD_CREATE;  // DS4 Share
    if (m & HOME) b |= PS_PAD_PS;
    if (m & TOUCHPAD_BUTTON) b |= PS_PAD_TOUCHPAD;
    return b;
}
}  // namespace

// Inject one full frame — the last button/stick state plus the latest motion — into the virtual pad. The /dev/hid INSERT
// ioctl is rejected on PS4 (EIO); inject through libScePad, which works there, addressing the pad by the /dev/hid ADD
// handle. Both gamepad_update (new buttons/sticks) and gamepad_motion (new sensors) call this, so motion reaches the game
// even when no button or stick moves. The caller holds input_raw_t::mutex.
static int inject_frame(const input_raw_t &raw) {
    return ps_scepad_inject(static_cast<int32_t>(ps_pad_device_id(raw.pad)), raw.last.buttons, raw.last.lx, raw.last.ly,
                            raw.last.rx, raw.last.ry, raw.last.l2, raw.last.r2, raw.gyro, raw.accel);
}

void gamepad_update(input_t &input, int nr, const gamepad_state_t &state) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    if (!raw->pad || nr != raw->pad_nr) return;
    ps_pad_state s{};
    s.buttons = to_ds4_buttons(state.buttonFlags);
    if (state.lt) s.buttons |= PS_PAD_L2;  // the digital L2/R2 bits, set once the analog trigger moves
    if (state.rt) s.buttons |= PS_PAD_R2;
    s.lx = to_axis(state.lsX);
    s.ly = to_axis(-static_cast<int>(state.lsY));  // DS4 Y grows downward
    s.rx = to_axis(state.rsX);
    s.ry = to_axis(-static_cast<int>(state.rsY));
    s.l2 = state.lt;
    s.r2 = state.rt;
    raw->last = s;  // remembered so a motion-only update re-injects the same buttons and sticks
    const int rc = inject_frame(*raw);
    static bool logged = false;
    if (!logged) {
        logged = true;
        BOOST_LOG(info) << "PS4 input: first controller frame injected via libScePad, rc="sv << rc;
    }
}

void gamepad_touch(input_t &, const gamepad_touch_t &) {}

// Store the latest sensor sample for this pad; gamepad_update stamps it into the next injected frame. Sunshine gives
// m/s² and deg/s; the pad takes g and rad/s. Orientation is not sent by Moonlight and is left level in the inject struct.
void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    if (!raw->pad || motion.id.globalIndex != raw->pad_nr) return;
    if (motion.motionType == prospero::moonlight::motion_accel) {
        raw->accel[0] = prospero::accel_to_g(motion.x);
        raw->accel[1] = prospero::accel_to_g(motion.y);
        raw->accel[2] = prospero::accel_to_g(motion.z);
        // One line, once, so the ordinary info log confirms accelerometer samples are arriving without the verbose flood.
        static bool logged_accel = false;
        if (!logged_accel) {
            logged_accel = true;
            BOOST_LOG(info) << "PS4 input: first accelerometer sample (g) "sv << raw->accel[0] << ", " << raw->accel[1]
                            << ", " << raw->accel[2];
        }
    } else if (motion.motionType == prospero::moonlight::motion_gyro) {
        raw->gyro[0] = prospero::gyro_to_rad(motion.x);
        raw->gyro[1] = prospero::gyro_to_rad(motion.y);
        raw->gyro[2] = prospero::gyro_to_rad(motion.z);
        static bool logged_gyro = false;
        if (!logged_gyro) {
            logged_gyro = true;
            BOOST_LOG(info) << "PS4 input: first gyro sample (rad/s) "sv << raw->gyro[0] << ", " << raw->gyro[1] << ", "
                            << raw->gyro[2];
        }
    } else {
        return;  // unknown motion type: nothing changed, no need to re-inject
    }
    // Push the updated sensors to the pad now, with the last buttons/sticks, so motion reaches the game between (or without
    // any) button/stick packets. The motion request is 100 Hz per sensor; a cheap libScePad insert, no file I/O.
    inject_frame(*raw);
}
void gamepad_battery(input_t &, const gamepad_battery_t &) {}
void touch_update(client_input_t *, const touch_port_t &, const touch_input_t &) {}
void pen_update(client_input_t *, const touch_port_t &, const pen_input_t &) {}

int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &, feedback_queue_t feedback) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    // Keep the owning session's queue and the client-relative index so the motion request reaches this client.
    raw->feedback = std::move(feedback);
    raw->client_index = id.clientRelativeIndex;
    BOOST_LOG(info) << "PS4 input: gamepad arrival, global="sv << id.globalIndex << " clientIdx="sv
                    << static_cast<int>(id.clientRelativeIndex)
                    << (raw->pad ? " (a pad is already open for nr="s + std::to_string(raw->pad_nr) + ")"s : ""s);
    if (raw->pad) {
        if (raw->pad_nr == id.globalIndex) {
            // Same slot re-announced: keep the open pad rather than refuse, so a reconnect doesn't strand the controller.
            // A reconnected client must be asked for motion again.
            BOOST_LOG(info) << "PS4 input: same global index re-announced; keeping the existing virtual controller"sv;
            request_motion(*raw);
            return 0;
        }
        // A different controller arrived while one is open. We support one at a time, so the newest wins: replace the old
        // pad. This covers a controller powered off without a Moonlight removal, then a different one turned on — the open
        // slot is stale and would otherwise block the new controller.
        BOOST_LOG(info) << "PS4 input: a different controller arrived (new global="sv << id.globalIndex << ", had nr="sv
                        << raw->pad_nr << "); replacing the virtual controller"sv;
        close_virtual_pad(raw->pad);
        raw->pad = nullptr;
        raw->pad_nr = -1;
    }
    // A signed-in user if there is one, else let the console ask "Who's using this controller?".
    uint32_t user = ps_pad_default_user();
    if (!user) user = PS_PAD_NO_USER;
    ps_pad *pad = nullptr;
    const int rc = ps_pad_open(user, &pad);
    if (rc < 0 || !pad) {
        BOOST_LOG(warning) << "PS4 input: could not add the virtual controller (/dev/hid): "sv << rc;
        return -1;
    }
    const int init_rc = ps_scepad_init();  // libScePad for the inject path; on PS4 this does not hang
    raw->pad = pad;
    raw->pad_nr = id.globalIndex;
    // A fresh pad starts level: no turn, gravity down, buttons/sticks neutral, until the client's first samples arrive.
    raw->gyro[0] = raw->gyro[1] = raw->gyro[2] = 0;
    raw->accel[0] = raw->accel[2] = 0;
    raw->accel[1] = 1;
    raw->last = ps_pad_state{0, 128, 128, 128, 128, 0, 0};
    BOOST_LOG(info) << "PS4 input: virtual DualShock 4 added, device id "sv << ps_pad_device_id(pad) << " for global index "sv
                    << id.globalIndex << " (scePadInit rc="sv << init_rc << ')'
                    << (user == PS_PAD_NO_USER ? "; pick a profile on the console"sv : ""sv);
    request_motion(*raw);
    return 0;
}

int rebind_gamepad(input_t &input, const gamepad_id_t &id, feedback_queue_t feedback) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    BOOST_LOG(info) << "PS4 input: gamepad rebind, global="sv << id.globalIndex << " clientIdx="sv
                    << static_cast<int>(id.clientRelativeIndex)
                    << (raw->pad ? " (open pad nr="s + std::to_string(raw->pad_nr) + ")"s : " (no pad open)"s);
    // Single pad: if this is our controller's slot, keep it; otherwise there is nothing to rebind.
    if (!raw->pad || raw->pad_nr != id.globalIndex) return -1;
    // A rebind carries a fresh feedback queue; keep it and re-request motion for the reconnected client.
    raw->feedback = std::move(feedback);
    raw->client_index = id.clientRelativeIndex;
    request_motion(*raw);
    return 0;
}

void free_gamepad(input_t &input, int nr) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    BOOST_LOG(info) << "PS4 input: gamepad free requested for nr="sv << nr
                    << (raw->pad ? ", open pad nr="s + std::to_string(raw->pad_nr) : ", no pad open"s);
    if (raw->pad && nr == raw->pad_nr) {
        close_virtual_pad(raw->pad);
        raw->pad = nullptr;
        raw->pad_nr = -1;
        raw->feedback.reset();  // the session's queue goes with the pad
    } else if (raw->pad) {
        // Moonlight asked to free a slot that is not the one we hold. Log it (diagnostic): in the single-slot model this
        // means the removal index didn't match our pad, which would leave the controller stranded.
        BOOST_LOG(info) << "PS4 input: free nr="sv << nr << " does not match the open pad (nr="sv << raw->pad_nr
                        << "); leaving it open"sv;
    }
}

void set_gamepad_owner(const feedback_queue_t &, std::string_view) {}
void set_input_owner(const feedback_queue_t *) {}
void trace_call(const std::string &) {}

std::vector<supported_gamepad_t> &supported_gamepads(input_t *) {
    static std::vector<supported_gamepad_t> pads{{"ds4", true, ""}};
    return pads;
}

// Advertise controller touch+motion support (x-ss-general.featureFlags). The client gates LiSendControllerMotionEvent on
// this flag (LI_FF_CONTROLLER_TOUCH_EVENTS), so without it no client ever sends accelerometer or gyro, however it is asked.
// The same flag also enables controller touch events, which gamepad_touch() currently ignores — harmless, the data is dropped.
platform_caps::caps_t get_capabilities() { return platform_caps::controller_touch; }

namespace {
class client_session final : public client_input_t {};
}

std::unique_ptr<client_input_t> allocate_client_input_context(input_t &) { return std::make_unique<client_session>(); }

}  // namespace platf
