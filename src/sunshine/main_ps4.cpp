// SPDX-License-Identifier: GPL-3.0-only
// The PS4 host's entry point; replaces Sunshine's main.cpp/entry_handler.cpp as main_ps5.cpp does on the PS5, with less
// around it: the GameStream HTTP(S) and RTSP services, mDNS discovery and the pairing page. No control socket, web page,
// UPnP or rest-mode recovery yet (main_ps5.cpp has them). It runs until the pairing page's "Stop host" (POST /api/quit).
#include "platform/ps4/pbs_session.h"  // ps_pbs_shutdown: release the warm capture daemon on host exit
#include "platform/ps5/mdns_responder.hpp"
#include "sunshine/pin_page_ps4.hpp"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/globals.h"
#include "src/httpcommon.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/video.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <sys/ucontext.h>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <rs.h>
extern "C" {
#include <ps4/kernel.h>
#include <ps4/klog.h>
#include "platform/ps5/diag.h"
int sceKernelSetProcessName(const char *name);
// patch 0003: resume SceRemotePlay if the host crashes while it is held under ptrace, so a crash can't freeze the console.
void pbs_inject_emergency_detach(void);
// On-screen system notification (the payload SDK's hello_world request; the message starts at byte 45). Shown on the
// console's own screen so the operator sees the host is up and where to pair, without a second device.
typedef struct {
    char reserved[45];
    char message[3075];
} ps4_notify_request;
int sceKernelSendNotificationRequest(int, ps4_notify_request *, size_t, int);
}

using namespace std::literals;

namespace {
// Fatal-signal handler. The host drives SceRemotePlay through ptrace; if it dies while the daemon is stopped mid-call, the
// daemon stays frozen and SceShellCore (which depends on it) hangs, locking the whole console. So on any fatal signal we
// first resume the daemon (pbs_inject_emergency_detach, async-signal-safe), write one crash line to host_diag.log, then
// _exit so GoldHEN can recover. Only async-signal-safe calls are used here.
int g_crash_fd = -1;
void crash_puts(const char *s) {
    if (g_crash_fd >= 0) {
        const ssize_t r = write(g_crash_fd, s, std::strlen(s));
        (void)r;
    }
}
void crash_hex(unsigned long v) {
    char b[16];
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) b[i] = h[(v >> ((15 - i) * 4)) & 15];
    if (g_crash_fd >= 0) {
        const ssize_t r = write(g_crash_fd, b, sizeof b);
        (void)r;
    }
}
void crash_handler(int sig, siginfo_t *info, void *context) {
    pbs_inject_emergency_detach();  // CRITICAL first: never leave SceRemotePlay ptrace-stopped
    static const char h[] = "0123456789abcdef";
    char sg[2] = {h[(sig >> 4) & 15], h[sig & 15]};
    crash_puts("ProsperoSunshine host crash: sig=");
    if (g_crash_fd >= 0) {
        const ssize_t r = write(g_crash_fd, sg, 2);
        (void)r;
    }
    crash_puts(" rip=");
    crash_hex(context ? static_cast<unsigned long>(static_cast<ucontext_t *>(context)->uc_mcontext.mc_rip) : 0);
    crash_puts(" addr=");
    crash_hex(info ? reinterpret_cast<unsigned long>(info->si_addr) : 0);
    crash_puts("\n");
    _exit(128 + sig);
}
// Put one line on the console's screen via the system notification queue.
void ps4_notify(const std::string &text) {
    ps4_notify_request request;
    std::memset(&request, 0, sizeof request);
    std::strncpy(request.message, text.c_str(), sizeof request.message - 1);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

// Where to tell the operator to pair. With the pin_lan dev flag the page is on the LAN, so show the console's address;
// by default it is bound to loopback, so pairing is done in the console's own browser.
std::string pairing_notice(uint16_t pin_port, bool pin_lan) {
    if (pin_lan) {
        char ip[INET_ADDRSTRLEN] = {};
        if (const auto lan = prospero::mdns::lan_address(); lan && inet_ntop(AF_INET, &*lan, ip, sizeof ip))
            return "ProsperoSunshine is running. Pair at http://"s + ip + ":" + std::to_string(pin_port);
    }
    return "ProsperoSunshine is running. To pair, open http://127.0.0.1:"s + std::to_string(pin_port) +
           " in the PS4 browser.";
}

void install_crash_handler() {
    g_crash_fd = open((platf::appdata() / "host_diag.log").string().c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGSYS, SIGABRT}) sigaction(sig, &sa, nullptr);
}

// Sunshine logs to stdout and a file; on the console the kernel log is what developers watch.
struct klog_backend : boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
    void consume(const boost::log::record_view &record, const std::string &) {
        const auto severity = record.attribute_values()["Severity"].extract<int>();
        const auto message = record.attribute_values()["Message"].extract<std::string>();
        klog_printf("ProsperoSunshine: host [%d] %s\n", severity ? severity.get() : 2, message ? message.get().c_str() : "");
    }
};
using klog_sink = boost::log::sinks::synchronous_sink<klog_backend>;

