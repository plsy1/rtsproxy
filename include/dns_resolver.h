#pragma once

#include <string>
#include <vector>

class DNSResolver
{
public:
    /**
     * Resolves a hostname to a list of IPv4 addresses.
     * Returns an empty vector if resolution fails.
     */
    static std::vector<std::string> resolve_ipv4(const std::string &hostname);
};
