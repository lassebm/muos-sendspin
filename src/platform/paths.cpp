#include "platform/paths.h"

#include <SDL.h>

#include <memory>

namespace paths {

namespace {

struct SdlFree {
    void operator()(char* p) const { SDL_free(p); }
};

}  // namespace

std::filesystem::path base() {
    const std::unique_ptr<char, SdlFree> p(SDL_GetBasePath());
    return p ? std::filesystem::path(p.get()) : std::filesystem::path();
}

std::filesystem::path asset(std::string_view relative) {
    return base() / "assets" / relative;
}

std::filesystem::path default_state_file() {
#ifndef MUOS_DEVICE
    // Desktops: ~/Library/Application Support/muos-sendspin/ (macOS), ~/.local/share/muos-sendspin/
    // (no organisation folder)
    if (const std::unique_ptr<char, SdlFree> p(SDL_GetPrefPath("", "muos-sendspin")); p)
        return std::filesystem::path(p.get()) / "state.json";
#endif
    return base() / "state.json";
}

}  // namespace paths
