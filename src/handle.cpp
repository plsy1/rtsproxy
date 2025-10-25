#include "../include/handle.h"
#include "../include/logger.h"
#include "../include/epoll_loop.h"
#include "../include/rtsp_client.h"
#include "../include/buffer_pool.h"
#include "../include/common/rtsp_client.h"
#include <unistd.h>
#include <arpa/inet.h> 

bool parse_http_url(const std::string &url, std::string &host, int &port, std::string &path)
{
    if (url.find("/rtp/") != 0)
        return false;
    std::string s = url.substr(5);
    size_t colon = s.find(':');
    size_t slash = s.find('/');
    if (colon == std::string::npos || slash == std::string::npos)
        return false;
    host = s.substr(0, colon);
    port = std::stoi(s.substr(colon + 1, slash - colon - 1));
    path = s.substr(slash + 1);
    return true;
}

void handle_http_request(int client_fd, sockaddr_in client_addr, EpollLoop *loop, BufferPool &pool)
{
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        if (n == 0)
        {

            Logger::debug("Client disconnected gracefully (EOF).");
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                Logger::debug("Resource temporarily unavailable: No data to read, retrying later.");
            }
            else
            {
                Logger::debug(std::string("Client request receive failed: ") + strerror(errno));
            }
        }

        close(client_fd);
        return;
    }

    buf[n] = 0;

    std::string req(buf);
    std::string url;
    if (req.find("GET ") == 0)
    {
        size_t pos1 = 4;
        size_t pos2 = req.find(' ', pos1);
        if (pos2 != std::string::npos)
            url = req.substr(pos1, pos2 - pos1);
    }

    std::string host, path;
    int port;
    if (!parse_http_url(url, host, port, path))
    {

        Logger::error(std::string("Failed to parse http url: ") + url);
        close(client_fd);
        return;
    }

    std::string rtsp_url = "rtsp://" + host + ":" + std::to_string(port) + "/" + path;

    std::string client_host = std::string(std::string(inet_ntoa(client_addr.sin_addr)) + ":" +
                                          std::to_string(ntohs(client_addr.sin_port)));

    Logger::info("New http client request: " + client_host + " -> " + rtsp_url);

    RTSPClientCtx info{
        .rtsp_url = rtsp_url,
        .client_fd = client_fd,
        .client_addr = client_addr};

    RTSPClient *client = new RTSPClient(info, loop, pool);

    client->set_on_closed_callback([client, client_host, loop]()
                                   {
    Logger::info("Client disconnect: " + client_host);
    loop->add_task([client]()
                {delete client;}
                                ); });
}
