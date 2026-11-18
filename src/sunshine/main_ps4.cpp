// SPDX-License-Identifier: GPL-3.0-only
// The PS4 host's entry point; replaces Sunshine's main.cpp/entry_handler.cpp as main_ps5.cpp does on the PS5, with less
// around it: the GameStream HTTP(S) and RTSP services, mDNS discovery and the pairing page. No control socket, web page,
// UPnP or rest-mode recovery yet (main_ps5.cpp has them). It runs until the process is killed.
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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <rs.h>
extern "C" {
#include <ps4/kernel.h>
#include <ps4/klog.h>
#include "platform/ps5/diag.h"
int sceKernelSetProcessName(const char *name);
}

using namespace std::literals;

namespace {
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
    {
        std::jthread http_thread{nvhttp::start};
        std::jthread rtsp_thread{rtsp_stream::start};
        std::jthread pin_thread([pin_port](std::stop_token stop) { prospero::ps4::run_pin_page(stop, pin_port); });
        // Moonlight discovery; on any socket problem the responder logs and ends, the host runs on.
        std::jthread mdns_thread([name = config::nvhttp.sunshine_name](std::stop_token stop) {
            prospero::mdns::run_responder(stop, name, net::map_port(nvhttp::PORT_HTTP));
        });
        ps_diag_log("ProsperoSunshine: host ready, pairing page on port %u\n", static_cast<unsigned>(pin_port));
        while (!shutdown_event->view(5s)) {}
        BOOST_LOG(info) << "ProsperoSunshine host shutting down"sv;
        pin_thread.request_stop();
        mdns_thread.request_stop();
        proc::proc.terminate();
    }
    task_pool.stop();
    task_pool.join();
    ps_diag_log("ProsperoSunshine: host stopped\n");
    return 0;
}
