#include "app/app.h"

#include "net/netinfo.h"
#include "platform/alsa.h"
#include "platform/input.h"
#include "platform/log.h"
#include "platform/muos.h"
#include "platform/paths.h"
#include "player/sendspin_service.h"
#include "ui/backend.h"
#include "ui/layout.h"
#include "ui/ui.h"
#include "version.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace app {

namespace {

#ifdef MUOS_DEVICE
// Exclusive fullscreen at the display's mode, as always on the device: muOS's patched SDL
// (rotation) creates a second renderer with desktop fullscreen.
constexpr Uint32 kFullscreen = SDL_WINDOW_FULLSCREEN;
#else
// Fullscreen at the desktop's own mode: an exclusive one would drop macOS to a non-Retina mode.
constexpr Uint32 kFullscreen = SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif

// Signals: SIGINT/SIGTERM quit cleanly, SIGHUP saves a screenshot of the next frame (remote
// testing), SIGUSR1/SIGUSR2 tune latency by +/-10 ms (scripted sync tests). Lock-free atomics: the
// handlers may run on any thread, and must not lose a step the main loop is taking at that moment.
std::atomic<bool> g_quit{false};
std::atomic<bool> g_screenshot{false};
std::atomic<int> g_latency_steps{0};
static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<int>::is_always_lock_free,
              "signal handlers may only use lock-free atomics");

void install_signal_handlers() {
    std::signal(SIGINT, [](int) { g_quit.store(true); });
    std::signal(SIGTERM, [](int) { g_quit.store(true); });
    std::signal(SIGHUP, [](int) { g_screenshot.store(true); });
    std::signal(SIGUSR1, [](int) { g_latency_steps.fetch_add(1); });
    std::signal(SIGUSR2, [](int) { g_latency_steps.fetch_sub(1); });
}

struct SdlDeleter {
    void operator()(SDL_Window* w) const { SDL_DestroyWindow(w); }
    void operator()(SDL_Renderer* r) const { SDL_DestroyRenderer(r); }
    void operator()(SDL_Surface* s) const { SDL_FreeSurface(s); }
};
using WindowPtr = std::unique_ptr<SDL_Window, SdlDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, SdlDeleter>;

/// SDL_Init / SDL_Quit.
struct SdlSession {
    bool ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0;
    SdlSession() = default;
    ~SdlSession() { SDL_Quit(); }
    SdlSession(const SdlSession&) = delete;
    SdlSession& operator=(const SdlSession&) = delete;
};

bool save_screenshot(SDL_Renderer* r, int w, int h, const std::filesystem::path& path) {
    const std::unique_ptr<SDL_Surface, SdlDeleter> s(
        SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888));
    const bool ok = s && SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
                    SDL_SaveBMP(s.get(), path.c_str()) == 0;
    if (ok) logx::info("saved {}", path.string());
    else logx::error("screenshot: {}", SDL_GetError());
    return ok;
}

// --- demo: mock content for layout work without a server -----------------------------------------

struct DemoTrack {
    std::string_view title, artist, album;
    uint32_t duration_ms;
};
constexpr std::array kDemoTracks{
    // The first track is the one shown with --demo-artwork (the README screenshots use Big Buck
    // Bunny's poster, CC BY 3.0 Blender Foundation).
    DemoTrack{"Main Theme", "Jan Morgenstern", "Big Buck Bunny", 305000},
    DemoTrack{"An Exceptionally Long Track Title That Will Not Fit On A Small Handheld Screen",
              "Some Artist feat. Another Artist & Friends", "Deluxe Edition (Remastered 2026)", 412000},
    DemoTrack{"I Move On", "Jan Morgenstern", "Sintel: Original Film Score", 246000},
};

