#include "player/sendspin_service.h"

#include "net/discovery.h"
#include "net/netinfo.h"
#include "platform/log.h"
#include "player/settings.h"
#include "version.h"

#include "file_persistence_provider.h"  // sendspin-cpp examples/common: atomic JSON blob store

#include <sendspin/artwork_role.h>
#include <sendspin/client.h>
#include <sendspin/color_role.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/persistence_codec.h>
#include <sendspin/persistence_keys.h>
#include <sendspin/player_role.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <utility>

using namespace sendspin;

namespace {

constexpr const char* kPath = "/sendspin";  // our WebSocket endpoint, announced over mDNS (path=)

const char* codec_name(SendspinCodecFormat c) {
    switch (c) {
        case SendspinCodecFormat::FLAC: return "flac";
        case SendspinCodecFormat::OPUS: return "opus";
        case SendspinCodecFormat::PCM: return "pcm";
        default: return "?";
    }
}

/// Nanoseconds the system has spent suspended since boot (Linux: BOOTTIME keeps counting through
/// suspend, MONOTONIC doesn't). -1 where that isn't available.
int64_t suspended_ns() {
#ifdef __linux__
    timespec boot{}, mono{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono) != 0) return -1;
    return (static_cast<int64_t>(boot.tv_sec) - mono.tv_sec) * 1000000000LL + (boot.tv_nsec - mono.tv_nsec);
#else
    return -1;
#endif
}

