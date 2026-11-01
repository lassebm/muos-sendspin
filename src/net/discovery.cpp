#include "net/discovery.h"

#include "net/mdns_responder.h"
#include "platform/log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

#ifdef __APPLE__
#include <arpa/inet.h>
#include <dns_sd.h>
#endif

namespace {

#ifdef __APPLE__

class BonjourAdvertiser final : public Advertiser {
public:
    BonjourAdvertiser() = default;
    BonjourAdvertiser(const BonjourAdvertiser&) = delete;
    BonjourAdvertiser& operator=(const BonjourAdvertiser&) = delete;
    ~BonjourAdvertiser() override { stop(); }

    void start(const std::string& name, uint16_t port, const std::string& path) override {
        stop();
        // Bonjour rejects a longer instance label; the TXT name= keeps the whole name.
        const std::string label(utf8_prefix(name, kMaxLabel));
        if (label.size() < name.size())
            logx::warn("mdns: '{}' is longer than a DNS label ({} bytes); advertising '{}'", name, kMaxLabel, label);
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        const auto set = [&](const char* key, std::string_view value) {
            value = utf8_prefix(value, kMaxTxtEntry - std::strlen(key) - 1);  // "key=value"
            TXTRecordSetValue(&txt, key, static_cast<uint8_t>(value.size()), value.data());
        };
        set("path", path);
        set("name", name);
        const DNSServiceErrorType err =
            DNSServiceRegister(&ref_, 0, 0, label.c_str(), "_sendspin._tcp", nullptr, nullptr, htons(port),
                               TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr);
        TXTRecordDeallocate(&txt);
        if (err != kDNSServiceErr_NoError) {
            ref_ = nullptr;
            logx::error("mdns: DNSServiceRegister failed ({})", err);
            return;
        }
        logx::info("mdns: advertising _sendspin._tcp '{}' on port {} (path {})", label, port, path);
    }

    void stop() override {
        if (ref_) {
            DNSServiceRefDeallocate(ref_);
            ref_ = nullptr;
        }
    }

private:
    DNSServiceRef ref_ = nullptr;
};

#endif

/// Our own responder (mdns_responder.h); on muOS it shares UDP 5353 with mudns.
class BuiltinAdvertiser : public Advertiser {
public:
    void start(const std::string& name, uint16_t port, const std::string& path) override {
        responder_.start(name, host_label(), port, path);
    }
    void stop() override { responder_.stop(); }
    void reannounce() override { responder_.reannounce(); }

private:
    /// The host name the system's own responder answers for, so our SRV target resolves.
    static std::string host_label() {
        std::string h;
        std::getline(std::ifstream("/run/muos/mdns_name"), h);  // muOS: mudns's actual name
        if (h.empty()) {
            std::array<char, 256> buf{};
            if (gethostname(buf.data(), buf.size() - 1) == 0) h = buf.data();
        }
        while (!h.empty() && (h.back() == '\r' || h.back() == ' ')) h.pop_back();
        if (const auto dot = h.find('.'); dot != std::string::npos) h.resize(dot);
        return h.empty() ? "muos" : h;
    }
    MdnsResponder responder_;
};

}  // namespace

std::unique_ptr<Advertiser> make_advertiser() {
#ifdef __APPLE__
    // Bonjour, unless testing the device's responder on the Mac.
    if (const char* m = std::getenv("MUOS_SENDSPIN_MDNS"); !m || std::strcmp(m, "builtin") != 0)
        return std::make_unique<BonjourAdvertiser>();
#endif
    return std::make_unique<BuiltinAdvertiser>();
}