class Demo {
public:
    Demo(int track, std::string_view pairing, const std::filesystem::path& artwork) {
        if (!artwork.empty()) {
            std::ifstream in(artwork, std::ios::binary);
            const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
            art_ = decode_image(data);
            if (!art_) logx::error("demo: can't read artwork {}", artwork.string());
        }
        st_.started = st_.connected = st_.paired = true;
        st_.name = "demo";
        st_.address = ipv4_text(local_ipv4());
        st_.device_name = "Anbernic RG35-H";
        st_.server_name = "Music Assistant";
        st_.server_id = "demo-music-assistant";
        st_.paired_servers = {{"demo-music-assistant", "Music Assistant"}, {"demo-other", ""}};
        // Shaped like a real token ("SP:" + base32, 107 characters), so the QR codes have the real size.
        st_.pairing_token = "SP:";
        for (int i = 0; st_.pairing_token.size() < 107; ++i)
            st_.pairing_token += "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"[(i * 7 + 3) % 32];
        if (pairing == "digits") st_.pairing_code = "042735";
        if (pairing == "qr") {
            st_.pairing_code = st_.pairing_token;
            st_.pairing_code_qr = true;
        }
        st_.group_name = "Living Room";
        st_.playing = st_.has_controller = true;
        st_.can_play = st_.can_pause = st_.can_next = st_.can_previous = true;
        st_.can_shuffle = st_.can_repeat = st_.can_volume = true;
        st_.shuffle = true;
        st_.repeat = Repeat::One;
        st_.volume = st_.group_volume = 60;
        st_.stream = "flac 44100 Hz 2 ch";
        st_.audio_format = st_.audio_format_next = "flac";
        load(static_cast<size_t>(track));
    }

    [[nodiscard]] const PlayerStatus& status() const { return st_; }

    /// Applies a transport or volume action; returns the volume to show, if it changed.
    std::optional<std::pair<std::string_view, int>> handle(Action a) {
        const auto step = [](uint8_t& v, int d) { v = static_cast<uint8_t>(std::clamp(v + d, 0, 100)); };
        switch (a) {
            case Action::PlayPause: st_.playing = !st_.playing; break;
            case Action::Next: load(track_ + 1); break;
            case Action::Previous:
                if (st_.progress_ms > 3000) st_.progress_ms = 0;
                else load(track_ + kDemoTracks.size() - 1);
                break;
            case Action::ToggleShuffle: st_.shuffle = !st_.shuffle; break;
            case Action::CycleRepeat:
                st_.repeat = st_.repeat == Repeat::Off   ? Repeat::All
                             : st_.repeat == Repeat::All ? Repeat::One
                                                         : Repeat::Off;
                break;
            case Action::VolumeUp:
            case Action::VolumeDown:
                step(st_.group_volume, a == Action::VolumeUp ? 5 : -5);
                return std::pair{std::string_view("Group volume"), int{st_.group_volume}};
            case Action::PlayerVolumeUp:
            case Action::PlayerVolumeDown:
                step(st_.volume, a == Action::PlayerVolumeUp ? 5 : -5);
                return std::pair{std::string_view("Player volume"), int{st_.volume}};
            default: break;
        }
        return std::nullopt;
    }

    void tick(uint64_t now_ms) {
        const uint64_t dt = last_ms_ ? now_ms - last_ms_ : 0;
        last_ms_ = now_ms;
        if (!st_.playing) return;
        st_.progress_ms += static_cast<uint32_t>(dt);
        if (st_.progress_ms >= st_.duration_ms) load(track_ + 1);
    }

private:
    void load(size_t i) {
        track_ = i % kDemoTracks.size();
        const DemoTrack& t = kDemoTracks[track_];
        st_.title = t.title;
        st_.artist = t.artist;
        st_.album = t.album;
        st_.duration_ms = t.duration_ms;
        st_.progress_ms = t.duration_ms / 3;  // mid-track, as a screenshot would catch it
        st_.artwork = track_ == 0 ? art_ : nullptr;
        ++st_.artwork_version;
    }
    PlayerStatus st_;
    std::shared_ptr<const Image> art_;
    size_t track_ = 0;
    uint64_t last_ms_ = 0;
};

