// SPDX-License-Identifier: GPL-3.0-only
// mDNS responder on UDP 5353 (ROADMAP P9.2): the PS5 has neither Avahi nor Bonjour, so the host
// answers _nvstream._tcp queries itself. Any failure disables discovery only, never the host.
#pragma once
#include <netinet/in.h>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

namespace prospero::mdns {

// The console's LAN IPv4 (the address Moonlight and the pairing page are reached on), or nothing when offline.
std::optional<in_addr> lan_address();

// Runs until `stop` is requested: announces three times, answers queries, sends goodbye on exit.
// Returns right away (after logging) when the socket cannot be set up.
void run_responder(std::stop_token stop, const std::string &name, uint16_t port);

// The debug build's self-check: sets the socket up as the responder does and closes it again. Empty when that worked,
// else what failed ("no LAN address", "bind 5353 errno=48", ...).
std::string check_socket();

}
