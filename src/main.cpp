#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/http_handle.h"
#include "../include/rtsp_handle.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/socket_helper.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/wait.h>
#include <signal.h>

static pid_t worker_pid = 0;

void crash_handler(int sig)
{
    // Use low-level logging or just exit since we are crashing
    // Logger might not be safe here depending on its implementation, 
    // but for now we'll try to log it.
    std::cerr << "[WORKER] Crashed with signal " << sig << std::endl;
    _exit(sig);
}

void supervisor_sig_handler(int sig)
{
    if (worker_pid > 0)
    {
        kill(worker_pid, sig);
    }
    Logger::flush();
    _exit(0);
}

void worker_sig_handler(int sig)
{
    (void)sig;
    Logger::flush();
    _exit(0);
}

int main(int argc, char *argv[])
{

    Logger::setLogLevel(LogLevel::INFO);
    signal(SIGINT, worker_sig_handler);
    signal(SIGTERM, worker_sig_handler);

    struct option long_options[] =
        {
            {"port", required_argument, nullptr, 'p'},
            {"enable-nat", no_argument, nullptr, 'n'},
            {"nat-method", required_argument, nullptr, 0},
            {"buffer-pool-count", required_argument, nullptr, 'b'},
            {"buffer-pool-block-size", required_argument, nullptr, 's'},
            {"auth-token", required_argument, nullptr, 't'},
            {"http-interface", required_argument, nullptr, 0},
            {"mitm-interface", required_argument, nullptr, 0},
            {"listen-interface", required_argument, nullptr, 'l'},
            {"config", required_argument, nullptr, 'c'},
            {"stun-port", required_argument, nullptr, 0},
            {"stun-host", required_argument, nullptr, 0},
            {"kill", no_argument, nullptr, 'k'},
            {"daemon", no_argument, nullptr, 'd'},
            {"watchdog", no_argument, nullptr, 'w'},
            {"log-file", required_argument, nullptr, 0},
            {"log-lines", required_argument, nullptr, 0},
            {"log-level", required_argument, nullptr, 0},
            {"strip-padding", no_argument, nullptr, 0},
            {"wait-keyframe", no_argument, nullptr, 0},
            {nullptr, 0, nullptr, 0}};

    // 1. Initial scan for -j or --json-path to determine the config file location
    int temp_opt;
    int temp_longindex = -1;
    while ((temp_opt = getopt_long(argc, argv, "p:nb:s:t:c:l:kdw", long_options, &temp_longindex)) != -1)
    {
        if (temp_opt == 'c') {
            ServerConfig::setJsonPath(optarg);
        } else if (temp_opt == 0 && temp_longindex >= 0 && strcmp(long_options[temp_longindex].name, "config") == 0) {
            ServerConfig::setJsonPath(optarg);
        }
    }
    // Reset getopt to parse again. Set to 0 to force full reset of getopt_long internal state.
    optind = 0;

    // 2. Load settings from config file (if it exists)
    ServerConfig::loadFromFile(ServerConfig::getJsonPath());

    // 3. Parse all arguments (command line overrides config file)
    int opt;
    int longindex = -1;
    while ((opt = getopt_long(argc, argv, "p:nb:s:t:c:l:kdw", long_options, &longindex)) != -1)
    {
        switch (opt)
        {
        case 'p':
            ServerConfig::setPort(std::atoi(optarg));
            break;
        case 'n':
            ServerConfig::setNatEnabled(true);
            break;
        case 'b':
            ServerConfig::setBufferPoolCount(std::atoi(optarg));
            break;
        case 's':
            ServerConfig::setBufferPoolBlockSize(std::atoi(optarg));
            break;
        case 't':
            ServerConfig::setToken(optarg);
            break;
        case 'c':
            ServerConfig::setJsonPath(optarg);
            break;
        case 'l':
            ServerConfig::setListenInterface(optarg);
            break;
        case 'k':
            ServerConfig::kill_previous_instance();
            return 0;
        case 'd':
            ServerConfig::setDaemonEnabled(true);
            break;
        case 'w':
            ServerConfig::setWatchdogEnabled(true);
            break;
        case 0:
            if (longindex >= 0 && strcmp(long_options[longindex].name, "nat-method") == 0)
            {
                ServerConfig::setNatMethod(optarg);
                ServerConfig::setNatEnabled(true);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-port") == 0)
            {
                ServerConfig::setStunPort(std::atoi(optarg));
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-host") == 0)
            {
                ServerConfig::setStunHost(optarg);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "http-interface") == 0)
            {
                ServerConfig::setHttpUpstreamInterface(optarg);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "mitm-interface") == 0)
            {
                ServerConfig::setMitmUpstreamInterface(optarg);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-file") == 0)
            {
                ServerConfig::setLogFile(optarg);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-lines") == 0)
            {
                ServerConfig::setLogLines(std::stoull(optarg));
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-level") == 0)
            {
                std::string level = optarg;
                if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
                else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
                else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
                else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "strip-padding") == 0)
            {
                ServerConfig::setStripPadding(true);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "wait-keyframe") == 0)
            {
                ServerConfig::setWaitKeyframe(true);
            }
            break;
        default:
            ServerConfig::printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    ServerConfig::printConfig();

    if (!ServerConfig::getLogFile().empty()) {
        Logger::setLogFile(ServerConfig::getLogFile(), ServerConfig::getLogLines());
    }

    // 3. Execution of process management (Daemon / Watchdog)
    if (ServerConfig::isDaemonEnabled())
    {
        if (daemon(1, 0) != 0)
        {
            Logger::error("[SERVER] Failed to daemonize");
            return EXIT_FAILURE;
        }
        Logger::info("[SERVER] Running in daemon mode");
    }

    if (ServerConfig::isWatchdogEnabled())
    {
        Logger::info("[SUPERVISOR] Starting in watchdog mode");
        Logger::flush();
        signal(SIGTERM, supervisor_sig_handler);
        signal(SIGINT, supervisor_sig_handler);

        while (true)
        {
            worker_pid = fork();
            if (worker_pid == 0)
            {
                // Child process (Worker)
                signal(SIGSEGV, crash_handler);
                signal(SIGABRT, crash_handler);
                signal(SIGFPE, crash_handler);
                signal(SIGILL, crash_handler);
                // Ensure worker flushes logs on signal
                signal(SIGTERM, worker_sig_handler);
                signal(SIGINT, worker_sig_handler);
                break; // Exit supervisor loop and continue to main logic
            }
            else if (worker_pid > 0)
            {
                // Parent process (Supervisor)
                int status;
                waitpid(worker_pid, &status, 0);
                if (WIFEXITED(status))
                {
                    int exit_code = WEXITSTATUS(status);
                    if (exit_code == 0)
                    {
                        Logger::info("[SUPERVISOR] Worker exited normally. Shutting down.");
                        Logger::flush();
                        return 0;
                    }
                    else
                    {
                        Logger::error("[SUPERVISOR] Worker exited with code " + std::to_string(exit_code) + ". Restarting in 2 seconds...");
                    }
                }
                else if (WIFSIGNALED(status))
                {
                    int sig = WTERMSIG(status);
                    Logger::error("[SUPERVISOR] Worker killed by signal " + std::to_string(sig) + ". Restarting in 2 seconds...");
                }
                sleep(2);
            }
            else
            {
                Logger::error("[SUPERVISOR] Fork failed: " + std::string(strerror(errno)));
                return -1;
            }
        }
    }

    // Already loaded at the beginning

    EpollLoop loop;
    BufferPool pool(ServerConfig::getBufferPoolBlockSize(), ServerConfig::getBufferPoolCount());

    int listen_port = ServerConfig::getPort();
    int listen_fd = create_listen_socket(listen_port, ServerConfig::getListenInterface());
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
                if (client_fd >= 0)
                {
                    // Peek at the first few bytes to determine protocol
                    char peek_buf[16];
                    ssize_t n = recv(client_fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
                    if (n > 0) {
                        peek_buf[n] = 0;
                        std::string start(peek_buf);
                        // HTTP typically starts with GET, POST, HEAD, or has HTTP/1.1
                        if (start.find("GET ") == 0 || start.find("POST") == 0 || start.find("HTTP") != std::string::npos) {
                            handle_http_request(client_fd, client_addr, &loop, pool);
                        } else {
                            // Assume RTSP for other methods like OPTIONS, DESCRIBE, SETUP...
                            handle_rtsp_request(client_fd, client_addr, &loop, pool);
                        }
                    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        loop.remove(client_fd);
                        close(client_fd);
                    }
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

    Logger::info("[SERVER] Unified HTTP/RTSP server listening on port " + std::to_string(listen_port));

    loop.loop();

    return 0;
}