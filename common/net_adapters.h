#pragma once

#include <string>
#include <vector>

namespace s3n {

struct NetAdapter {
    std::string name;  // friendly name, UTF-8
    std::string ipv4;
    bool radmin = false;  // Radmin VPN adapter (description match or 26.0.0.0/8)
};

// Connected IPv4 adapters, Radmin VPN first.
std::vector<NetAdapter> ListNetAdapters();

}  // namespace s3n
