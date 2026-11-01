#include "player/audio_sdl.h"

#include "platform/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

// ---------------------------------------------------------------------------
// SpscRing
// ---------------------------------------------------------------------------

SpscRing::SpscRing(size_t capacity) : buf_(capacity), cap_(capacity) {}

void SpscRing::resize(size_t capacity) {
    buf_.assign(capacity, 0);
    cap_ = capacity;
    reset();
}

void SpscRing::reset() {
    rpos_.store(0, std::memory_order_relaxed);
    wpos_.store(0, std::memory_order_release);
}

size_t SpscRing::available() const {
    const size_t w = wpos_.load(std::memory_order_acquire);
    const size_t r = rpos_.load(std::memory_order_acquire);
    return (w - r + cap_) % cap_;
}

size_t SpscRing::free_space() const {
    return cap_ - 1 - available();
}

size_t SpscRing::write(const uint8_t* data, size_t len) {
    const size_t w = wpos_.load(std::memory_order_relaxed);
    const size_t r = rpos_.load(std::memory_order_acquire);
    const size_t used = (w - r + cap_) % cap_;
    const size_t n = std::min(len, cap_ - 1 - used);
    if (n == 0) return 0;
    const size_t first = std::min(n, cap_ - w);
    std::memcpy(&buf_[w], data, first);
    if (n > first) std::memcpy(buf_.data(), data + first, n - first);
    wpos_.store((w + n) % cap_, std::memory_order_release);
    return n;
}

size_t SpscRing::read(uint8_t* dest, size_t len) {
    const size_t r = rpos_.load(std::memory_order_relaxed);
    const size_t w = wpos_.load(std::memory_order_acquire);
    const size_t n = std::min(len, (w - r + cap_) % cap_);
    if (n > 0) {
        const size_t first = std::min(n, cap_ - r);
        std::memcpy(dest, &buf_[r], first);
        if (n > first) std::memcpy(dest + first, buf_.data(), n - first);
        rpos_.store((r + n) % cap_, std::memory_order_release);
    }
    if (n < len) std::memset(dest + n, 0, len - n);
    return n;
}

// ---------------------------------------------------------------------------
// SdlAudioSink
// ---------------------------------------------------------------------------

namespace {

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#ifdef __APPLE__
template <typename T>
bool ca_get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T& out) {
    const AudioObjectPropertyAddress addr{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &addr, 0, nullptr, &size, &out) == noErr;
}

/// Output latency CoreAudio reports for the default device: hardware latency + safety offset +
/// IO buffer + stream latency. SDL2 hides all of this, so without it we'd report audio as
/// finishing earlier than it really does and play late relative to other players.
int64_t platform_output_latency_us() {
    AudioObjectID dev = kAudioObjectUnknown;
    if (!ca_get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                dev) ||
        dev == kAudioObjectUnknown)
        return 0;
    Float64 rate = 0;
    UInt32 latency = 0, safety = 0, buffer = 0, stream_latency = 0;
    ca_get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
    ca_get(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, latency);
    ca_get(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, safety);
    ca_get(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput, buffer);
    AudioStreamID streams[8];
    const AudioObjectPropertyAddress saddr{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                           kAudioObjectPropertyElementMain};
    UInt32 ssize = sizeof streams;
    if (AudioObjectGetPropertyData(dev, &saddr, 0, nullptr, &ssize, streams) == noErr && ssize >= sizeof(AudioStreamID))
        ca_get(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, stream_latency);
    if (rate <= 0) return 0;
    const UInt32 total = latency + safety + buffer + stream_latency;
    logx::info(
        "audio: CoreAudio output latency {} frames @ {:.0f} Hz = {:.1f} ms (hw {}, safety {}, buffer {}, stream {})",
        total, rate, 1000.0 * total / rate, latency, safety, buffer, stream_latency);
    return static_cast<int64_t>(1e6 * total / rate);
}
#elifdef MUOS_DEVICE
/// SDL's ALSA backend hides the PCM handle, and the muOS stack (ALSA pipewire plugin -> PipeWire
/// graph with an adaptive 256..2048 quantum -> sunxi codec) reports nothing usable, so this is
/// measured: 62 ms on an RG35XX H, muOS 2606.0, 1024-frame SDL buffer, 1 queued buffer, mic +
/// click track against a HA Voice (measured with tools/sync/; override with --audio-os-latency).
int64_t platform_output_latency_us() {
    return 62000;
}
#else
/// Linux desktops: nothing measured (it depends on the sound server and the hardware); tune with
/// Settings > Sync or --audio-os-latency.
int64_t platform_output_latency_us() {
    return 0;
}
#endif

}  // namespace

