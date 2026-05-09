#include "core/proxy_server.h"
#include "core/logger.h"
#include "core/server_config.h"
#include "handlers/master_handle.h"
#include "utils/socket_helper.h"
#include "common/socket_ctx.h"
#include <fcntl.h>
#include <getopt.h>
#include <sys/wait.h>
#include <cstring>
#include <signal.h>
#include <unistd.h>

static pid_t worker_pid = 0;

static void worker_sig_handler(int sig)
{
    Logger::info("[SERVER] Worker received signal " + std::to_string(sig) + ". Exiting...");
    Logger::flush();
    _exit(0);
}

static void supervisor_sig_handler(int sig)
{
    Logger::info("[SUPERVISOR] Received signal " + std::to_string(sig) + ". Killing worker and exiting...");
    if (worker_pid > 0)
    {
        kill(worker_pid, SIGTERM);
    }
    _exit(0);
}

static void crash_handler(int sig)
{
    Logger::error("[SERVER] CRITICAL: Worker process crashed with signal " + std::to_string(sig));
    Logger::flush();
    _exit(sig);
}

ProxyServer::ProxyServer() {}
ProxyServer::~ProxyServer() {}

int ProxyServer::run(int argc, char *argv[])
{
    // 1. Parse command line and load config
    ServerConfig::parseCommandLine(argc, argv);
    
    if (!ServerConfig::getLogFile().empty()) {
        Logger::setLogFile(ServerConfig::getLogFile(), ServerConfig::getLogLines());
    }

    ServerConfig::printConfig();

    // 2. Daemonize if requested
    if (ServerConfig::isDaemonEnabled())
    {
        if (daemon(1, 0) != 0)
        {
            Logger::error("[SERVER] Failed to daemonize");
            return EXIT_FAILURE;
        }
        Logger::info("[SERVER] Running in daemon mode");
    }

    // 3. Watchdog logic
    if (ServerConfig::isWatchdogEnabled())
    {
        return start_watchdog();
    }
    else
    {
        return start_worker();
    }
}

int ProxyServer::start_watchdog()
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
            return start_worker();
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
                    return 0;
                }
                Logger::error("[SUPERVISOR] Worker exited with code " + std::to_string(exit_code) + ". Restarting in 2 seconds...");
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
            return EXIT_FAILURE;
        }
    }
}

int ProxyServer::start_worker()
{
    setup_signals();

    EpollLoop loop;
    BufferPool pool(ServerConfig::getBufferPoolBlockSize(), ServerConfig::getBufferPoolCount());

    int listen_port = ServerConfig::getPort();
    int listen_fd = create_listen_socket(listen_port, ServerConfig::getListenInterface());
    if (listen_fd < 0) return EXIT_FAILURE;

    setup_accept_handler(listen_fd, loop, pool);

    Logger::info("[SERVER] Unified HTTP/RTSP server listening on port " + std::to_string(listen_port));
    loop.loop();

    return EXIT_SUCCESS;
}

void ProxyServer::setup_signals()
{
    signal(SIGINT, worker_sig_handler);
    signal(SIGTERM, worker_sig_handler);
    
    if (ServerConfig::isWatchdogEnabled()) {
        signal(SIGSEGV, crash_handler);
        signal(SIGABRT, crash_handler);
        signal(SIGFPE, crash_handler);
        signal(SIGILL, crash_handler);
    }
}

void ProxyServer::setup_accept_handler(int listen_fd, EpollLoop &loop, BufferPool &pool)
{
    auto accept_handler = [listen_fd, &loop, &pool](uint32_t events)
    {
        [[maybe_unused]] uint32_t unused_events = events;
        while (true)
        {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (sockaddr *)&client_addr, &len);
            if (client_fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                Logger::error("[SERVER] Accept client request failed: " + std::string(strerror(errno)));
                break;
            }
            
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            set_tcp_nodelay(client_fd);

            auto ctx = std::make_unique<SocketCtx>();
            ctx->fd = client_fd;
            ctx->handler = [client_fd, client_addr, &loop, &pool](uint32_t e)
            {
                [[maybe_unused]] uint32_t unused_events = e;
                MasterHandle::handle(client_fd, client_addr, &loop, pool);
            };

            loop.set(std::move(ctx), client_fd, EPOLLIN);
        }
    };

    auto listen_ctx = std::make_unique<SocketCtx>(
        listen_fd,
        [accept_handler](uint32_t event) { accept_handler(event); });

    loop.set(std::move(listen_ctx), listen_fd, EPOLLIN);
}
