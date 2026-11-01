#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// Decoded artwork, RGBA8 (byte order R,G,B,A). Immutable once published.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::array<uint8_t, 3> average{};  // mean colour, fallback accent when the server sends no palette
};

/// Decodes JPEG/PNG/BMP (stb_image) into RGBA8 and computes the mean colour. nullptr on failure.
[[nodiscard]] std::shared_ptr<Image> decode_image(std::span<const uint8_t> encoded);
