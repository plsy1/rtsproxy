#include "../include/http_handle.h"
#include "../include/statistics.h"
#include "../include/logger.h"
#include "../include/epoll_loop.h"
#include "../include/rtsp_client.h"
#include "../include/iclient.h"
#include "../include/buffer_pool.h"
#include "../include/http_parser.h"
#include "../include/server_config.h"
#include "../include/3rd/json.hpp"
#include "../include/rtsp_parser.h"
#include "../include/blacklist_checker.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <string>

using json = nlohmann::json;

void send_json_response(int client_fd, const json &j)
{
    std::string body = j.dump();
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n"
                         "\r\n";
    send(client_fd, header.c_str(), header.size(), 0);
    send(client_fd, body.c_str(), body.size(), 0);
    close(client_fd);
}

std::string get_mime_type(const std::string &path)
{
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    return "text/plain";
}

void serve_admin_file(int client_fd, std::string path)
{
    // Strip query parameters
    size_t qpos = path.find('?');
    std::string clean_path = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

    if (clean_path == "/admin")
    {
        std::string response = "HTTP/1.1 301 Moved Permanently\r\n"
                               "Location: /admin/\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
        return;
    }

    if (clean_path == "/admin/") clean_path = "/admin/index.html";
    
    // Replace /admin with webui
    std::string rel_path = "webui" + clean_path.substr(6);
    std::string sys_path = "/usr/share/rtsproxy/www" + clean_path.substr(6);
    std::string local_path = (access(rel_path.c_str(), F_OK) == 0) ? rel_path : sys_path;
    
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open())
    {
        Logger::error("[SERVER] Admin file not found: " + local_path);
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
        return;
    }
    
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: " + get_mime_type(local_path) + "\r\n"
                         "Content-Length: " + std::to_string(content.size()) + "\r\n"
                         "Connection: close\r\n"
                         "\r\n";
    send(client_fd, header.c_str(), header.size(), 0);
    send(client_fd, content.c_str(), content.size(), 0);
    close(client_fd);
}

void send_unauthorized(int client_fd)
{
    std::string response = "HTTP/1.1 401 Unauthorized\r\n"
                           "Content-Type: text/html\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<html><head><title>401 Unauthorized</title></head>"
                           "<body style=\"font-family:sans-serif;text-align:center;padding-top:50px;\">"
                           "<h1>401 Unauthorized</h1>"
                           "<p>Invalid or missing access token.</p>"
                           "<p>Usage: <code>/admin/?token=YOUR_TOKEN</code></p>"
                           "</body></html>";
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
        }
    }

    // Allow static assets to load without token as they don't contain sensitive data.
    // This allows the WebUI to load its CSS/JS after the initial authorized index.html hit.
    bool is_static_asset = (url.find(".js") != std::string::npos || 
                            url.find(".css") != std::string::npos || 
                            url.find(".ico") != std::string::npos);

    if (!ServerConfig::getToken().empty() && !is_static_asset)
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

    if (url.find("/api/status") == 0)
    {
        json status;
        status["pool"]["available"] = pool.get_available_count();
        status["pool"]["allocated"] = pool.get_total_allocated();
        status["pool"]["used"] = pool.get_total_allocated() - pool.get_available_count();
        status["pool"]["peak"] = pool.get_peak_used();
        status["pool"]["buffer_size"] = pool.get_buffer_size();
        status["pool"]["total_bytes"] = pool.get_total_allocated() * pool.get_buffer_size();
        
        auto& stats = Statistics::getInstance();
        stats.setActiveClients(loop->get_client_count());
        status["stats"]["up_traffic"] = stats.getTotalUpstreamBytes();
        status["stats"]["down_traffic"] = stats.getTotalDownstreamBytes();
        status["stats"]["traffic"] = stats.getTotalBytes();
        status["stats"]["up_bandwidth"] = (uint64_t)stats.getUpstreamBandwidth();
        status["stats"]["down_bandwidth"] = (uint64_t)stats.getDownstreamBandwidth();
        status["stats"]["bandwidth"] = (uint64_t)stats.getDownstreamBandwidth(); // Keep for backward compatibility
        status["stats"]["active_clients"] = stats.getActiveClients();
        status["clients"] = loop->get_all_clients_info();
        
        send_json_response(client_fd, status);
        return;
    }
    
    if (url.find("/api/logs") == 0)
    {
        json response;
        response["logs"] = Logger::getRecentLogs();
        response["level"] = (int)Logger::getLogLevel();
        send_json_response(client_fd, response);
        return;
    }

    if (url.find("/admin") == 0)
    {
        serve_admin_file(client_fd, url);
        return;
    }

    if (url == "/favicon.ico")
    {
        std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
        close(client_fd);
        return;
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

    Logger::debug("[SERVER] New http client request: " + client_host + " -> " + rtsp_url);

    try
    {
        rtspCtx ctx;
        if (rtspParser::parse_url(rtsp_url, ctx) != 0)
        {
            throw std::runtime_error("Failed to parse RTSP URL: " + rtsp_url);
        }

        if (BlacklistChecker::is_blacklisted(ctx.server_ip))
        {
            throw std::runtime_error("Connection to upstream " + ctx.server_ip + " is forbidden by blacklist.");
        }

        // Loopback detection
        {
            struct sockaddr_in local_addr{};
            socklen_t addr_len = sizeof(local_addr);
            if (getsockname(client_fd, (struct sockaddr *)&local_addr, &addr_len) == 0)
            {
                std::string proxy_ip = inet_ntoa(local_addr.sin_addr);
                uint16_t proxy_port = ntohs(local_addr.sin_port);
                if ((ctx.server_ip == "127.0.0.1" || ctx.server_ip == "localhost" || ctx.server_ip == proxy_ip) &&
                    ctx.server_rtsp_port == proxy_port)
                {
                    throw std::runtime_error("Recursive connection detected: URL points back to proxy itself.");
                }
            }
        }

        std::unique_ptr<IClient> client = std::make_unique<RTSPClient>(loop, pool, client_addr, client_fd, ctx);
        loop->add_client_to_map(client_fd, std::move(client));

        loop->get_client_from_map(client_fd)->set_on_closed_callback([client_fd, loop, client_host]()
                                                                     {
            Logger::debug("[SERVER] Client disconnect: " + client_host);
            loop->add_task([client_fd, loop]()
            {
                loop->remove_client_from_map(client_fd);
            });
        });
    }
    catch (const std::exception &e)
    {
        Logger::error("[SERVER] Failed to initialize RTSP client for " + client_host + ": " + e.what());
        close(client_fd);
    }
}
