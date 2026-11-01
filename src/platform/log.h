#pragma once

#include <SDL.h>

#include <filesystem>
#include <format>
#include <string>
#include <utility>

/// std::format-style logging on top of SDL's logger (which the device log and macOS's console
/// already use). Categories: application unless stated.
namespace logx {

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", std::format(fmt, std::forward<Args>(args)...).c_str());
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", std::format(fmt, std::forward<Args>(args)...).c_str());
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", std::format(fmt, std::forward<Args>(args)...).c_str());
}

/// Debug lines are enabled per category with --log-level debug.
template <typename... Args>
void debug(int category, std::format_string<Args...> fmt, Args&&... args) {
    SDL_LogDebug(category, "%s", std::format(fmt, std::forward<Args>(args)...).c_str());
}

/// Routes stderr (SDL_Log's and sendspin-cpp's lines) through a thread that, on Linux, prefixes every
/// line with the wall-clock time, so the device log shows where time goes (macOS's SDL_Log already
/// has timestamps). With a file, stdout and stderr go there instead: the previous two runs are kept
/// as FILE.1 and FILE.2 (Sendspin.1.log), and a run rotates them again at 1 MiB.
void capture_output(const std::filesystem::path& file);

}  // namespace logx