// --- policies ------------------------------------------------------------------------------------

/// When to show the volume overlay. Group and player volume move together (the server rebalances
/// one when the other changes), so only the kind the user is adjusting is shown. Changes made
/// elsewhere show too, once the connection has settled: never for the values a (re)connection
/// starts with (the server sends a placeholder, then the real volume), nor those of joining or
/// leaving a group.
class VolumeOsdPolicy {
public:
    enum class Kind { Group, Player };

    /// The user pressed a volume key.
    void local(Kind k, uint64_t now) {
        focus_ = k;
        focus_until_ms_ = now + 2000;
    }

    /// What to show for this frame's status, if anything.
    std::optional<std::pair<std::string_view, int>> update(const PlayerStatus& st, uint64_t now) {
        if (!st.connected || st.group_name != group_) {
            group_ = st.group_name;
            settled_ms_ = 0;
            last_group_ = last_player_ = -1;
        }
        if (!st.connected) return std::nullopt;
        if (!settled_ms_) settled_ms_ = now + 3000;
        const bool local = now < focus_until_ms_;
        const bool may_show = local || now >= settled_ms_;
        const Kind wanted = local ? focus_ : st.has_controller ? Kind::Group : Kind::Player;

        std::optional<std::pair<std::string_view, int>> shown;
        if (st.has_controller && st.group_volume != last_group_) {
            if (last_group_ >= 0 && may_show && wanted == Kind::Group) shown = {"Group volume", st.group_volume};
            last_group_ = st.group_volume;
        }
        if (st.volume != last_player_) {
            if (last_player_ >= 0 && may_show && wanted == Kind::Player) shown = {"Player volume", st.volume};
            last_player_ = st.volume;
        }
        return shown;
    }

private:
    Kind focus_ = Kind::Group;
    uint64_t focus_until_ms_ = 0;
    uint64_t settled_ms_ = 0;
    std::string group_;
    int last_group_ = -1, last_player_ = -1;
};

/// The waiting screen stays up at least 3 s once shown (start, reconnect), and confirms a new
/// connection for 1.5 s, so it's readable instead of a flash before Now Playing.
class ConnectionScreenPolicy {
public:
    bool hold(bool connected, uint64_t now) {
        if (!connected) {
            if (!waiting_since_ms_) waiting_since_ms_ = now;
            connected_since_ms_ = 0;
        } else if (waiting_since_ms_ && !connected_since_ms_) {
            connected_since_ms_ = now;
        }
        const bool held =
            waiting_since_ms_ && (!connected || now < waiting_since_ms_ + 3000 || now < connected_since_ms_ + 1500);
        if (connected && !held) waiting_since_ms_ = connected_since_ms_ = 0;
        return held;
    }

private:
    uint64_t waiting_since_ms_ = 0, connected_since_ms_ = 0;
};

// --- the app -------------------------------------------------------------------------------------

class App {
public:
    App(Options opt, SDL_Window* window, SDL_Renderer* renderer)
        : opt_(std::move(opt)),
          window_(window),
          renderer_(renderer),
          demo_(opt_.demo_track, opt_.demo_pairing, opt_.demo_artwork) {}

    /// extras: the info page's renderer and audio driver; the rest is filled in here.
    int run(ui::Extras extras);

private:
    void dispatch(Action a);
    void handle_event(const SDL_Event& e);
    [[nodiscard]] std::pair<int, int> output_size() const;
    [[nodiscard]] const PlayerStatus& status() const { return opt_.demo ? demo_.status() : service_->status(); }

    Options opt_;
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<SendspinService> service_;
    std::unique_ptr<ui::Backend> backend_;
    std::unique_ptr<ui::Ui> ui_;
    Input input_;
    Demo demo_;
    VolumeOsdPolicy volume_osd_;
    ConnectionScreenPolicy connection_screen_;
    bool running_ = true;
    // A touch screen or mouse is there: at start, a touch device or a desktop (which has a cursor);
    // later, any real pointer event (a mouse plugged into the handheld, say).
#ifdef MUOS_DEVICE
    bool pointer_ = SDL_GetNumTouchDevices() > 0;
#else
    bool pointer_ = true;
#endif
};

