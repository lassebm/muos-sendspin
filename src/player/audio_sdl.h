#pragma once

#include <SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

/// Lock-free single-producer/single-consumer byte ring. The producer is the sendspin sync thread
/// (on_audio_write), the consumer the SDL audio callback.
class SpscRing {
public:
    explicit SpscRing(size_t capacity = 16384);
    /// Changes capacity and empties the ring; only while no producer/consumer is running.
    void resize(size_t capacity);

    size_t write(const uint8_t* data, size_t len);  // producer
    size_t read(uint8_t* dest, size_t len);         // consumer: returns real bytes, zero-fills the rest
    [[nodiscard]] size_t available() const;
    [[nodiscard]] size_t free_space() const;

    /// Hard reset; only while no producer/consumer is running.
    void reset();

private:
    std::vector<uint8_t> buf_;
    size_t cap_;
    std::atomic<size_t> wpos_{0};
    std::atomic<size_t> rpos_{0};
};

/// Plays sendspin's decoded PCM through an SDL audio device and reports, per callback, how many
/// frames were consumed and when the last of them will have left the DAC (steady_clock µs, the
/// sendspin-cpp client clock). That feedback is what drives synchronization.
class SdlAudioSink {
public:
    struct Options {
        int buffer_frames = 1024;  // SDL device buffer (power of two)
        int os_latency_us = -1;    // output latency below SDL; -1 = platform default (see audio_sdl.cpp)

        // Device buffers SDL keeps queued ahead of the one the callback is filling. macOS
        // (sdl2-compat on SDL3's CoreAudio backend): 2, by ear against other Sendspin players (1
        // buffer + CoreAudio latency left the Mac ~20 ms late = one 1024-frame buffer at 44.1 kHz).
        // Elsewhere 1: the device's measured 62 ms below SDL (audio_sdl.cpp) was taken with 1 queued
        // buffer and buffer_frames = 1024, so the three are coupled: changing either value means
        // re-measuring with tools/sync/.
#ifdef __APPLE__
        int queued_buffers = 2;
#else
        int queued_buffers = 1;
#endif
    };

    explicit SdlAudioSink(Options opt);
    ~SdlAudioSink();
    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    /// (Re)opens the device for the given format. Main loop thread. 16-bit only (what we advertise).
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits);
    /// Closes the device and drops buffered audio. Main loop thread.
    void close();
    /// Drops buffered audio synchronously, keeping the device open. Main loop thread.
    /// Must complete before the SDK starts the next stream: the SDK resets its count of buffered
    /// frames at a stream boundary, so any old frame reported as played afterwards (or any new frame
    /// dropped) skews its playtime estimate and forces a resync gap. on_stream_start() runs before
    /// the SDK starts the new stream, so a synchronous flush there is exact.
    void flush();

    /// Producer side (sendspin sync thread). Blocks up to timeout_ms for space; with no device open
    /// (none configured yet, or opening it failed) it waits out the timeout and returns 0, so the
    /// SDK's sync thread, which retries at once, doesn't spin. close() wakes it early.
    size_t write(const uint8_t* data, size_t len, uint32_t timeout_ms);

    /// Extra output latency on top of the estimate (user tuning), adjustable while playing.
    void set_extra_latency_us(int64_t us) { extra_latency_us_.store(us, std::memory_order_relaxed); }
    [[nodiscard]] int64_t extra_latency_us() const { return extra_latency_us_.load(std::memory_order_relaxed); }
    /// Output latency below SDL: --audio-os-latency, else CoreAudio's report on macOS (0 if it has
    /// none) and the measured constant elsewhere.
    [[nodiscard]] int64_t platform_latency_us() const { return platform_latency_us_; }
    /// Before any device opens: how long audio written now takes to be heard at this sample rate
    /// (the latency below SDL plus the buffer being filled and the queued ones).
    [[nodiscard]] int64_t expected_output_latency_us(uint32_t sample_rate) const;

    void set_volume(uint8_t volume);  // 0..100
    void set_muted(bool muted);

    /// Wired to PlayerRole::notify_audio_played().
    std::function<void(uint32_t frames, int64_t finish_us)> on_frames_played;

private:
    static void sdl_callback(void* user, Uint8* stream, int len);
    [[nodiscard]] int64_t latency_below_sdl_us() const;
    void fill(Uint8* stream, int len);
    void update_multiplier();

    Options opt_;
    SpscRing ring_;
    SDL_AudioDeviceID dev_ = 0;
    uint32_t rate_ = 0;
    uint8_t channels_ = 0;
    size_t bytes_per_frame_ = 0;
    int obtained_frames_ = 0;

    std::mutex write_mutex_;
    std::condition_variable write_cv_;
    // Both guarded by write_mutex_.
    bool writable_ = false;  // a device is open
    uint64_t closes_ = 0;    // bumped by close(), so a blocked write() returns at once

    std::atomic<uint64_t> multiplier_{uint64_t{1} << 32};  // Q32 gain
    uint8_t volume_ = 100;
    bool muted_ = false;
    std::atomic<int64_t> extra_latency_us_{0};
    // Smoothed timing anchor (audio callback thread only; reset in configure()): the steady-clock
    // time at which frame 0 of the device stream would start, if the device consumed frames
    // perfectly evenly. Callback wake-ups are bursty (e.g. when SDL resamples 48 kHz -> 44.1 kHz
    // they alternate one and two calls per device period), so stamping each buffer with the
    // callback's own wall time adds ~10 ms of jitter. Frame counts are exact; the anchor only
    // has to track slow clock drift.
    int64_t frames_consumed_ = 0;
    double anchor_us_ = 0;
    bool anchor_valid_ = false;
    int64_t platform_latency_us_;  // refreshed in configure() before the device starts
};
