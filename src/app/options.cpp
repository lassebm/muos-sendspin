#include "app/options.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace app {

namespace {

constexpr std::string_view kUsage = R"(usage: {} [options]
Sendspin:
  --name NAME          player name shown by servers (default: settings.ini name=,
                       else the device's name)
  --port N             listen port (default 8928)
  --server URL         also dial this server, e.g. ws://192.168.1.10:8927/sendspin
  --unpaired-access    let unpaired servers play (development convenience)
  --no-mdns            don't advertise _sendspin._tcp
  --state FILE         keypair/pairing store (default: per-user app data on a desktop,
                       state.json next to the binary on the device); settings.ini
                       (volume, latency tuning) is kept beside it
  --audio-buffer N     SDL audio buffer in frames (default 1024)
  --audio-queued N     device buffers SDL queues ahead, for the latency estimate
                       (default 2 on macOS, 1 elsewhere)
  --audio-os-latency MS  output latency below SDL (default: CoreAudio's report on
                       macOS, 62 ms measured on muOS, 0 on Linux desktops)
  --log-level LEVEL    none|error|warn|info|debug|verbose (default info)
  --log-file FILE      log to FILE instead of stderr; keeps the previous two runs,
                       numbered before the extension (Sendspin.log: Sendspin.1.log,
                       Sendspin.2.log), and rotates at 1 MiB
Display:
  --size WxH           window size (default 640x480; windowed mode only)
  --fullscreen         fullscreen at the desktop display mode (default on device)
  --windowed           windowed (default on a desktop)
  --no-hidpi           1 window point = 1 pixel on macOS Retina displays
  --no-focus           open the window without taking keyboard focus
  --gamepad-ui         draw the UI as on a buttons-only handheld: gamepad hints, no
                       pointer controls (for screenshots on a desktop)
Testing:
  --demo               mock Now Playing content, no sendspin client
  --demo-track N       start the demo on track N
  --demo-pairing F     show the demo's pairing overlay: digits or qr
  --demo-artwork FILE  cover art (JPEG/PNG) for the demo's first track
  --menu PAGE          open the menu at start: main, help, settings, sync, pairing,
                       name, info, quit
  --frames N           quit after N frames
  --screenshot FILE    save the last frame as BMP (use with --frames)
  -h, --help           show this help
)";

template <typename T>
std::optional<T> parse_number(std::string_view s) {
    T v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<sendspin::LogLevel> parse_log_level(std::string_view s) {
    using sendspin::LogLevel;
    static constexpr std::pair<std::string_view, LogLevel> kLevels[] = {
        {"none", LogLevel::NONE}, {"error", LogLevel::ERROR}, {"warn", LogLevel::WARN},
        {"info", LogLevel::INFO}, {"debug", LogLevel::DEBUG}, {"verbose", LogLevel::VERBOSE},
    };
    const auto* const it = std::ranges::find(kLevels, s, &std::pair<std::string_view, LogLevel>::first);
    return it == std::end(kLevels) ? std::nullopt : std::optional(it->second);
}

}  // namespace

ParseResult parse_options(int argc, char** argv) {
    Options o;
    const std::span args(argv + 1, static_cast<size_t>(std::max(0, argc - 1)));
    const std::string_view argv0 = argc > 0 ? argv[0] : "muos-sendspin";
    const auto fail = [](std::string_view message) {
        std::println(stderr, "{}", message);
        return std::unexpected(2);
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        // The option's value; an error (after a message) when it's missing.
        const auto value = [&] -> std::expected<std::string_view, int> {
            if (i + 1 < args.size()) return args[++i];
            std::println(stderr, "{} needs a value", a);
            return std::unexpected(2);
        };
        // An integer value within [lo, hi].
        const auto number = [&](int lo, int hi) {
            return value().and_then([&](std::string_view v) -> std::expected<int, int> {
                if (const auto n = parse_number<int>(v); n && *n >= lo && *n <= hi) return *n;
                std::println(stderr, "{} must be a number in {}..{}", a, lo, hi);
                return std::unexpected(2);
            });
        };
        // For transform(): stores the value in out, converted to out's type.
        const auto into = [](auto& out) {
            return [&out](const auto& v) { out = static_cast<std::remove_cvref_t<decltype(out)>>(v); };
        };

        std::expected<void, int> parsed;  // checked after the chain: a missing or bad value
        if (a == "--size") {
            const auto v = value();
            if (!v) return std::unexpected(v.error());
            const auto x = v->find('x');
            const auto w = parse_number<int>(v->substr(0, x));
            const auto h = x == std::string_view::npos ? std::nullopt : parse_number<int>(v->substr(x + 1));
            if (!w || !h || *w <= 0 || *h <= 0) return fail("bad --size (expected WxH, e.g. 720x720)");
            o.width = *w;
            o.height = *h;
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--windowed") {
            o.fullscreen = false;
        } else if (a == "--no-hidpi") {
            o.hidpi = false;
        } else if (a == "--no-focus") {
            o.no_focus = true;
        } else if (a == "--gamepad-ui") {
            o.gamepad_ui = true;
        } else if (a == "--frames") {
            parsed = number(1, 1'000'000).transform(into(o.frames));
        } else if (a == "--screenshot") {
            parsed = value().transform(into(o.screenshot));
        } else if (a == "--demo") {
            o.demo = true;
        } else if (a == "--demo-track") {
            parsed = number(0, 100).transform(into(o.demo_track));
            o.demo = true;
        } else if (a == "--demo-pairing") {
            parsed = value().transform(into(o.demo_pairing));
            if (parsed && o.demo_pairing != "digits" && o.demo_pairing != "qr")
                return fail("--demo-pairing must be digits or qr");
            o.demo = true;
        } else if (a == "--demo-artwork") {
            parsed = value().transform(into(o.demo_artwork));
            o.demo = true;
        } else if (a == "--menu") {
            parsed = value().transform(into(o.menu));
        } else if (a == "--name") {
            parsed = value().transform(into(o.service.name));
        } else if (a == "--port") {
            parsed = number(1, 65535).transform(into(o.service.port));
        } else if (a == "--server") {
            parsed = value().transform(into(o.service.connect_url));
        } else if (a == "--unpaired-access") {
            o.service.unpaired_access = true;
        } else if (a == "--no-mdns") {
            o.service.advertise = false;
        } else if (a == "--state") {
            parsed = value().transform(into(o.service.state_file));
        } else if (a == "--audio-buffer") {
            int& frames = o.service.audio.buffer_frames;
            parsed = number(64, 16384).transform(into(frames));
            if (parsed && (frames & (frames - 1)) != 0) return fail("--audio-buffer must be a power of two");
        } else if (a == "--audio-queued") {
            parsed = number(0, 8).transform(into(o.service.audio.queued_buffers));
        } else if (a == "--audio-os-latency") {
            parsed = number(0, 1000).transform([&](int ms) { o.service.audio.os_latency_us = ms * 1000; });
        } else if (a == "--log-file") {
            parsed = value().transform(into(o.log_file));
        } else if (a == "--log-level") {
            const auto v = value();
            if (!v) return std::unexpected(v.error());
            const auto level = parse_log_level(*v);
            if (!level) return fail("--log-level must be none, error, warn, info, debug or verbose");
            o.log_level = *level;
        } else if (a == "-h" || a == "--help") {
            std::print(kUsage, argv0);
            return std::unexpected(0);
        } else {
            std::println(stderr, "unknown option: {}", a);
            std::print(stderr, kUsage, argv0);
            return std::unexpected(2);
        }
        if (!parsed) return std::unexpected(parsed.error());
    }
    return o;
}

}  // namespace app