// The latency below SDL is known before any device opens, so Info shows it from the start;
// configure() refreshes it (CoreAudio's report follows the current output device).
SdlAudioSink::SdlAudioSink(Options opt) : opt_(opt), platform_latency_us_(latency_below_sdl_us()) {}

int64_t SdlAudioSink::latency_below_sdl_us() const {
    return opt_.os_latency_us >= 0 ? opt_.os_latency_us : platform_output_latency_us();
}

SdlAudioSink::~SdlAudioSink() {
    close();
}

bool SdlAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    if (bits != 16) {
        logx::error("audio: unsupported bit depth {} (only 16 is advertised)", bits);
        return false;
    }
    if (dev_ != 0 && rate_ == sample_rate && channels_ == channels) {
        flush();  // same format: keep the device running, start the new stream from empty
        return true;
    }
    close();

    rate_ = sample_rate;
    channels_ = channels;
    bytes_per_frame_ = static_cast<size_t>(channels) * 2;

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(sample_rate);
    want.format = AUDIO_S16LSB;
    want.channels = channels;
    want.samples = static_cast<Uint16>(opt_.buffer_frames);
    want.callback = &SdlAudioSink::sdl_callback;
    want.userdata = this;
    SDL_AudioSpec have{};
    // No ALLOW_* flags: SDL converts to whatever the hardware wants, so the callback always
    // sees exactly the stream format and frame counts map 1:1 to sendspin frames.
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_ == 0) {
        logx::error("audio: SDL_OpenAudioDevice({} Hz, {} ch): {}", sample_rate, channels, SDL_GetError());
        return false;
    }
    obtained_frames_ = have.samples;
    frames_consumed_ = 0;
    anchor_valid_ = false;
    platform_latency_us_ = latency_below_sdl_us();

    // Small ring (as in the sendspin-cpp reference sink): backpressure comes from the callback
    // rate, and a big ring would only add zero-stuffing at sync start. It must hold a few device
    // buffers so the callback never starves on scheduling jitter.
    {
        std::scoped_lock lock(write_mutex_);
        ring_.resize(std::max<size_t>(16384, 4 * static_cast<size_t>(obtained_frames_) * bytes_per_frame_));
        writable_ = true;
    }
    write_cv_.notify_all();  // a write() waiting for a device

    logx::info("audio: {}, {} Hz {} ch s16, device buffer {} frames ({:.1f} ms) x{} queued",
               SDL_GetCurrentAudioDriver(), sample_rate, channels, obtained_frames_,
               1000.0 * obtained_frames_ / sample_rate, opt_.queued_buffers);
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void SdlAudioSink::close() {
    {
        std::scoped_lock lock(write_mutex_);
        writable_ = false;
        ++closes_;
    }
    write_cv_.notify_all();
    if (dev_ != 0) {
        SDL_CloseAudioDevice(dev_);  // waits for a running callback
        dev_ = 0;
    }
    std::scoped_lock lock(write_mutex_);
    ring_.reset();
}

void SdlAudioSink::flush() {
    std::scoped_lock lock(write_mutex_);       // no producer mid-write
    if (dev_ != 0) SDL_LockAudioDevice(dev_);  // no callback mid-read (it notifies within the read)
    ring_.reset();
    if (dev_ != 0) SDL_UnlockAudioDevice(dev_);
}

int64_t SdlAudioSink::expected_output_latency_us(uint32_t sample_rate) const {
    return latency_below_sdl_us() + int64_t{opt_.queued_buffers + 1} * opt_.buffer_frames * 1000000 / sample_rate;
}

