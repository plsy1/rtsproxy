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
#include <ctime>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t worker_pid = 0;

// Everything below runs in signal context, where the only tools available are
// async-signal-safe calls. Logger::info()/error()/flush() are none of those:
// they allocate, touch iostreams, and take a mutex. A crash signal raised while
// the loop was inside the logger would deadlock the handler, leaving the worker
// hung rather than dead -- and the supervisor's blocking waitpid() would then
// never return to restart it. Logger::emergency() is a bare write(2).
static const char *signal_name(int sig)
{
    switch (sig)
    {
    case SIGINT:  return "SIGINT";
    case SIGTERM: return "SIGTERM";
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    default:      return "an unexpected signal";
    }
}

static void worker_sig_handler(int sig)
{
    Logger::emergency("[SERVER] Worker received ");
    Logger::emergency(signal_name(sig));
    Logger::emergency(". Exiting...\n");
    _exit(0);
}

static void supervisor_sig_handler(int sig)
{
    Logger::emergency("[SUPERVISOR] Received ");
    Logger::emergency(signal_name(sig));
    Logger::emergency(". Killing worker and exiting...\n");
    if (worker_pid > 0)
    {
        kill(static_cast<pid_t>(worker_pid), SIGTERM);
    }
    _exit(0);
}

static void crash_handler(int sig)
{
    Logger::emergency("[SERVER] CRITICAL: Worker process crashed with ");
    Logger::emergency(signal_name(sig));
    Logger::emergency("\n");
    _exit(sig);
}

// Held open from startup for the sole purpose of surviving EMFILE. With the
// descriptor table full, accept() cannot dequeue the pending connection, and the
// listen socket is non-blocking and registered level-triggered, so epoll_wait
// would hand it back immediately, forever, and burn the only thread at 100% CPU.
// Closing this reserve frees exactly one slot: enough to accept the connection
// and drop it, which clears the readable condition and lets the loop breathe.
static int emfile_reserve_fd = -1;

// A descriptor shortage is usually a flood, and every log line costs a
// mutex-held flush, so report it at most once per interval with a tally.
static void log_fd_exhaustion(const char *what)
{
    static time_t last_report = 0;
    static unsigned long suppressed = 0;

    time_t now = time(nullptr);
    if (last_report != 0 && now - last_report < 5)
    {
        ++suppressed;
        return;
    }

    std::string msg = "[SERVER] Out of file descriptors, " + std::string(what);
    if (suppressed > 0)
        msg += " (" + std::to_string(suppressed) + " similar events suppressed)";
    Logger::error(msg);

    last_report = now;
    suppressed = 0;
}

// Dequeue and immediately drop one pending connection so that the listen fd
// stops being readable. Returns false when no descriptor could be spared, in
// which case the caller must not simply spin.
static bool drop_pending_connection(int listen_fd)
{
    if (emfile_reserve_fd < 0)
        return false;

    close(emfile_reserve_fd);
    emfile_reserve_fd = -1;

    sockaddr_in victim_addr{};
    socklen_t victim_len = sizeof(victim_addr);
    int victim_fd = accept(listen_fd, (sockaddr *)&victim_addr, &victim_len);
    if (victim_fd >= 0)
        close(victim_fd);

    // Re-arm the reserve for next time. If even this fails the table is still
    // full; the next EMFILE takes the throttled path below instead.
    emfile_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return victim_fd >= 0;
}

ProxyServer::ProxyServer() {}
ProxyServer::~ProxyServer() {}

int ProxyServer::run(int argc, char *argv[])
{
    // 1. Parse command line and load config
    try
    {
        ServerConfig::parseCommandLine(argc, argv);
    }
    catch (const std::exception &e)
    {
        Logger::error("[CONFIG] Invalid configuration: " + std::string(e.what()));
        return EXIT_FAILURE;
    }
    
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
    signal(SIGPIPE, SIG_IGN);
    
    if (ServerConfig::isWatchdogEnabled()) {
        signal(SIGSEGV, crash_handler);
        signal(SIGABRT, crash_handler);
        signal(SIGFPE, crash_handler);
        signal(SIGILL, crash_handler);
    }
}

void ProxyServer::setup_accept_handler(int listen_fd, EpollLoop &loop, BufferPool &pool)
{
    // Claim the EMFILE reserve now, after any daemon() call has already taken
    // over 0/1/2, so that a descriptor is genuinely available later.
    if (emfile_reserve_fd < 0)
    {
        emfile_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (emfile_reserve_fd < 0)
            Logger::warn("[SERVER] Could not reserve a spare fd; accept() will throttle instead of shedding under fd exhaustion");
    }

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
                // Backlog drained: the only condition that should end the batch
                // quietly, because it is the only one that clears the readable
                // state of a level-triggered listen fd.
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;

                // Interrupted, or the peer went away between the SYN and here.
                // The connection behind it is still queued, so keep going --
                // returning here would leave the fd readable and spin.
                if (errno == EINTR || errno == ECONNABORTED) continue;

                if (errno == EMFILE || errno == ENFILE ||
                    errno == ENOBUFS || errno == ENOMEM)
                {
                    if (drop_pending_connection(listen_fd))
                    {
                        log_fd_exhaustion("dropped a pending connection");
                    }
                    else
                    {
                        // No descriptor to spare, so the connection cannot be
                        // dequeued and the fd stays readable. Sleeping caps the
                        // damage at a few wakeups per second instead of a full
                        // core until an existing session frees a descriptor.
                        log_fd_exhaustion("cannot accept, throttling");
                        usleep(50 * 1000);
                    }
                    break;
                }

                Logger::error("[SERVER] Accept client request failed: " + std::string(strerror(errno)));
                break;
            }

            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            set_tcp_nodelay(client_fd);

            auto ctx = std::make_unique<SocketCtx>();
            auto request_buffer = std::make_shared<std::string>();
            ctx->fd = client_fd;
            ctx->handler = [client_fd, client_addr, &loop, &pool, request_buffer](uint32_t e)
            {
                MasterHandle::handle(client_fd, client_addr, &loop, pool,
                                     *request_buffer, e);
            };

            loop.set(std::move(ctx), client_fd,
                     EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
        }
    };

    auto listen_ctx = std::make_unique<SocketCtx>(
        listen_fd,
        [accept_handler](uint32_t event) { accept_handler(event); });

    loop.set(std::move(listen_ctx), listen_fd, EPOLLIN);
}
