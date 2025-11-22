#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#include <algorithm>
#include <unistd.h>
#include <string>
#include <random>
#include <chrono>

int create_listen_socket(int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    if (listen(sockfd, 5) < 0)
        return -1;

    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    return sockfd;
}

int create_nonblocking_tcp(const std::string &ip, uint16_t port, const std::string &iface = "")
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    fcntl(sockfd, F_SETFL, O_NONBLOCK);

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
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
        return -1;

    return sockfd;
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
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    return 0;
}