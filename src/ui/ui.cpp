#include "ui/ui.h"

#include "platform/log.h"
#include "ui/glyphs.h"
#include "ui/layout.h"
#include "version.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace ui {

namespace {

constexpr uint64_t kMarqueeMs = 20000;  // long texts scroll this long after a change, then show "…"
constexpr uint64_t kVolumeOsdMs = 1500;

lv_color_t col(SDL_Color c) {
    return lv_color_make(c.r, c.g, c.b);
}

void show(lv_obj_t* obj, bool visible) {
    if (lv_obj_is_hidden(obj) == visible) lv_obj_set_hidden(obj, !visible);
}

// Style setters invalidate (redraw) the object even when the value doesn't change, so every
// per-frame setter compares first: otherwise the UI redraws constantly.
void set_color(lv_obj_t* obj, SDL_Color c) {
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), col(c)))
        lv_obj_set_style_text_color(obj, col(c), 0);
}

void set_bg(lv_obj_t* obj, SDL_Color c, lv_part_t part = LV_PART_MAIN) {
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, part), col(c))) lv_obj_set_style_bg_color(obj, col(c), part);
}

/// A plain container: no theme styles, no scrolling.
lv_obj_t* box(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, int x = 0, int y = 0, int w = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_label_set_text(l, "");
    return l;
}

/// The menu panel's padding.
int menu_pad(const Layout& l) {
    return std::max(8, l.unit * 45 / 1000);
}

int line_height(const lv_font_t* font) {
    return static_cast<int>(lv_font_get_line_height(font));
}

/// Single-line label that scrolls (marquee) when the text is wider than w. The height is fixed
/// to one line: in LVGL's "…" mode the label truncates at its height, so an automatic height
/// would wrap instead.
lv_obj_t* marquee(lv_obj_t* parent, const lv_font_t* font, int x, int y, int w) {
    lv_obj_t* l = label(parent, font, x, y, w);
    lv_obj_set_height(l, line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(l, 12000, 0);  // calm scroll speed
    return l;
}

/// A QR code, dark on light whatever the theme (scanners expect that), with its quiet zone.
lv_obj_t* qr_code(lv_obj_t* parent) {
    lv_obj_t* q = lv_qrcode_create(parent);
    lv_qrcode_set_dark_color(q, lv_color_black());
    lv_qrcode_set_light_color(q, lv_color_white());
    lv_qrcode_set_quiet_zone(q, true);
    return q;
}

/// Encodes data at about `target` pixels. LVGL scales modules by whole pixels and, given room,
/// picks a bigger version instead of bigger modules, so the size is a whole multiple of the
/// module count it will use (byte mode, ECC M, as lv_qrcode.c encodes), with at least 3 px per
/// module: smaller doesn't scan from a handheld's screen. A pairing token (107 bytes) is
/// version 7: 45 modules plus LVGL's quiet zone of 2 per side.
void set_qr(lv_obj_t* q, std::string_view data, int target) {
    static constexpr int kByteCapacityM[] = {14, 26, 42, 62, 84, 106, 122, 152, 180, 213, 251, 287, 331, 362};
    int version = 1;
    while (version < static_cast<int>(std::size(kByteCapacityM)) &&
           kByteCapacityM[version - 1] < static_cast<int>(data.size()))
        ++version;
    const int modules = 17 + 4 * version + 4;
    lv_qrcode_set_size(q, modules * std::max(3, target / modules));
    if (lv_qrcode_update(q, data.data(), static_cast<uint32_t>(data.size())) != LV_RESULT_OK)
        LV_LOG_WARN("QR code: %u bytes don't fit", static_cast<unsigned>(data.size()));
}

std::string server_label(const PairedServer& s) {
    return s.name.empty() ? "Server " + s.id.substr(0, 8) : short_server_name(s.name);
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The ink (drawn pixels) of one character in a label of that font, relative to the label's
/// top-left. Centring uses ink, not the line box: a line reserves descender space below the
/// baseline (so capitals sit high in it) and the advance includes side bearings.
struct Ink {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] float cx() const { return x + w / 2; }
    [[nodiscard]] float cy() const { return y + h / 2; }
};

Ink ink_of(const lv_font_t* font, uint32_t ch) {
    const auto line = static_cast<float>(lv_font_get_line_height(font));
    lv_font_glyph_dsc_t g{};
    if (!lv_font_get_glyph_dsc(font, &g, ch, 0) || g.box_w == 0 || g.box_h == 0) return {0, 0, 0, line};
    // LVGL draws a glyph's bitmap at x = ofs_x, y = (line height - base line) - box_h - ofs_y. The
    // bitmap box is not the ink: rasterizers pad it for anti-aliasing. So scan the rendered bitmap
    // and take the rows and columns at least half covered: the edges the eye sees.
    const float box_x = g.ofs_x;
    const auto box_y = static_cast<float>(lv_font_get_line_height(font) - font->base_line - g.box_h - g.ofs_y);
    Ink ink{box_x, box_y, static_cast<float>(g.box_w), static_cast<float>(g.box_h)};
    lv_draw_buf_t* scratch = lv_draw_buf_create(g.box_w, g.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    g.req_raw_bitmap = 0;  // A8
    const auto* bmp = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&g, scratch));
    if (bmp && bmp->header.cf == LV_COLOR_FORMAT_A8) {
        int x0 = g.box_w, x1 = -1, y0 = g.box_h, y1 = -1;
        for (int y = 0; y < g.box_h; ++y)
            for (int x = 0; x < g.box_w; ++x)
                if (bmp->data[static_cast<size_t>(y) * bmp->header.stride + static_cast<size_t>(x)] >= 128) {
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                }
        if (x1 >= 0) ink = {box_x + x0, box_y + y0, static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1)};
    }
    lv_font_glyph_release_draw_data(&g);
    if (scratch) lv_draw_buf_destroy(scratch);
    return ink;
}

/// First code point of UTF-8 text (LV_SYMBOL_* are three-byte sequences).
uint32_t first_code_point(std::string_view s) {
    const auto b = [&](size_t i) { return static_cast<uint32_t>(static_cast<uint8_t>(s[i])); };
    if (s.empty()) return 0;
    if (b(0) < 0x80) return b(0);
    if ((b(0) & 0xE0) == 0xC0 && s.size() >= 2) return (b(0) & 0x1F) << 6 | (b(1) & 0x3F);
    if ((b(0) & 0xF0) == 0xE0 && s.size() >= 3) return (b(0) & 0x0F) << 12 | (b(1) & 0x3F) << 6 | (b(2) & 0x3F);
    return 0;
}

/// The one rounding step of all centring: to the nearest pixel, halves always the same way.
int32_t snap(float v) {
    return static_cast<int32_t>(std::floor(v + 0.5f));
}

/// The button that triggers an action, as the footer names it. The D-pad's directions have no
/// name: the footer draws a D-pad glyph for them.
std::string_view key_name(Action a, bool gamepad) {
    switch (a) {
        case Action::PlayPause: return gamepad ? "A" : "Space";
        case Action::Back: return gamepad ? "B" : "Bksp";
        case Action::Menu: return gamepad ? "MENU" : "Esc";
        case Action::PlayerVolumeDown: return gamepad ? "L1" : "-";
        case Action::PlayerVolumeUp: return gamepad ? "R1" : "+";
        case Action::ToggleShuffle: return gamepad ? "X" : "S";
        case Action::CycleRepeat: return gamepad ? "Y" : "R";
        default: return "?";
    }
}

// On-screen keyboard, laid out like muOS's own (common/ui/osk.c): number row, three letter rows,
// then the mode keys, space and done. "ABC" / "abc" / "1#" switch maps (lv_keyboard's names);
// LV_SYMBOL_OK saves.
/// A key's control word: its relative width, plus flags (LVGL's C enum, combined as integers).
constexpr lv_buttonmatrix_ctrl_t key_ctrl(int width, int flags = 0) {
    return static_cast<lv_buttonmatrix_ctrl_t>(width | flags);
}
constexpr lv_buttonmatrix_ctrl_t kMode = key_ctrl(2, LV_KEYBOARD_CTRL_BUTTON_FLAGS);
constexpr lv_buttonmatrix_ctrl_t kKey = key_ctrl(2);
constexpr lv_buttonmatrix_ctrl_t kSpace = key_ctrl(6);
constexpr lv_buttonmatrix_ctrl_t kBksp = key_ctrl(3, LV_KEYBOARD_CTRL_BUTTON_FLAGS);

/// A style selector: part and state (two C enums, combined as integers).
constexpr lv_style_selector_t sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part) | static_cast<lv_style_selector_t>(state);
}

