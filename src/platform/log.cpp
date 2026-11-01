#include "platform/log.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <semaphore>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace logx {

namespace {

constexpr int kKeptLogs = 2;                          // FILE.1, FILE.2 besides FILE
constexpr size_t kMaxLogBytes = size_t{1024} * 1024;  // rotate within a run at this size

/// FILE.N with N before the extension: Sendspin.log → Sendspin.1.log.
std::filesystem::path numbered(const std::filesystem::path& file, int n) {
    auto p = file;
    p.replace_filename(std::format("{}.{}{}", file.stem().string(), n, file.extension().string()));
    return p;
}

/// Shifts FILE → FILE.1 → FILE.2, dropping the oldest.
void rotate(const std::filesystem::path& file) {
    std::error_code ec;
    for (int n = kKeptLogs; n > 1; --n) std::filesystem::rename(numbered(file, n - 1), numbered(file, n), ec);
    std::filesystem::rename(file, numbered(file, 1), ec);
}

/// Released when the writer has seen the end of the pipe. Never destroyed: when the drain at exit
/// times out, the detached writer may still release it after static destruction.
std::binary_semaphore& drained() {
    static auto* const semaphore = new std::binary_semaphore(0);
    return *semaphore;
}

/// At exit: close our ends of the pipe and give the writer a moment to write out the last lines.
void drain_at_exit() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (const int null_fd = open("/dev/null", O_WRONLY); null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
    (void)drained().try_acquire_for(std::chrono::milliseconds(500));
}

int open_log(const std::filesystem::path& file) {
    return open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

}  // namespace

void capture_output(const std::filesystem::path& file) {
#ifndef __linux__
    if (file.empty()) return;  // nothing to add: macOS's SDL_Log has timestamps already
#endif
    int out_fd = -1;
    if (file.empty()) {
        out_fd = dup(STDERR_FILENO);
    } else {
        rotate(file);  // keep the previous runs (a crash and relaunch would otherwise lose its log)
        out_fd = open_log(file);
    }
    if (out_fd < 0) return;
    int fds[2];
    if (pipe(fds) != 0) {
        close(out_fd);
        return;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    if (dup2(fds[1], STDERR_FILENO) < 0) {
        for (const int fd : {fds[0], fds[1], out_fd}) close(fd);
        return;
    }
    // With a file, stdout goes there too; otherwise it stays apart from stderr.
    if (!file.empty()) dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    std::atexit(drain_at_exit);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    // Raw read()/write() on purpose: a thread blocked in stdio holds that FILE's lock, and exit()
    // then deadlocks flushing all streams.
    std::thread([in_fd = fds[0], out_fd, file] mutable {
        size_t written = 0;
        const auto write_all = [&](std::string_view s) {
            while (out_fd >= 0 && !s.empty()) {
                const ssize_t w = write(out_fd, s.data(), s.size());
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) return;
                s.remove_prefix(static_cast<size_t>(w));
                written += static_cast<size_t>(w);
            }
        };
        char buf[4096];
        bool line_start = true;
        for (;;) {
            const ssize_t n = read(in_fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                drained().release();
                return;
            }
            std::string out;
            for (const char c : std::string_view(buf, static_cast<size_t>(n))) {
#ifdef __linux__
                if (line_start) {
                    // localtime_r, not std::chrono::current_zone(): that needs a tz database muOS
                    // may not ship (and throws without one); muOS sets local time via TZ.
                    const auto now = std::chrono::system_clock::now();
                    const std::time_t t = std::chrono::system_clock::to_time_t(now);
                    std::tm local{};
                    localtime_r(&t, &local);
                    const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                    out +=
                        std::format("{:02}:{:02}:{:02}.{:03} ", local.tm_hour, local.tm_min, local.tm_sec, ms.count());
                }
#endif
                out += c;
                line_start = c == '\n';
            }
            write_all(out);
            if (!file.empty() && line_start && written >= kMaxLogBytes) {
                close(out_fd);
                rotate(file);
                out_fd = open_log(file);  // on failure, keep draining: a pipe without a reader kills writers
                written = 0;
            }
        }
    }).detach();
}

}  // namespace logx
