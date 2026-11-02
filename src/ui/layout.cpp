#include "ui/layout.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

int pct(int v, float f) {
    return static_cast<int>(std::lround(v * f));
}

}  // namespace

Layout compute_layout(int w, int h) {
    Layout l;
    l.stacked = h > 0 && static_cast<float>(w) / static_cast<float>(h) < 1.2f;
    const int unit = std::min(w, h);
    l.unit = unit;
    l.margin = std::max(4, pct(unit, 0.05f));
    // The small step stays readable at arm's length on a 3.5" screen (22 px at 640x480).
    l.type.small = std::max(12, pct(unit, 0.046f));
    l.type.body = pct(l.type.small, 1.25f);
    l.type.large = pct(l.type.small, 1.5625f);
    l.type.display = pct(l.type.small, 2.44f);
    l.icon = pct(unit, 0.0584f);  // 28 and 20 px at 640x480
    l.icon_small = pct(unit, 0.0417f);

    const int footer_h = l.type.small * 3 / 2 + l.margin / 3;  // hint pills: a line of small text, padded
    l.footer = {l.margin, h - l.margin / 3 - footer_h, w - 2 * l.margin, footer_h};
    const int content_bottom = l.footer.y - l.margin / 3;  // content area: [margin, content_bottom)

    const int bar_h = std::max(3, pct(unit, 0.012f));
    const int transport_h = std::max(16, pct(unit, 0.085f));

    if (!l.stacked) {
        const int content_h = content_bottom - l.margin;
        // Artwork square, but leave at least ~42% of the width for text.
        const int art = std::min(content_h, pct(w, 0.58f) - l.margin);
        l.artwork = {l.margin, l.margin + (content_h - art) / 2, art, art};

        const int tx = l.artwork.x + art + l.margin;
        const int tw = w - tx - l.margin;
        l.text = {tx, l.artwork.y, tw, art};
        l.progress = {tx, l.artwork.y + art - bar_h - l.type.small - l.margin / 4, tw, bar_h};
        l.transport = {tx, l.progress.y - l.margin / 2 - transport_h, tw, transport_h};
    } else {
        // Below the artwork: title, artist, album, transport row, progress bar, time labels.
        const int text_block = l.type.large + l.type.body + l.type.small + transport_h + bar_h + l.type.small +
                               l.margin * 3 / 2 + l.margin / 4;
        const int avail_h = content_bottom - l.margin - text_block;
        const int art = std::max(0, std::min(avail_h, w - 2 * l.margin));
        l.artwork = {(w - art) / 2, l.margin, art, art};

        const int tx = l.margin;
        const int tw = w - 2 * l.margin;
        const int ty = l.artwork.y + art + l.margin;
        l.text = {tx, ty, tw, content_bottom - ty};
        l.progress = {tx, content_bottom - bar_h - l.type.small - l.margin / 4, tw, bar_h};
        l.transport = {tx, l.progress.y - l.margin / 2 - transport_h, tw, transport_h};
    }
    return l;
}

}  // namespace ui