std::string read_line(const std::filesystem::path& path) {
    std::string s;
    std::getline(std::ifstream(path), s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

/// The device's own name, as the muOS dashboard shows it: the frontend's board title
/// ("Anbernic RG35-H"), else the board id ("rg35xx-h").
std::string device_name() {
    if (std::string t = read_line("/run/muos/board_title"); !t.empty()) return t;
    if (std::string b = read_line("/opt/muos/device/config/board/name"); !b.empty()) return b;
    return "MustardOS Sendspin";
}

bool supports(const ServerStateControllerObject& s, SendspinControllerCommand c) {
    return std::ranges::contains(s.supported_commands, c);
}

Rgb rgb(const RgbColor& c) {
    return {c[0], c[1], c[2]};
}

/// s on one line: settings.ini is line-based, and a name is shown on one line.
std::string one_line(std::string_view s) {
    std::string out(s);
    std::ranges::replace_if(out, [](char c) { return c == '\n' || c == '\r'; }, ' ');
    return out;
}

}  // namespace

struct SendspinService::Impl : PlayerRoleListener,
                               MetadataRoleListener,
                               ControllerRoleListener,
                               ArtworkRoleListener,
                               ColorRoleListener,
                               SendspinClientListener,
                               SendspinNetworkProvider {
    ServiceOptions opt;
    PlayerStatus st;
    SdlAudioSink sink;
    FilePersistenceProvider persistence;
    std::unique_ptr<Advertiser> advertiser;
    std::unique_ptr<SendspinClient> client;
    PlayerRole* player = nullptr;
    MetadataRole* metadata = nullptr;
    ControllerRole* controller = nullptr;
    ArtworkRole* artwork = nullptr;
    ColorRole* color = nullptr;

    // Local volume changes in flight: the server's replies to earlier steps arrive after newer
    // steps, so show the value the user set until the server reports it (or 1 s passes; then
    // loop() shows the real level, as the server may settle on a different one, e.g. 52 for 53).
    struct Pending {
        int target = -1;
        Uint64 until_ms = 0;
        void set(int v) {
            target = v;
            until_ms = SDL_GetTicks64() + 1000;
        }
        /// Value to display for a server report: the pending target while it is in flight.
        uint8_t filter(uint8_t reported) {
            if (target < 0) return reported;
            if (reported == target || SDL_GetTicks64() > until_ms) {
                target = -1;
                return reported;
            }
            return static_cast<uint8_t>(target);
        }
        /// True once, when the target has been held for its second without the server confirming it.
        bool expired() {
            if (target < 0 || SDL_GetTicks64() <= until_ms) return false;
            target = -1;
            return true;
        }
    };
    Pending pending_group_volume, pending_player_volume;

    int64_t last_suspended_ns = -1;
    Uint64 last_sync_ms = 0;
    double sync_interval_s = 0;
    uint32_t last_ip = 0;
    bool ip_checked = false;  // the first check only records the address
    Uint64 next_ip_check_ms = 0;

    bool records_stale = true;  // re-read the pairing records from the state file
    bool was_connected = false;

    /// Calls f(slot key, record) for each long-term pairing record in the state file (the SDK
    /// keeps its record store private), in slot order.
    void for_each_record(const std::function<void(const std::string&, const SendspinPairingRecord&)>& f) {
        for (size_t slot = 0; slot < SendspinClientConfig::DEFAULT_MAX_PAIRING_RECORDS; ++slot) {
            const std::string key = persistence_keys::record_slot_key(slot);
            auto blob = persistence.load_blob(key);
            if (!blob) continue;
            const auto rec = decode_pairing_record(blob->data(), blob->size());
            std::ranges::fill(*blob, uint8_t{0});  // it held a PSK
            if (rec) f(key, *rec);                 // else a free slot
        }
    }

    /// The servers we hold long-term pairing records for. Called at start and whenever the
    /// records may have changed.
    void load_paired_servers() {
        records_stale = false;
        std::vector<PairedServer> list;
        for_each_record([&](const std::string&, const SendspinPairingRecord& rec) {
            const auto name = settings.server_names.find(rec.server_id);
            list.push_back({rec.server_id, name != settings.server_names.end() ? name->second : std::string()});
        });
        st.paired_servers = std::move(list);
    }

    Settings settings;
    bool settings_loaded = false;
    bool settings_dirty = false;
    Uint64 settings_changed_ms = 0;

    void settings_changed() {
        // The level playing, not st.volume: that may be a pending target the server never confirms.
        if (player) settings.volume = player->get_volume();
        settings.muted = st.muted;
        settings.extra_latency_ms = static_cast<int>(sink.extra_latency_us() / 1000);
        settings_dirty = true;
        settings_changed_ms = SDL_GetTicks64();
    }

    std::mutex art_mutex;  // decode thread -> main loop hand-off
    std::shared_ptr<const Image> art_pending;

    explicit Impl(ServiceOptions o) : opt(std::move(o)), sink(opt.audio), persistence(opt.state_file) {}

    // --- SendspinNetworkProvider ---
    bool is_network_ready() override { return true; }  // host and device: the OS handles Wi-Fi

    // --- PlayerRoleListener (on_audio_write runs on the sync thread; the rest on the main loop) ---
    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
        return sink.write(data, length, timeout_ms);
    }
    void on_stream_start() override {
        const auto& p = player->get_current_stream_params();
        if (!p.is_complete()) {
            logx::warn("sendspin: stream start without complete parameters");
            st.stream.clear();
            sink.flush();
            return;
        }
        const std::string stream = std::format("{} {} Hz {} ch", codec_name(*p.codec), *p.sample_rate, *p.channels);
        logx::info("sendspin: stream start: {} {}-bit", stream, *p.bit_depth);
        if (sink.configure(*p.sample_rate, *p.channels, *p.bit_depth)) {
            st.stream = stream;
        } else {
            logx::error("sendspin: no audio output for {}", stream);
            st.stream.clear();
        }
    }
    void on_stream_end() override {
        logx::info("sendspin: stream end");
        st.stream.clear();
        sink.flush();
    }
    void on_volume_changed(uint8_t v) override {
        logx::info("sendspin: player volume {} (from server)", v);
        sink.set_volume(v);  // audio always follows the server
        st.volume = pending_player_volume.filter(v);
        settings_changed();
    }
    void on_mute_changed(bool m) override {
        st.muted = m;
        sink.set_muted(m);
        settings_changed();
    }

    // --- MetadataRoleListener ---
    void on_metadata(const ServerMetadataStateObject& md) override {
        // Each call carries the full state: overwrite everything so server-side clears propagate.
        st.title = md.title.value_or("");
        st.artist = md.artist.value_or("");
        st.album = md.album.value_or("");
        if (st.artist.empty()) st.artist = md.album_artist.value_or("");
        st.year = md.year.value_or(0);
        if (md.progress) {
            logx::info("sendspin: metadata: {} - {} (progress {}/{} ms, speed {})", st.artist, st.title,
                       md.progress->track_progress, md.progress->track_duration, md.progress->playback_speed);
        } else {
            logx::info("sendspin: metadata: {} - {} (no progress)", st.artist, st.title);
        }
    }
    void on_metadata_clear() override {
        st.title.clear();
        st.artist.clear();
        st.album.clear();
        st.year = 0;
    }

    // --- ControllerRoleListener ---
    void on_controller_state(const ServerStateControllerObject& s) override {
        st.has_controller = true;
        st.group_volume = pending_group_volume.filter(s.volume);
        st.shuffle = s.shuffle;
        st.repeat = s.repeat == SendspinRepeatMode::ONE   ? Repeat::One
                    : s.repeat == SendspinRepeatMode::ALL ? Repeat::All
                                                          : Repeat::Off;
        using C = SendspinControllerCommand;
        st.can_play = supports(s, C::PLAY);
        st.can_pause = supports(s, C::PAUSE);
        st.can_next = supports(s, C::NEXT);
        st.can_previous = supports(s, C::PREVIOUS);
        st.can_shuffle = supports(s, C::SHUFFLE) && supports(s, C::UNSHUFFLE);
        st.can_repeat = supports(s, C::REPEAT_OFF) && supports(s, C::REPEAT_ALL);
        st.can_volume = supports(s, C::VOLUME);
        logx::info("sendspin: controller: group volume {}{}, shuffle {}, repeat {}, {} commands", s.volume,
                   s.muted ? " (muted)" : "", s.shuffle, static_cast<int>(s.repeat), s.supported_commands.size());
    }
    void on_controller_state_clear() override {
        st.has_controller = false;
        st.can_play = st.can_pause = st.can_next = st.can_previous = false;
        st.can_shuffle = st.can_repeat = st.can_volume = false;
    }

    // --- ArtworkRoleListener ---
    void on_image_decode(uint8_t /*slot*/, const uint8_t* data, size_t length, SendspinImageFormat) override {
        auto img = decode_image(std::span(data, length));  // decode thread
        if (!img) return;
        std::scoped_lock lock(art_mutex);
        art_pending = std::move(img);
    }
    void on_image_display(uint8_t /*slot*/, uint32_t /*lateness_ms*/) override {
        std::shared_ptr<const Image> img;
        {
            std::scoped_lock lock(art_mutex);
            img = std::move(art_pending);
        }
        if (!img) return;
        logx::info("artwork: {}x{}", img->w, img->h);
        st.artwork = std::move(img);
        ++st.artwork_version;
    }
    void on_image_clear(uint8_t /*slot*/) override {
        {
            std::scoped_lock lock(art_mutex);
            art_pending.reset();
        }
        if (st.artwork) {
            st.artwork.reset();
            ++st.artwork_version;
        }
    }

    // --- ColorRoleListener ---
    void on_color(const ServerColorStateObject& c) override {
        Palette p;
        if (c.background_dark) p.background_dark = rgb(*c.background_dark);
        if (c.accent) p.accent = rgb(*c.accent);
        if (c.on_dark) p.on_dark = rgb(*c.on_dark);
        st.palette = p;
    }
    void on_color_clear() override { st.palette = Palette{}; }

    // NOLINTNEXTLINE(modernize-use-nodiscard): callers may ignore it, an unsupported command does nothing
    bool send(SendspinControllerCommand cmd) const {
        if (!controller || !supports(controller->get_controller_state(), cmd)) return false;
        return controller->send_command({.command = cmd});
    }

    // --- SendspinClientListener ---
    void on_time_sync_updated(float error) override {
        // Average interval between bursts (EMA), so the graph can say how fast it fills.
        const Uint64 now = SDL_GetTicks64();
        if (last_sync_ms) {
            const double dt = static_cast<double>(now - last_sync_ms) / 1000.0;
            sync_interval_s = sync_interval_s > 0 ? sync_interval_s * 0.7 + dt * 0.3 : dt;
            st.clock_sync_interval_s = static_cast<int>(std::lround(sync_interval_s));
        }
        last_sync_ms = now;
        st.clock_error_us.push_back(error);
        if (st.clock_error_us.size() > PlayerStatus::kClockHistory) st.clock_error_us.erase(st.clock_error_us.begin());
    }
    void on_trust_changed(ConnectionTrust trust) override {
        st.paired = trust == ConnectionTrust::USER;
        logx::info("sendspin: connection trust: {}", st.paired ? "paired" : "unpaired");
    }
    void on_pairing_started(const std::string& server_id) override {
        logx::info("sendspin: pairing started with {}", server_id);
    }
    void on_pairing_succeeded(const std::string& server_id) override {
        logx::info("sendspin: paired with {}", server_id);
        records_stale = true;  // the SDK has written the record by now
    }
    void on_pairing_failed(const std::string& server_id, SendspinPairAbortReason reason) override {
        logx::info("sendspin: pairing with {} failed (reason {})", server_id, static_cast<int>(reason));
    }
    void on_display_pairing_code(const std::string& code, SendspinPairingCodeFormat format) override {
        st.pairing_code = code;
        st.pairing_code_qr = format == SendspinPairingCodeFormat::QR_CODE;
        if (st.pairing_code_qr) logx::info("sendspin: pairing code shown as a QR code");
        else logx::info("sendspin: pairing code {}-{}", code.substr(0, 3), code.substr(3));
    }
    void on_clear_pairing_code() override {
        st.pairing_code.reset();
        st.pairing_code_qr = false;
    }
    void on_open_pairing_window() override {
        st.pairing_window_prompt = true;
        logx::info("sendspin: pairing needs confirmation on the device (press A)");
    }
    void on_close_pairing_window() override { st.pairing_window_prompt = false; }
};

