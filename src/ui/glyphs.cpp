#include "ui/glyphs.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr int kSamples = 8;  // per axis and pixel: 64 coverage samples

/// Inside a rectangle of half extents (hw, hh) with corner radius r, centred on the origin.
bool in_rounded_rect(float u, float v, float hw, float hh, float r) {
    const float dx = std::max(std::abs(u) - (hw - r), 0.0f);
    const float dy = std::max(std::abs(v) - (hh - r), 0.0f);
    return std::abs(u) <= hw && std::abs(v) <= hh && dx * dx + dy * dy <= r * r;
}

bool in_circle(float u, float v, float cu, float cv, float r) {
    return (u - cu) * (u - cu) + (v - cv) * (v - cv) <= r * r;
}

/// The glyph's shape at (u, v), relative to the centre, for a glyph of size d: filled or not.
bool filled(Glyph g, float u, float v, float d) {
    const float half = d / 2.0f;
    if (g == Glyph::Menu) {
        if (!in_circle(u, v, 0, 0, half)) return false;
        const float r = d * 0.075f, spacing = d * 0.22f;
        return !(in_circle(u, v, -spacing, 0, r) || in_circle(u, v, 0, 0, r) || in_circle(u, v, spacing, 0, r));
    }
    // D-pad: a cross of two rounded arms; the axis in use has an arrow cut into each end.
    const float arm = d * 0.21f;  // half the arm width
    const float corner = arm * 0.6f;
    if (!in_rounded_rect(u, v, half, arm, corner) && !in_rounded_rect(u, v, arm, half, corner)) return false;
    // Arrow pointing outward along the axis: |along| from base to tip, narrowing to the tip.
    const float along = std::abs(g == Glyph::DpadLeftRight ? u : v);
    const float across = std::abs(g == Glyph::DpadLeftRight ? v : u);
    const float tip = half - d * 0.06f, width = arm * 1.3f, length = width * 0.85f;
    const float base = tip - length;
    const bool in_arrow = along >= base && along <= tip && across <= (tip - along) / length * (width / 2.0f);
    return !in_arrow;
}

/// Renders size x size pixels of the shape inside(u, v), u and v relative to the exact centre.
template <typename Shape>
std::unique_ptr<ArgbImage> rasterize(int size, lv_color_t color, Shape inside) {
    auto img = std::make_unique<ArgbImage>(size, size);
    const auto n = static_cast<size_t>(size);
    const float centre = size / 2.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0;
            for (int sy = 0; sy < kSamples; ++sy)
                for (int sx = 0; sx < kSamples; ++sx) {
                    // Sample offsets (k + 0.5) / n are symmetric within the pixel, so mirrored
                    // pixels sample mirrored points.
                    const float u = x + (sx + 0.5f) / kSamples - centre;
                    const float v = y + (sy + 0.5f) / kSamples - centre;
                    covered += inside(u, v) ? 1 : 0;
                }
            uint8_t* p = &img->pixels[(static_cast<size_t>(y) * n + static_cast<size_t>(x)) * 4];
            p[0] = color.blue;
            p[1] = color.green;
            p[2] = color.red;
            p[3] = static_cast<uint8_t>((covered * 255 + kSamples * kSamples / 2) / (kSamples * kSamples));
        }
    }
    return img;
}

}  // namespace

ArgbImage::ArgbImage(int w, int h) : pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = static_cast<uint32_t>(w);
    dsc.header.h = static_cast<uint32_t>(h);
    dsc.header.stride = static_cast<uint32_t>(w * 4);
    dsc.data_size = static_cast<uint32_t>(pixels.size());
    dsc.data = pixels.data();
}

std::unique_ptr<ArgbImage> render_glyph(Glyph glyph, int size, lv_color_t color) {
    const auto d = static_cast<float>(size);
    return rasterize(size, color, [&](float u, float v) { return filled(glyph, u, v, d); });
}

std::unique_ptr<ArgbImage> render_dot(int box, float diameter, lv_color_t color) {
    return rasterize(box, color, [r = diameter / 2](float u, float v) { return u * u + v * v <= r * r; });
}

}  // namespace ui
