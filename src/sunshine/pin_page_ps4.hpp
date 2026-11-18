// SPDX-License-Identifier: GPL-3.0-only
// The PS4 host's pairing page (pin_page_ps4.cpp).
#pragma once
#include <cstdint>
#include <functional>
#include <stop_token>

namespace prospero::ps4 {

// Serves the pairing page on `port` until `stop` is requested. Logs and returns when the port can't be bound.
// quit: called once the page's "Stop host" (POST /api/quit) has been answered; it should stop the host.
void run_pin_page(std::stop_token stop, uint16_t port, std::function<void()> quit);

}
