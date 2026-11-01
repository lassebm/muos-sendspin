#include "ui/backend.h"

#include "platform/log.h"

namespace ui {

Backend::Backend(SDL_Renderer* renderer, int w, int h) : renderer_(renderer), w_(w), h_(h) {
    if (!lv_is_initialized()) {
        lv_init();
        lv_tick_set_cb([] { return SDL_GetTicks(); });
    }
    display_ = lv_display_create(w, h);  // XRGB8888: LV_COLOR_FORMAT_DEFAULT (lv_conf.h)
    lv_display_set_driver_data(display_, this);
    lv_display_set_flush_cb(display_, &Backend::flush_cb);
    resize(w, h);

    // Not the default group: widgets join it explicitly (menu rows, the on-screen keyboard). As
    // the default, text areas would join it too, and a tap on one would take the keys' focus.
    group_ = lv_group_create();
    keypad_ = lv_indev_create();
    lv_indev_set_type(keypad_, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad_, &Backend::keypad_cb);
    lv_indev_set_driver_data(keypad_, this);
    lv_indev_set_group(keypad_, group_);

    pointer_ = lv_indev_create();
    lv_indev_set_type(pointer_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer_, &Backend::pointer_cb);
    lv_indev_set_driver_data(pointer_, this);
}

Backend::~Backend() {
    if (pointer_) lv_indev_delete(pointer_);
    if (keypad_) lv_indev_delete(keypad_);
    if (group_) lv_group_delete(group_);
    if (display_) lv_display_delete(display_);
    if (texture_) SDL_DestroyTexture(texture_);
}

void Backend::resize(int w, int h) {
    w_ = w;
    h_ = h;
    if (texture_) SDL_DestroyTexture(texture_);
    // XRGB8888 in LVGL is the 32-bit word 0xXXRRGGBB in native (little-endian) order, which is
    // SDL's ARGB8888; the X byte is ignored by rendering with no blending.
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
    // Partial rendering: LVGL redraws only invalidated areas, in chunks of up to a quarter screen.
    buf_.assign(static_cast<size_t>(w) * static_cast<size_t>(h / 4 + 1) * 4, 0);
    lv_display_set_resolution(display_, w, h);
    lv_display_set_buffers(display_, buf_.data(), nullptr, static_cast<uint32_t>(buf_.size()),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    flushed_ = true;
}

void Backend::flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    auto* self = static_cast<Backend*>(lv_display_get_driver_data(disp));
    const SDL_Rect r{area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area)};
    SDL_UpdateTexture(self->texture_, &r, px, r.w * 4);
    self->flushed_ = true;
    lv_display_flush_ready(disp);
}

void Backend::keypad_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<Backend*>(lv_indev_get_driver_data(indev));
    if (self->pressed_key_) {  // release the key pressed last time
        data->key = self->pressed_key_;
        data->state = LV_INDEV_STATE_RELEASED;
        self->pressed_key_ = 0;
    } else if (!self->keys_.empty()) {
        self->pressed_key_ = self->keys_.front();
        self->keys_.pop_front();
        data->key = self->pressed_key_;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->continue_reading = self->pressed_key_ != 0 || !self->keys_.empty();
}

void Backend::pointer_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<Backend*>(lv_indev_get_driver_data(indev));
    if (!self->pointer_queue_.empty()) {
        self->pointer_state_ = self->pointer_queue_.front();
        self->pointer_queue_.pop_front();
    }
    data->point = {self->pointer_state_.x, self->pointer_state_.y};
    data->state = self->pointer_state_.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !self->pointer_queue_.empty();
}

void Backend::key(uint32_t lv_key) {
    keys_.push_back(lv_key);
}

void Backend::pointer(int x, int y, bool pressed) {
    pointer_queue_.push_back({x, y, pressed});
}

bool Backend::update() {
    lv_timer_handler();
    const bool changed = flushed_;
    flushed_ = false;
    if (changed) ++frames_;
    const uint64_t now = SDL_GetTicks64();
    if (now - frames_since_ms_ >= 10000) {  // a static screen should draw ~nothing
        logx::debug(SDL_LOG_CATEGORY_RENDER, "lvgl: {} frames in {:.1f} s", frames_,
                    static_cast<double>(now - frames_since_ms_) / 1000.0);
        frames_ = 0;
        frames_since_ms_ = now;
    }
    return changed;
}

void Backend::draw() {
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
}

}  // namespace ui