SendspinService::SendspinService(ServiceOptions opt) : impl_(std::make_unique<Impl>(std::move(opt))) {}

SendspinService::~SendspinService() {
    stop();
}

bool SendspinService::start() {
    Impl& d = *impl_;
    // Once: a restart (forget_server, set_name) keeps the settings in memory, which are current.
    if (!d.settings_loaded) {
        if (d.opt.settings_file.empty())
            d.opt.settings_file = std::filesystem::path(d.opt.state_file).replace_filename("settings.ini");
        d.settings.load(d.opt.settings_file);
        d.settings_loaded = true;
    }
    // Name: --name, else settings.ini name=, else the device's own name.
    d.st.device_name = device_name();
    d.st.custom_name = !d.opt.name.empty() || !d.settings.name.empty();
    d.st.name = !d.opt.name.empty() ? d.opt.name : !d.settings.name.empty() ? d.settings.name : d.st.device_name;
    d.st.idle_screensaver = d.settings.idle_screensaver;

    SendspinClientConfig cfg;
    cfg.name = d.st.name;
    cfg.product_name = "Sendspin for MustardOS";
    cfg.software_version = MUOS_SENDSPIN_VERSION;
    cfg.server_port = d.opt.port;
    // We have a screen: offer dynamic pairing codes as digits (typed into the server) or as a QR
    // code (scanned by it); the server picks.
    cfg.pairing_code_out_channels = {SendspinPairingCodeChannel::DISPLAY};
    cfg.pairing_code_formats = {SendspinPairingCodeFormat::DIGITS, SendspinPairingCodeFormat::QR_CODE};

#ifdef SENDSPIN_ENABLE_MESSAGE_TRACE
    // Protocol-task callback: log JSON messages and audio chunk timestamp discontinuities.
    cfg.message_trace = [&d, last_ts = int64_t{0}, last_log_us = int64_t{0},
                         count = 0](const SendspinTracedMessage& m) mutable {
        if (m.direction != SendspinTraceDirection::INBOUND || !m.plaintext || m.plaintext_len == 0) return;
        if (m.audio_bytes > 0 && m.plaintext_len >= 9) {
            int64_t ts = 0;
            for (int i = 1; i <= 8; ++i) ts = (ts << 8) | m.plaintext[i];
            const int64_t client_ts = d.client ? d.client->get_client_time(ts) : 0;
            const int64_t lead_ms = client_ts ? (client_ts - m.stamp_us) / 1000 : 0;
            const int64_t delta_ms = last_ts ? (ts - last_ts) / 1000 : 0;
            ++count;
            if (count <= 3 || delta_ms > 1000 || delta_ms < -100 || m.stamp_us - last_log_us > 5000000) {
                logx::info("trace: audio #{} ts={} delta={} ms lead={} ms bytes={}", count, ts, delta_ms, lead_ms,
                           m.audio_bytes);
                last_log_us = m.stamp_us;
            }
            last_ts = ts;
            return;
        }
        const char* p = reinterpret_cast<const char*>(m.plaintext);
        const std::string_view text(p, m.plaintext_len);
        const size_t brace = text.find('{');
        if (brace == std::string_view::npos || text.find("server/time") != std::string_view::npos) return;
        if (text.find("stream/start") != std::string_view::npos) count = 0, last_ts = 0;
        logx::info("trace: <- {}", text.substr(brace, 300));
        // Metadata progress sits past the 300-char cut (after artwork_url): show it raw too.
        if (const size_t pg = text.find("\"progress\""); pg != std::string_view::npos)
            logx::info("trace:    {}", text.substr(pg, 110));
    };
#endif
    d.client = std::make_unique<SendspinClient>(std::move(cfg));
    d.client->set_persistence_provider(&d.persistence);
    d.client->set_network_provider(&d);
    d.client->set_listener(&d);
    d.client->set_unpaired_access_enabled(d.opt.unpaired_access);
    d.st.unpaired_access = d.opt.unpaired_access;

    PlayerRoleConfig pc;
    // 16-bit only: SDL2 has no packed 24-bit format, and the handheld DAC gains nothing from it.
    std::vector<AudioSupportedFormatObject> flac = {{SendspinCodecFormat::FLAC, 2, 48000, 16},
                                                    {SendspinCodecFormat::FLAC, 2, 44100, 16}};
    std::vector<AudioSupportedFormatObject> opus;
#ifdef SENDSPIN_ENABLE_OPUS
    opus = {{SendspinCodecFormat::OPUS, 2, 48000, 16}};
#endif
    std::vector<AudioSupportedFormatObject> pcm = {{SendspinCodecFormat::PCM, 2, 48000, 16},
                                                   {SendspinCodecFormat::PCM, 2, 44100, 16}};
    // The server picks from this list in order; settings.ini audio_format= moves one codec to the
    // front (default flac). FLAC: lossless, usually native 44.1 kHz (no resampling), cheap on the
    // A53. Opus saves Wi-Fi traffic but doesn't avoid Music Assistant's late-join gaps (see
    // AGENTS.md, known upstream issues).
    std::vector<std::vector<AudioSupportedFormatObject>*> order = {&flac, &opus, &pcm};
    const std::string& codec = d.settings.audio_format;
    d.st.audio_format = codec;
    d.st.audio_format_next = d.settings.audio_format;
    if (codec == "opus") order = {&opus, &flac, &pcm};
    else if (codec == "pcm") order = {&pcm, &flac, &opus};
    for (auto* group : order) pc.audio_formats.insert(pc.audio_formats.end(), group->begin(), group->end());
    // Lead the server should give the first chunk after a start or seek. The SDK's default allows
    // 75 ms for codec start, first decode and output on an ESP32-class device; our output alone
    // takes longer (on muOS ~108 ms at 44.1 kHz, the usual and slowest rate), so ask for the SDK's
    // priming and startup silence, a decode allowance and our output latency.
    constexpr int kDecodeAllowanceMs = 25;
    const auto output_ms = d.sink.expected_output_latency_us(44100) / 1000;
    pc.required_lead_time_ms = static_cast<uint16_t>(PlayerRoleConfig::INITIAL_SYNC_PRIMING_MS +
                                                     pc.extra_startup_silence_ms + kDecodeAllowanceMs + output_ms);
    logx::info("sendspin: asking servers for {} ms of startup lead", *pc.required_lead_time_ms);
    d.player = &d.client->add_player(std::move(pc));
    d.player->set_output_delay_adjustable(true);
    // The SDK starts at volume 0 and doesn't persist it; restore ours before the first client/state.
    d.player->update_volume(static_cast<uint8_t>(d.settings.volume));
    d.player->update_muted(d.settings.muted);
    d.sink.set_extra_latency_us(int64_t{d.settings.extra_latency_ms} * 1000);
    d.player->set_listener(&d);
    d.metadata = &d.client->add_metadata();
    d.metadata->set_listener(&d);
    d.controller = &d.client->add_controller();
    d.controller->set_listener(&d);

    ArtworkRoleConfig ac;
    ImageSlotPreference slot;
    slot.source = SendspinImageSource::ALBUM;
    slot.format = SendspinImageFormat::JPEG;
    slot.width = slot.height = static_cast<uint16_t>(std::clamp(d.opt.artwork_px, 64, 1024));
    slot.max_image_bytes = 512u * 1024u;  // big squares on larger screens; we have the RAM
    ac.preferred_formats = {slot};
    d.artwork = &d.client->add_artwork(std::move(ac));
    d.artwork->set_listener(&d);

    d.color = &d.client->add_color();
    d.color->set_listener(&d);

    d.sink.on_frames_played = [p = d.player](uint32_t frames, int64_t finish_us) {
        p->notify_audio_played(frames, finish_us);
    };

    if (!d.client->start()) {
        logx::error("sendspin: client start failed");
        return false;
    }
    d.st.started = true;
    d.st.volume = d.player->get_volume();
    d.st.muted = d.player->get_muted();
    d.sink.set_volume(d.st.volume);
    d.sink.set_muted(d.st.muted);

    logx::info("sendspin: '{}' listening on port {}, client_id {}, player volume {}{}", d.st.name, d.opt.port,
               d.client->client_id(), d.st.volume, d.st.muted ? " (muted)" : "");
    logx::info("sendspin: state file {}", d.opt.state_file.string());
    d.st.pairing_token = d.client->pairing_token().value_or("");
    if (!d.st.pairing_token.empty()) logx::info("sendspin: pairing token {}", d.st.pairing_token);
    d.load_paired_servers();
    logx::info("sendspin: {} paired server(s)", d.st.paired_servers.size());
    if (d.opt.unpaired_access) logx::info("sendspin: unpaired access ENABLED (any server on the LAN can play)");

    if (d.opt.advertise) {
        d.advertiser = make_advertiser();
        d.advertiser->start(d.st.name, d.opt.port, kPath);
    }
    if (!d.opt.connect_url.empty()) {
        logx::info("sendspin: connecting to {}", d.opt.connect_url);
        d.client->connect_to(d.opt.connect_url);
    }
    return true;
}

