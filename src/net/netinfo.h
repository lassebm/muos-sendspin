#pragma once

#include <cstdint>
#include <string>

/// IPv4 (network byte order) of the first up, non-loopback, multicast-capable interface,
/// preferring Wi-Fi/Ethernet names; 0 if there is none.
uint32_t local_ipv4();

/// An IPv4 address (network byte order) as dotted text; empty for 0 (no network).
std::string ipv4_text(uint32_t ip);