std::pair<int, int> App::output_size() const {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer_, &w, &h);
    return {w, h};
}

void App::dispatch(Action a) {
    logx::debug(SDL_LOG_CATEGORY_INPUT, "action: {}", action_name(a));
    const PlayerStatus& st = status();
    if (a == Action::Quit) {
        running_ = false;
    } else if (a == Action::Menu) {
        ui_->toggle_menu();
    } else if (ui_->menu_open()) {
        if (ui_->menu_action(a, *service_, st)) running_ = false;
    } else if (a == Action::ToggleFullscreen) {
        const bool fs = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN;
        SDL_SetWindowFullscreen(window_, fs ? 0 : kFullscreen);
    } else if (opt_.demo) {
        if (const auto shown = demo_.handle(a)) ui_->show_volume(shown->first, shown->second);
    } else if (a == Action::PlayPause && st.pairing_window_prompt) {
        service_->confirm_pairing_window();
    } else {
        // Remember which volume the user is changing, so the overlay doesn't flip to the other
        // one when the server rebalances group and member volumes in response.
        const uint64_t now = SDL_GetTicks64();
        SendspinService& s = *service_;
        switch (a) {
            case Action::PlayPause: s.play_pause(); break;
            case Action::Next: s.next(); break;
            case Action::Previous: s.previous(); break;
            case Action::VolumeUp:
            case Action::VolumeDown:
                volume_osd_.local(VolumeOsdPolicy::Kind::Group, now);
                s.change_group_volume(a == Action::VolumeUp ? +5 : -5);
                break;
            case Action::PlayerVolumeUp:
            case Action::PlayerVolumeDown:
                volume_osd_.local(VolumeOsdPolicy::Kind::Player, now);
                s.change_volume(a == Action::PlayerVolumeUp ? +5 : -5);
                break;
            case Action::ToggleShuffle: s.toggle_shuffle(); break;
            case Action::CycleRepeat: s.cycle_repeat(); break;
            case Action::SeekBack: s.seek_relative(-10000); break;
            case Action::SeekForward: s.seek_relative(+10000); break;
            case Action::LatencyUp: s.change_latency(+5); break;
            case Action::LatencyDown: s.change_latency(-5); break;
            default: break;
        }
    }
}

void App::handle_event(const SDL_Event& e) {
    // The name editor takes a physical keyboard's typing; gamepad buttons and arrows go on below.
    if (ui_->text_entry()) {
        if (e.type == SDL_TEXTINPUT) {
            ui_->type_text(e.text.text);
            return;
        }
        if (e.type == SDL_KEYDOWN && ui_->text_key(e.key.keysym.sym)) return;
    }
    // Pointer input (mouse, touch screen) goes to LVGL, in output pixels. Touches also arrive as
    // synthesized mouse events; those are skipped in favour of the finger events.
    const auto [ow, oh] = output_size();
    switch (e.type) {
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION: {
            if (e.button.which == SDL_TOUCH_MOUSEID ||
                (e.type == SDL_MOUSEMOTION && !(e.motion.state & SDL_BUTTON_LMASK)))
                break;
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window_, &ww, &wh);
            const int x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
            const int y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            const bool pressed = e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN;
            if (e.type != SDL_MOUSEMOTION && e.button.button != SDL_BUTTON_LEFT) break;
            backend_->pointer(ww ? x * ow / ww : x, wh ? y * oh / wh : y, pressed);
            pointer_ = true;
            break;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION:
            backend_->pointer(static_cast<int>(e.tfinger.x * static_cast<float>(ow)),
                              static_cast<int>(e.tfinger.y * static_cast<float>(oh)), e.type != SDL_FINGERUP);
            pointer_ = true;
            break;
        default:
            if (const Action a = input_.handle(e); a != Action::None) dispatch(a);
    }
}