void SendspinService::loop() {
    Impl& d = *impl_;
    if (!d.client || !d.st.started) return;

    // The one place that watches our address (the UI shows st.address; the mDNS responder follows
    // reannounce()). Losing it (muOS "sleep" on the RG35XX H stops Wi-Fi without suspending the
    // kernel; or Wi-Fi drops) leaves a dead connection the SDK would only notice through timeouts,
    // while the UI keeps estimating progress: drop it at once (RESTART: the server may reconnect).
    // A new address is announced at once, so the server finds us without waiting.
    if (SDL_GetTicks64() >= d.next_ip_check_ms) {
        d.next_ip_check_ms = SDL_GetTicks64() + 1000;
        const uint32_t ip = local_ipv4();
        if (d.ip_checked && ip != d.last_ip) {
            if (!ip) {
                logx::info("sendspin: network lost: dropping the connection");
                d.client->disconnect(SendspinGoodbyeReason::RESTART);
            } else {
                logx::info("sendspin: address {}: re-announcing", ipv4_text(ip));
                if (d.advertiser) d.advertiser->reannounce();
            }
        }
        d.st.address = ipv4_text(ip);
        d.last_ip = ip;
        d.ip_checked = true;
    }

    // Where the kernel really suspends, the same applies after resume: BOOTTIME jumps ahead of
    // MONOTONIC by the time spent suspended.
    if (const int64_t s = suspended_ns(); s >= 0) {
        if (d.last_suspended_ns >= 0 && s - d.last_suspended_ns > 1000000000LL) {
            logx::info("sendspin: resumed after {:.1f} s suspended: dropping the stale connection",
                       static_cast<double>(s - d.last_suspended_ns) / 1e9);
            d.client->disconnect(SendspinGoodbyeReason::RESTART);
            if (d.advertiser) d.advertiser->reannounce();
        }
        d.last_suspended_ns = s;
    }

    d.client->loop();
    // Debounced: holding volume-up shouldn't write the SD card ten times a second.
    if (d.settings_dirty && SDL_GetTicks64() - d.settings_changed_ms > 1000) {
        d.settings.save(d.opt.settings_file);
        d.settings_dirty = false;
    }

    d.st.connected = d.client->is_connected();
    if (auto info = d.client->get_server_information()) {
        d.st.server_name = std::move(info->name);
        d.st.server_id = std::move(info->server_id);
    } else if (!d.st.connected) {
        d.st.server_name.clear();
        d.st.server_id.clear();
    }
    // A server/unpair ends the connection: re-read the records whenever one ends.
    if (d.was_connected && !d.st.connected) d.records_stale = true;
    d.was_connected = d.st.connected;
    if (d.records_stale) d.load_paired_servers();
    const GroupUpdateObject& g = d.client->get_group_state();
    d.st.group_name = g.group_name.value_or("");
    d.st.playing = g.playback_state && *g.playback_state == SendspinPlaybackState::PLAYING;
    d.st.progress_ms = d.metadata->get_track_progress_ms();
    d.st.duration_ms = d.metadata->get_track_duration_ms();
    d.st.extra_latency_ms = static_cast<int>(d.sink.extra_latency_us() / 1000);
    d.st.platform_latency_ms = static_cast<int>(d.sink.platform_latency_us() / 1000);
    if (!d.st.connected) d.st.paired = false;
    if (d.pending_player_volume.expired()) d.st.volume = d.player->get_volume();
    if (d.pending_group_volume.expired()) d.st.group_volume = d.controller->get_controller_state().volume;

    // Remember the names of paired servers, for the Pairing page while they're away.
    if (d.st.paired && !d.st.server_id.empty() && !d.st.server_name.empty()) {
        const std::string name = one_line(d.st.server_name);
        if (std::string& saved = d.settings.server_names[d.st.server_id]; saved != name) {
            saved = name;
            d.settings_changed();
            d.records_stale = true;  // picks up the name
        }
    }
}

