#include "platform/alsa.h"

#include "platform/log.h"
#include "platform/paths.h"

#include <SDL.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

namespace alsa {

namespace {

bool g_lean = false;  // ALSA_CONFIG_PATH is ours

}  // namespace

void use_lean_config() {
#ifdef MUOS_DEVICE
    if (std::getenv("ALSA_CONFIG_PATH")) return;  // someone chose a configuration: keep it
    constexpr const char* kSystemConfig = "/etc/asound.conf";
    std::ifstream in(kSystemConfig);
    const std::string system{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!system.contains("pcm.!default") || !system.contains("type pipewire"))
        return;  // not muOS's PipeWire routing: the default device may need the full configuration
    const std::filesystem::path ours = paths::asset("alsa/hw.conf");
    if (!std::filesystem::exists(ours)) return;
    const std::string path = std::format("{}:{}", kSystemConfig, ours.string());
    setenv("ALSA_CONFIG_PATH", path.c_str(), 1);
    g_lean = true;
#endif
}

void verify_lean_config() {
    if (!g_lean) return;
    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;  // no callback: queue mode, never started
    if (const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0); dev != 0) {
        SDL_CloseAudioDevice(dev);
        logx::info("audio: lean ALSA configuration ({})", std::getenv("ALSA_CONFIG_PATH"));
        return;
    }
    logx::warn("audio: default device doesn't open with the lean ALSA configuration ({}); using ALSA's own",
               SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);  // unloads libasound, so the next load reads the configuration afresh
    unsetenv("ALSA_CONFIG_PATH");
    g_lean = false;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) logx::error("audio: SDL audio init: {}", SDL_GetError());
}

}  // namespace alsa
