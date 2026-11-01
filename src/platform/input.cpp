#include "platform/input.h"

#include "platform/log.h"

const char* action_name(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::Quit: return "quit";
        case Action::Menu: return "menu";
        case Action::Back: return "back";
        case Action::PlayPause: return "play/pause";
        case Action::Previous: return "previous";
        case Action::Next: return "next";
        case Action::VolumeUp: return "group volume up";
        case Action::VolumeDown: return "group volume down";
        case Action::PlayerVolumeUp: return "player volume up";
        case Action::PlayerVolumeDown: return "player volume down";
        case Action::ToggleShuffle: return "shuffle";
        case Action::CycleRepeat: return "repeat";
        case Action::SeekBack: return "seek back";
        case Action::SeekForward: return "seek forward";
        case Action::ToggleFullscreen: return "fullscreen";
        case Action::LatencyUp: return "latency +";
        case Action::LatencyDown: return "latency -";
    }
    return "?";
}

Input::~Input() {
    for (auto& [id, gc] : controllers_) SDL_GameControllerClose(gc);
}

void Input::open_all() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) open(i);
}

void Input::open(int device_index) {
    if (!SDL_IsGameController(device_index)) {
        logx::info("input: joystick {} ({}) has no gamecontroller mapping", device_index,
                   SDL_JoystickNameForIndex(device_index));
        return;
    }
    SDL_GameController* gc = SDL_GameControllerOpen(device_index);
    if (!gc) {
        logx::warn("input: can't open controller {}: {}", device_index, SDL_GetError());
        return;
    }
    const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    if (controllers_.contains(id)) {
        SDL_GameControllerClose(gc);  // already open (ADDED event for a startup device)
        return;
    }
    controllers_[id] = gc;
    logx::info("input: opened controller '{}'", SDL_GameControllerName(gc));
}

void Input::close(SDL_JoystickID id) {
    if (const auto it = controllers_.find(id); it != controllers_.end()) {
        SDL_GameControllerClose(it->second);
        controllers_.erase(it);
    }
}

Action Input::handle(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: return Action::Quit;

        case SDL_CONTROLLERDEVICEADDED: open(e.cdevice.which); return Action::None;

        case SDL_CONTROLLERDEVICEREMOVED: close(e.cdevice.which); return Action::None;

        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: return Action::Menu;
                case SDLK_BACKSPACE: return Action::Back;
                case SDLK_q: return Action::Quit;
                case SDLK_SPACE:
                case SDLK_k:
                case SDLK_RETURN: return Action::PlayPause;
                case SDLK_LEFT: return Action::Previous;
                case SDLK_RIGHT: return Action::Next;
                case SDLK_UP: return Action::VolumeUp;
                case SDLK_DOWN: return Action::VolumeDown;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                case SDLK_KP_PLUS: return Action::PlayerVolumeUp;
                case SDLK_MINUS:
                case SDLK_KP_MINUS: return Action::PlayerVolumeDown;
                case SDLK_s: return Action::ToggleShuffle;
                case SDLK_r: return Action::CycleRepeat;
                case SDLK_j: return Action::SeekBack;
                case SDLK_l: return Action::SeekForward;
                case SDLK_f: return Action::ToggleFullscreen;
                case SDLK_RIGHTBRACKET:
                case SDLK_PERIOD: return Action::LatencyUp;
                case SDLK_LEFTBRACKET:
                case SDLK_COMMA: return Action::LatencyDown;
                default: return Action::None;
            }

        case SDL_CONTROLLERBUTTONDOWN: {
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: return Action::PlayPause;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Action::Previous;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Action::Next;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: return Action::VolumeUp;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Action::VolumeDown;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Action::PlayerVolumeUp;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Action::PlayerVolumeDown;
                case SDL_CONTROLLER_BUTTON_X: return Action::ToggleShuffle;
                case SDL_CONTROLLER_BUTTON_Y: return Action::CycleRepeat;
                case SDL_CONTROLLER_BUTTON_GUIDE: return Action::Menu;  // the Menu button
                case SDL_CONTROLLER_BUTTON_B: return Action::Back;
                case SDL_CONTROLLER_BUTTON_BACK: return Action::Menu;  // Select: muOS apps use it for options too
                default: return Action::None;
            }
        }

        case SDL_CONTROLLERAXISMOTION: {
            // Triggers report 0..32767; treat > half as pressed, < a quarter as released.
            const auto edge = [&](bool& state, Action a) {
                const int v = e.caxis.value;
                if (!state && v > 16384) {
                    state = true;
                    return a;
                }
                if (state && v < 8192) state = false;
                return Action::None;
            };
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return edge(trigger_left_, Action::SeekBack);
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return edge(trigger_right_, Action::SeekForward);
            return Action::None;
        }

        default: return Action::None;
    }
}
