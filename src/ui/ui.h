#pragma once

#include "platform/input.h"
#include "player/sendspin_service.h"
#include "ui/backend.h"
#include "ui/glyphs.h"
#include "ui/layout.h"
#include "ui/view.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui {

/// What the screens show besides PlayerStatus.
struct Extras {
    std::string address;
    std::string renderer_info;
    std::string audio_driver;
    bool demo = false;
    bool has_controller = false;  // gamepad: gamepad hints and help, else keyboard
    bool hold_waiting = false;    // keep the waiting/connected screen up (the app decides timing)
    bool pointer = false;         // a touch screen or mouse: show on-screen previous/next
};

/// The user interface, built from LVGL widgets. Widgets are created once per resolution and only
/// updated when their content changes, so LVGL redraws only what changed.
class Ui {
public:
    Ui(Backend& backend, const std::filesystem::path& regular_ttf, const std::filesystem::path& bold_ttf);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void update(const PlayerStatus& st, const Extras& x);

    void show_volume(std::string_view lbl, int value);

    [[nodiscard]] bool menu_open() const { return page_ != Page::Closed; }
    void toggle_menu();
    /// Opens a menu page by name (main, help, settings, sync, pairing, name, info, quit): --menu for
    /// screenshots.
    [[nodiscard]] bool open_menu_page(std::string_view name);
    /// Handles an action while the menu is open. Returns true when the user confirmed Quit.
    [[nodiscard]] bool menu_action(Action a, SendspinService& service, const PlayerStatus& st);
    /// Actions from taps and clicks (transport icons, menu rows, footer hints) since the last call.
    [[nodiscard]] std::vector<Action> take_actions();

    /// The player-name editor is open: a physical keyboard types into it.
    [[nodiscard]] bool text_entry() const { return page_ == Page::Name; }
    /// Typed text (SDL_TEXTINPUT) for the editor.
    void type_text(std::string_view utf8);
    /// Editing keys of a physical keyboard while the editor is open. Returns false for keys
    /// that should go through the normal mapping (arrows move over the on-screen keys).
    [[nodiscard]] bool text_key(SDL_Keycode key);
    /// The name the user saved in the editor, once.
    [[nodiscard]] std::optional<std::string> take_name();

private:
    enum class Page { Closed, Main, Help, Settings, Sync, Pairing, Forget, Name, Info, Quit, Count };
    enum class SettingsRow { Screen, SyncOffset, AudioFormat, Pairing, Name, Count };

    /// What the user has done in the name editor, carried over when the page is rebuilt under them.
    struct NameDraft {
        std::string text;
        uint32_t cursor = 0;
        lv_keyboard_mode_t mode = LV_KEYBOARD_MODE_TEXT_LOWER;
        uint32_t key = 0;  // the selected key
    };

    /// One footer hint: button pills (each tappable, with its own action) and a caption.
    struct Hint {
        struct Key {
            std::string label;
            Action action;
            bool operator==(const Key&) const = default;
        };
        std::vector<Key> keys;
        std::string text;
        bool operator==(const Hint&) const = default;
    };

    struct Fonts;
    void build();  // (re)creates every widget for the current resolution
    void apply_theme(const Theme& theme);
    void update_now_playing(const PlayerStatus& st, const NowPlayingView& v, bool pointer);
    void place_transport();  // icons, state dots and the repeat-one mark, centred on their ink
    void update_artwork(const PlayerStatus& st);
    void update_pairing(const PlayerStatus& st);
    void update_footer(const PlayerStatus& st, const NowPlayingView& v, const Extras& x);
    /// back: returning to a parent page restores its focused row; going deeper starts at the top.
    void open_page(Page p, bool back = false);
    void refresh_page(const PlayerStatus& st, const Extras& x);
    void clear_menu_panel();  // removes the page's widgets
    void build_name_editor(const PlayerStatus& st, const std::optional<NameDraft>& draft);
    [[nodiscard]] int focused_row() const;
    /// Sets a label's text when it differs from what we last set. Compares against our copy, not
    /// the label's: in "…" mode LVGL rewrites the label's own text.
    void set_text(lv_obj_t* lbl, std::string_view s);
    /// Makes obj tappable: a click queues the action.
    void on_click(lv_obj_t* obj, Action a);
    static void click_cb(lv_event_t* e);
    static void dpad_click_cb(lv_event_t* e);  // left/right or top/bottom half: the matching direction

