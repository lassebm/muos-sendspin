#pragma once

#include "lvgl.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ui {

/// Button glyphs drawn as shapes, in muOS's footer style (MustardOS theme glyph/footer/).
enum class Glyph { DpadLeftRight, DpadUpDown, Menu };

/// An image in memory: LVGL image descriptor plus the pixels it points at (keep it alive, and in
/// place, while shown). Rendered glyphs, and the artwork.
struct ArgbImage {
    ArgbImage(int w, int h);  // transparent black

    std::vector<uint8_t> pixels;  // w x h, ARGB8888 in LVGL's byte order: B, G, R, A
    lv_image_dsc_t dsc{};
};

/// Renders a glyph at size x size pixels in one colour; cut-outs (arrows, dots) are transparent.
/// Shapes are defined around the exact centre and sampled at sub-pixel positions that mirror
/// within each pixel, so the result is symmetric by construction, at any size.
[[nodiscard]] std::unique_ptr<ArgbImage> render_glyph(Glyph glyph, int size, lv_color_t color);

/// A dot of the given diameter, anti-aliased and centred in a box x box image. Lets dots of one
/// size centre exactly under ink of either pixel parity: the box takes the ink's parity.
[[nodiscard]] std::unique_ptr<ArgbImage> render_dot(int box, float diameter, lv_color_t color);

}  // namespace ui
