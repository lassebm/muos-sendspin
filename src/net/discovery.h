#pragma once

#include <cstdint>
#include <memory>
#include <string>

/// Announces this player as `_sendspin._tcp` so Music Assistant can find and connect to it.
///   macOS:  Bonjour (dns_sd.h, part of libSystem).
///   device: muOS's own `mudns` only announces a fixed service list, so we run our own minimal
///           responder for `_sendspin._tcp`, sharing UDP 5353 with it (mdns_responder.h).
///           MUOS_SENDSPIN_MDNS=builtin selects it on macOS too, for testing.
class Advertiser {
public:
    virtual ~Advertiser() = default;
    /// Failures are logged: the player still works without being found.
    virtual void start(const std::string& name, uint16_t port, const std::string& path) = 0;
    virtual void stop() = 0;
    /// Announce again now (e.g. after resume from suspend). Default: nothing to do.
    virtual void reannounce() {}
};

std::unique_ptr<Advertiser> make_advertiser();
