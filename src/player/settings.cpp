#include "player/settings.h"

#include "platform/log.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

/// Owns a POSIX file descriptor (we need fsync, which iostreams can't do).
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

private:
    int fd_;
};

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

bool write_atomic(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        const Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
        if (!fd.valid() || !write_all(fd.get(), content) || ::fsync(fd.get()) != 0) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) return false;
    // Make the rename itself durable (FAT/exFAT on the SD card ignores this; ext4/APFS honour it).
    if (const Fd dir(::open(path.parent_path().empty() ? "." : path.parent_path().c_str(), O_RDONLY)); dir.valid())
        ::fsync(dir.get());
    return true;
}

std::optional<int> parse_int(std::string_view s) {
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}

}  // namespace

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r\n";
    const size_t first = s.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

void Settings::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return;  // first run
    std::string line;
    bool first_line = true;
    while (std::getline(in, line)) {
        // Hand-edited on the SD card from Windows: CRLF line endings, maybe a UTF-8 byte order mark.
        std::string_view text = line;
        if (std::exchange(first_line, false) && text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        text = trim(text);
        if (text.empty() || text.front() == '#') continue;
        const auto eq = text.find('=');
        if (eq == std::string_view::npos) {
            logx::warn("settings: ignoring malformed line in {}: {}", path.string(), text);
            continue;
        }
        const std::string_view key = trim(text.substr(0, eq));
        const std::string_view value = trim(text.substr(eq + 1));
        const auto number = parse_int(value);
        const auto set_int = [&](int& out, int lo, int hi) {
            if (number) out = std::clamp(*number, lo, hi);
            return number.has_value();
        };
        const auto set_bool = [&](bool& out) {
            if (number) out = *number != 0;
            return number.has_value();
        };
        bool known = true, valid = true;
        if (key == "name") name = value;
        else if (key == "volume") valid = set_int(volume, 0, 100);
        else if (key == "muted") valid = set_bool(muted);
        else if (key == "extra_latency_ms") valid = set_int(extra_latency_ms, -kMaxExtraLatencyMs, kMaxExtraLatencyMs);
        else if (key == "idle_screensaver") valid = set_bool(idle_screensaver);
        else if (key == "audio_format") {
            valid = value == "flac" || value == "opus" || value == "pcm";
            if (valid) audio_format = value;
        } else if (key.starts_with("server.") && key.size() > 7) server_names[std::string(key.substr(7))] = value;
        else known = false;
        if (!known) logx::warn("settings: ignoring unknown key in {}: {}", path.string(), key);
        else if (!valid) logx::warn("settings: ignoring bad value in {}: {}={}", path.string(), key, value);
    }
}

void Settings::save(const std::filesystem::path& path) const {
    std::string content = std::format(
        "# Sendspin settings (written by the app). name= sets the player name servers show;\n"
        "# leave it empty to use the device's name.\n"
        "name={}\n"
        "volume={}\n"
        "muted={}\n"
        "extra_latency_ms={}\n"
        "# 1: let muOS dim the screen while playing (mutes too if muOS idle mute is on)\n"
        "idle_screensaver={}\n"
        "# flac (lossless, default), opus (less Wi-Fi traffic) or pcm; applies at the next start\n"
        "audio_format={}\n",
        name, volume, muted ? 1 : 0, extra_latency_ms, idle_screensaver ? 1 : 0, audio_format);
    if (!server_names.empty()) content += "# names of paired servers (server.<server_id>=<name>)\n";
    for (const auto& [id, server] : server_names) content += std::format("server.{}={}\n", id, server);
    if (!write_atomic(path, content)) logx::warn("settings: can't write {}", path.string());
}
