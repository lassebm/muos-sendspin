#include "platform/muos.h"

#include "platform/log.h"

#include <fstream>

#include <unistd.h>

namespace muos {

IdleInhibit::IdleInhibit(Kind kind) {
    if (access("/run/muos", W_OK) != 0) return;  // not muOS
    path_ = kind == Kind::Game ? "/run/muos/idle_game_inhibit" : "/run/muos/idle_sleep_inhibit";
}

IdleInhibit::~IdleInhibit() {
    set(false);
}

void IdleInhibit::set(bool on) {
    if (path_.empty() || on == on_) return;
    on_ = on;
    if (on) {
        std::ofstream(path_) << getpid() << '\n';
    } else if (owned()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);  // only if it's still ours
    }
    logx::info("idle: {} {}", on ? "holding" : "released", path_.string());
}

bool IdleInhibit::owned() const {
    int pid = 0;
    std::ifstream(path_) >> pid;
    return pid == getpid();
}

}  // namespace muos
