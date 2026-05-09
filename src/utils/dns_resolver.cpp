#include "utils/dns_resolver.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>

std::vector<std::string> DNSResolver::resolve_ipv4(const std::string &hostname)
{
    std::vector<std::string> ips;
    struct addrinfo hints{}, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 only for now
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &res) != 0)
    {
        return ips;
    }

    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next)
    {
        char ip_str[INET_ADDRSTRLEN];
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
        if (inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, sizeof(ip_str)))
        {
            ips.push_back(std::string(ip_str));
        }
    }

    freeaddrinfo(res);
    return ips;
}