int App::run(ui::Extras extras) {
    const auto [ow, oh] = output_size();
    backend_ = std::make_unique<ui::Backend>(renderer_, ow, oh);
    ui_ = std::make_unique<ui::Ui>(*backend_, paths::asset("fonts/NotoSans-Regular.ttf"),
                                   paths::asset("fonts/NotoSans-Bold.ttf"), paths::asset("fonts/Symbols.ttf"));
    if (!opt_.menu.empty() && !ui_->open_menu_page(opt_.menu)) {
        logx::error("unknown --menu page: {}", opt_.menu);
        return 2;
    }
    input_.open_all();
    logx::info("input: {} touch device(s){}", SDL_GetNumTouchDevices(), pointer_ ? ", pointer available" : "");

    // Ask the server for artwork at the size we draw it.
    opt_.service.artwork_px = std::max(64, ui::compute_layout(ow, oh).artwork.w);
    if (opt_.service.state_file.empty()) opt_.service.state_file = paths::default_state_file();
    service_ = std::make_unique<SendspinService>(opt_.service);
    if (!opt_.demo && !service_->start()) return 1;

    extras.demo = opt_.demo;

    // Like Wasabi in the foreground: while playing, no idle sleep, and (unless the user allows the
    // idle screensaver) no display idle, so idle mute never hits playback.
    muos::IdleInhibit sleep_inhibit(muos::IdleInhibit::Kind::Sleep);
    muos::IdleInhibit display_inhibit(muos::IdleInhibit::Kind::Game);

    int frame = 0;
    while (running_) {
        const uint64_t frame_start = SDL_GetTicks64();
        for (SDL_Event e; SDL_PollEvent(&e);) handle_event(e);
        for (const Action a : ui_->take_actions()) dispatch(a);  // taps from the previous pass
        if (auto name = ui_->take_name(); name && !opt_.demo) service_->set_name(*name);
        if (g_quit.load()) running_ = false;
        if (const int steps = g_latency_steps.exchange(0); steps != 0 && !opt_.demo)
            service_->change_latency(10 * steps);

        // The service runs every pass, drawn or not: it delivers the SDK's callbacks.
        const uint64_t now = SDL_GetTicks64();
        if (opt_.demo) {
            demo_.tick(now);
        } else {
            service_->loop();
            const PlayerStatus& st = service_->status();
            const bool playing = st.connected && st.playing && !st.stream.empty();
            sleep_inhibit.set(playing);
            display_inhibit.set(playing && !st.idle_screensaver);
            if (const auto shown = volume_osd_.update(st, now)) ui_->show_volume(shown->first, shown->second);
        }
        const PlayerStatus& st = status();
        extras.address = st.address.empty() ? "no network" : std::format("{}:{}", st.address, opt_.service.port);
        extras.has_controller = opt_.gamepad_ui || input_.has_controller();
        extras.pointer = !opt_.gamepad_ui && pointer_;
        // An unpaired server can't play here (unless unpaired access is on): until it pairs, the
        // connection screen stays, saying so, rather than an empty Now Playing.
        const bool usable = st.connected && (st.paired || st.unpaired_access);
        extras.hold_waiting = !opt_.demo && connection_screen_.hold(usable, now);
        ui_->update(st, extras);

        const auto [w, h] = output_size();
        if (w != backend_->width() || h != backend_->height()) backend_->resize(w, h);
        const bool last_frame = opt_.frames > 0 && frame + 1 >= opt_.frames;
        if (!backend_->update() && !g_screenshot.load() && opt_.frames == 0) {
            SDL_Delay(10);  // nothing changed: LVGL draws only what's invalidated
            continue;
        }
        backend_->draw();
        if (g_screenshot.exchange(false)) {
            save_screenshot(
                renderer_, w, h,
                opt_.screenshot.empty() ? opt_.service.state_file.parent_path() / "screenshot.bmp" : opt_.screenshot);
        }
        if (last_frame) {
            if (!opt_.screenshot.empty()) save_screenshot(renderer_, w, h, opt_.screenshot);
            running_ = false;
        }
        ++frame;
        SDL_RenderPresent(renderer_);
        // vsync normally paces us; cap at ~100 fps when it doesn't (minimized window, software
        // renderer), so the service and LVGL don't spin a core.
        if (const uint64_t spent = SDL_GetTicks64() - frame_start; spent < 10)
            SDL_Delay(static_cast<Uint32>(10 - spent));
    }
    logx::info("quitting");  // a log ending without this line means the app didn't exit normally
    service_->stop();
    ui_.reset();
    backend_.reset();
    return 0;
}

}  // namespace