void SendspinService::stop() {
    Impl& d = *impl_;
    if (d.settings_dirty) {
        d.settings.save(d.opt.settings_file);
        d.settings_dirty = false;
    }
    if (d.advertiser) {
        d.advertiser->stop();
        d.advertiser.reset();
    }
    if (d.client && d.st.started) {
        d.client->stop();  // goodbye to the server, joins role threads
        // start() refills it, the settings-derived fields first: forget_server() and set_name()
        // restart at once, and nothing reads the status after the final stop.
        d.st = PlayerStatus{};
    }
    d.sink.close();
}

const PlayerStatus& SendspinService::status() const {
    return impl_->st;
}

void SendspinService::confirm_pairing_window() {
    if (impl_->client) impl_->client->confirm_pairing_window();
}

void SendspinService::forget_server(std::string_view id) {
    Impl& d = *impl_;
    if (!d.client || !d.st.started) return;
    const std::string server_id(id);  // a copy: stop() clears the status that id may point into
    // The SDK holds its records in RAM and offers no call to drop one, and a record only goes
    // away when its slot is cleared: so stop the client, clear the slot in the state file, and
    // start again (pairing records are the application's to wipe; integration-guide.md).
    logx::info("sendspin: forgetting server {}", server_id);
    stop();
    bool found = false;
    d.for_each_record([&](const std::string& key, const SendspinPairingRecord& rec) {
        if (rec.server_id != server_id) return;
        const std::array<uint8_t, persistence_keys::RECORD_SLOT_SIZE> zero{};  // a free slot
        if (!d.persistence.save_blob(key, zero.data(), zero.size())) logx::error("sendspin: can't clear {}", key);
        found = true;
    });
    if (!found) logx::warn("sendspin: no pairing record for {}", server_id);
    // Nor should it keep the last-played server's priority (Sendspin specification, connection.md,
    // "Multiple servers").
    if (auto last = d.persistence.load_blob(persistence_keys::LAST_PLAYED);
        last && base64url_encode(last->data(), last->size()) == server_id) {
        const uint8_t none = 0;  // a blob of the wrong size reads as absent
        d.persistence.save_blob(persistence_keys::LAST_PLAYED, &none, 0);
    }
    if (d.settings.server_names.erase(server_id) > 0) d.settings.save(d.opt.settings_file);
    if (!start()) logx::error("sendspin: restart after forgetting a server failed");
}

