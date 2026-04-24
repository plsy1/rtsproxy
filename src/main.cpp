#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/http_handle.h"
#include "../include/rtsp_handle.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/http_parser.h"
#include "../include/socket_helper.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

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
            {"set-interface", required_argument, nullptr, 'i'},
            {"set-json-path", required_argument, nullptr, 'j'},
            {"set-stun-port", required_argument, nullptr, 0},
            {"set-stun-host", required_argument, nullptr, 0},
            {"rtsp-port", required_argument, nullptr, 0},
            {"kill", no_argument, nullptr, 'k'},
            {"daemon", no_argument, nullptr, 'd'},
            {nullptr, 0, nullptr, 0}};

    int opt;
    int longindex = -1;
    while ((opt = getopt_long(argc, argv, "p:nr:u:t:j:i:kd", long_options, &longindex)) != -1)
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
        case 'i':
            ServerConfig::setInterface(optarg);
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
            if (longindex >= 0 && strcmp(long_options[longindex].name, "set-stun-port") == 0)
            {
                ServerConfig::setStunPort(std::atoi(optarg));
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "set-stun-host") == 0)
            {
                ServerConfig::setStunHost(optarg);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "rtsp-port") == 0)
            {
                ServerConfig::setRtspPort(std::atoi(optarg));
            }
            break;
        default:
            ServerConfig::printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    httpParser::load_json(ServerConfig::getJsonPath());

    EpollLoop loop;
    BufferPool pool(ServerConfig::getUdpPacketSize(), ServerConfig::getRtpBufferSize());

    int listen_port = ServerConfig::getPort();
    int listen_fd = create_listen_socket(listen_port);
    if (listen_fd < 0)
        return -1;

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

            auto ctx = std::make_unique<SocketCtx>();
            ctx->fd = client_fd;
            ctx->handler = [client_fd, client_addr, &loop, &pool](uint32_t e)
            {
                [[maybe_unused]] uint32_t unused_events = e;
                if (client_fd)
                {
                    Logger::debug("[SERVER] Handling request for client_fd: " + std::to_string(client_fd));
                    handle_http_request(client_fd, client_addr, &loop, pool);
                }
                else
                {
                    Logger::error("[SERVER] Invalid client_fd, cannot handle request");
                }
            };

            loop.set(std::move(ctx), client_fd, EPOLLIN);
        }
    };

    auto listen_ctx = std::make_unique<SocketCtx>(
        listen_fd,
        [&](uint32_t event)
        { accept_handler(event); });

    loop.set(listen_ctx.get(), listen_fd, EPOLLIN);

    Logger::info("[SERVER] HTTP server listening on port " + std::to_string(listen_port));

    // ------------------------------------------------------------------ //
    // Optional: RTSP MITM proxy listener                                  //
    // ------------------------------------------------------------------ //
    int rtsp_listen_port = ServerConfig::getRtspPort();
    int rtsp_listen_fd = -1;
    std::unique_ptr<SocketCtx> rtsp_listen_ctx;

    if (rtsp_listen_port > 0)
    {
        rtsp_listen_fd = create_listen_socket(rtsp_listen_port);
        if (rtsp_listen_fd < 0)
        {
            Logger::error("[SERVER] Failed to create RTSP listen socket on port " +
                          std::to_string(rtsp_listen_port));
            return -1;
        }

        rtsp_listen_ctx = std::make_unique<SocketCtx>(
            rtsp_listen_fd,
            [&, rtsp_listen_fd](uint32_t events)
            {
                [[maybe_unused]] uint32_t unused_events = events;
                while (true)
                {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(rtsp_listen_fd, (sockaddr *)&client_addr, &len);
                    if (client_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        Logger::error("[RTSP-MITM] Accept failed: " + std::string(strerror(errno)));
                        break;
                    }
                    fcntl(client_fd, F_SETFL, O_NONBLOCK);

                    auto ctx = std::make_unique<SocketCtx>();
                    ctx->fd = client_fd;
                    ctx->handler = [client_fd, client_addr, &loop, &pool](uint32_t e)
                    {
                        [[maybe_unused]] uint32_t unused_ev = e;
                        handle_rtsp_request(client_fd, client_addr, &loop, pool);
                    };
                    loop.set(std::move(ctx), client_fd, EPOLLIN);
                }
            });

        loop.set(rtsp_listen_ctx.get(), rtsp_listen_fd, EPOLLIN);
        Logger::info("[SERVER] RTSP MITM proxy listening on port " +
                     std::to_string(rtsp_listen_port));
    }

    loop.loop();

    return 0;
}