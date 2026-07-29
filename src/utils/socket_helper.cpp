#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <string>
#include <random>
#include <netdb.h>
#include <cstring>

static void optimize_udp_buffer(int fd)
{
    int buf_size = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
}

void set_tcp_nodelay(int fd)
{
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

int create_listen_socket(int port, const std::string &iface)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (!iface.empty())
    {
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), iface.length()) < 0)
        {
            close(sockfd);
            return -1;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sockfd);
        return -1;
    }
    if (listen(sockfd, 5) < 0)
    {
        close(sockfd);
        return -1;
    }

    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    return sockfd;
}

int create_nonblocking_tcp(const std::string &ip, uint16_t port, const std::string &iface = "")
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *results = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(ip.c_str(), service.c_str(), &hints, &results) != 0)
        return -1;

    int connected_fd = -1;
    for (addrinfo *entry = results; entry != nullptr; entry = entry->ai_next)
    {
        int sockfd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sockfd < 0)
            continue;

        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            close(sockfd);
            continue;
        }

        if (!iface.empty() &&
            setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                       iface.c_str(), iface.length()) < 0)
        {
            close(sockfd);
            continue;
        }

        if (connect(sockfd, entry->ai_addr, entry->ai_addrlen) == 0 ||
            errno == EINPROGRESS)
        {
            connected_fd = sockfd;
            break;
        }

        close(sockfd);
    }

    freeaddrinfo(results);
    if (connected_fd >= 0)
        set_tcp_nodelay(connected_fd);
    return connected_fd;
}

uint16_t get_random_port()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(10000, 60000);

    return dis(gen);
}
int bind_udp_socket_with_retry(int &fd, uint16_t &port, int max_attempts, const std::string &iface = "")
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    for (int i = 0; i < max_attempts; ++i)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return -1;

        port = get_random_port();
        addr.sin_port = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            if (!iface.empty())
            {
                if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), iface.length()) < 0)
                {
                    close(fd);
                    return -1;
                }
            }
            optimize_udp_buffer(fd);
            return 0;
        }

        close(fd);
    }

    return -1;
}

int bind_udp_socket(int &fd, const uint16_t &port, const std::string &iface = "")
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    fcntl(fd, F_SETFL, O_NONBLOCK);

    if (!iface.empty())
    {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), iface.length()) < 0)
        {
            close(fd);
            fd = -1;
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        fd = -1;
        return -1;
    }

    optimize_udp_buffer(fd);
    return 0;
}

#include "core/port_pool.h"

int bind_udp_pair_from_pool(int &rtp_fd, int &rtcp_fd, uint16_t &rtp_port, const std::string &iface)
{
    auto &pool = PortPool::getInstance();
    
    for (int i = 0; i < 10; ++i) // Try up to 10 pairs from pool
    {
        rtp_port = pool.acquire_pair();
        if (rtp_port == 0) return -1;

        if (bind_udp_socket(rtp_fd, rtp_port, iface) == 0)
        {
            if (bind_udp_socket(rtcp_fd, rtp_port + 1, iface) == 0)
            {
                return 0; // Success!
            }
            // RTCP failed, close RTP and mark port as occupied
            close(rtp_fd);
            rtp_fd = -1;
            // Hand the pair back before blocking the half that some other
            // process owns. mark_occupied() alone would leave the pool still
            // counting this pair as allocated, so the pair would be lost for
            // the lifetime of the process and a client that keeps hitting
            // taken ports would drain the whole range.
            pool.release_pair(rtp_port);
            pool.mark_occupied(rtp_port + 1);
        }
        else
        {
            pool.release_pair(rtp_port);
            pool.mark_occupied(rtp_port);
        }
    }

    // Nothing is bound and nothing is still held; make sure the caller cannot
    // mistake the last attempted port for one it now owns.
    rtp_port = 0;
    return -1;
}
