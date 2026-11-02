#pragma once

#include <SDL.h>

namespace ui {

/// The whole UI's layout and scale, computed from the renderer output size. Everything is derived
/// from relative units; nothing assumes 640x480.
struct Layout {
    /// Square and portrait screens (aspect < 1.2, e.g. RG CubeXX 720x720): artwork on top, text
    /// below. Landscape (4:3, 3:2, 16:9): artwork left, text right.
    bool stacked = false;
    int unit = 0;  // the shorter side: base for all sizes
    int margin = 0;

    SDL_Rect artwork{};
    SDL_Rect text{};       // title/artist/album block
    SDL_Rect transport{};  // prev / play-pause / next row (+ shuffle/repeat at the ends)
    SDL_Rect progress{};   // progress bar track
    SDL_Rect footer{};     // button hints along the bottom edge

    /// Type scale: every text uses one of these line heights (px), each step 1.25x the one below.
    struct TypeScale {
        int small = 0;    // times, group, album, hints, captions
        int body = 0;     // artist, menu rows, messages
        int large = 0;    // title, headings
        int display = 0;  // big readouts (sync offset, pairing code)
    } type;
    int icon = 0;        // em (px) of the transport's play/pause, previous and next symbols
    int icon_small = 0;  // ... and of shuffle and repeat
};

[[nodiscard]] Layout compute_layout(int w, int h);

}  // namespace ui
