#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

constexpr size_t kMaxLabel = 63;      // bytes in a DNS label, such as the instance name
constexpr size_t kMaxTxtEntry = 255;  // bytes in a TXT entry ("key=value")

/// The longest prefix of s within max bytes that doesn't split a UTF-8 character.
std::string_view utf8_prefix(std::string_view s, size_t max);

/// Minimal mDNS/DNS-SD responder (RFC 6762/6763) for one service instance, for systems whose own
/// responder can't be told about our service. On muOS, `mudns` answers the host name (muos.local)
/// and a fixed service list; we share UDP 5353 with it (SO_REUSEADDR/SO_REUSEPORT) and answer
/// only for `_sendspin._tcp`:
///   PTR _sendspin._tcp.local         -> <name>._sendspin._tcp.local
///   SRV <name>._sendspin._tcp.local  -> <host>.local:<port>
///   TXT <name>._sendspin._tcp.local  -> path=<path>, name=<name>
///   A   <host>.local (additional)    -> our IPv4 (the same data mudns serves)
/// plus the DNS-SD service enumeration PTR. Announces at start and on IP change, and sends a
/// goodbye (TTL 0) on stop. IPv4 only.
class MdnsResponder {
public:
    MdnsResponder() = default;
    ~MdnsResponder();
    MdnsResponder(const MdnsResponder&) = delete;
    MdnsResponder& operator=(const MdnsResponder&) = delete;

    /// host: label without ".local" (e.g. "muos"). Starts the responder thread, which logs failures.
    /// A name over 63 bytes is shortened for the DNS label; TXT name= keeps it whole.
    void start(const std::string& instance, const std::string& host, uint16_t port, const std::string& path);
    void stop();
    /// Re-check the address and announce again on the responder thread. The service calls it when
    /// the address changes and after a resume; the responder doesn't poll the address itself.
    void reannounce() { reannounce_ = true; }

private:
    void run();

    std::string instance_;  // the DNS label: the name, shortened to fit
    std::string name_, host_, path_;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reannounce_{false};
    std::thread thread_;
};
