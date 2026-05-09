#include "utils/blacklist_checker.h"
#include "core/server_config.h"
#include "utils/dns_resolver.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>

bool BlacklistChecker::is_blacklisted(const std::string &host)
{
    const auto &blacklist = ServerConfig::getBlacklist();
    if (blacklist.empty()) return false;

    auto check_once = [&](const std::string &h) -> bool {
        for (const auto &pattern : blacklist)
        {
            if (pattern.find('/') != std::string::npos)
            {
                if (match_cidr(h, pattern)) return true;
            }
            else if (pattern.find('*') != std::string::npos)
            {
                if (match_wildcard(h, pattern)) return true;
            }
            else
            {
                if (h == pattern) return true;
            }
        }
        return false;
    };

    // 1. First check with the original host string (matches domain patterns or literal IPs)
    if (check_once(host)) return true;

    // 2. Resolve host to IPs and check each
    auto ips = DNSResolver::resolve_ipv4(host);
    for (const auto &ip : ips)
    {
        if (check_once(ip)) return true;
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

bool BlacklistChecker::is_loopback(const std::string &target_ip, uint16_t target_port, int client_fd)
{
    struct sockaddr_in local_addr{};
    socklen_t addr_len = sizeof(local_addr);
    
    if (getsockname(client_fd, (struct sockaddr *)&local_addr, &addr_len) == 0)
    {
        std::string proxy_ip = inet_ntoa(local_addr.sin_addr);
        uint16_t proxy_port = ntohs(local_addr.sin_port);
        
        if ((target_ip == "127.0.0.1" || target_ip == "localhost" || target_ip == proxy_ip) &&
            target_port == proxy_port)
        {
            return true;
        }
    }
    return false;
}
