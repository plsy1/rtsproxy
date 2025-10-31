#include "../include/http_handle.h"
#include "../include/logger.h"
#include "../include/epoll_loop.h"
#include "../include/rtsp_client.h"
#include "../include/buffer_pool.h"
#include "../include/http_parser.h"
#include "../include/server_config.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <string>

void send_unauthorized(int client_fd)
{
    std::string response = "HTTP/1.1 401 Unauthorized\r\n"
                           "Content-Type: text/html\r\n"
                           "WWW-Authenticate: Basic realm=\"Restricted\"\r\n"
                           "\r\n"
                           "<html><body><h1>401 Unauthorized</h1></body></html>";
    send(client_fd, response.c_str(), response.size(), 0);
    close(client_fd);
}

std::map<std::string, std::string> parse_query_params(const std::string &query)
{
    std::map<std::string, std::string> params;
    std::stringstream ss(query);
    std::string kv;
    while (std::getline(ss, kv, '&'))
    {
        size_t eq_pos = kv.find('=');
        if (eq_pos != std::string::npos)
        {
            std::string key = kv.substr(0, eq_pos);
            std::string value = kv.substr(eq_pos + 1);
            params[key] = value;
        }
    }
    return params;
}

void handle_http_request(int client_fd, sockaddr_in client_addr, EpollLoop *loop, BufferPool &pool)
{
    if (client_fd < 0)
    {
        Logger::error("[SERVER] Received request on invalid file descriptor");
        return;
    }

    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);

    if (n == 0)
    {
        Logger::debug("[SERVER] Client disconnected gracefully (EOF).");
        close(client_fd);
        return;
    }
    else if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            Logger::debug("[SERVER] No data available yet, will retry later.");
            return;
        }
        else if (errno == EBADF)
        {
            Logger::error("[SERVER] Received data from an invalid file descriptor.");
            return;
        }
        else
        {
            Logger::error("[SERVER] Client request receive failed: " + std::string(strerror(errno)));
            close(client_fd);
            return;
        }
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

    std::string token;
    size_t qpos = url.find('?');
    if (qpos != std::string::npos)
    {
        std::string query_str = url.substr(qpos + 1);
        auto params = parse_query_params(query_str);
        auto it = params.find("token");

        if (it != params.end())
        {
            token = it->second;
            Logger::debug(token);
        }
    }

    if (!ServerConfig::getToken().empty())
    {
        if (token.empty())
        {
            Logger::debug("[SERVER] Token missing in request");
            send_unauthorized(client_fd);
            return;
        }

        if (token != ServerConfig::getToken())
        {
            Logger::debug("[SERVER] Invalid token");
            send_unauthorized(client_fd);
            return;
        }
    }

    std::string rtsp_url;

    if (!httpParser::parse_http_url(url, rtsp_url))
    {

        Logger::error(std::string("[SERVER] Failed to parse http url: ") + url);
        close(client_fd);
        return;
    }

    std::string client_host = std::string(std::string(inet_ntoa(client_addr.sin_addr)) + ":" +
                                          std::to_string(ntohs(client_addr.sin_port)));

    Logger::info("[SERVER] New http client request: " + client_host + " -> " + rtsp_url);

    auto client = std::make_unique<RTSPClient>(loop, pool, client_addr, client_fd, rtsp_url);

    loop->add_client_to_map(client_fd, std::move(client));

    loop->get_client_from_map(client_fd)->set_on_closed_callback([&client_fd, &loop, client_host]()
                                                                 {
    Logger::info("[SERVER] Client disconnect: " + client_host);
    loop->add_task([&client_fd, &loop]()
    {
        loop->remove_client_from_map(client_fd);
    }); });
}
