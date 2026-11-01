#include "player/artwork.h"

#include "platform/log.h"

#include "stb_image.h"

#include <algorithm>

std::shared_ptr<Image> decode_image(std::span<const uint8_t> encoded) {
    int w = 0, h = 0, channels = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> px(
        stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 4),
        &stbi_image_free);
    if (!px) {
        logx::warn("artwork: decode failed: {}", stbi_failure_reason());
        return nullptr;
    }
    auto img = std::make_shared<Image>();
    img->w = w;
    img->h = h;
    img->rgba.assign(px.get(), px.get() + static_cast<size_t>(w) * h * 4);

    // Mean colour from every 7th pixel: plenty for a fallback accent.
    std::array<uint64_t, 3> sum{};
    const size_t count = static_cast<size_t>(w) * h;
    size_t samples = 0;
    for (size_t i = 0; i < count; i += 7, ++samples)
        for (size_t c = 0; c < 3; ++c) sum[c] += img->rgba[i * 4 + c];
    for (size_t c = 0; c < 3; ++c) img->average[c] = static_cast<uint8_t>(sum[c] / std::max<size_t>(1, samples));
    return img;
}
