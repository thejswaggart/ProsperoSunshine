// SPDX-License-Identifier: GPL-3.0-only
// The PS4 host's pairing page (pin_page_ps4.cpp).
#pragma once
#include <cstdint>
#include <functional>
#include <stop_token>

namespace prospero::ps4 {

// Serves the pairing page on `port` until `stop` is requested. Logs and returns when the port can't be bound.
// lan: false (the default for a release) binds loopback only, so pairing and /api/quit are reachable from the console
// itself, not from anyone on the LAN; true binds the LAN interface (the `pin_lan` dev flag) so a dev host can be paired
// and stopped remotely (ps4_deploy.sh). quit: called once the page's "Stop host" (POST /api/quit) has been answered.
void run_pin_page(std::stop_token stop, uint16_t port, bool lan, std::function<void()> quit);

}
