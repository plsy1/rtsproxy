#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/handle.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/parse_url.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

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

    return sockfd;
}

int main(int argc, char *argv[])
{

    Logger::setLogLevel(LogLevel::ERROR);

    struct option long_options[] =
        {
            {"port", required_argument, nullptr, 'p'},
            {"enable-nat", no_argument, nullptr, 'n'},
            {"set-rtp-buffer-size", required_argument, nullptr, 'r'},
            {"set-max-udp-packet-size", required_argument, nullptr, 'u'},
            {"set-auth-token", required_argument, nullptr, 't'},
            {"set-json-path", required_argument, nullptr, 'j'},
            {"set-stun-port", required_argument, nullptr, 0},
            {"set-stun-host", required_argument, nullptr, 0},
            {"kill", no_argument, nullptr, 'k'},
            {"daemon", no_argument, nullptr, 'd'},
            {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "p:nr:u:t:j:kd", long_options, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'p':
            ServerConfig::setPort(std::atoi(optarg));
            break;
        case 'n':
            ServerConfig::enableNat();
            break;
        case 'r':
            ServerConfig::setRtpBufferSize(std::atoi(optarg));
            break;
        case 'u':
            ServerConfig::setUdpPacketSize(std::atoi(optarg));
            break;
        case 't':
            ServerConfig::setToken(optarg);
            break;
        case 'j':
            ServerConfig::setJsonPath(optarg);
            break;
        case 'k':
            ServerConfig::kill_previous_instance();
            return 0;
        case 'd':
            if (daemon(0, 0) == -1)
            {
                Logger::error("[SERVER] Failed to daemonize the process");
                return -1;
            }
            Logger::info("[SERVER] Running in daemon mode");
            break;
        case 0:
            if (strcmp(long_options[opt].name, "set-stun-port") == 0)
            {
                ServerConfig::setStunPort(std::atoi(optarg));
            }
            else if (strcmp(long_options[opt].name, "set-stun-host") == 0)
            {
                ServerConfig::setStunHost(optarg);
            }
            break;
        default:
            ServerConfig::printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    ParseURL::load_json(ServerConfig::getJsonPath());

    EpollLoop loop;
    BufferPool pool(ServerConfig::getUdpPacketSize(), ServerConfig::getRtpBufferSize());

    int listen_port = ServerConfig::getPort();
    int listen_fd = create_listen_socket(listen_port);
    if (listen_fd < 0)
        return -1;

    fcntl(listen_fd, F_SETFL, O_NONBLOCK);

    auto accept_handler = [&](uint32_t events)
    {
        [[maybe_unused]] uint32_t unused_events = events;
        while (true)
        {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (sockaddr *)&client_addr, &len);
            if (client_fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                Logger::error("[SERVER] Accept client request failed: " + std::string(strerror(errno)));
                break;
            }
            fcntl(client_fd, F_SETFL, O_NONBLOCK);

            auto ctx = std::make_shared<SocketCtx>();
            ctx->fd = client_fd;
            ctx->handler = [ctx, client_addr, &loop, &pool](uint32_t e)
            {
                [[maybe_unused]] uint32_t unused_events = e;
                if (ctx->fd >= 0)
                {
                    Logger::debug("[SERVER] Handling request for client_fd: " + std::to_string(ctx->fd));
                    handle_http_request(ctx->fd, client_addr, &loop, pool);
                }
                else
                {
                    Logger::error("[SERVER] Invalid client_fd, cannot handle request");
                }
            };

            loop.set(ctx.get(), client_fd, EPOLLIN);
        }
    };

    auto listen_ctx = std::make_shared<SocketCtx>();
    listen_ctx->fd = listen_fd;
    listen_ctx->handler = accept_handler;
    loop.set(listen_ctx.get(), listen_fd, EPOLLIN);

    Logger::info("[SERVER] HTTP server listening on port " + std::to_string(listen_port));
    loop.loop();

    return 0;
}