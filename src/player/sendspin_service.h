#pragma once

#include "player/artwork.h"
#include "player/audio_sdl.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Rgb = std::array<uint8_t, 3>;

/// Server palette (color role); every field optional, replaced wholesale on each update.
struct Palette {
    std::optional<Rgb> background_dark, accent, on_dark;
};

enum class Repeat { Off, One, All };

/// A server this player holds a long-term pairing record for.
struct PairedServer {
    std::string id;    // server_id (base64url public key)
    std::string name;  // as the server last named itself; empty if never seen
    bool operator==(const PairedServer&) const = default;
};

/// What the UI shows. Written only on the main loop thread (inside SendspinService::loop()),
/// read by the UI on the same thread, so no locking is needed.
struct PlayerStatus {
    static constexpr size_t kClockHistory = 30;  // time-sync bursts kept for the Sync graph

    bool started = false;
    std::string address;                // this device's IPv4 address as text; empty without a network
    std::string name;                   // the player name in use
    std::string device_name;            // the device's own name: the player name unless one is set
    bool custom_name = false;           // name comes from settings.ini name= (or --name)
    bool idle_screensaver = false;      // settings.ini: let muOS dim the display while playing
    std::string audio_format;           // codec the client asked for at start (flac / opus / pcm)
    std::string audio_format_next;      // the saved setting, used at the next start
    std::vector<float> clock_error_us;  // SDK time-sync error per burst, newest last (graph)
    int clock_sync_interval_s = 0;      // average seconds between time-sync bursts (0: unknown)
    bool connected = false;
    bool paired = false;           // connection trust is USER (long-term pairing record)
    bool unpaired_access = false;  // unpaired servers may play too (--unpaired-access)
    std::string server_name;       // from the server's hello (e.g. "Music Assistant (d5369777-...)")

    std::string group_name;
    bool playing = false;

    std::string title, artist, album;
    uint16_t year = 0;
    uint32_t progress_ms = 0;
    uint32_t duration_ms = 0;

    uint8_t volume = 100;  // this player's volume
    bool muted = false;

    // Controller role: the group as a whole.
    bool has_controller = false;
    uint8_t group_volume = 0;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    bool can_play = false, can_pause = false, can_next = false, can_previous = false;
    bool can_shuffle = false, can_repeat = false, can_volume = false;

    std::shared_ptr<const Image> artwork;  // nullptr: none
    uint32_t artwork_version = 0;          // bumps on every change (new image or clear)
    Palette palette;
    std::string stream;           // e.g. "flac 48000 Hz 2 ch", empty when no stream
    int extra_latency_ms = 0;     // user tuning on top of the estimate
    int platform_latency_ms = 0;  // output latency below SDL (SdlAudioSink::platform_latency_us)

    std::optional<std::string> pairing_code;   // shown while a pairing attempt runs: six digits, or a token
    bool pairing_code_qr = false;              // pairing_code is a pairing token to show as a QR code
    bool pairing_window_prompt = false;        // operator gesture needed: press A to allow pairing
    std::string server_id;                     // the connected server's, empty when not connected
    std::vector<PairedServer> paired_servers;  // from the state file, in record-slot order
    std::string pairing_token;                 // "SP:..." Pairing PSK token: a server can scan it to pair (QR)
};

struct ServiceOptions {
    std::string name;  // player name; empty: settings.ini name=, else the device's name
    uint16_t port = 8928;
    std::string connect_url;              // non-empty: dial this server (client-initiated)
    bool unpaired_access = false;         // let unpaired servers play (development convenience)
    bool advertise = true;                // mDNS _sendspin._tcp
    std::filesystem::path state_file;     // persisted keypair + pairing records (JSON)
    std::filesystem::path settings_file;  // volume, mute, latency tuning (key=value); default: next to state_file
    SdlAudioSink::Options audio;
    int artwork_px = 400;  // requested artwork size (square), from the layout
};

/// Owns the sendspin-cpp client, its roles, the audio sink and the mDNS advertisement.
/// All methods are main-loop-thread only.
class SendspinService {
public:
    explicit SendspinService(ServiceOptions opt);
    ~SendspinService();
    SendspinService(const SendspinService&) = delete;
    SendspinService& operator=(const SendspinService&) = delete;

    [[nodiscard]] bool start();
    void loop();  // call every frame: delivers callbacks and refreshes status()
    void stop();  // also resets status(); start() fills it again

    [[nodiscard]] const PlayerStatus& status() const;

    // Controls
    void confirm_pairing_window();
    /// Drops this player's pairing record for a server: the client restarts without it (any
    /// playback stops), and that server must pair again. The server keeps its own record until
    /// it next connects and finds ours gone.
    void forget_server(std::string_view server_id);
    /// Saves the player name (empty: the device's own) and restarts the client, so servers and
    /// the mDNS announcement pick it up (any playback stops for a moment). Replaces --name.
    void set_name(std::string_view name);
    void play_pause();
    void next();
    void previous();
    void change_volume(int delta);        // this player's volume, 0..100
    void change_group_volume(int delta);  // the group's volume via the controller role
    void toggle_shuffle();
    void cycle_repeat();  // off -> all -> one -> off
    void seek_relative(int delta_ms);
    void change_latency(int delta_ms);
    void set_idle_screensaver(bool on);
    void set_audio_format(std::string_view codec);  // saved; applies at the next start

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
