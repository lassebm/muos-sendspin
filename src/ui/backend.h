#pragma once

#include <SDL.h>

#include "lvgl.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace ui {

/// LVGL on top of our SDL renderer: LVGL renders changed areas into a partial buffer, flush_cb
/// copies them into a streaming SDL texture, and draw() puts that texture on screen. Input comes
/// from the app: keys (keypad device, for menu focus) and pointer presses (mouse / touch screen).
class Backend {
public:
    Backend(SDL_Renderer* renderer, int w, int h);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    /// Queues a key press (LV_KEY_*) for the focused widget's group.
    void key(uint32_t lv_key);
    /// Queues a pointer state in display pixels. Queued, so a tap shorter than a frame still
    /// reaches LVGL as a press and a release.
    void pointer(int x, int y, bool pressed);
    /// Runs LVGL's timers (input, animations, refresh). Returns true if the screen changed.
    bool update();
    /// Copies the frame to the renderer (the caller presents).
    void draw();
    /// Output size changed (window resize).
    void resize(int w, int h);

    [[nodiscard]] lv_group_t* group() const { return group_; }
    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }

private:
    struct PointerState {
        int x, y;
        bool pressed;
    };

    static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px);
    static void keypad_cb(lv_indev_t* indev, lv_indev_data_t* data);
    static void pointer_cb(lv_indev_t* indev, lv_indev_data_t* data);

    SDL_Renderer* renderer_;
    SDL_Texture* texture_ = nullptr;
    int w_, h_;
    std::vector<uint8_t> buf_;
    lv_display_t* display_ = nullptr;
    lv_indev_t* keypad_ = nullptr;
    lv_indev_t* pointer_ = nullptr;
    lv_group_t* group_ = nullptr;
    std::deque<uint32_t> keys_;
    uint32_t pressed_key_ = 0;  // key reported pressed last read; released on the next
    std::deque<PointerState> pointer_queue_;
    PointerState pointer_state_{0, 0, false};
    bool flushed_ = false;
    uint32_t frames_ = 0;  // frames drawn since frames_since_ms_ (debug log)
    uint64_t frames_since_ms_ = 0;
};

}  // namespace ui
