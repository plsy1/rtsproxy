#pragma once

#include <string>
#include <vector>
#include <cstdint>

class BlacklistChecker
{
public:
    static bool is_blacklisted(const std::string &ip_or_hostname);

    /**
     * Checks if the target IP and port point back to the proxy itself.
     * @param target_ip The upstream IP to connect to.
     * @param target_port The upstream port.
     * @param client_fd The socket FD of the current client to determine local listening address.
     */
    static bool is_loopback(const std::string &target_ip, uint16_t target_port, int client_fd);

private:
    static bool match_cidr(const std::string &ip, const std::string &cidr);
    static bool match_wildcard(const std::string &host, const std::string &pattern);
};