void SendspinService::set_name(std::string_view name) {
    Impl& d = *impl_;
    if (!d.client || !d.st.started) return;
    const std::string n(trim(one_line(name)));
    if (n == d.settings.name && d.opt.name.empty()) return;
    logx::info("sendspin: player name '{}'", n.empty() ? "(the device's)" : n);
    stop();  // the name is in the client's config (client/hello) and the mDNS announcement
    d.opt.name.clear();
    d.settings.name = n;
    d.settings.save(d.opt.settings_file);
    if (!start()) logx::error("sendspin: restart after renaming failed");
}

void SendspinService::play_pause() {
    const Impl& d = *impl_;
    using C = SendspinControllerCommand;
    if (d.st.playing) {
        if (!d.send(C::PAUSE)) d.send(C::STOP);
    } else {
        d.send(C::PLAY);
    }
}

void SendspinService::next() {
    impl_->send(SendspinControllerCommand::NEXT);
}

void SendspinService::previous() {
    impl_->send(SendspinControllerCommand::PREVIOUS);
}

void SendspinService::toggle_shuffle() {
    const Impl& d = *impl_;
    d.send(d.st.shuffle ? SendspinControllerCommand::UNSHUFFLE : SendspinControllerCommand::SHUFFLE);
}

void SendspinService::cycle_repeat() {
    const Impl& d = *impl_;
    using C = SendspinControllerCommand;
    switch (d.st.repeat) {
        case Repeat::Off: d.send(C::REPEAT_ALL); break;
        case Repeat::All: d.send(C::REPEAT_ONE); break;
        case Repeat::One: d.send(C::REPEAT_OFF); break;
    }
}

