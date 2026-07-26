#include "utils/blacklist_checker.h"
#include "core/server_config.h"
#include "utils/dns_resolver.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>

namespace
{

// DNS names are case-insensitive (RFC 4343); IP literals are unaffected by
// ASCII case folding, so hostname comparisons fold both sides.
std::string to_lower_ascii(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace

bool BlacklistChecker::is_blacklisted(const std::string &host)
{
    const auto &blacklist = ServerConfig::getBlacklist();
    if (blacklist.empty()) return false;

    auto check_once = [&](const std::string &h) -> bool {
        const std::string h_lower = to_lower_ascii(h);
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
                if (h_lower == to_lower_ascii(pattern)) return true;
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
    std::string bits_str = cidr.substr(slash_pos + 1);
    // std::stoi() accepts signs and leading whitespace ("-0" parses as 0 and
    // would degrade the entry to /0), so require plain decimal digits first.
    if (bits_str.empty() || bits_str.find_first_not_of("0123456789") != std::string::npos)
        return false;

    int bits = 0;
    try
    {
        size_t parsed = 0;
        bits = std::stoi(bits_str, &parsed);
        if (parsed != bits_str.size() || bits < 0 || bits > 32)
            return false;
    }
    catch (...)
    {
        return false;
    }

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

    const std::string h = to_lower_ascii(host);
    const std::string p = to_lower_ascii(pattern);

    if (p.front() == '*')
    {
        std::string suffix = p.substr(1);
        if (h.size() >= suffix.size())
        {
            return h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }
    else if (p.back() == '*')
    {
        std::string prefix = p.substr(0, p.size() - 1);
        if (h.size() >= prefix.size())
        {
            return h.compare(0, prefix.size(), prefix) == 0;
        }
    }
    else
    {
        // General case (very simple): split by * and match segments
        // For now, let's just support prefix/suffix for simplicity as requested (*.domain.com)
        return h == p;
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