const char* const kKeysLower[] = {"1",
                                  "2",
                                  "3",
                                  "4",
                                  "5",
                                  "6",
                                  "7",
                                  "8",
                                  "9",
                                  "0",
                                  "\n",
                                  "q",
                                  "w",
                                  "e",
                                  "r",
                                  "t",
                                  "y",
                                  "u",
                                  "i",
                                  "o",
                                  "p",
                                  "\n",
                                  "a",
                                  "s",
                                  "d",
                                  "f",
                                  "g",
                                  "h",
                                  "j",
                                  "k",
                                  "l",
                                  "'",
                                  "\n",
                                  "z",
                                  "x",
                                  "c",
                                  "v",
                                  "b",
                                  "n",
                                  "m",
                                  ",",
                                  ".",
                                  LV_SYMBOL_BACKSPACE,
                                  "\n",
                                  "ABC",
                                  "1#",
                                  " ",
                                  LV_SYMBOL_OK,
                                  ""};
const char* const kKeysUpper[] = {"1",
                                  "2",
                                  "3",
                                  "4",
                                  "5",
                                  "6",
                                  "7",
                                  "8",
                                  "9",
                                  "0",
                                  "\n",
                                  "Q",
                                  "W",
                                  "E",
                                  "R",
                                  "T",
                                  "Y",
                                  "U",
                                  "I",
                                  "O",
                                  "P",
                                  "\n",
                                  "A",
                                  "S",
                                  "D",
                                  "F",
                                  "G",
                                  "H",
                                  "J",
                                  "K",
                                  "L",
                                  "\"",
                                  "\n",
                                  "Z",
                                  "X",
                                  "C",
                                  "V",
                                  "B",
                                  "N",
                                  "M",
                                  "-",
                                  "_",
                                  LV_SYMBOL_BACKSPACE,
                                  "\n",
                                  "abc",
                                  "1#",
                                  " ",
                                  LV_SYMBOL_OK,
                                  ""};
const char* const kKeysSpecial[] = {"!",
                                    "@",
                                    "#",
                                    "$",
                                    "%",
                                    "&",
                                    "*",
                                    "(",
                                    ")",
                                    "?",
                                    "\n",
                                    "-",
                                    "_",
                                    "+",
                                    "=",
                                    "/",
                                    "\\",
                                    ":",
                                    ";",
                                    "\"",
                                    "'",
                                    "\n",
                                    "[",
                                    "]",
                                    "{",
                                    "}",
                                    "<",
                                    ">",
                                    "~",
                                    "`",
                                    "|",
                                    "^",
                                    "\n",
                                    "\u00e6",
                                    "\u00f8",
                                    "\u00e5",
                                    "\u00e9",
                                    "\u00fc",
                                    "\u00f6",
                                    "\u00e4",
                                    "\u00df",
                                    "\u00f1",
                                    LV_SYMBOL_BACKSPACE,
                                    "\n",
                                    "abc",
                                    "ABC",
                                    " ",
                                    LV_SYMBOL_OK,
                                    ""};
// Ten keys in the first three rows, nine plus backspace in the fourth, four in the last.
constexpr lv_buttonmatrix_ctrl_t kKeyCtrl[] = {
    kKey,  kKey,  kKey,   kKey,  kKey, kKey, kKey, kKey, kKey, kKey,   //
    kKey,  kKey,  kKey,   kKey,  kKey, kKey, kKey, kKey, kKey, kKey,   //
    kKey,  kKey,  kKey,   kKey,  kKey, kKey, kKey, kKey, kKey, kKey,   //
    kKey,  kKey,  kKey,   kKey,  kKey, kKey, kKey, kKey, kKey, kBksp,  //
    kMode, kMode, kSpace, kMode,
};

}  // namespace

// --- fonts: Noto Sans and the symbols (Symbols.ttf) through tiny_ttf, so both scale -------------

struct Ui::Fonts {
    std::vector<uint8_t> regular, bold, symbols;
    std::vector<lv_font_t*> owned;
    std::map<int, const lv_font_t*> symbols_by_em;

    /// Text at line height px (ascent + descent), as the layout computes it; tiny_ttf sizes by the
    /// em, and Noto Sans' ascent + descent is 1.362 em. Symbols in the text (arrows, OK, ...) come
    /// from Symbols.ttf at the same em.
    const lv_font_t* text(const std::vector<uint8_t>& data, int px) {
        return text_em(data, static_cast<int>(std::lround(px / 1.362)));
    }
    /// Without the font file: LVGL's built-in Montserrat (with its own symbols), at its one size.
    const lv_font_t* text_em(const std::vector<uint8_t>& data, int em) {
        em = std::max(6, em);
        lv_font_t* f = create(data, em, 256);
        if (!f) return &lv_font_montserrat_14;
        f->fallback = symbol(em);
        return f;
    }
    /// Symbols (LV_SYMBOL_*) at em px. Without Symbols.ttf: LVGL's built-in Montserrat, at its size.
    const lv_font_t* symbol(int em) {
        em = std::max(6, em);
        auto [it, inserted] = symbols_by_em.try_emplace(em, &lv_font_montserrat_14);
        if (inserted) {
            if (lv_font_t* f = create(symbols, em, 16)) {
                f->fallback = &lv_font_montserrat_14;
                it->second = f;
            }
        }
        return it->second;
    }
    lv_font_t* create(const std::vector<uint8_t>& data, int em, size_t cache) {
        // tiny_ttf trusts the data (it doesn't check the size), so a missing file must not reach it.
        if (data.empty()) return nullptr;
        lv_font_t* f = lv_tiny_ttf_create_data_ex(data.data(), data.size(), em, LV_FONT_KERNING_NORMAL, cache);
        if (!f) logx::error("font: not a usable TrueType font, using the built-in one");
        else owned.push_back(f);
        return f;
    }
    void clear() {
        for (lv_font_t* f : owned) lv_tiny_ttf_destroy(f);
        owned.clear();
        symbols_by_em.clear();
    }
    Fonts() = default;
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;
    ~Fonts() { clear(); }

    // One font per step of the layout's type scale (and weight).
    const lv_font_t *small = nullptr, *small_bold = nullptr, *body = nullptr, *body_bold = nullptr;
    const lv_font_t *large_bold = nullptr, *display_bold = nullptr;
};

Ui::Ui(Backend& backend, const std::filesystem::path& regular_ttf, const std::filesystem::path& bold_ttf,
       const std::filesystem::path& symbols_ttf)
    : b_(backend), fonts_(std::make_unique<Fonts>()) {
    // Without them the text comes out in LVGL's built-in Montserrat, at fixed sizes: still usable.
    const auto load = [](const std::filesystem::path& path) {
        std::vector<uint8_t> data = read_file(path);
        if (data.empty()) logx::error("font: can't read {}, using the built-in one", path.string());
        return data;
    };
    fonts_->regular = load(regular_ttf);
    fonts_->bold = load(bold_ttf);
    fonts_->symbols = load(symbols_ttf);
}

Ui::~Ui() {
    lv_obj_clean(lv_screen_active());
}

// --- helpers -------------------------------------------------------------------------------------

void Ui::set_text(lv_obj_t* lbl, std::string_view s) {
    auto [it, inserted] = texts_.try_emplace(lbl, s);
    if (!inserted) {
        if (it->second == s) return;
        it->second = s;
    }
    lv_label_set_text(lbl, it->second.c_str());
}

void Ui::on_click(lv_obj_t* obj, Action a) {
    lv_obj_set_clickable(obj, true);
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(a)));
    lv_obj_add_event_cb(obj, &Ui::click_cb, LV_EVENT_CLICKED, this);
}

void Ui::click_cb(lv_event_t* e) {
    auto* self = static_cast<Ui*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    self->actions_.push_back(static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj))));
}

std::vector<Action> Ui::take_actions() {
    return std::exchange(actions_, {});
}

// --- building ------------------------------------------------------------------------------------

