#pragma once

#include "app/options.h"

namespace app {

/// Runs the app: SDL window and input, the Sendspin service, the UI. Returns the exit status.
[[nodiscard]] int run(Options opt);

}  // namespace app
