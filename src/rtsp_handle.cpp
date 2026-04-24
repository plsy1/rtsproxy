#include "../include/rtsp_handle.h"
#include "../include/rtsp_mitm_client.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/buffer_pool.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>

void handle_rtsp_request(int client_fd, sockaddr_in client_addr,
                         EpollLoop *loop, BufferPool &pool)
{
    if (client_fd < 0)
    {
        Logger::error("[RTSP-MITM] Received request on invalid file descriptor");
        return;
    }

    // Read the first RTSP request (we need at least the first line to know the URL).
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);

    if (n == 0)
    {
        Logger::debug("[RTSP-MITM] Client disconnected before sending a request.");
        close(client_fd);
        return;
    }
    else if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // no data yet – will be called again by epoll
        Logger::error("[RTSP-MITM] recv failed: " + std::string(strerror(errno)));
        close(client_fd);
        return;
    }

    buf[n] = 0;
    std::string first_request(buf, n);

    std::string client_host =
        std::string(inet_ntoa(client_addr.sin_addr)) + ":" +
        std::to_string(ntohs(client_addr.sin_port));

    Logger::info("[RTSP-MITM] New RTSP client: " + client_host);

    auto client = std::make_unique<RTSPMitmClient>(
        loop, pool, client_addr, client_fd, first_request);

    loop->add_client_to_map(client_fd, std::move(client));

    // Note: RTSPMitmClient now owns the fd and manages its own epoll registration.
    // We set the on_closed callback to clean up the map entry.
    loop->get_client_from_map(client_fd)->set_on_closed_callback(
        [client_fd, loop, client_host]()
        {
            Logger::info("[RTSP-MITM] Client disconnect: " + client_host);
            loop->add_task([client_fd, loop]()
                           { loop->remove_client_from_map(client_fd); });
        });
}