boost::shared_ptr<klog_sink> add_klog_sink() {
    namespace expr = boost::log::expressions;
    auto sink = boost::make_shared<klog_sink>();
    sink->set_formatter(expr::stream << expr::smessage);
    sink->set_filter(expr::attr<int>("Severity") >= 2);  // info and above until the config is read
    boost::log::core::get()->add_sink(sink);
    return sink;
}
}

int main(int argc, char *argv[]) {
    // The ELF loader names every payload "payload.elf"; this is what a process list shows.
    sceKernelSetProcessName("ProsperoSunshine");
    const auto klog = add_klog_sink();
    std::error_code ec;
    std::filesystem::create_directories(platf::appdata(), ec);
    char header[160];
    std::snprintf(header, sizeof header, "ProsperoSunshine: PS4 host version=%s commit=%s firmware=%#x", PROJECT_VERSION,
                  PROJECT_VERSION_COMMIT, kernel_get_fw_version());
    ps_diag_open((platf::appdata() / "host_diag.log").c_str(), header);
    // Catch fatal signals: resume SceRemotePlay (so a crash can't freeze the console) and record the fault, then exit.
    install_crash_handler();
    // config::parse copies a template apps.json from Sunshine's assets when none exists; there are no assets on the
    // console, and the app list is fixed in process_ps4.cpp anyway.
    if (const auto apps = platf::appdata() / "apps.json"; !std::filesystem::exists(apps, ec))
        std::ofstream(apps) << R"({"env":{},"apps":[{"name":"PS4 Screen"}]})" << '\n';
    // A dead client is noticed after 2 s instead of upstream's 10 (Moonlight pings every 100 ms); sunshine.conf can override it.
    config::stream.ping_timeout = 2s;
    mail::man = std::make_shared<safe::mail_raw_t>();
    if (const int rc = config::parse(argc, argv)) {
        ps_diag_log("ProsperoSunshine: host config::parse returned %d\n", rc);
        return rc < 0 ? 1 : 0;
    }
    klog->set_filter(boost::log::expressions::attr<int>("Severity") >= config::sunshine.min_log_level);
    auto log_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
    if (!log_guard) ps_diag_log("ProsperoSunshine: host logging failed to initialize\n");
    BOOST_LOG(info) << "ProsperoSunshine PS4 host "sv << PROJECT_VERSION << " (commit "sv << PROJECT_VERSION_COMMIT
                    << ") starting, data in "sv << platf::appdata().string();

    task_pool.start(1);
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto platform_guard = platf::init();
    auto proc_guard = proc::init();
    reed_solomon_init();
    auto input_guard = input::init();
    if (video::probe_encoders()) {
        BOOST_LOG(fatal) << "Video capture unavailable"sv;
        return 1;
    }
    if (http::init()) {
        ps_diag_log("ProsperoSunshine: host HTTP interface failed to initialize\n");
        BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;
        return 1;
    }

    const uint16_t pin_port = net::map_port(nvhttp::PORT_HTTP + 1);
    // The pairing page binds loopback only (console-local pairing and quit) unless the operator drops an empty pin_lan
    // file, which opens it to the LAN for development (ps4_deploy.sh's remote /api/quit), like control_lan on the PS5.
    const bool pin_lan = std::filesystem::exists(platf::appdata() / "pin_lan", ec);
    {
        std::jthread http_thread{nvhttp::start};
        std::jthread rtsp_thread{rtsp_stream::start};
        std::jthread pin_thread([pin_port, pin_lan, shutdown_event](std::stop_token stop) {
            prospero::ps4::run_pin_page(stop, pin_port, pin_lan, [shutdown_event] { shutdown_event->raise(true); });
        });
        // Moonlight discovery; on any socket problem the responder logs and ends, the host runs on.
        std::jthread mdns_thread([name = config::nvhttp.sunshine_name](std::stop_token stop) {
            prospero::mdns::run_responder(stop, name, net::map_port(nvhttp::PORT_HTTP));
        });
        ps_diag_log("ProsperoSunshine: host ready, pairing page on port %u (%s)\n", static_cast<unsigned>(pin_port),
                    pin_lan ? "LAN" : "loopback");
        // Tell the operator, on the console's own screen, that the host is up and where to pair.
        ps4_notify(pairing_notice(pin_port, pin_lan));
        while (!shutdown_event->view(5s)) {}
        BOOST_LOG(info) << "ProsperoSunshine host shutting down"sv;
        ps_diag_log("ProsperoSunshine: host stopping\n");
        // A service thread that doesn't end would keep the ports, and the next host couldn't bind them: after 10 s the
        // process ends anyway.
        std::thread([] {
            std::this_thread::sleep_for(10s);
            ps_diag_log("ProsperoSunshine: host did not stop within 10 s, exiting\n");
            std::_Exit(0);
        }).detach();
        pin_thread.request_stop();
        mdns_thread.request_stop();
        proc::proc.terminate();
    }
    task_pool.stop();
    task_pool.join();
    // The stream is down (proc terminate above), so release the capture daemon we were holding warm across reconnects.
    ps_pbs_shutdown();
    ps_diag_log("ProsperoSunshine: host stopped\n");
    // Returning from main doesn't end a payload on the PS4: the process stayed until the watchdog above ended it, 10 s
    // later and next to the next host. The services are down and the logs written, so it ends here.
    std::_Exit(0);
}
