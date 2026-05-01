#pragma once

#include <string>

class BlacklistChecker
{
public:
    static bool is_blacklisted(const std::string &host);

private:
    static bool match_cidr(const std::string &ip, const std::string &cidr);
    static bool match_wildcard(const std::string &host, const std::string &pattern);
};
