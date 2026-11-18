// SPDX-License-Identifier: GPL-3.0-only
// The PS4 screen as a capture backend (capture/capture_session.hpp), through PBS-Payload's capture (pbs_session.h).
#pragma once
#include "capture/capture_session.hpp"
#include <memory>
#include <string>

namespace prospero {

// 720p60 H.264 of the console's screen at bitrate_kbps (PBS takes 1000..15000). log_path: PBS's log for each session.
std::unique_ptr<CaptureBackend> make_pbs_capture_backend(unsigned bitrate_kbps, std::string log_path);

// What a failed open() returned, in words (pbs_session.h's step codes).
const char *pbs_open_error(int rc);

}
