#pragma once

#include <filesystem>

namespace paths {

/// Directory of the executable (the muOS app folder on the device; build/ on a desktop).
std::filesystem::path base();

/// Bundled asset, e.g. asset("fonts/NotoSans-Regular.ttf").
std::filesystem::path asset(std::string_view relative);

/// Default sendspin state file: per-user app data on macOS, next to the binary on the device
/// (the muOS app folder is ours to write).
std::filesystem::path default_state_file();

}  // namespace paths
