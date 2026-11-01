#!/bin/sh
# HELP: Play music from Music Assistant or any Sendspin server, in sync with your other speakers. Shows what's playing with artwork, and controls playback and volume
# ICON: music
# GRID: Sendspin

# shellcheck source=/dev/null  # muOS's own script, only on the device
. /opt/muos/script/var/func.sh

APP_BIN="muos-sendspin"
SETUP_APP "$APP_BIN" ""

SETUP_STAGE_OVERLAY

# --- Sendspin -----------------------------------------------------------------
# The bind-mounted runtime path (Application Runner guide): muOS maps it to SD2 or SD1.
APP_DIR="/run/muos/storage/application/Sendspin"
cd "$APP_DIR" || exit 1

# While playing, the app itself holds off idle sleep the way muOS's own player does (its PID in
# /run/muos/idle_sleep_inhibit). No CAFFEINE: that would also block power-button sleep.
# The app writes and rotates Sendspin.log itself (previous runs: Sendspin.1.log, Sendspin.2.log);
# the redirect only catches what comes before that, such as a library that fails to load.
# shellcheck disable=SC2094  # the app writes Sendspin.log, it doesn't read it
"./$APP_BIN" --fullscreen --log-file "$APP_DIR/Sendspin.log" >>"$APP_DIR/Sendspin.log" 2>&1
