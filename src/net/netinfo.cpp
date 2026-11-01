#include "net/netinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <string_view>

uint32_t local_ipv4() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return 0;
    uint32_t best = 0;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        // No IFF_RUNNING (carrier) check: muOS's network stop (sleep) flushes the address, which is
        // what callers watch, and a carrier blip while roaming shouldn't count as a lost network.
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_MULTICAST)) continue;
        const uint32_t ip = reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
        const std::string_view name = i->ifa_name ? i->ifa_name : "";
        const bool preferred = name.starts_with("wlan") || name.starts_with("eth") || name.starts_with("en");
        if (!best || preferred) best = ip;
        if (preferred) break;
    }
    freeifaddrs(list);
    return best;
}

std::string ipv4_text(uint32_t ip) {
    if (!ip) return {};
    in_addr a{};
    a.s_addr = ip;
    char text[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &a, text, sizeof text) ? text : "";
}
