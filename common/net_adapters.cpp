#include "net_adapters.h"

#include <algorithm>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

namespace s3n {

#ifdef _WIN32
namespace {

std::string WideToUtf8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

std::vector<NetAdapter> ListNetAdapters() {
    std::vector<NetAdapter> out;
    std::vector<uint8_t> buffer;
    ULONG size = 16 * 1024;
    ULONG rc;
    do {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    } while (rc == ERROR_BUFFER_OVERFLOW);
    if (rc != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::string description = WideToUtf8(a->Description);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            NetAdapter adapter;
            adapter.name = WideToUtf8(a->FriendlyName);
            adapter.ipv4 = ip;
            adapter.radmin = description.find("Radmin") != std::string::npos || (ntohl(sin->sin_addr.s_addr) >> 24) == 26;
            out.push_back(std::move(adapter));
        }
    }
    std::stable_partition(out.begin(), out.end(), [](const NetAdapter& n) { return n.radmin; });
    return out;
}
#else
std::vector<NetAdapter> ListNetAdapters() { return {}; }
#endif

}  // namespace s3n
