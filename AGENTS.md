# Working on muos-sendspin

A Sendspin receiver for muOS handhelds: C++23, [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp)
for the protocol, SDL2 for video, input and audio, LVGL 9.6 for the UI. Target: Anbernic RG35XX H
(H700, aarch64, 640×480, muOS 2606.0). Developed and runnable on macOS and Linux desktops. User-facing docs: README.md.

## Layout

```
src/app/       main, command line (std::expected), App: SDL window, event loop, actions, frame pacing,
               demo mode, signals, idle inhibits
src/player/    SendspinService (SDK client, roles, callbacks → PlayerStatus), SDL audio sink and its
               latency model, artwork decode, settings.ini
src/net/       mDNS: Bonjour on macOS, our own responder on the device; local IPv4 watching
src/platform/  input mapping, logging, paths, muOS idle-inhibit files, ALSA configuration
src/ui/        LVGL ↔ SDL backend, the screens (ui.cpp), theme from artwork, layout, glyphs
packaging/     the muOS app folder (mux_launch.sh, mux_lang.ini; the icon is the theme's "music" glyph)
assets/        bundled fonts (Noto Sans, OFL; Symbols.ttf: the LV_SYMBOL_* glyphs we use, a subset of
               LVGL's Font Awesome 5, so they scale) and the lean ALSA configuration
docker/        device build container (arm64 Debian, upstream SDL 2.28.5)
scripts/       device (build, lint, deploy) and lint (the Makefile's targets call them)
third_party/   stb_image (its commit is in COMMIT)
tools/sync/    measuring sync against another player with a microphone
```

Threading: everything UI-facing runs on the main loop. `SendspinService::loop()` runs every pass and
delivers the SDK's callbacks there, so `PlayerStatus` needs no locking. SDL audio runs on its callback
thread, artwork is decoded on the SDK's thread. Only the main loop touches LVGL.

## Build and run

Use the Makefile's targets; they are what CI runs too.

