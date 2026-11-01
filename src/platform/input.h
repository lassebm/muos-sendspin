#pragma once

#include <SDL.h>

#include <unordered_map>

/// High-level actions, independent of keyboard vs gamepad.
enum class Action {
    None,
    Quit,  // immediate (window close, q on the keyboard)
    Menu,  // Menu button / Esc: open or close the menu
    Back,  // B / Backspace
    PlayPause,
    Previous,
    Next,
    VolumeUp,  // group volume (controller role), as a remote would
    VolumeDown,
    PlayerVolumeUp,  // this player's own volume
    PlayerVolumeDown,
    ToggleShuffle,
    CycleRepeat,
    SeekBack,
    SeekForward,
    ToggleFullscreen,
    LatencyUp,  // dev tuning: report more output latency (plays earlier)
    LatencyDown,
};

[[nodiscard]] const char* action_name(Action a);

/// Maps SDL events to actions. Uses the GameController API so muOS's
/// SDL_GAMECONTROLLERCONFIG (the user's A/B layout and remaps) is respected.
class Input {
public:
    Input() = default;
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    /// Opens controllers already attached at startup.
    void open_all();

    /// Translates one event. Controller hotplug is handled internally.
    Action handle(const SDL_Event& e);

    [[nodiscard]] bool has_controller() const { return !controllers_.empty(); }

private:
    void open(int device_index);
    void close(SDL_JoystickID id);

    std::unordered_map<SDL_JoystickID, SDL_GameController*> controllers_;
    bool trigger_left_ = false, trigger_right_ = false;  // L2/R2 are axes: edge-detect presses
};