    Backend& b_;
    std::unique_ptr<Fonts> fonts_;
    Theme theme_;
    bool theme_applied_ = false;
    int built_w_ = 0, built_h_ = 0;
    Layout layout_;  // for built_w_ x built_h_
    std::vector<Action> actions_;
    std::unordered_map<lv_obj_t*, std::string> texts_;  // what set_text last set, per label

    // Now Playing
    lv_obj_t* np_ = nullptr;
    lv_obj_t* art_ = nullptr;
    lv_obj_t* art_placeholder_ = nullptr;
    lv_obj_t *title_ = nullptr, *artist_ = nullptr, *album_ = nullptr, *group_ = nullptr;
    lv_obj_t *shuffle_ = nullptr, *prev_ = nullptr, *play_ = nullptr, *next_ = nullptr, *repeat_ = nullptr;
    lv_obj_t *shuffle_dot_ = nullptr, *repeat_dot_ = nullptr, *repeat_one_ = nullptr;
    lv_obj_t *bar_ = nullptr, *pos_ = nullptr, *dur_ = nullptr;
    int bar_px_ = 0;  // progress bar range = its width in pixels: it only redraws when a pixel changes
    float transport_cx_ = 0, transport_cy_ = 0, transport_step_ = 0;
    float dot_px_ = 0;              // state dot diameter, the same for both dots
    std::array<int, 2> dot_box_{};  // shuffle, repeat: image box of each dot (its icon's parity)
    std::array<std::unique_ptr<ArgbImage>, 2> dot_images_;
    uint32_t art_version_ = UINT32_MAX;
    std::unique_ptr<ArgbImage> art_image_;
    uint64_t marquee_until_ms_ = 0;  // long texts scroll until then, then show "…"

    // Footer
    lv_obj_t* footer_ = nullptr;
    std::vector<Hint> hints_;
    std::vector<std::unique_ptr<ArgbImage>> footer_glyphs_;  // pixels of the footer's glyph images

    // Waiting / connected
    lv_obj_t* idle_ = nullptr;
    lv_obj_t *idle_head_ = nullptr, *idle_name_ = nullptr, *idle_addr_ = nullptr, *idle_hint_ = nullptr;

    // Overlays
    lv_obj_t* osd_ = nullptr;
    lv_obj_t *osd_label_ = nullptr, *osd_value_ = nullptr, *osd_bar_ = nullptr;
    uint64_t osd_until_ms_ = 0;  // SDL_GetTicks64() time
    lv_obj_t* pair_ = nullptr;
    lv_obj_t *pair_head_ = nullptr, *pair_code_ = nullptr, *pair_hint_ = nullptr;
    lv_obj_t* pair_qr_ = nullptr;  // the pairing code as a QR code, when the server asked for one
    std::string pair_qr_data_;     // what pair_qr_ encodes

    // Menu
    Page page_ = Page::Closed;
    Page built_page_ = Page::Closed;
    bool restyle_ = false;  // the theme changed: rebuild the open page in its colours, keeping its state
    lv_obj_t* menu_dim_ = nullptr;
    lv_obj_t* menu_panel_ = nullptr;
    std::vector<lv_obj_t*> rows_;          // menu rows
    std::vector<lv_obj_t*> row_values_;    // right-hand value labels, parallel to rows_
    std::vector<std::string> row_labels_;  // the rows' labels as built: the page rebuilds when they change
    lv_obj_t* menu_qr_ = nullptr;          // Pairing page: this player's pairing token
    std::string forget_id_, forget_name_;  // Forget page: the server chosen on the Pairing page
    lv_obj_t* name_ta_ = nullptr;          // Name page: the text field
    lv_obj_t* name_kb_ = nullptr;          // Name page: the on-screen keyboard
    std::optional<std::string> name_saved_;
    static void name_ready_cb(lv_event_t* e);
    bool rows_selectable_ = false;
    lv_obj_t* sync_big_ = nullptr;
    lv_obj_t* sync_chart_ = nullptr;
    lv_obj_t* sync_caption_ = nullptr;
    lv_chart_series_t* sync_series_ = nullptr;
    size_t sync_points_ = 0;
    float sync_last_ = 0;
    std::array<int, static_cast<size_t>(Page::Count)> focus_{};  // remembered focused row per page
    bool restore_focus_ = false;
};

}  // namespace ui
