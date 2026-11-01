#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

/// Bound of the user's sync tuning (settings.ini, Settings > Sync), either way.
inline constexpr int kMaxExtraLatencyMs = 500;

/// s without surrounding whitespace (spaces, tabs, CR, LF).
[[nodiscard]] std::string_view trim(std::string_view s);

/// App settings the sendspin SDK doesn't persist itself (it starts every player at volume 0).
/// Stored as key=value lines next to the SDK's state.json, written atomically.
struct Settings {
    std::string name;  // player name shown by servers; empty = the device's name
    int volume = 50;   // this player's volume, 0..100; cautious first-run default
    bool muted = false;
    int extra_latency_ms = 0;  // user sync tuning on top of the latency estimate
    // Like Wasabi's "idle screensaver": false (default) keeps the display awake while playing;
    // true lets muOS dim it (and mute, if the user enabled idle mute).
    bool idle_screensaver = false;
    // Codec asked of the server first: "flac" (default), "opus" or "pcm". Applies at the next start.
    std::string audio_format = "flac";
    // Names of paired servers by server_id, so the Pairing page can list them while they're
    // offline (the SDK's pairing records hold only keys).
    std::map<std::string, std::string> server_names;

    /// Missing file or keys leave the defaults; malformed lines, unknown keys and bad values are
    /// skipped with a warning. Tolerates CRLF line endings and spaces around keys and values.
    void load(const std::filesystem::path& path);
    /// tmp file + fsync + rename, so a crash or power loss never leaves a half-written file.
    /// A failure is logged; the settings stay in memory.
    void save(const std::filesystem::path& path) const;
};