int run(Options opt) {
    // Before SDL starts its ALSA device polling, and before the log thread starts: setenv must not
    // race its localtime_r reading TZ.
    alsa::use_lean_config();
    logx::capture_output(opt.log_file);
    install_signal_handlers();

    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    SDL_LogSetPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_INFO);
    // --log-level debug or verbose also turns on our INPUT/RENDER debug lines.
    const SDL_LogPriority ours =
        opt.log_level >= sendspin::LogLevel::DEBUG ? SDL_LOG_PRIORITY_DEBUG : SDL_LOG_PRIORITY_INFO;
    for (const int category : {SDL_LOG_CATEGORY_INPUT, SDL_LOG_CATEGORY_RENDER}) SDL_LogSetPriority(category, ours);
    sendspin::SendspinClient::set_log_level(opt.log_level);

    if (opt.no_focus) {  // must precede SDL_Init: macOS activates the whole app at video init
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        SDL_SetHint("SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "0");  // SDL3 (sdl2-compat on macOS)
    }
    const SdlSession sdl;
    if (!sdl.ok) {
        std::println(stderr, "SDL_Init: {}", SDL_GetError());
        return 1;
    }
    alsa::verify_lean_config();
    SDL_version linked;
    SDL_GetVersion(&linked);
    const char* const audio_driver = SDL_GetCurrentAudioDriver();
    ui::Extras extras;
    extras.audio_driver = audio_driver ? audio_driver : "none";
    logx::info("muos-sendspin {}, SDL {}.{}.{}, video: {}, audio: {}", MUOS_SENDSPIN_VERSION, linked.major,
               linked.minor, linked.patch, SDL_GetCurrentVideoDriver(), extras.audio_driver);

    Uint32 wflags = opt.hidpi ? SDL_WINDOW_ALLOW_HIGHDPI : 0;
    int ww = opt.width, wh = opt.height;
    if (opt.fullscreen) {
        if (SDL_DisplayMode mode; SDL_GetDesktopDisplayMode(0, &mode) == 0) {
            ww = mode.w;
            wh = mode.h;
        }
        wflags |= kFullscreen;
    } else {
        wflags |= SDL_WINDOW_RESIZABLE;
    }
    const WindowPtr window(
        SDL_CreateWindow("Sendspin", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags));
    if (!window) {
        std::println(stderr, "SDL_CreateWindow: {}", SDL_GetError());
        return 1;
    }

    RendererPtr renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
    if (!renderer) {
        logx::info("accelerated renderer unavailable ({}); falling back to software", SDL_GetError());
        renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
    }
    if (!renderer) {
        std::println(stderr, "SDL_CreateRenderer: {}", SDL_GetError());
        return 1;
    }
    SDL_RendererInfo rinfo{};
    SDL_GetRendererInfo(renderer.get(), &rinfo);
    logx::info("renderer: {}{}", rinfo.name, (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) ? " (vsync)" : "");
    extras.renderer_info = std::format("{} · {}", SDL_GetCurrentVideoDriver(), rinfo.name);

    // The app owns SDL textures and audio: it must go before the renderer, window and SDL_Quit.
    return App(std::move(opt), window.get(), renderer.get()).run(std::move(extras));
}

}  // namespace app
