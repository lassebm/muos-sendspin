#pragma once

#include <filesystem>

namespace muos {

/// One of muOS's idle inhibit files, held while playing the way muOS's own player (Wasabi) does
/// (MustardOS/frontend video/settings/session.c): our PID in /run/muos/idle_{game,sleep}_inhibit.
/// muhotkey honours a file only while that PID is alive (kill(pid, 0)), so a crash can't leave the
/// handheld unable to idle. Unlike CAFFEINE, it doesn't block power-button sleep.
///   sleep: no idle sleep.
///   game:  no display idle either (no dim, no idle mute, no idle CPU governor).
/// A no-op where /run/muos isn't writable (not muOS).
class IdleInhibit {
public:
    enum class Kind { Sleep, Game };
    explicit IdleInhibit(Kind kind);
    ~IdleInhibit();
    IdleInhibit(const IdleInhibit&) = delete;
    IdleInhibit& operator=(const IdleInhibit&) = delete;

    void set(bool on);

private:
    [[nodiscard]] bool owned() const;
    std::filesystem::path path_;
    bool on_ = false;
};

}  // namespace muos
