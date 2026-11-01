#include "ui/view.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace ui {

namespace {

float channel_lin(uint8_t c) {
    const float v = c / 255.0f;
    return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float luminance(SDL_Color c) {
    return 0.2126f * channel_lin(c.r) + 0.7152f * channel_lin(c.g) + 0.0722f * channel_lin(c.b);
}

float contrast(SDL_Color a, SDL_Color b) {
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    const auto m = [t](Uint8 x, Uint8 y) { return static_cast<Uint8>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

SDL_Color from(const Rgb& c) {
    return {c[0], c[1], c[2], 255};
}

/// Lightens c towards white until it reaches the contrast ratio against bg.
SDL_Color readable_on(SDL_Color c, SDL_Color bg, float ratio) {
    for (int i = 0; i < 10 && contrast(c, bg) < ratio; ++i) c = mix(c, {255, 255, 255, 255}, 0.2f);
    return c;
}

/// Darkens c towards bg while it stays at or above the contrast ratio: the quietest colour at
/// that ratio along the way.
SDL_Color quieter_on(SDL_Color c, SDL_Color bg, float ratio) {
    for (int i = 0; i < 40; ++i) {
        const SDL_Color next = mix(c, bg, 0.05f);
        if (contrast(next, bg) < ratio) break;
        c = next;
    }
    return c;
}

Theme make_theme(const std::optional<Rgb>& background, const std::optional<Rgb>& accent,
                 const std::optional<Rgb>& foreground, const std::optional<Rgb>& artwork_average) {
    Theme t;
    if (background) t.bg = from(*background);
    else if (artwork_average) t.bg = mix(t.bg, from(*artwork_average), 0.18f);  // a hint of the cover
    // Keep the background dark enough for light text even if the server palette isn't.
    for (int i = 0; i < 10 && luminance(t.bg) > 0.06f; ++i) t.bg = mix(t.bg, {0, 0, 0, 255}, 0.2f);

    if (foreground) t.fg = from(*foreground);
    t.fg = readable_on(t.fg, t.bg, 7.0f);
    t.dim = readable_on(mix(t.fg, t.bg, 0.30f), t.bg, 4.5f);
    t.faint = readable_on(mix(t.fg, t.bg, 0.55f), t.bg, 3.0f);

    if (accent) t.accent = from(*accent);
    else if (artwork_average) t.accent = from(*artwork_average);
    t.accent = readable_on(t.accent, t.bg, 3.0f);

    t.track = mix(t.bg, t.fg, 0.18f);
    t.panel = mix(t.bg, t.fg, 0.08f);
    // Menus, overlays and the Sync graph draw text on the panel, which is lighter than the
    // background: hold the same contrast there, whatever colours the artwork brings.
    t.fg = readable_on(t.fg, t.panel, 7.0f);
    t.dim = readable_on(t.dim, t.panel, 4.5f);
    t.faint = readable_on(t.faint, t.panel, 3.0f);
    t.accent = readable_on(t.accent, t.panel, 3.0f);
    // Keep the tiers apart. When the text colour is only just readable, lifting dim and faint to
    // their minimums leaves them almost as bright as fg (focused and unfocused rows, typed text
    // and a placeholder look alike). So each tier gets at most 70% of the contrast of the one above
    // (never below its minimum), and fg is lifted if it is still too close to dim.
    t.dim = quieter_on(t.dim, t.panel, std::max(4.5f, contrast(t.fg, t.panel) * 0.7f));
    t.faint = quieter_on(t.faint, t.panel, std::max(3.0f, contrast(t.dim, t.panel) * 0.7f));
    t.fg = readable_on(t.fg, t.panel, contrast(t.dim, t.panel) * 1.4f);
    // The progress track (and the volume bar's, the Sync graph's grid): visible on any palette, but
    // quieter than text. The panel stays darker than the track.
    t.track = readable_on(t.track, t.bg, 1.8f);
    t.track = readable_on(t.track, t.panel, 1.5f);
    return t;
}

}  // namespace

Theme make_theme(const PlayerStatus& st) {
    std::optional<Rgb> average;
    if (st.artwork) average = st.artwork->average;
    return make_theme(st.palette.background_dark, st.palette.accent, st.palette.on_dark, average);
}

NowPlayingView make_view(const PlayerStatus& st) {
    NowPlayingView v;
    v.title = st.title;
    v.artist = st.artist;
    v.album = st.album;
    if (st.year) v.album += std::format("{}{}", v.album.empty() ? "" : " · ", st.year);
    v.group = st.group_name;
    v.position_ms = static_cast<int>(st.progress_ms);
    v.duration_ms = static_cast<int>(st.duration_ms);
    // Music Assistant sometimes reports a playing track pinned at its end (late joins, repeat-one).
    // A frozen "3:57 / 3:57" is worse than no position.
    if (st.playing && st.duration_ms > 0 && st.progress_ms >= st.duration_ms) v.position_ms = v.duration_ms = 0;
    v.playing = st.playing;
    v.transport = st.has_controller;
    v.can_previous = st.can_previous;
    v.can_next = st.can_next;
    v.can_play_pause = st.playing ? (st.can_pause || st.can_play) : st.can_play;
    v.shuffle = !st.can_shuffle ? Toggle::Hidden : st.shuffle ? Toggle::On : Toggle::Off;
    v.repeat = !st.can_repeat             ? RepeatShown::Hidden
               : st.repeat == Repeat::One ? RepeatShown::One
               : st.repeat == Repeat::All ? RepeatShown::All
                                          : RepeatShown::Off;
    return v;
}

std::string short_server_name(std::string_view name) {
    // Music Assistant appends its instance id in parentheses; that's noise in a sentence.
    if (const auto paren = name.rfind(" ("); paren != std::string_view::npos && name.ends_with(')'))
        name = name.substr(0, paren);
    return name.empty() ? "Sendspin server" : std::string(name);
}

std::string format_time(int ms) {
    const int s = std::max(0, ms / 1000);
    return s >= 3600 ? std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60)
                     : std::format("{}:{:02}", s / 60, s % 60);
}

}  // namespace ui
