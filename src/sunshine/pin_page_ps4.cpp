// SPDX-License-Identifier: GPL-3.0-only
// The PS4 host's pairing page: a small plain-HTTP page on the base port + 1 (47990 by default) that lists the pending
// pairing requests and passes the PIN Moonlight shows to nvhttp::pin(), as the PS5 host's control socket does with PIN.
// Until the PS5 web page (src/web/) is ported it is the only way to pair. It also stops the host (POST /api/quit, the
// PS5 control socket's QUIT), so a new build can be loaded without restarting the console. By default it binds loopback
// only, so pairing and quit answer the console itself and nobody else: the PIN is the only guard and Moonlight shows it
// on the pairing device, so a LAN-reachable page would let anyone pair, and the bare /api/quit POST would let any page on
// the network stop the host. An empty `pin_lan` file opens it to the LAN for development (ps4_deploy.sh's remote quit),
// as control_lan does for the PS5 control socket; even then it is the LAN only, never the internet.
#include "sunshine/pin_page_ps4.hpp"
#include "src/config.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>
#include <thread>

using namespace std::literals;

namespace prospero::ps4 {
namespace {
using http_server = SimpleWeb::Server<SimpleWeb::HTTP>;
using response_t = std::shared_ptr<http_server::Response>;
using request_t = std::shared_ptr<http_server::Request>;

constexpr auto page = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>PS4 pairing</title>
<style>
body{font-family:system-ui,sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;color:#222;background:#fafafa}
@media (prefers-color-scheme:dark){body{color:#eee;background:#181818}input,button{color:inherit;background:#2a2a2a;border-color:#555}}
.req{border:1px solid #8884;border-radius:8px;padding:1rem;margin:1rem 0}
input{font-size:1.2rem;padding:.4rem;width:8rem;border:1px solid #8888;border-radius:4px}
button{font-size:1rem;padding:.45rem .9rem;margin-left:.4rem;border:1px solid #8888;border-radius:4px;cursor:pointer}
#msg{min-height:1.5rem}
footer{margin-top:3rem;padding-top:1rem;border-top:1px solid #8884}
footer button{margin-left:0}
</style></head><body>
<h1>Pair a Moonlight device</h1>
<p>Start pairing in Moonlight, then enter the PIN it shows here.</p>
<p id="msg"></p><div id="list">Loading…</div>
<footer><button id="quit">Stop host</button> ends every stream and stops the host until it is loaded again.</footer>
<script>
document.getElementById('quit').onclick = async () => {
  if (!confirm('Stop the host? Streams end, and it runs again only once it is loaded again.')) return;
  const r = await fetch('/api/quit', {method:'POST'}); const j = await r.json();
  msg.textContent = j.message; clearInterval(timer); list.innerHTML = '';
};
const list = document.getElementById('list'), msg = document.getElementById('msg');
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
async function load(){
  const r = await fetch('/api/pairings'); const items = await r.json();
  if (document.activeElement && document.activeElement.tagName === 'INPUT') return;
  list.innerHTML = items.length ? '' : '<p>No device is waiting. Start pairing in Moonlight.</p>';
  for (const p of items){
    const d = document.createElement('div'); d.className = 'req';
    d.innerHTML = `<b>${esc(p.name || 'Moonlight')}</b> from ${esc(p.address)}<br><br>
      <input inputmode="numeric" maxlength="8" placeholder="PIN"><button data-a="pin">Pair</button><button data-a="cancel">Deny</button>`;
    d.querySelector('[data-a=pin]').onclick = () => send('/api/pin', {id:p.id, pin:d.querySelector('input').value, name:p.name});
    d.querySelector('[data-a=cancel]').onclick = () => send('/api/pin/cancel', {id:p.id});
    list.appendChild(d);
  }
}
async function send(url, body){
  msg.textContent = url.endsWith('cancel') ? 'Denying…' : 'Pairing… finish in Moonlight if it asks';
  const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
  const j = await r.json(); msg.textContent = j.message; document.activeElement.blur(); load();
}
load(); const timer = setInterval(load, 2000);
</script></body></html>
)html";

void send_json(const response_t &response, SimpleWeb::StatusCode status, const nlohmann::json &body) {
    SimpleWeb::CaseInsensitiveMultimap headers{{"Content-Type", "application/json"}, {"Cache-Control", "no-store"}};
    response->write(status, body.dump(), headers);
}

// The console itself and the LAN; Sunshine's own classification of the address.
bool from_lan(const request_t &request) {
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    return net::from_address(address) != net::WAN;
}

template<class Handler>
auto lan_only(Handler handler) {
    return [handler](response_t response, request_t request) {
        if (!from_lan(request)) {
            response->write(SimpleWeb::StatusCode::client_error_forbidden, "Pairing is answered on the local network only");
            return;
        }
        handler(response, request);
    };
}

std::optional<nlohmann::json> json_body(const request_t &request) {
    try {
        return nlohmann::json::parse(request->content.string());
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
}

void run_pin_page(std::stop_token stop, uint16_t port, bool lan, std::function<void()> quit) {
    http_server server;
    server.config.port = port;
    // Default: loopback only, so the pairing page and /api/quit answer the console itself and no one on the network.
    // With the pin_lan dev flag, bind the LAN interface as nvhttp does (without an address Simple-Web-Server takes the
    // IPv6 wildcard, which on the console's FreeBSD kernel accepts no IPv4 connections, so an IPv4 network reached nobody).
    server.config.address = lan ? net::get_bind_address(net::af_from_enum_string(config::sunshine.address_family))
                                : std::string("127.0.0.1");
    // nvhttp::pin() waits for Moonlight to finish the handshake: a second thread keeps the list answering meanwhile.
    server.config.thread_pool_size = 2;
    server.resource["^/$"]["GET"] = lan_only([](response_t response, request_t) {
        SimpleWeb::CaseInsensitiveMultimap headers{{"Content-Type", "text/html; charset=utf-8"}, {"Cache-Control", "no-store"}};
        response->write(SimpleWeb::StatusCode::success_ok, page, headers);
    });
    server.resource["^/api/pairings$"]["GET"] = lan_only([](response_t response, request_t) {
        auto list = nlohmann::json::array();
        for (const auto &p : nvhttp::get_pending_pairings()) list.push_back({{"id", p.id}, {"name", p.name}, {"address", p.address}});
        send_json(response, SimpleWeb::StatusCode::success_ok, list);
    });
    server.resource["^/api/pin$"]["POST"] = lan_only([](response_t response, request_t request) {
        const auto body = json_body(request);
        const std::string id = body ? body->value("id", "") : "", pin = body ? body->value("pin", "") : "";
        std::string name = body ? body->value("name", "") : "";
        if (id.empty() || !nvhttp::is_valid_pairing_pin(pin)) {
            send_json(response, SimpleWeb::StatusCode::client_error_bad_request, {{"ok", false}, {"message", "Enter the PIN Moonlight shows."}});
            return;
        }
        if (name.empty()) name = "moonlight";
        BOOST_LOG(info) << "Pairing page: PIN submitted for a pending pairing"sv;
        const bool paired = nvhttp::pin(id, pin, name);
        send_json(response, SimpleWeb::StatusCode::success_ok,
                  {{"ok", paired}, {"message", paired ? "Paired." : "Pairing failed: wrong PIN, or Moonlight gave up."}});
    });
    server.resource["^/api/pin/cancel$"]["POST"] = lan_only([](response_t response, request_t request) {
        const auto body = json_body(request);
        const std::string id = body ? body->value("id", "") : "";
        const bool cancelled = !id.empty() && nvhttp::cancel_pairing(id);
        if (cancelled) BOOST_LOG(info) << "Pairing page: a pending pairing was denied"sv;
        send_json(response, SimpleWeb::StatusCode::success_ok,
                  {{"ok", cancelled}, {"message", cancelled ? "Denied." : "That request is gone already."}});
    });
    server.resource["^/api/quit$"]["POST"] = lan_only([quit](response_t response, request_t) {
        BOOST_LOG(info) << "Pairing page: the host was asked to stop"sv;
        send_json(response, SimpleWeb::StatusCode::success_ok, {{"ok", true}, {"message", "The host is stopping."}});
        // The answer leaves first: stopping the host also stops this server.
        std::thread([quit] {
            std::this_thread::sleep_for(200ms);
            quit();
        }).detach();
    });
    server.on_error = [](request_t, const SimpleWeb::error_code &) {};

    std::thread runner([&] {
        try {
            server.start([port](unsigned short bound) { BOOST_LOG(info) << "Pairing page on port "sv << bound; });
        } catch (const std::exception &e) {
            BOOST_LOG(error) << "Pairing page: can't listen on port "sv << port << ": "sv << e.what();
        }
    });
    std::stop_callback on_stop(stop, [&] { server.stop(); });
    runner.join();
}

}
