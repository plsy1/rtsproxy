#pragma once

#include "core/epoll_loop.h"
#include "core/buffer_pool.h"
#include <memory>
#include <string>
#include <netinet/in.h>

class ProxyServer
{
public:
    ProxyServer();
    ~ProxyServer();

    /**
     * Initializes the server: loads config, sets up sockets, and handles daemon/watchdog logic.
     * @return EXIT_SUCCESS or EXIT_FAILURE.
     */
    int run(int argc, char *argv[]);

private:
    void setup_signals();
    int start_worker();
    int start_watchdog();
    
    void setup_accept_handler(int listen_fd, EpollLoop &loop, BufferPool &pool);

private:
    bool running_{true};
};
