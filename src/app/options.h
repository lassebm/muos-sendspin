#pragma once

#include "player/sendspin_service.h"

#include <sendspin/client.h>

#include <expected>
#include <filesystem>
#include <string>

namespace app {

struct Options {
    int width = 640;
    int height = 480;
#ifdef MUOS_DEVICE
    bool fullscreen = true;  // device: native display mode
#else
    bool fullscreen = false;  // desktop: a window you can resize to test aspect ratios
#endif
    bool hidpi = true;
    bool gamepad_ui = false;           // draw the UI as on a buttons-only handheld (screenshots on a desktop)
    bool no_focus = false;             // show the window without taking keyboard focus (test runs)
    int frames = 0;                    // >0: exit after N frames
    std::filesystem::path screenshot;  // write the last frame as BMP before exiting
    bool demo = false;                 // mock content, no sendspin client (layout work)
    int demo_track = 0;
    std::string demo_pairing;            // "digits" or "qr": show the pairing overlay in the demo
    std::filesystem::path demo_artwork;  // cover art for the demo's first track
    std::string menu;                    // open this menu page at start (screenshots)
    sendspin::LogLevel log_level = sendspin::LogLevel::INFO;
    std::filesystem::path log_file;  // write stdout/stderr here, rotated (the device launcher sets it)
    ServiceOptions service;
};

/// What the command line asked for: run with these options, or exit (help, or an error already
/// printed) with this status.
using ParseResult = std::expected<Options, int>;

[[nodiscard]] ParseResult parse_options(int argc, char** argv);

}  // namespace app
