#include "net/mdns_responder.h"

#include "net/netinfo.h"
#include "platform/log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Labels = std::vector<std::string>;

constexpr uint16_t kMdnsPort = 5353;
constexpr uint16_t kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33, kTypeAny = 255;
constexpr uint16_t kClassIn = 1, kCacheFlush = 0x8000;
constexpr uint32_t kTtlHost = 120, kTtlOther = 4500;  // RFC 6762 §10
constexpr uint32_t kTtlLegacy = 10;                   // RFC 6762 §6.7

bool label_eq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool name_eq(const Labels& a, const Labels& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!label_eq(a[i], b[i])) return false;
    return true;
}

/// Reads a (possibly compressed) DNS name at off; advances off past it in the original stream.
bool read_name(const uint8_t* p, size_t len, size_t& off, Labels& out) {
    size_t pos = off;
    bool jumped = false;
    for (int guard = 0; guard < 32; ++guard) {
        if (pos >= len) return false;
        const uint8_t l = p[pos];
        if (l == 0) {
            if (!jumped) off = pos + 1;
            return true;
        }
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            if (!jumped) off = pos + 2;
            pos = static_cast<size_t>((l & 0x3F) << 8 | p[pos + 1]);
            jumped = true;
            continue;
        }
        if (l & 0xC0) return false;  // 0x40 and 0x80 label types are reserved
        if (pos + 1 + l > len) return false;
        out.emplace_back(reinterpret_cast<const char*>(p + pos + 1), l);
        pos += 1 + l;
    }
    return false;
}

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void name(const Labels& labels) {
        for (const std::string& l : labels) {
            const size_t n = std::min<size_t>(l.size(), 63);
            u8(static_cast<uint8_t>(n));
            b.insert(b.end(), l.begin(), l.begin() + static_cast<long>(n));
        }
        u8(0);
    }
    /// Writes a resource record whose RDATA is produced by fill().
    template <typename F>
    void record(const Labels& owner, uint16_t type, uint16_t cls, uint32_t ttl, F fill) {
        name(owner);
        u16(type);
        u16(cls);
        u32(ttl);
        const size_t len_at = b.size();
        u16(0);
        fill(*this);
        const size_t rdlen = b.size() - len_at - 2;
        b[len_at] = static_cast<uint8_t>(rdlen >> 8);
        b[len_at + 1] = static_cast<uint8_t>(rdlen);
    }
};

enum Rec : unsigned { kPtr = 1, kSrv = 2, kTxt = 4, kAddr = 8, kEnum = 16 };

/// A legacy unicast query (RFC 6762 §6.7): the reply repeats its id and questions.
struct LegacyQuery {
    uint16_t id;
    uint16_t count;
    std::span<const uint8_t> questions;  // the query's question section, copied verbatim
};

}  // namespace

std::string_view utf8_prefix(std::string_view s, size_t max) {
    if (s.size() <= max) return s;
    while (max > 0 && (static_cast<unsigned char>(s[max]) & 0xC0) == 0x80) --max;  // a continuation byte
    return s.substr(0, max);
}

MdnsResponder::~MdnsResponder() {
    stop();
}

void MdnsResponder::start(const std::string& instance, const std::string& host, uint16_t port,
                          const std::string& path) {
    stop();
    // Queries name the label we send, so they're matched against the same shortened name.
    instance_ = utf8_prefix(instance, kMaxLabel);
    if (instance_.size() < instance.size())
        logx::warn("mdns: '{}' is longer than a DNS label ({} bytes); advertising '{}'", instance, kMaxLabel,
                   instance_);
    name_ = instance;
    host_ = host;
    port_ = port;
    path_ = path;
    stop_ = false;
    thread_ = std::thread(&MdnsResponder::run, this);
}