size_t SdlAudioSink::write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(write_mutex_);
    const uint64_t closes = closes_;
    while (total < len) {
        if (writable_) {
            const size_t remaining = len - total;
            size_t n = std::min(remaining, ring_.free_space());
            // Only ever buffer whole frames: the SDK counts frames by integer division, so a
            // stray partial frame would make played > sent.
            if (n < remaining && bytes_per_frame_ > 0) n -= n % bytes_per_frame_;
            if (n > 0) total += ring_.write(data + total, n);
            if (total == len) break;
        }
        const size_t need = bytes_per_frame_ ? bytes_per_frame_ : 1;
        if (!write_cv_.wait_until(lock, deadline,
                                  [&] { return closes_ != closes || (writable_ && ring_.free_space() >= need); }))
            break;  // timed out
        if (closes_ != closes) break;
    }
    return total;
}

void SdlAudioSink::sdl_callback(void* user, Uint8* stream, int len) {
    static_cast<SdlAudioSink*>(user)->fill(stream, len);
}

void SdlAudioSink::fill(Uint8* stream, int len) {
    const int64_t t = now_us();
    const size_t got = ring_.read(stream, static_cast<size_t>(len));
    write_cv_.notify_one();

    const uint64_t gain = multiplier_.load(std::memory_order_relaxed);
    if (gain < (uint64_t{1} << 32)) {
        auto* s = reinterpret_cast<int16_t*>(stream);
        const size_t count = got / 2;
        const auto g = static_cast<int64_t>(gain);
        for (size_t i = 0; i < count; ++i)
            s[i] = static_cast<int16_t>((static_cast<int64_t>(s[i]) * g + (int64_t{1} << 31)) >> 32);
    }

    if (bytes_per_frame_ == 0) return;
    // Every requested frame is consumed by the device, silence included, so the frame count is a
    // continuous sample clock. Fit the anchor to it: jump on large disagreement (start, stall,
    // device change), otherwise follow slowly (time constant ~1 s at ~45 callbacks/s).
    const int64_t first_frame = frames_consumed_;
    frames_consumed_ += static_cast<int64_t>(static_cast<size_t>(len) / bytes_per_frame_);
    const double us_per_frame = 1e6 / rate_;
    const double anchor_now = static_cast<double>(t) - static_cast<double>(first_frame) * us_per_frame;
    if (!anchor_valid_ || std::abs(anchor_now - anchor_us_) > 50000.0) {
        anchor_us_ = anchor_now;
        anchor_valid_ = true;
    } else {
        anchor_us_ += (anchor_now - anchor_us_) * 0.02;
    }

    if (got > 0 && on_frames_played) {
        const auto frames = static_cast<uint32_t>(got / bytes_per_frame_);
        // SDL2 has no DAC timestamp. The buffer we are filling starts playing once the buffers
        // SDL already queued have drained (plus the OS latency below SDL); our real frames sit at
        // its start. Its nominal start time comes from the smoothed anchor, not the wake-up time.
        const double start_us = anchor_us_ + static_cast<double>(first_frame) * us_per_frame;
        const double ahead_us =
            static_cast<double>(opt_.queued_buffers * obtained_frames_ + static_cast<int>(frames)) * us_per_frame;
        const int64_t finish = static_cast<int64_t>(start_us + ahead_us) + platform_latency_us_ +
                               extra_latency_us_.load(std::memory_order_relaxed);
        on_frames_played(frames, finish);
    }
}

void SdlAudioSink::set_volume(uint8_t volume) {
    volume_ = std::min<uint8_t>(volume, 100);
    update_multiplier();
}

void SdlAudioSink::set_muted(bool muted) {
    muted_ = muted;
    update_multiplier();
}

void SdlAudioSink::update_multiplier() {
    constexpr uint64_t one = uint64_t{1} << 32;
    // Quadratic in the volume: perceptually even steps.
    const uint64_t m = muted_ || volume_ == 0 ? 0
                       : volume_ >= 100       ? one
                                              : static_cast<uint64_t>(volume_) * volume_ * one / 10000;
    multiplier_.store(m, std::memory_order_relaxed);
}