void SendspinService::seek_relative(int delta_ms) {
    const Impl& d = *impl_;
    if (!d.controller) return;
    const auto& cs = d.controller->get_controller_state();
    if (supports(cs, SendspinControllerCommand::SEEK_RELATIVE)) {
        d.controller->send_command({.command = SendspinControllerCommand::SEEK_RELATIVE, .offset_ms = delta_ms});
    } else if (supports(cs, SendspinControllerCommand::SEEK)) {
        const int64_t target = static_cast<int64_t>(d.metadata->get_track_progress_ms()) + delta_ms;
        const int64_t max = cs.seek_max_ms ? static_cast<int64_t>(*cs.seek_max_ms) : target;
        d.controller->send_command(
            {.command = SendspinControllerCommand::SEEK,
             .position_ms = static_cast<uint32_t>(std::clamp<int64_t>(target, 0, std::max<int64_t>(0, max)))});
    }
}

void SendspinService::change_group_volume(int delta) {
    Impl& d = *impl_;
    if (!d.st.can_volume) {
        change_volume(delta);  // no group control offered: fall back to this player
        return;
    }
    const int v = std::clamp(static_cast<int>(d.st.group_volume) + delta, 0, 100);
    if (d.controller->send_command({.command = SendspinControllerCommand::VOLUME, .volume = static_cast<uint8_t>(v)})) {
        d.pending_group_volume.set(v);
        d.st.group_volume = static_cast<uint8_t>(v);  // optimistic; held until the server confirms
    }
}

