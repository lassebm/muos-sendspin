#pragma once

#include "player/sendspin_service.h"

#include <SDL.h>

#include <array>
#include <string>
#include <string_view>

namespace ui {

using Rgb = std::array<uint8_t, 3>;

struct Theme {
    SDL_Color bg{18, 18, 22, 255};
    SDL_Color fg{240, 240, 245, 255};
    SDL_Color dim{165, 165, 178, 255};
    SDL_Color faint{100, 100, 112, 255};
    SDL_Color accent{90, 170, 255, 255};
    SDL_Color track{70, 70, 80, 255};  // progress bar background
    SDL_Color panel{34, 34, 42, 255};
    bool operator==(const Theme& o) const {
        const auto eq = [](SDL_Color x, SDL_Color y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; };
        return eq(bg, o.bg) && eq(fg, o.fg) && eq(dim, o.dim) && eq(faint, o.faint) && eq(accent, o.accent) &&
               eq(track, o.track) && eq(panel, o.panel);
    }
};

/// Theme from the server palette (color role), falling back to the artwork's mean colour, then
/// to the default. Colours are adjusted so text and accents stay readable on the background.
[[nodiscard]] Theme make_theme(const PlayerStatus& st);

enum class Toggle { Hidden, Off, On };
enum class RepeatShown { Hidden, Off, All, One };

/// What the Now Playing screen shows, derived from PlayerStatus.
struct NowPlayingView {
    std::string title;
    std::string artist;
    std::string album;  // with " · <year>" when known
    std::string group;
    int position_ms = 0;
    int duration_ms = 0;  // 0: unknown (no position shown)
    bool playing = false;
    bool transport = false;  // a controller role is active
    bool can_previous = false, can_next = false, can_play_pause = false;
    Toggle shuffle = Toggle::Hidden;
    RepeatShown repeat = RepeatShown::Hidden;
};

[[nodiscard]] NowPlayingView make_view(const PlayerStatus& st);

/// "Music Assistant (d5369777-music-assistant)" -> "Music Assistant"; empty -> "Sendspin server".
[[nodiscard]] std::string short_server_name(std::string_view name);

/// 4:05, or 1:02:05 for an hour or more.
[[nodiscard]] std::string format_time(int ms);

}  // namespace ui