- `make`: build for this machine (`build/`). `make run` / `make demo`, with `ARGS="..."` for options.
- `make device-build`: the muOS package in an arm64 Debian trixie container (GCC 14, glibc 2.41 ≤ the
  device's 2.44, upstream SDL 2.28.5). Output: `dist/Sendspin/` and `dist/Sendspin.zip`.
- Versions come from git tags (`git describe`, cmake/version.cmake): `1.2.0` on tag `v1.2.0`,
  `1.2.0-3-gabc1234` after it, `-dirty` with local changes. Releasing is pushing a `vX.Y.Z` tag;
  CI builds the package and publishes the release with `Sendspin.zip`.
- `make device-deploy DEVICE=user@host`: installs the package over SSH and stops the running app.
- `make lint` before committing: clang-format, clang-tidy (on `build/`'s compile commands) and
  shellcheck. `make format` reformats. CI runs `make device-lint`, the reference: the same checks
  on the device build (the code that ships, `MUOS_DEVICE`), in the container with pinned tools. On
  macOS, libc++ hides some findings (`modernize-use-nodiscard` goes quiet once `<atomic>` is included).
  Suppress a finding only with `// NOLINTNEXTLINE(check): reason`.
- Test runs on a desktop: always `ARGS="--no-hidpi --no-focus"` (without `--no-focus` the window
  takes keyboard focus and catches the user's typing).
- Screenshots: `--demo --frames N --screenshot out.bmp`, plus `--size WxH`, `--menu PAGE`,
  `--demo-track N`, `--demo-pairing digits|qr`, `--demo-artwork FILE`, and `--gamepad-ui` to draw
  the handheld's view on a desktop. `kill -HUP <pid>` saves one from a running instance.
  docs/*.png (README) are 640×480, 480×640 and `--menu settings`, with `--gamepad-ui` and a
  600×600 square crop of the Big Buck Bunny poster (CC BY 3.0) as `--demo-artwork`, run through `pngquant --strip`.
- `assets/fonts/Symbols.ttf`: after using another `LV_SYMBOL_*`, add its code point and regenerate:
  `pyftsubset build/_deps/lvgl-src/scripts/generators/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
  --unicodes=F00C,F048,F04B,F04C,F051,F054,F074,F079,F55A --flavor= --layout-features= --no-hinting
  --desubroutinize --output-file=assets/fonts/Symbols.ttf` (fontTools).
- Debugging: `--log-level debug`, `--log-file FILE`, `-DMUOS_SENDSPIN_TRACE=ON` (inbound protocol
  messages and audio chunk timestamps), `kill -USR1/-USR2` shifts sync by ±10 ms.

## On the device

- The app folder is `/run/muos/storage/application/Sendspin` (muOS binds SD1 or SD2 there). Launched
  by `mux_launch.sh` through muOS's `SETUP_APP`, which sets SDL's environment (controller mapping,
  rotation). Never remove the `func.sh` line or hardcode `/mnt/mmc` or `/mnt/sdcard`.
- `make device-deploy` updates the app's files and leaves `state.json` (pairings), `settings.ini` and the
  logs alone. Never overwrite those on someone's device. After a deploy muOS shows Applications
  with Sendspin selected; A starts it again.
- The muOS web dashboard (`http://<device>/tools/api`) can press buttons and stream the screen.
  `/api/screen/image` is up to 30 s stale; read a frame from `/api/screen/live` instead. Check that
  the app is running and the screen is the expected one before pressing anything: blind presses
  have navigated muOS's own menus.

## Rules

**Code**
- Latest stable C++ and current library APIs: std::format/print, std::expected, std::filesystem,
  RAII, std::span/string_view. Deprecation warnings are bugs. Apple's libc++ lacks
  `std::views::enumerate`.
- SDL is the exception: the device's patched libSDL2 2.28.5 is linked dynamically (it provides
  rotation and the user's controller mapping). Never bundle or statically link SDL, set no rpath, and
  use no SDL API newer than 2.28. Use the GameController API, not raw joysticks.
- No new runtime dependencies on the device: LVGL, stb_image and the fonts are built in or bundled.
- Responsive layout: sizes come from the screen (`ui/layout.*`), never a hardcoded 640×480. Check
  landscape, square and portrait, down to 480×320.
- Match the surrounding code: naming, comment density, `logx::` for logging. Formatting is
  clang-format's (`.clang-format`); `src/ui/lv_conf.h` (LVGL's configuration template) is exempt.

**LVGL pitfalls**
- Style setters invalidate even when the value is unchanged: compare first (e.g. `lv_color_eq`).
- DOTS ("…") labels need a fixed one-line height (automatic height re-lays out forever), and LVGL
  rewrites their text: compare against our own copy (`Ui::set_text`).
- Sizes read back before LVGL's layout pass are 0 (`lv_obj_get_width`).
- Drawing is change-driven: a frame is presented only when LVGL flushed something. Keep it that way
  (CPU on the device: ~3.4% idle, ~9-12% playing).
- LVGL 9.6.0 logs `lv_image_src_get_type: … invalid magic` when deleting a canvas (QR codes); an
  upstream bug, harmless, fixed after 9.6.0.

**UI** (judged on the handheld's screen, not from numbers)
- Faint grey reads poorly on the device. Prefer words to subtle colour cues, and keep text colours at
  their contrast targets on any artwork (`make_theme`: fg 7:1, dim 4.5:1, faint and accent 3:1).
- Keep left edges consistent within a page. Glyphs and icons are centred on their measured ink, not
  their boxes (`ui/glyphs.*`).
- Wording must not be misread in a music context (e.g. "idle" reads as paused playback).
- After a UI change, look at a real frame from the device and measure before calling it right.

**Audio and sync**
- Output latency below SDL is measured, not guessed: 62 ms on muOS (ALSA → PipeWire), CoreAudio's
  report on macOS, nothing measured on Linux desktops (0). Re-measure with `tools/sync/` after
  touching the audio path.
- The ring buffer must be flushed synchronously inside `on_stream_start/end`.
- The startup lead we ask servers for (`required_lead_time_ms`) is derived from the output
  latency; it moves with `--audio-os-latency`, the SDL buffer size and the queued buffers.
- Idle: hold muOS's `idle_{game,sleep}_inhibit` (our PID) only while playing. Never use muOS's
  CAFFEINE: it also blocks power-button sleep.

## Known upstream issues (not ours)

- Music Assistant / aiosendspin: a player joining a playing group gets a short backfill, then a jump
  to the live position, so a few seconds are skipped. Any codec. Not fixable client-side.
- Music Assistant: repeat-one progress is wrong (reports position = duration while playing), and seek
  is sometimes "unsupported" until playback is stopped.
