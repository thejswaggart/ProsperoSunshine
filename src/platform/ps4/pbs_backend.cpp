// SPDX-License-Identifier: GPL-3.0-only
#include "platform/ps4/pbs_backend.hpp"
#include "platform/ps4/pbs_session.h"
#include <array>
#include <cstring>

namespace prospero {
namespace {
class PbsCaptureBackend final : public CaptureBackend {
public:
    PbsCaptureBackend(unsigned bitrate_kbps, std::string log_path) : bitrate_kbps_(bitrate_kbps), log_path_(std::move(log_path)) {}
    ~PbsCaptureBackend() override { close(); }

    int open() override {
        const int rc = ps_pbs_open(log_path_.c_str(), bitrate_kbps_, headers_.data(), headers_.size());
        if (rc < 0) return rc;
        header_size_ = static_cast<std::size_t>(rc);
        open_ = true;
        return 0;
    }
    int headers(void *destination, size_t capacity) override {
        if (!open_ || header_size_ > capacity) return -1;
        std::memcpy(destination, headers_.data(), header_size_);
        return static_cast<int>(header_size_);
    }
    int read(void *destination, size_t capacity, uint8_t descriptor[0x88]) override {
        if (descriptor) std::memset(descriptor, 0, 0x88);
        return open_ ? ps_pbs_read(destination, capacity) : -1;
    }
    int close() override {
        if (!open_) return 0;
        open_ = false;
        return ps_pbs_close();
    }
    int request_keyframe() override { return open_ ? ps_pbs_keyframe() : -1; }

private:
    unsigned bitrate_kbps_;
    std::string log_path_;
    std::array<uint8_t, 512> headers_{};
    std::size_t header_size_ = 0;
    bool open_ = false;
};
}

std::unique_ptr<CaptureBackend> make_pbs_capture_backend(unsigned bitrate_kbps, std::string log_path) {
    return std::make_unique<PbsCaptureBackend>(bitrate_kbps, std::move(log_path));
}

const char *pbs_open_error(int rc) {
    switch (rc) {
    case -1: return "a capture session is open already";
    case -2: return "this firmware's kernel layout wasn't found";
    case -3: return "PBS refused to begin: this host's earlier capture wasn't released cleanly (reload the host)";
    case -4: return "Avcap Init failed";
    case -5: return "the video request was rejected";
    case -6: return "OpenVideoEx failed (is Remote Play or another capture running?)";
    case -7: return "the encoder gave no SPS/PPS";
    case -8: return "Avcap Start failed";
    default: return "unknown error";
    }
}

}