void Ui::build() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    texts_.clear();
    rows_.clear();
    row_values_.clear();
    hints_.clear();
    built_page_ = Page::Closed;
    row_labels_.clear();
    sync_big_ = sync_chart_ = sync_caption_ = menu_qr_ = name_ta_ = name_kb_ = nullptr;
    sync_series_ = nullptr;
    pair_qr_data_.clear();
    art_version_ = UINT32_MAX;
    theme_applied_ = false;

    const int w = b_.width(), h = b_.height();
    built_w_ = w;
    built_h_ = h;
    layout_ = compute_layout(w, h);
    const Layout& l = layout_;
    const int unit = l.unit;

    Fonts& f = *fonts_;
    f.clear();
    f.small = f.text(f.regular, l.type.small);
    f.small_bold = f.text(f.bold, l.type.small);
    f.body = f.text(f.regular, l.type.body);
    f.body_bold = f.text(f.bold, l.type.body);
    f.large_bold = f.text(f.bold, l.type.large);
    f.display_bold = f.text(f.bold, l.type.display);

    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    // Now Playing --------------------------------------------------------------------------------
    np_ = box(scr);
    lv_obj_set_size(np_, w, h);
    art_placeholder_ = box(np_);
    lv_obj_set_pos(art_placeholder_, l.artwork.x, l.artwork.y);
    lv_obj_set_size(art_placeholder_, l.artwork.w, l.artwork.h);
    lv_obj_set_style_bg_opa(art_placeholder_, LV_OPA_COVER, 0);
    lv_obj_t* noart = label(art_placeholder_, f.small);
    lv_label_set_text(noart, "No artwork");
    lv_obj_center(noart);
    art_ = lv_image_create(np_);
    lv_obj_set_pos(art_, l.artwork.x, l.artwork.y);
    lv_obj_set_size(art_, l.artwork.w, l.artwork.h);
    lv_image_set_inner_align(art_, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_hidden(art_, true);

    const bool center = l.stacked;
    int y = l.text.y + (center ? 0 : l.margin / 3);
    title_ = marquee(np_, f.large_bold, l.text.x, y, l.text.w);
    y += line_height(f.large_bold);
    artist_ = marquee(np_, f.body, l.text.x, y, l.text.w);
    y += line_height(f.body);
    album_ = marquee(np_, f.small, l.text.x, y, l.text.w);
    y += line_height(f.small) + l.margin / 3;
    group_ = marquee(np_, f.small, l.text.x, y, l.text.w);
    if (center) {
        for (lv_obj_t* o : {title_, artist_, album_, group_}) lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
        // Stacked layouts show the group between the times.
        lv_obj_set_pos(group_, l.progress.x + l.progress.w / 4, l.progress.y + l.progress.h + l.margin / 5);
        lv_obj_set_width(group_, l.progress.w / 2);
    }

    // Transport: symbols, each tappable; placed by place_transport().
    transport_cx_ = l.transport.x + l.transport.w / 2.0f;
    transport_cy_ = l.transport.y + l.transport.h / 2.0f;
    transport_step_ = std::min(l.transport.w / 5.0f, l.transport.h * 1.7f);
    const auto symbol = [&](const char* sym, const lv_font_t* font, Action a) {
        lv_obj_t* s = label(np_, font);
        lv_label_set_text(s, sym);
        on_click(s, a);
        lv_obj_set_ext_click_area(s, static_cast<int32_t>(transport_step_ / 3));
        return s;
    };
    shuffle_ = symbol(LV_SYMBOL_SHUFFLE, f.symbol(l.icon_small), Action::ToggleShuffle);
    prev_ = symbol(LV_SYMBOL_PREV, f.symbol(l.icon), Action::Previous);
    play_ = symbol(LV_SYMBOL_PLAY, f.symbol(l.icon), Action::PlayPause);
    next_ = symbol(LV_SYMBOL_NEXT, f.symbol(l.icon), Action::Next);
    repeat_ = symbol(LV_SYMBOL_LOOP, f.symbol(l.icon_small), Action::CycleRepeat);
    dot_px_ = std::max(4.0f, unit / 100.0f);  // one diameter for both dots; their boxes are per icon
    shuffle_dot_ = lv_image_create(np_);      // rendered in apply_theme(), sized in place_transport()
    repeat_dot_ = lv_image_create(np_);
    repeat_one_ = label(np_, f.text_em(f.bold, static_cast<int>(std::lround(l.icon_small * 0.7f))));  // badge
    lv_label_set_text(repeat_one_, "1");
    place_transport();

    bar_ = lv_bar_create(np_);
    // Opaque: LVGL's default bar style draws the track translucent, which would blend our
    // contrast-checked track colour into the background.
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    lv_obj_set_pos(bar_, l.progress.x, l.progress.y);
    lv_obj_set_size(bar_, l.progress.w, l.progress.h);
    bar_px_ = std::max(1, l.progress.w);
    lv_bar_set_range(bar_, 0, bar_px_);
    lv_obj_set_style_radius(bar_, 0, 0);
    lv_obj_set_style_radius(bar_, 0, LV_PART_INDICATOR);
    const int ty = l.progress.y + l.progress.h + l.margin / 5;
    pos_ = label(np_, f.small, l.progress.x, ty);
    dur_ = label(np_, f.small, l.progress.x, ty, l.progress.w);
    lv_obj_set_style_text_align(dur_, LV_TEXT_ALIGN_RIGHT, 0);

    // Waiting / connected ------------------------------------------------------------------------
    idle_ = box(scr);
    lv_obj_set_size(idle_, w, l.footer.y);
    lv_obj_set_flex_flow(idle_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idle_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(idle_, l.margin / 3, 0);
    const auto idle_line = [&](const lv_font_t* font) {
        lv_obj_t* t = label(idle_, font, 0, 0, w - 2 * l.margin);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        return t;
    };
    idle_head_ = idle_line(f.large_bold);
    idle_name_ = idle_line(f.body);
    idle_addr_ = idle_line(f.small);
    idle_hint_ = idle_line(f.small);
    lv_obj_set_hidden(idle_, true);

    // Volume overlay -----------------------------------------------------------------------------
    const int pad = std::max(6, unit * 3 / 100);
    osd_ = box(scr);
    lv_obj_set_size(osd_, std::min(w - 2 * pad, unit * 70 / 100), LV_SIZE_CONTENT);
    lv_obj_align(osd_, LV_ALIGN_TOP_MID, 0, pad);
    lv_obj_set_style_bg_opa(osd_, LV_OPA_90, 0);
    lv_obj_set_style_radius(osd_, unit * 3 / 100, 0);
    lv_obj_set_style_pad_all(osd_, pad, 0);
    lv_obj_set_style_border_width(osd_, std::max(1, unit / 240), 0);  // edge against a panel-coloured background
    osd_label_ = label(osd_, f.small);
    osd_value_ = label(osd_, f.small);
    lv_obj_align(osd_value_, LV_ALIGN_TOP_RIGHT, 0, 0);
    osd_bar_ = lv_bar_create(osd_);
    lv_obj_set_style_bg_opa(osd_bar_, LV_OPA_COVER, 0);  // as the progress bar
    lv_obj_set_size(osd_bar_, LV_PCT(100), std::max(4, unit / 60));
    lv_obj_set_pos(osd_bar_, 0, line_height(f.small) + pad / 3);
    lv_bar_set_range(osd_bar_, 0, 100);
    lv_obj_set_hidden(osd_, true);

    // Pairing overlay ----------------------------------------------------------------------------
    pair_ = box(scr);
    lv_obj_set_size(pair_, std::min(w - pad, unit * 95 / 100), LV_SIZE_CONTENT);
    lv_obj_center(pair_);
    lv_obj_set_style_bg_opa(pair_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pair_, unit * 3 / 100, 0);
    lv_obj_set_style_pad_all(pair_, unit * 5 / 100, 0);
    lv_obj_set_style_border_width(pair_, std::max(1, unit / 240), 0);  // edge against e.g. the "No artwork" box
    lv_obj_set_flex_flow(pair_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pair_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pair_head_ = label(pair_, f.body_bold);
    pair_code_ = label(pair_, f.display_bold);
    pair_qr_ = qr_code(pair_);
    lv_obj_set_hidden(pair_qr_, true);
    pair_hint_ = label(pair_, f.small);  // how to allow a pairing request: the footer says it
    lv_obj_set_hidden(pair_, true);

    // Menu ---------------------------------------------------------------------------------------
    menu_dim_ = box(scr);
    lv_obj_set_size(menu_dim_, w, h);
    lv_obj_set_style_bg_color(menu_dim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(menu_dim_, LV_OPA_60, 0);
    on_click(menu_dim_, Action::Back);  // a tap beside the panel goes back
    menu_panel_ = box(menu_dim_);       // clickable (lv_obj default) without a handler: taps stop here
    lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(menu_panel_, std::max(6, unit * 3 / 100), 0);
    lv_obj_set_style_pad_all(menu_panel_, menu_pad(l), 0);
    // Centred above the footer and no taller than the room there (centred, it reaches the footer
    // at this height): a long page (many paired servers) scrolls, following the focused row.
    lv_obj_align(menu_panel_, LV_ALIGN_CENTER, 0, -l.footer.h / 2);
    lv_obj_set_style_max_height(menu_panel_, l.footer.y - l.margin / 3, 0);
    lv_obj_set_scrollable(menu_panel_, true);
    lv_obj_set_scroll_dir(menu_panel_, LV_DIR_VER);
    lv_obj_set_style_pad_row(menu_panel_, std::max(2, unit / 120), 0);
    lv_obj_set_flex_flow(menu_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_hidden(menu_dim_, true);

    // Footer: above everything, so its hints stay visible (and tappable) with the menu open.
    footer_ = box(scr);
    lv_obj_set_pos(footer_, l.footer.x, l.footer.y);
    lv_obj_set_size(footer_, l.footer.w, l.footer.h);
    lv_obj_set_flex_flow(footer_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer_, l.margin * 2 / 3, 0);
}

void Ui::apply_theme(const Theme& th) {
    theme_ = th;
    theme_applied_ = true;
    lv_obj_set_style_bg_color(lv_screen_active(), col(th.bg), 0);
    set_bg(art_placeholder_, th.panel);
    set_color(lv_obj_get_child(art_placeholder_, 0), th.faint);
    set_color(title_, th.fg);
    set_color(artist_, th.dim);
    set_color(album_, th.dim);
    set_color(group_, th.accent);
    set_color(repeat_one_, th.fg);
    for (size_t i = 0; i < dot_images_.size(); ++i) {
        dot_images_[i] = render_dot(dot_box_[i], dot_px_, col(th.accent));
        lv_image_set_src(i == 0 ? shuffle_dot_ : repeat_dot_, &dot_images_[i]->dsc);
    }
    set_bg(bar_, th.track);
    set_bg(bar_, th.accent, LV_PART_INDICATOR);
    for (lv_obj_t* o : {pos_, dur_}) set_color(o, th.dim);
    set_color(idle_head_, th.fg);
    set_color(idle_name_, th.accent);
    set_color(idle_addr_, th.dim);
    set_bg(osd_, th.panel);
    lv_obj_set_style_border_color(osd_, col(th.track), 0);
    set_color(osd_label_, th.dim);
    set_color(osd_value_, th.fg);
    set_bg(osd_bar_, th.track);
    set_bg(osd_bar_, th.accent, LV_PART_INDICATOR);
    set_bg(pair_, th.panel);
    lv_obj_set_style_border_color(pair_, col(th.track), 0);
    set_color(pair_head_, th.fg);
    set_color(pair_code_, th.accent);
    set_color(pair_hint_, th.dim);
    set_bg(menu_panel_, th.panel);
    // The open page is rebuilt in the new colours (the theme follows the artwork, so this happens
    // on every track change), keeping its focus and what was typed.
    restyle_ = true;
    hints_.clear();  // the footer pills too
}

// --- menu ----------------------------------------------------------------------------------------

void Ui::toggle_menu() {
    // The name editor's Cancel (Esc, its footer pill, the Menu button) returns to Settings.
    if (page_ == Page::Name) open_page(Page::Settings, true);
    else open_page(page_ == Page::Closed ? Page::Main : Page::Closed);
}

bool Ui::open_menu_page(std::string_view n) {
    static constexpr std::pair<std::string_view, Page> kPages[] = {
        {"main", Page::Main},       {"help", Page::Help}, {"settings", Page::Settings}, {"sync", Page::Sync},
        {"pairing", Page::Pairing}, {"name", Page::Name}, {"info", Page::Info},         {"quit", Page::Quit},
    };
    const auto* const it = std::ranges::find(kPages, n, &std::pair<std::string_view, Page>::first);
    if (it == std::end(kPages)) return false;
    open_page(it->second);
    return true;
}

void Ui::open_page(Page p, bool back) {
    if (page_ != Page::Closed && rows_selectable_) focus_[static_cast<size_t>(page_)] = focused_row();
    restore_focus_ = back;
    page_ = p;
    built_page_ = Page::Closed;  // rebuilt on the next update
}

int Ui::focused_row() const {
    const auto it = std::ranges::find(rows_, lv_group_get_focused(b_.group()));
    return it == rows_.end() ? 0 : static_cast<int>(it - rows_.begin());
}

bool Ui::menu_action(Action a, SendspinService& service, const PlayerStatus& st) {
    const bool ok = a == Action::PlayPause, back = a == Action::Back;
    const bool left = a == Action::Previous, right = a == Action::Next;
    if (page_ == Page::Name) {
        // muOS's keyboard: the D-pad moves over the keys, A types, B deletes, Y adds a space,
        // X cancels; the keyboard's own ✓ key saves.
        if (!name_kb_) return false;
        switch (a) {
            case Action::Previous: b_.key(LV_KEY_LEFT); break;
            case Action::Next: b_.key(LV_KEY_RIGHT); break;
            case Action::VolumeUp: b_.key(LV_KEY_UP); break;
            case Action::VolumeDown: b_.key(LV_KEY_DOWN); break;
            case Action::PlayPause: b_.key(LV_KEY_ENTER); break;
            case Action::Back: lv_textarea_delete_char(name_ta_); break;
            case Action::CycleRepeat: lv_textarea_add_char(name_ta_, ' '); break;
            case Action::PlayerVolumeDown: lv_textarea_set_text(name_ta_, ""); break;  // L1: clear
            case Action::ToggleShuffle: open_page(Page::Settings, true); break;
            default: break;
        }
        return false;
    }
    if (a == Action::VolumeUp || a == Action::VolumeDown) {
        b_.key(a == Action::VolumeUp ? LV_KEY_PREV : LV_KEY_NEXT);
        return false;
    }
    const int row = focused_row();
    switch (page_) {
        case Page::Main:
            if (back) open_page(Page::Closed, true);
            else if (ok)
                open_page(std::array{Page::Help, Page::Settings, Page::Info, Page::Quit}[std::clamp(row, 0, 3)]);
            break;
        case Page::Help:
        case Page::Info:
            if (ok || back) open_page(Page::Main, true);
            break;
        case Page::Settings: {
            const auto r = static_cast<SettingsRow>(row);
            if (back) open_page(Page::Main, true);
            else if (r == SettingsRow::Screen && (ok || left || right))
                service.set_idle_screensaver(!st.idle_screensaver);
            else if (r == SettingsRow::SyncOffset && ok) open_page(Page::Sync);
            else if (r == SettingsRow::SyncOffset && (left || right)) service.change_latency(right ? +5 : -5);
            else if (r == SettingsRow::AudioFormat && (ok || left || right))
                service.set_audio_format(st.audio_format_next == "opus" ? "flac" : "opus");
            else if (r == SettingsRow::Pairing && ok) open_page(Page::Pairing);
            else if (r == SettingsRow::Name && ok) open_page(Page::Name);
        } break;
        case Page::Pairing:
            if (back) {
                open_page(Page::Settings, true);
            } else if (ok && row < static_cast<int>(st.paired_servers.size())) {
                const PairedServer& server = st.paired_servers[static_cast<size_t>(row)];
                forget_id_ = server.id;
                forget_name_ = server_label(server);
                open_page(Page::Forget);
            }
            break;
        case Page::Forget:
            if (ok) service.forget_server(forget_id_);
            if (ok || back) open_page(Page::Pairing, true);
            break;
        case Page::Sync:
            if (left || right) service.change_latency(right ? +5 : -5);
            else if (ok || back) open_page(Page::Settings, true);
            break;
        case Page::Quit:
            if (ok) return true;
            if (back) open_page(Page::Main, true);
            break;
        case Page::Name:  // handled above
        case Page::Closed:
        case Page::Count: break;
    }
    return false;
}

void Ui::refresh_page(const PlayerStatus& st, const Extras& x) {
    const int unit = layout_.unit;
    const int pad = menu_pad(layout_);
    const int inset = pad / 2;  // rows pad their text for the highlight; everything else matches it
    const Theme& th = theme_;
    const Fonts& f = *fonts_;

    struct Row {
        std::string label, value;
        bool opens = false;  // leads to a page: an arrow after the value
    };
    std::vector<Row> rows;
    std::string title, message, big, qr;
    bool selectable = true, chart = false;
    switch (page_) {
        case Page::Main: rows = {{"Help", ""}, {"Settings", ""}, {"Info", ""}, {"Quit", ""}}; break;
        case Page::Help:
            title = "Controls";
            selectable = false;
            rows = x.has_controller
                       ? std::vector<Row>{{"A", "Play / pause"},
                                          {"D-pad left / right", "Previous / next"},
                                          {"D-pad up / down", "Group volume"},
                                          {"L1 / R1", "This player's volume"},
                                          {"L2 / R2", "Seek 10 s"},
                                          {"X", "Shuffle"},
                                          {"Y", "Repeat"},
                                          {"Menu", "Menu"}}
                       : std::vector<Row>{{"Space", "Play / pause"},     {"Left / right", "Previous / next"},
                                          {"Up / down", "Group volume"}, {"- / +", "This player's volume"},
                                          {"J / L", "Seek 10 s"},        {"S / R", "Shuffle / repeat"},
                                          {", / .", "Sync tuning 5 ms"}, {"Esc", "Menu"}};
            break;
        case Page::Settings: {
            title = "Settings";
            std::string format = st.audio_format_next == "opus" ? "Opus" : "FLAC";
            if (!x.demo && st.audio_format_next != st.audio_format) format += " (after restart)";
            rows.resize(static_cast<size_t>(SettingsRow::Count));
            const auto set = [&](SettingsRow r, Row row) { rows[static_cast<size_t>(r)] = std::move(row); };
            set(SettingsRow::Screen, {"Screen while playing", st.idle_screensaver ? "Dims if left alone" : "Stays on"});
            set(SettingsRow::SyncOffset, {"Sync offset", std::format("{:+} ms", st.extra_latency_ms), true});
            set(SettingsRow::AudioFormat, {"Audio format", format});
            set(SettingsRow::Pairing, {"Paired servers", std::to_string(st.paired_servers.size()), true});
            set(SettingsRow::Name, {"Player name", x.demo ? std::string("demo") : st.name, true});
            break;
        }
        case Page::Pairing:
            title = "Paired servers";
            for (const PairedServer& server : st.paired_servers)
                rows.push_back({server_label(server), st.connected && server.id == st.server_id ? "connected" : ""});
            if (rows.empty()) message = "None yet. A server asks to pair the first time it plays here.";
            selectable = !rows.empty();
            qr = st.pairing_token;
            break;
        case Page::Name:
            title = "Player name";
            selectable = false;
            break;
        case Page::Forget:
            title = "Forget this server?";
            message = std::format(
                "{} can't play here until it pairs again. Anything playing stops while the "
                "player restarts.",
                forget_name_);
            selectable = false;
            break;
        case Page::Sync:
            title = "Sync";
            big = std::format("{:+} ms", st.extra_latency_ms);
            message = "Shifts this player against the others. Higher plays earlier.";
            rows = {{"Output latency", std::format("{} ms", st.platform_latency_ms)}};
            selectable = false;
            chart = true;
            break;
        case Page::Info: {
            title = "Info";
            selectable = false;
            std::string volume = st.has_controller ? std::format("player {}, group {}", st.volume, st.group_volume)
                                                   : std::to_string(st.volume);
            if (st.muted) volume += ", muted";
            rows = {
                {"Version", MUOS_SENDSPIN_VERSION},
                {"Player", x.demo ? "demo" : st.name},
                {"Address", x.address},
                {"Server", !st.connected ? "not connected"
                                         : short_server_name(st.server_name) + (st.paired ? ", paired" : ", unpaired")},
                {"Stream", st.stream.empty() ? "none" : st.stream},
                {"Volume", volume},
                {"Latency", std::format("{} ms, tuning {:+} ms", st.platform_latency_ms, st.extra_latency_ms)},
                {"Drivers", std::format("{} · {}", x.renderer_info, x.audio_driver)},
                {"GitHub", MUOS_SENDSPIN_GITHUB}};
            break;
        }
        case Page::Quit:
            title = "Quit Sendspin?";
            message = "Servers can't play here until you start it again.";
            selectable = false;
            break;
        case Page::Closed:
        case Page::Count: return;
    }

    std::vector<std::string> labels;
    labels.reserve(rows.size());
    for (const Row& r : rows) labels.push_back(r.label);
    if (built_page_ != page_ || labels != row_labels_ || restyle_) {
        // Rebuild the panel's contents for this page. Rebuilding an open page (a server paired
        // meanwhile, new colours) keeps the focus where it was, and the name being typed.
        std::optional<NameDraft> draft;
        if (built_page_ == page_) {
            if (rows_selectable_) {
                focus_[static_cast<size_t>(page_)] = focused_row();
                restore_focus_ = true;
            }
            if (name_ta_ && name_kb_)
                draft = NameDraft{.text = lv_textarea_get_text(name_ta_),
                                  .cursor = lv_textarea_get_cursor_pos(name_ta_),
                                  .mode = lv_keyboard_get_mode(name_kb_),
                                  .key = lv_buttonmatrix_get_selected_button(name_kb_)};
        }
        built_page_ = page_;
        restyle_ = false;
        clear_menu_panel();
        row_labels_ = std::move(labels);
        rows_selectable_ = selectable;
        const int pw = std::min(built_w_ - 2 * pad, page_ == Page::Main ? unit * 60 / 100 : unit * 115 / 100);
        lv_obj_set_size(menu_panel_, pw, LV_SIZE_CONTENT);
        const auto text = [&](const lv_font_t* font, SDL_Color c, std::string_view s) {
            lv_obj_t* t = label(menu_panel_, font);
            lv_obj_set_style_pad_hor(t, inset, 0);
            set_color(t, c);
            lv_label_set_text(t, std::string(s).c_str());
            return t;
        };
        if (!title.empty()) {
            lv_obj_t* t = text(f.body_bold, th.dim, title);
            lv_obj_set_style_pad_bottom(t, pad / 4, 0);
            // Titles line up with row text, inset in the rows' highlight; the name editor has no
            // rows, so its title lines up with the field and the keys instead.
            if (page_ == Page::Name) lv_obj_set_style_pad_hor(t, 0, 0);
        }
        if (!big.empty()) {
            sync_big_ = text(f.display_bold, th.fg, "");
            lv_obj_set_width(sync_big_, LV_PCT(100));
            lv_obj_set_style_text_align(sync_big_, LV_TEXT_ALIGN_CENTER, 0);
        }
        if (!message.empty()) {
            lv_obj_t* m = text(f.body, th.fg, message);
            lv_obj_set_width(m, LV_PCT(100));
            lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        }
        for (const Row& r : rows) {
            lv_obj_t* row = box(menu_panel_);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(row, inset, 0);
            lv_obj_set_style_pad_ver(row, selectable ? pad / 2 : pad / 6, 0);
            lv_obj_set_style_radius(row, std::max(4, unit / 50), 0);
            lv_obj_t* lbl = label(row, f.body);
            set_color(lbl, th.dim);
            lv_label_set_text(lbl, r.label.c_str());
            lv_obj_t* val = label(row, f.body);
            set_color(val, th.dim);
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
            if (r.opens) {
                // The arrow is a symbol (Symbols.ttf): inside the value's Noto line it would sit
                // where that font puts it. Its own label, centred on the ink of the value's capitals.
                lv_obj_t* arrow = label(row, f.body);
                set_color(arrow, th.dim);
                lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
                const uint32_t cp = first_code_point(LV_SYMBOL_RIGHT);
                const Ink a = ink_of(f.body, cp), cap = ink_of(f.body, 'H');
                lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, snap(cap.cy() - a.cy()));
                const auto advance = static_cast<int32_t>(lv_font_get_glyph_width(f.body, cp, 0));
                lv_obj_set_style_pad_right(val, advance + inset, 0);
            }
            if (selectable) {
                // LVGL tracks the focused row (keys move it, a tap focuses it); a tap also confirms.
                lv_obj_set_style_bg_color(row, col(th.fg), LV_STATE_FOCUSED);
                lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_FOCUSED);
                lv_group_add_obj(b_.group(), row);
                lv_obj_set_scroll_on_focus(row, true);  // a panel taller than the screen scrolls
                on_click(row, Action::PlayPause);
            }
            rows_.push_back(row);
            row_values_.push_back(val);
        }
        if (chart) {
            sync_chart_ = lv_chart_create(menu_panel_);
            lv_obj_set_size(sync_chart_, pw - 2 * pad - 2 * inset, unit * 22 / 100);  // the text column
            lv_obj_set_style_margin_left(sync_chart_, inset, 0);
            lv_chart_set_type(sync_chart_, LV_CHART_TYPE_LINE);
            lv_chart_set_point_count(sync_chart_, PlayerStatus::kClockHistory);
            lv_chart_set_div_line_count(sync_chart_, 3, 0);
            lv_obj_set_style_line_color(sync_chart_, col(th.track), LV_PART_MAIN);  // subdued grid
            lv_obj_set_style_size(sync_chart_, 0, 0, LV_PART_INDICATOR);            // no point markers
            lv_obj_set_style_bg_color(sync_chart_, col(th.bg), 0);
            lv_obj_set_style_border_width(sync_chart_, 0, 0);
            sync_series_ = lv_chart_add_series(sync_chart_, col(th.accent), LV_CHART_AXIS_PRIMARY_Y);
            sync_caption_ = text(f.small, th.dim, "");
        }
        if (page_ == Page::Name) build_name_editor(st, draft);
        if (!qr.empty()) {
            // This player's pairing token: a server that scans it pairs without a code.
            lv_obj_t* row = box(menu_panel_);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(row, inset, 0);
            lv_obj_set_style_pad_top(row, pad / 2, 0);
            lv_obj_set_style_pad_column(row, pad, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            menu_qr_ = qr_code(row);
            set_qr(menu_qr_, qr, unit * 35 / 100);
            lv_obj_t* caption = label(row, f.small);
            set_color(caption, th.dim);
            lv_obj_set_flex_grow(caption, 1);
            lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_WRAP);
            lv_label_set_text(caption, "A Sendspin server can also pair by scanning this code.");
        }
        if (!rows_.empty() && selectable) {
            const size_t row = restore_focus_ ? static_cast<size_t>(std::clamp(focus_[static_cast<size_t>(page_)], 0,
                                                                               static_cast<int>(rows_.size()) - 1))
                                              : 0;
            lv_group_focus_obj(rows_[row]);
        }
    }

    // Live values (rows keep their widgets; only text changes).
    for (size_t i = 0; i < rows.size() && i < row_values_.size(); ++i) set_text(row_values_[i], rows[i].value);
    if (sync_big_) set_text(sync_big_, big);
    if (sync_chart_ && sync_series_) {
        const auto& hist = st.clock_error_us;
        const float last = hist.empty() ? 0.0f : hist.back();
        float peak = 0.0f;
        for (const float v : hist) peak = std::max(peak, std::abs(v));
        if (hist.size() != sync_points_ || last != sync_last_) {
            sync_points_ = hist.size();
            sync_last_ = last;
            lv_chart_set_axis_range(sync_chart_, LV_CHART_AXIS_PRIMARY_Y, 0,
                                    static_cast<int32_t>(std::ceil(std::max(1.0f, peak))));
            lv_chart_set_all_values(sync_chart_, sync_series_, LV_CHART_POINT_NONE);
            for (const float v : hist) lv_chart_set_next_value(sync_chart_, sync_series_, static_cast<int32_t>(v));
        }
        std::string caption = std::format("Clock sync error, peak {:.0f} µs, last {} syncs", peak, hist.size());
        if (st.clock_sync_interval_s > 0) caption += std::format(", every ~{} s", st.clock_sync_interval_s);
        set_text(sync_caption_, caption);
    }
    // The focused row's text is brighter.
    for (lv_obj_t* row : rows_) {
        const SDL_Color c = lv_obj_has_state(row, LV_STATE_FOCUSED) ? th.fg : th.dim;
        for (uint32_t k = 0; k < lv_obj_get_child_count(row); ++k)
            set_color(lv_obj_get_child(row, static_cast<int32_t>(k)), c);
    }
}

void Ui::build_name_editor(const PlayerStatus& st, const std::optional<NameDraft>& draft) {
    const int unit = layout_.unit;
    const int gap = std::max(2, unit / 120);
    const int radius = std::max(3, unit / 90);
    const Theme& th = theme_;
    const Fonts& f = *fonts_;

    name_ta_ = lv_textarea_create(menu_panel_);
    lv_obj_set_width(name_ta_, LV_PCT(100));
    lv_textarea_set_one_line(name_ta_, true);
    lv_textarea_set_max_length(name_ta_, 40);  // well inside an mDNS instance name (63 bytes)
    // Not customized: an empty field. What an empty name means is said below it in words; a greyed
    // placeholder was too easy to mistake for typed text on the handheld's screen.
    lv_textarea_set_text(name_ta_, draft ? draft->text.c_str() : st.custom_name ? st.name.c_str() : "");
    if (draft) lv_textarea_set_cursor_pos(name_ta_, static_cast<int32_t>(draft->cursor));
    lv_obj_set_style_text_font(name_ta_, f.body, 0);
    lv_obj_set_style_text_color(name_ta_, col(th.fg), 0);
    lv_obj_set_style_bg_color(name_ta_, col(th.bg), 0);
    lv_obj_set_style_border_width(name_ta_, 0, 0);
    lv_obj_set_style_radius(name_ta_, radius, 0);
    // The keyboard marks its text area focused (the keys hold the real focus): our cursor style
    // must win over the theme's for that state. It doesn't blink: no group focus event starts the
    // blinking, so an idle page doesn't redraw.
    lv_obj_set_style_border_color(name_ta_, col(th.accent), sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
    lv_obj_set_style_border_width(name_ta_, std::max(2, unit / 240), sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
    lv_obj_set_style_border_side(name_ta_, LV_BORDER_SIDE_LEFT, sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
    lv_obj_set_style_pad_left(name_ta_, 0, sel(LV_PART_CURSOR, LV_STATE_FOCUSED));

    lv_obj_t* note = label(menu_panel_, f.small);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(note, line_height(f.small));
    set_color(note, th.dim);
    lv_label_set_text(note, std::format("Leave empty to use the device's name: {}",
                                        st.device_name.empty() ? "the device's" : st.device_name)
                                .c_str());

    name_kb_ = lv_keyboard_create(menu_panel_);
    lv_obj_set_size(name_kb_, LV_PCT(100), unit * 47 / 100);
    for (const auto mode : {LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER, LV_KEYBOARD_MODE_SPECIAL}) {
        const char* const* keys = mode == LV_KEYBOARD_MODE_TEXT_UPPER ? kKeysUpper
                                  : mode == LV_KEYBOARD_MODE_SPECIAL  ? kKeysSpecial
                                                                      : kKeysLower;
        lv_keyboard_set_map(name_kb_, mode, keys, kKeyCtrl);
    }
    lv_keyboard_set_mode(name_kb_, draft ? draft->mode : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(name_kb_, name_ta_);
    lv_obj_set_style_bg_opa(name_kb_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(name_kb_, 0, 0);
    lv_obj_set_style_pad_all(name_kb_, 0, 0);
    lv_obj_set_style_pad_gap(name_kb_, gap, 0);
    lv_obj_set_style_text_font(name_kb_, f.body, LV_PART_ITEMS);
    lv_obj_set_style_radius(name_kb_, radius, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(name_kb_, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(name_kb_, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(name_kb_, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(name_kb_, col(th.bg), LV_PART_ITEMS);
    lv_obj_set_style_text_color(name_kb_, col(th.fg), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(name_kb_, col(th.track), sel(LV_PART_ITEMS, LV_STATE_CHECKED));  // mode keys
    lv_obj_set_style_text_color(name_kb_, col(th.dim), sel(LV_PART_ITEMS, LV_STATE_CHECKED));
    for (const lv_state_t state : {LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_PRESSED}) {  // the selected key
        lv_obj_set_style_bg_color(name_kb_, col(th.accent), sel(LV_PART_ITEMS, state));
        lv_obj_set_style_text_color(name_kb_, col(th.bg), sel(LV_PART_ITEMS, state));
    }
    lv_obj_set_style_outline_width(name_kb_, 0, LV_STATE_FOCUS_KEY);  // the theme's focus ring
    lv_obj_add_event_cb(name_kb_, &Ui::name_ready_cb, LV_EVENT_READY, this);
    lv_group_add_obj(b_.group(), name_kb_);
    lv_group_focus_obj(name_kb_);
    lv_buttonmatrix_set_selected_button(name_kb_, draft ? draft->key : 0);
}

void Ui::name_ready_cb(lv_event_t* e) {
    auto* self = static_cast<Ui*>(lv_event_get_user_data(e));
    if (!self->name_ta_) return;
    self->name_saved_ = lv_textarea_get_text(self->name_ta_);
    self->open_page(Page::Settings, true);
}

void Ui::type_text(std::string_view utf8) {
    if (name_ta_) lv_textarea_add_text(name_ta_, std::string(utf8).c_str());
}

bool Ui::text_key(SDL_Keycode key) {
    if (!name_ta_) return false;
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            name_saved_ = lv_textarea_get_text(name_ta_);
            open_page(Page::Settings, true);
            return true;
        case SDLK_ESCAPE: open_page(Page::Settings, true); return true;
        case SDLK_BACKSPACE: lv_textarea_delete_char(name_ta_); return true;
        case SDLK_LEFT:
        case SDLK_RIGHT:
        case SDLK_UP:
        case SDLK_DOWN: return false;  // move over the on-screen keys
        default: return true;          // printable keys arrive as SDL_TEXTINPUT
    }
}

std::optional<std::string> Ui::take_name() {
    return std::exchange(name_saved_, std::nullopt);
}

void Ui::clear_menu_panel() {
    for (lv_obj_t* o : row_values_) texts_.erase(o);
    texts_.erase(sync_big_);
    texts_.erase(sync_caption_);
    lv_obj_clean(menu_panel_);
    rows_.clear();
    row_values_.clear();
    sync_big_ = sync_chart_ = sync_caption_ = menu_qr_ = name_ta_ = name_kb_ = nullptr;
    sync_series_ = nullptr;
    sync_points_ = 0;
}

// --- per-frame update ----------------------------------------------------------------------------

void Ui::show_volume(std::string_view lbl, int value) {
    if (!osd_) return;
    set_text(osd_label_, lbl);
    set_text(osd_value_, std::to_string(value));
    lv_bar_set_value(osd_bar_, value, LV_ANIM_OFF);
    osd_until_ms_ = SDL_GetTicks64() + kVolumeOsdMs;
    show(osd_, true);
}

void Ui::update_artwork(const PlayerStatus& st) {
    if (st.artwork_version == art_version_) return;
    art_version_ = st.artwork_version;
    const bool has = st.artwork && st.artwork->w > 0;
    show(art_, has);
    show(art_placeholder_, !has);
    if (!has) return;
    const Image& img = *st.artwork;
    // A fresh image (and descriptor) per artwork: LVGL's image cache is off (lv_conf.h keeps the
    // default 0), so nothing holds on to the old one.
    auto art = std::make_unique<ArgbImage>(img.w, img.h);
    // LVGL ARGB8888 is B,G,R,A in memory; ours is R,G,B,A.
    std::vector<uint8_t>& px = art->pixels;
    for (size_t i = 0; i + 3 < px.size() && i + 3 < img.rgba.size(); i += 4) {
        px[i] = img.rgba[i + 2];
        px[i + 1] = img.rgba[i + 1];
        px[i + 2] = img.rgba[i];
        px[i + 3] = img.rgba[i + 3];
    }
    lv_image_set_src(art_, &art->dsc);
    art_image_ = std::move(art);
}

void Ui::place_transport() {
    // Five equally spaced slots around the centre. Shuffle and repeat keep the outer ones (the row
    // spans the same width either way); without previous/next, play/pause stands alone between them.
    const auto slot = [&](int k) { return transport_cx_ + k * transport_step_; };
    const auto place = [&](lv_obj_t* o, float x) {  // icon ink centred on (x, transport centre)
        const Ink ink = ink_of(lv_obj_get_style_text_font(o, LV_PART_MAIN), first_code_point(lv_label_get_text(o)));
        const int32_t ox = snap(x - ink.cx()), oy = snap(transport_cy_ - ink.cy());
        lv_obj_set_pos(o, ox, oy);
        return Ink{ox + ink.x, oy + ink.y, ink.w, ink.h};  // ink on screen (LVGL lays out later)
    };
    const float shuffle_x = slot(-2), repeat_x = slot(2);
    const Ink shuffle = place(shuffle_, shuffle_x);
    place(prev_, slot(-1));
    place(play_, slot(0));
    place(next_, slot(1));
    const Ink repeat = place(repeat_, repeat_x);
    // State dots centred under the toggles' ink: each dot's image box takes the parity of its
    // icon's ink width, so box and ink share a centre exactly; the dot itself (same diameter for
    // both) is drawn centred in its box (glyphs.cpp). Geometry comes from our numbers, not from
    // LVGL, which only knows sizes after its next layout pass.
    const auto under = [&](size_t i, lv_obj_t* dot_obj, const Ink& icon) {
        const int ink_w = static_cast<int>(icon.w);
        int box = static_cast<int>(std::ceil(dot_px_)) + 1;  // room for the anti-aliased edge
        if ((box - ink_w) % 2 != 0) ++box;
        dot_box_[i] = box;
        lv_obj_set_size(dot_obj, box, box);
        const float gap = std::max(2.0f, dot_px_);
        lv_obj_set_pos(dot_obj, static_cast<int32_t>(icon.x) + (ink_w - box) / 2, snap(icon.y + icon.h + gap));
    };
    under(0, shuffle_dot_, shuffle);
    under(1, repeat_dot_, repeat);
    const Ink one = ink_of(lv_obj_get_style_text_font(repeat_one_, LV_PART_MAIN), '1');
    lv_obj_set_pos(repeat_one_, snap(repeat.x + repeat.w - one.x - one.w / 2), snap(repeat.y - one.y - one.h / 2));
}

void Ui::update_now_playing(const PlayerStatus& st, const NowPlayingView& v, bool pointer) {
    // Long texts scroll for a while after they change (enough to read them), then settle to "…":
    // a scrolling label redraws every frame, which a static screen shouldn't pay for.
    const std::string title = v.title.empty() ? "Nothing playing" : v.title;
    const uint64_t now = SDL_GetTicks64();
    const auto differs = [&](lv_obj_t* o, const std::string& text) {
        const auto it = texts_.find(o);
        return it == texts_.end() || it->second != text;
    };
    const auto texts = {title_, artist_, album_, group_};
    if (differs(title_, title) || differs(artist_, v.artist) || differs(album_, v.album) || differs(group_, v.group)) {
        marquee_until_ms_ = now + kMarqueeMs;
        for (lv_obj_t* o : texts)
            if (lv_label_get_long_mode(o) != LV_LABEL_LONG_MODE_SCROLL_CIRCULAR)
                lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    } else if (marquee_until_ms_ && now >= marquee_until_ms_) {
        marquee_until_ms_ = 0;
        for (lv_obj_t* o : texts) lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    }
    set_text(title_, title);
    set_text(artist_, v.artist);
    set_text(album_, v.album);
    set_text(group_, v.group);
    update_artwork(st);

    // Play and pause have different ink: centre the new one.
    if (const char* sym = v.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        std::string_view(lv_label_get_text(play_)) != sym) {
        set_text(play_, sym);
        place_transport();
    }
    set_color(prev_, v.can_previous ? theme_.fg : theme_.faint);
    set_color(next_, v.can_next ? theme_.fg : theme_.faint);
    set_color(play_, v.can_play_pause ? theme_.fg : theme_.faint);
    // Previous/next on screen are for touch and mouse; with buttons only, the footer's D-pad says it.
    show(play_, v.transport);
    for (lv_obj_t* o : {prev_, next_}) show(o, v.transport && pointer);
    show(shuffle_, v.transport && v.shuffle != Toggle::Hidden);
    show(repeat_, v.transport && v.repeat != RepeatShown::Hidden);
    const bool shuffle_on = v.shuffle == Toggle::On;
    const bool repeat_on = v.repeat == RepeatShown::All || v.repeat == RepeatShown::One;
    set_color(shuffle_, shuffle_on ? theme_.fg : theme_.faint);
    set_color(repeat_, repeat_on ? theme_.fg : theme_.faint);
    show(shuffle_dot_, v.transport && shuffle_on);
    show(repeat_dot_, v.transport && repeat_on);
    show(repeat_one_, v.transport && v.repeat == RepeatShown::One);

    const int32_t px = v.duration_ms > 0 ? static_cast<int32_t>(static_cast<int64_t>(bar_px_) *
                                                                std::min(v.position_ms, v.duration_ms) / v.duration_ms)
                                         : 0;
    if (lv_bar_get_value(bar_) != px) lv_bar_set_value(bar_, px, LV_ANIM_OFF);
    set_text(pos_, (v.duration_ms > 0 || v.position_ms > 0) ? format_time(v.position_ms) : std::string());
    set_text(dur_, v.duration_ms > 0 ? format_time(v.duration_ms) : std::string());
}

void Ui::update_pairing(const PlayerStatus& st) {
    const bool pairing = st.pairing_code || st.pairing_window_prompt;
    show(pair_, pairing);
    if (!pairing) return;
    const std::string who = short_server_name(st.server_name);
    std::string code = st.pairing_code.value_or("");
    const bool qr = st.pairing_code_qr && !code.empty();
    if (qr && code != pair_qr_data_) {
        pair_qr_data_ = code;
        set_qr(pair_qr_, code, layout_.unit * 45 / 100);
    }
    // Shown as 3-3 digits: the Sendspin specification (pairing, "Pairing Code Presentation").
    if (!qr && code.size() == 6) code.insert(3, " ");
    set_text(pair_head_, "Pair with " + who);
    set_text(pair_code_, qr ? std::string() : code);
    show(pair_code_, !code.empty() && !qr);
    show(pair_qr_, qr);
    set_text(pair_hint_, code.empty() ? who + " wants to pair with this player"
                         : qr         ? "Scan this code in " + who
                                      : "Enter this code in " + who);
}

void Ui::update_footer(const PlayerStatus& st, const NowPlayingView& v, const Extras& x) {
    const bool pad = x.has_controller;
    const auto hint = [pad](std::initializer_list<Action> actions, std::string text) {
        Hint h{.keys = {}, .text = std::move(text)};
        for (const Action a : actions) h.keys.push_back({std::string(key_name(a, pad)), a});
        return h;
    };
    std::vector<Hint> hints;
    switch (page_) {
        case Page::Closed:
            if (st.pairing_window_prompt) hints.push_back(hint({Action::PlayPause}, "Allow pairing"));
            else if (v.transport && !x.hold_waiting) {
                // A constant caption: a changing one would shift the hints after it.
                hints.push_back(hint({Action::PlayPause}, "Play / pause"));
                hints.push_back(hint({Action::Previous, Action::Next}, "Skip"));
                hints.push_back(hint({Action::VolumeUp, Action::VolumeDown}, "Volume"));
            } else if (!x.hold_waiting) {
                hints.push_back(hint({Action::PlayerVolumeDown, Action::PlayerVolumeUp}, "Volume"));
            }
            hints.push_back(hint({Action::Menu}, "Menu"));
            break;
        case Page::Main: hints = {hint({Action::PlayPause}, "Select"), hint({Action::Back}, "Back")}; break;
        case Page::Settings: {
            // Screen, sync offset and audio format change in place (and with left/right); the sync
            // offset and paired servers also open a page, the name the editor.
            const auto row = static_cast<SettingsRow>(focused_row());
            const bool opens = row == SettingsRow::SyncOffset || row == SettingsRow::Pairing;
            hints = {hint({Action::PlayPause}, opens ? "Open" : row == SettingsRow::Name ? "Edit" : "Change")};
            if (row != SettingsRow::Pairing && row != SettingsRow::Name)
                hints.push_back(hint({Action::Previous, Action::Next}, "Adjust"));
            hints.push_back(hint({Action::Back}, "Back"));
        } break;
        case Page::Sync:
            hints = {hint({Action::Previous, Action::Next}, "Adjust"), hint({Action::Back}, "Back")};
            break;
        case Page::Pairing:
            if (!st.paired_servers.empty()) hints.push_back(hint({Action::PlayPause}, "Forget"));
            hints.push_back(hint({Action::Back}, "Back"));
            break;
        case Page::Forget: hints = {hint({Action::PlayPause}, "Forget"), hint({Action::Back}, "Cancel")}; break;
        case Page::Name:
            if (pad) {
                hints = {hint({Action::PlayPause}, "Type"), hint({Action::Back}, "Delete"),
                         hint({Action::CycleRepeat}, "Space"), hint({Action::PlayerVolumeDown}, "Clear"),
                         hint({Action::ToggleShuffle}, "Cancel")};
            } else {  // a physical keyboard types; Return saves, Esc cancels
                hints = {hint({Action::Back}, "Delete"), hint({Action::Menu}, "Cancel")};
            }
            break;
        case Page::Quit: hints = {hint({Action::PlayPause}, "Quit"), hint({Action::Back}, "Cancel")}; break;
        case Page::Help:
        case Page::Info:
        case Page::Count: hints = {hint({Action::Back}, "Back")}; break;
    }
    if (hints == hints_) return;
    hints_ = std::move(hints);

    lv_obj_clean(footer_);
    footer_glyphs_.clear();  // after the images that showed them are gone
    const Fonts& f = *fonts_;
    const int unit = layout_.unit;
    const int gap = std::max(2, unit / 160);
    for (const Hint& h : hints_) {
        const bool dpad =
            h.keys.size() == 2 && (h.keys[0].action == Action::Previous || h.keys[0].action == Action::VolumeUp);
        // The whole hint is tappable (its first key's action); each pill also on its own. A D-pad
        // hint has two directions, so only its glyph is, each half its own.
        lv_obj_t* item = box(footer_);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_PCT(100));
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, gap * 2, 0);
        if (!dpad) on_click(item, h.keys.front().action);
        lv_obj_t* keys = box(item);
        lv_obj_set_size(keys, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(keys, gap, 0);
        // Centring rules: glyph boxes are odd-sized, so they have a centre pixel; shapes are
        // rendered symmetric by construction (glyphs.cpp); letters are centred on their ink with one
        // rounding step (snap), so odd-sized letters land exactly and even ones always lean the same way.
        const int d = (line_height(f.small_bold) + gap) | 1;
        const auto glyph = [&](Glyph g) {
            footer_glyphs_.push_back(render_glyph(g, d, col(theme_.dim)));
            lv_obj_t* o = lv_image_create(keys);
            lv_image_set_src(o, &footer_glyphs_.back()->dsc);
            lv_obj_set_size(o, d, d);
            lv_obj_set_ext_click_area(o, gap * 2);
            return o;
        };
        if (dpad) {
            // One D-pad glyph, as muOS draws it, with the axis in use marked; tapping a half picks
            // that direction.
            lv_obj_t* o = glyph(h.keys[0].action == Action::Previous ? Glyph::DpadLeftRight : Glyph::DpadUpDown);
            lv_obj_set_clickable(o, true);
            lv_obj_set_user_data(o, reinterpret_cast<void*>(static_cast<intptr_t>(h.keys[0].action)));
            lv_obj_add_event_cb(o, &Ui::dpad_click_cb, LV_EVENT_CLICKED, this);
        }
        for (const Hint::Key& k : h.keys) {
            if (dpad) break;
            if (k.action == Action::Menu && pad) {
                on_click(glyph(Glyph::Menu), k.action);  // muOS's menu glyph: a circle with three dots
                continue;
            }
            // Single buttons (A, B, X, Y) are circles, like the buttons themselves; longer names
            // (L1, Space, Esc) are pills of the same height.
            lv_obj_t* pill = box(keys);
            const bool circle = k.label.size() == 1;
            lv_obj_set_size(pill, circle ? d : LV_SIZE_CONTENT, d);
            if (!circle) lv_obj_set_style_pad_hor(pill, gap * 3, 0);
            lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(pill, col(theme_.dim), 0);
            lv_obj_t* t = label(pill, f.small_bold);
            lv_obj_set_style_text_color(t, col(theme_.bg), 0);
            lv_label_set_text(t, k.label.c_str());
            // One letter: centred on its own ink. A word: centred by the label (symmetric padding),
            // and vertically on the cap height.
            const Ink ink = ink_of(f.small_bold, circle ? first_code_point(k.label) : 'H');
            if (circle) lv_obj_set_x(t, snap(d / 2.0f - ink.cx()));
            lv_obj_set_y(t, snap(d / 2.0f - ink.cy()));
            on_click(pill, k.action);
            lv_obj_set_ext_click_area(pill, gap * 2);
        }
        lv_obj_t* text = label(item, f.small);
        lv_obj_set_style_text_color(text, col(theme_.dim), 0);
        lv_label_set_text(text, h.text.c_str());
    }

    // Narrow screens: drop captions from the end until the hints fit (the glyphs stay).
    lv_obj_update_layout(footer_);
    const auto fits = [&] {
        const uint32_t n = lv_obj_get_child_count(footer_);
        if (n == 0) return true;
        const lv_obj_t* last = lv_obj_get_child(footer_, static_cast<int32_t>(n) - 1);
        return lv_obj_get_x2(last) <= lv_obj_get_width(footer_);  // x2: exclusive, relative to the footer
    };
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_count(footer_)) - 1; i >= 0 && !fits(); --i) {
        lv_obj_t* item = lv_obj_get_child(footer_, i);
        lv_obj_set_hidden(lv_obj_get_child(item, -1), true);  // the caption
        lv_obj_update_layout(footer_);
    }
}

void Ui::dpad_click_cb(lv_event_t* e) {
    auto* self = static_cast<Ui*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const auto first = static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    lv_point_t p{};
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (first == Action::Previous) self->actions_.push_back(p.x < (a.x1 + a.x2) / 2 ? Action::Previous : Action::Next);
    else self->actions_.push_back(p.y < (a.y1 + a.y2) / 2 ? Action::VolumeUp : Action::VolumeDown);
}

void Ui::update(const PlayerStatus& st, const Extras& x) {
    if (b_.width() != built_w_ || b_.height() != built_h_ || !np_) build();

    // Colours follow the server palette, else the artwork: applied when they change. The connection
    // screen keeps the default colours, so the palette (which arrives during its "Connected"
    // confirmation) changes together with Now Playing instead of under the splash.
    const bool waiting = x.hold_waiting;
    if (const Theme theme = waiting ? Theme{} : make_theme(st); !theme_applied_ || theme != theme_) apply_theme(theme);
    const NowPlayingView view = make_view(st);

    // Waiting / connected screen.
    show(idle_, waiting);
    show(np_, !waiting);
    if (waiting) {
        set_text(idle_head_, st.connected ? "Connected to " + short_server_name(st.server_name)
                                          : std::string("Waiting for a Sendspin server"));
        set_text(idle_name_, st.name);
        set_text(idle_addr_, x.address);
        const bool needs_pairing = st.connected && !st.paired && !st.unpaired_access;
        set_color(idle_hint_, needs_pairing ? theme_.fg : theme_.dim);  // something to do: full contrast
        set_text(idle_hint_, needs_pairing
                                 ? "Not paired yet: pair this player in " + short_server_name(st.server_name)
                                 : std::string("Music Assistant and other Sendspin servers find it on your network"));
    } else {
        update_now_playing(st, view, x.pointer);
    }

    if (osd_until_ms_ && SDL_GetTicks64() >= osd_until_ms_) {
        osd_until_ms_ = 0;
        show(osd_, false);
    }
    update_pairing(st);

    show(menu_dim_, page_ != Page::Closed);
    if (page_ != Page::Closed) {
        refresh_page(st, x);
    } else if (built_page_ != Page::Closed) {
        clear_menu_panel();
        row_labels_.clear();
        built_page_ = Page::Closed;
    }
    update_footer(st, view, x);
}

}  // namespace ui
