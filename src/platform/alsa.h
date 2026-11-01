#pragma once

namespace alsa {

/// SDL 2.28's ALSA backend lists devices every 5 s on a thread it starts with the audio subsystem
/// (no hint turns it off). With ALSA's full configuration a listing costs ~120 ms of CPU on the
/// RG35XX H (it evaluates every card's PCM definitions and connects to PipeWire): 2% of a core,
/// as much as the rest of the app when idle. muOS routes all audio through PipeWire
/// (/etc/asound.conf: default = the pipewire plugin, which follows PipeWire's default sink, so
/// Bluetooth too), so the rest of the configuration is unused here. With only /etc/asound.conf
/// and our ctl.hw definition (assets/alsa/hw.conf) a listing costs ~8 ms.
///
/// Call before SDL_Init. Sets ALSA_CONFIG_PATH for this process when /etc/asound.conf routes the
/// default device to PipeWire and ALSA_CONFIG_PATH isn't already set; otherwise does nothing.
void use_lean_config();

/// Call after SDL_Init(SDL_INIT_AUDIO). If the lean configuration is in use and the default
/// device doesn't open with it (a muOS change), restarts SDL audio with ALSA's full configuration.
void verify_lean_config();

}  // namespace alsa
