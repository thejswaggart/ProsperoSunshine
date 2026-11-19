// SPDX-License-Identifier: GPL-3.0-only
// Sunshine's platf:: layer for the PS4: networking as on the PS5 (platform_ps5.cpp), silent audio, and no input yet. The
// first milestone streams video only; Moonlight's audio stream still gets correctly paced silence so it stays up, and
// controller, keyboard and mouse input is accepted and dropped.
#include "net/send_retry.hpp"
#include "platform/ps4/audio_bridge.hpp"  // float ring filled by the capture thread, drained by the game-audio mic
#include "platform/ps5/pad.h"  // native /dev/hid virtual controller, shared with the PS5 host
#include "platform/ps4/scepad_inject.h"  // per-frame input via libScePad (the /dev/hid INSERT ioctl is rejected on PS4)
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
// gamepad state. Keyboard, mouse, touch and motion are not forwarded yet. One controller for now: a second announced
// gamepad is refused.
struct input_raw_t {
    std::mutex mutex;
    ps_pad *pad = nullptr;
    int pad_nr = -1;  // the Moonlight global index that owns the pad
};

void freeInput(input_raw_t *input) {
    if (input->pad) ps_pad_close(input->pad);
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
    // The /dev/hid INSERT ioctl is rejected on PS4 (EIO); inject through libScePad, which works there, addressing the pad
    // by the /dev/hid ADD handle.
    const int rc = ps_scepad_inject(static_cast<int32_t>(ps_pad_device_id(raw->pad)), s.buttons, s.lx, s.ly, s.rx, s.ry,
                                    s.l2, s.r2);
    static bool logged = false;
    if (!logged) {
        logged = true;
        BOOST_LOG(info) << "PS4 input: first controller frame injected via libScePad, rc="sv << rc;
    }
}

void gamepad_touch(input_t &, const gamepad_touch_t &) {}
void gamepad_motion(input_t &, const gamepad_motion_t &) {}
void gamepad_battery(input_t &, const gamepad_battery_t &) {}
void touch_update(client_input_t *, const touch_port_t &, const touch_input_t &) {}
void pen_update(client_input_t *, const touch_port_t &, const pen_input_t &) {}

int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &, feedback_queue_t) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    if (raw->pad) {
        BOOST_LOG(info) << "PS4 input: a second controller was announced; only one is supported for now, ignoring"sv;
        return -1;
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
    BOOST_LOG(info) << "PS4 input: virtual DualShock 4 added, device id "sv << ps_pad_device_id(pad) << " (scePadInit rc="sv
                    << init_rc << ')' << (user == PS_PAD_NO_USER ? "; pick a profile on the console"sv : ""sv);
    return 0;
}

int rebind_gamepad(input_t &, const gamepad_id_t &, feedback_queue_t) { return -1; }

void free_gamepad(input_t &input, int nr) {
    auto raw = static_cast<input_raw_t *>(input.get());
    std::lock_guard lock(raw->mutex);
    if (raw->pad && nr == raw->pad_nr) {
        ps_pad_close(raw->pad);
        raw->pad = nullptr;
        raw->pad_nr = -1;
        BOOST_LOG(info) << "PS4 input: virtual controller removed"sv;
    }
}

void set_gamepad_owner(const feedback_queue_t &, std::string_view) {}
void set_input_owner(const feedback_queue_t *) {}
void trace_call(const std::string &) {}

std::vector<supported_gamepad_t> &supported_gamepads(input_t *) {
    static std::vector<supported_gamepad_t> pads{{"ds4", true, ""}};
    return pads;
}

platform_caps::caps_t get_capabilities() { return {}; }

namespace {
class client_session final : public client_input_t {};
}

std::unique_ptr<client_input_t> allocate_client_input_context(input_t &) { return std::make_unique<client_session>(); }

}  // namespace platf