void MdnsResponder::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void MdnsResponder::run() {
    const Labels service{"_sendspin", "_tcp", "local"};
    const Labels inst{instance_, "_sendspin", "_tcp", "local"};
    const Labels host{host_, "local"};
    const Labels services_enum{"_services", "_dns-sd", "_udp", "local"};

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logx::error("mdns: socket: {}", std::strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(kMdnsPort);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof bind_addr) != 0) {
        logx::error("mdns: bind 5353: {}", std::strerror(errno));
        close(fd);
        return;
    }
    const unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);

    uint32_t ip = 0;
    // ttl_scale 0 makes the goodbye. A legacy unicast reply has short TTLs and no cache-flush bit:
    // the asker is a plain DNS resolver, not an mDNS cache.
    const auto build = [&](unsigned answers, unsigned additional, uint32_t ttl_scale,
                           const std::optional<LegacyQuery>& legacy = std::nullopt) {
        Writer w;
        w.u16(legacy ? legacy->id : 0);
        w.u16(0x8400);  // response, authoritative
        w.u16(legacy ? legacy->count : 0);
        w.u16(static_cast<uint16_t>(std::popcount(answers)));
        w.u16(0);
        w.u16(static_cast<uint16_t>(std::popcount(additional & ~answers)));
        if (legacy) w.b.insert(w.b.end(), legacy->questions.begin(), legacy->questions.end());
        const uint16_t flush = legacy ? 0 : kCacheFlush;
        const auto ttl_of = [&](uint32_t t) { return legacy ? std::min(t, kTtlLegacy) : t * ttl_scale; };
        const auto emit = [&](unsigned rec) {
            const uint32_t t_host = ttl_of(kTtlHost), t_other = ttl_of(kTtlOther);
            switch (rec) {
                case kPtr: w.record(service, kTypePtr, kClassIn, t_other, [&](Writer& x) { x.name(inst); }); break;
                case kEnum:
                    w.record(services_enum, kTypePtr, kClassIn, t_other, [&](Writer& x) { x.name(service); });
                    break;
                case kSrv:
                    w.record(inst, kTypeSrv, kClassIn | flush, t_host, [&](Writer& x) {
                        x.u16(0);
                        x.u16(0);
                        x.u16(port_);
                        x.name(host);
                    });
                    break;
                case kTxt:
                    w.record(inst, kTypeTxt, kClassIn | flush, t_other, [&](Writer& x) {
                        for (const std::string& kv : {"path=" + path_, "name=" + name_}) {
                            const std::string_view entry = utf8_prefix(kv, kMaxTxtEntry);
                            x.u8(static_cast<uint8_t>(entry.size()));
                            x.b.insert(x.b.end(), entry.begin(), entry.end());
                        }
                    });
                    break;
                case kAddr:
                    w.record(host, kTypeA, kClassIn | flush, t_host, [&](Writer& x) {
                        const auto* a = reinterpret_cast<const uint8_t*>(&ip);  // network order
                        x.b.insert(x.b.end(), a, a + 4);
                    });
                    break;
            }
        };
        for (const unsigned rec : {kPtr, kEnum, kSrv, kTxt, kAddr})
            if (answers & rec) emit(rec);
        for (const unsigned rec : {kPtr, kEnum, kSrv, kTxt, kAddr})
            if ((additional & rec) && !(answers & rec)) emit(rec);
        return w.b;
    };
    const auto send_to = [&](const std::vector<uint8_t>& msg, const sockaddr_in& to) {
        sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    };

    std::vector<Clock::time_point> announce_at;
    bool check_ip = true;  // at start, then whenever reannounce() says the address changed
    uint8_t buf[9000];

    while (!stop_) {
        const auto now = Clock::now();
        if (reannounce_.exchange(false)) {
            ip = 0;  // forces the membership re-join and a fresh announcement below
            check_ip = true;
        }
        if (check_ip) {
            check_ip = false;
            const uint32_t ip_now = local_ipv4();
            if (ip_now != ip) {
                ip = ip_now;
                if (ip) {
                    ip_mreq mreq{};
                    mreq.imr_multiaddr = group.sin_addr;
                    mreq.imr_interface.s_addr = ip;
                    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);  // EADDRINUSE is fine
                    in_addr ifaddr{};
                    ifaddr.s_addr = ip;
                    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr);
                    char text[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ifaddr, text, sizeof text);
                    logx::info("mdns: advertising _sendspin._tcp '{}' -> {}.local:{} ({})", instance_, host_, port_,
                               text);
                    // RFC 6762 §8.3: at least two announcements, one second apart.
                    announce_at = {now, now + std::chrono::seconds(1), now + std::chrono::seconds(3)};
                }
            }
        }
        while (!announce_at.empty() && ip && Clock::now() >= announce_at.front()) {
            send_to(build(kPtr | kSrv | kTxt | kAddr, 0, 1), group);
            announce_at.erase(announce_at.begin());
        }

        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0 || !(pfd.revents & POLLIN)) continue;
        sockaddr_in from{};
        socklen_t from_len = sizeof from;
        const ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 12 || !ip) continue;
        const auto len = static_cast<size_t>(n);
        const auto id = static_cast<uint16_t>(buf[0] << 8 | buf[1]);
        const auto flags = static_cast<uint16_t>(buf[2] << 8 | buf[3]);
        if (flags & 0x8000) continue;  // a response, not a query
        const int qd = buf[4] << 8 | buf[5];

        unsigned answers = 0, additional = 0;
        size_t off = 12;         // past the last question read in full
        uint16_t questions = 0;  // those read in full
        for (int q = 0; q < qd; ++q) {
            Labels name;
            size_t next = off;
            if (!read_name(buf, len, next, name) || next + 4 > len) break;
            const auto qtype = static_cast<uint16_t>(buf[next] << 8 | buf[next + 1]);
            off = next + 4;  // type + class (QU bit ignored: we always answer by multicast)
            ++questions;
            const bool any = qtype == kTypeAny;
            if (name_eq(name, service) && (qtype == kTypePtr || any)) {
                answers |= kPtr;
                additional |= kSrv | kTxt | kAddr;
            } else if (name_eq(name, services_enum) && (qtype == kTypePtr || any)) {
                answers |= kEnum;
            } else if (name_eq(name, inst)) {
                if (qtype == kTypeSrv || any) answers |= kSrv;
                if (qtype == kTypeTxt || any) answers |= kTxt;
                additional |= kAddr;
            }
        }
        if (!answers) continue;
        if (ntohs(from.sin_port) != kMdnsPort) {
            // The questions keep their offsets behind our 12-byte header, so their compression
            // pointers stay valid.
            const LegacyQuery legacy{id, questions, std::span<const uint8_t>(buf + 12, off - 12)};
            send_to(build(answers, additional, 1, legacy), from);
        } else {
            send_to(build(answers, additional, 1), group);
        }
    }

    if (ip) {  // goodbye: our records with TTL 0 so browsers drop us at once (not the A record: mudns owns the host)
        send_to(build(kPtr | kSrv | kTxt, 0, 0), group);
    }
    close(fd);
}
