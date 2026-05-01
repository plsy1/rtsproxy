#include "../include/blacklist_checker.h"
#include "../include/server_config.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>

bool BlacklistChecker::is_blacklisted(const std::string &host)
{
    const auto &blacklist = ServerConfig::getBlacklist();
    if (blacklist.empty()) return false;

    for (const auto &pattern : blacklist)
    {
        if (pattern.find('/') != std::string::npos)
        {
            // Likely CIDR
            if (match_cidr(host, pattern)) return true;
        }
        else if (pattern.find('*') != std::string::npos)
        {
            // Domain Wildcard
            if (match_wildcard(host, pattern)) return true;
        }
        else
        {
            // Exact match
            if (host == pattern) return true;
        }
    }

    return false;
}

bool BlacklistChecker::match_cidr(const std::string &ip, const std::string &cidr)
{
    size_t slash_pos = cidr.find('/');
    if (slash_pos == std::string::npos) return ip == cidr;

    std::string base_ip_str = cidr.substr(0, slash_pos);
    int bits = std::stoi(cidr.substr(slash_pos + 1));

    struct in_addr base_addr, target_addr;
    if (inet_pton(AF_INET, base_ip_str.c_str(), &base_addr) != 1) return false;
    if (inet_pton(AF_INET, ip.c_str(), &target_addr) != 1) return false;

    uint32_t mask = (bits == 0) ? 0 : (0xFFFFFFFF << (32 - bits));
    mask = htonl(mask);

    return (target_addr.s_addr & mask) == (base_addr.s_addr & mask);
}

bool BlacklistChecker::match_wildcard(const std::string &host, const std::string &pattern)
{
    // Simple wildcard support: *.example.com or example.*
    // We'll support * at the beginning or end.
    if (pattern == "*") return true;

    if (pattern.front() == '*')
    {
        std::string suffix = pattern.substr(1);
        if (host.size() >= suffix.size())
        {
            return host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }
    else if (pattern.back() == '*')
    {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        if (host.size() >= prefix.size())
        {
            return host.compare(0, prefix.size(), prefix) == 0;
        }
    }
    else
    {
        // General case (very simple): split by * and match segments
        // For now, let's just support prefix/suffix for simplicity as requested (*.domain.com)
        return host == pattern;
    }

    return false;
}
