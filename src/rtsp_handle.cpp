#include "../include/rtsp_handle.h"
#include "../include/rtsp_mitm_client.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/buffer_pool.h"
#include "../include/server_config.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <sstream>

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

    Logger::debug("[RTSP-MITM] New RTSP client: " + client_host);

    // Token authentication
    if (!ServerConfig::getToken().empty())
    {
        std::istringstream ss(first_request);
        std::string method, uri, version;
        ss >> method >> uri >> version;

        bool authorized = false;
        size_t qpos = uri.find('?');
        if (qpos != std::string::npos)
        {
            std::string query = uri.substr(qpos + 1);
            // Search for token=YOUR_TOKEN in the query string
            std::string target = "token=" + ServerConfig::getToken();
            if (query.find(target) != std::string::npos)
            {
                authorized = true;
            }
        }

        if (!authorized)
        {
            Logger::warn("[RTSP-MITM] Unauthorized RTSP request from " + client_host + " (Missing or invalid token)");
            std::string response = "RTSP/1.0 401 Unauthorized\r\n"
                                   "CSeq: 1\r\n"
                                   "Content-Length: 0\r\n"
                                   "\r\n";
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }
    }

    auto client = std::make_unique<RTSPMitmClient>(
        loop, pool, client_addr, client_fd, first_request);

    loop->add_client_to_map(client_fd, std::move(client));

    // Note: RTSPMitmClient now owns the fd and manages its own epoll registration.
    // We set the on_closed callback to clean up the map entry.
    loop->get_client_from_map(client_fd)->set_on_closed_callback(
        [client_fd, loop, client_host]()
        {
                    Logger::debug("[RTSP-MITM] Client disconnect: " + client_host);
            loop->add_task([client_fd, loop]()
                           { loop->remove_client_from_map(client_fd); });
        });
}