void SendspinService::change_volume(int delta) {
    Impl& d = *impl_;
    if (!d.player) return;
    const int v = std::clamp(static_cast<int>(d.player->get_volume()) + delta, 0, 100);
    d.player->update_volume(static_cast<uint8_t>(v));  // reported to the server in client/state
    d.pending_player_volume.set(v);
    d.st.volume = static_cast<uint8_t>(v);
    d.sink.set_volume(static_cast<uint8_t>(v));
    d.settings_changed();
}

void SendspinService::change_latency(int delta_ms) {
    Impl& d = *impl_;
    constexpr int64_t max_us = int64_t{kMaxExtraLatencyMs} * 1000;
    const int64_t us = std::clamp(d.sink.extra_latency_us() + int64_t{delta_ms} * 1000, -max_us, max_us);
    d.sink.set_extra_latency_us(us);
    d.settings_changed();
    logx::info("audio: extra output latency {:+} ms (platform {} ms)", us / 1000, d.sink.platform_latency_us() / 1000);
}

void SendspinService::set_idle_screensaver(bool on) {
    Impl& d = *impl_;
    d.st.idle_screensaver = on;
    d.settings.idle_screensaver = on;
    d.settings_changed();
}

void SendspinService::set_audio_format(std::string_view codec) {
    Impl& d = *impl_;
    d.st.audio_format_next = codec;
    d.settings.audio_format = codec;
    d.settings_changed();
}
