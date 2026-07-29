#include "handlers/api_handle.h"
#include "core/statistics.h"
#include "core/logger.h"
#include "common/socket_ctx.h"
#include "3rd/json.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <memory>
#include <cerrno>

using json = nlohmann::json;

namespace
{
void send_response(EpollLoop *loop, int client_fd, std::string response)
{
    auto data = std::make_shared<std::string>(std::move(response));
    auto offset = std::make_shared<size_t>(0);
    auto ctx = std::make_unique<SocketCtx>();
    ctx->fd = client_fd;
    ctx->handler = [loop, client_fd, data, offset](uint32_t events)
    {
        auto close_connection = [loop, client_fd]()
        {
            loop->remove(client_fd);
            close(client_fd);
        };

        if (events & (EPOLLHUP | EPOLLERR))
        {
            close_connection();
            return;
        }

        while (*offset < data->size())
        {
            ssize_t n = send(client_fd, data->data() + *offset,
                             data->size() - *offset, MSG_NOSIGNAL);
            if (n > 0)
            {
                *offset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            close_connection();
            return;
        }

        close_connection();
    };

    loop->set(std::move(ctx), client_fd, EPOLLOUT | EPOLLHUP | EPOLLERR);
}
}

bool ApiHandle::dispatch(int client_fd, const RequestInfo &info, EpollLoop *loop, BufferPool &pool)
{
    const std::string &path = info.clean_uri;

    // 1. Route match checks
    bool is_api = (path.find("/api/") == 0);
    bool is_admin = (path == "/admin" || path.find("/admin/") == 0);
    bool is_favicon = (path == "/favicon.ico");

    if (!is_api && !is_admin && !is_favicon) return false;

    // 2. Authorization check
    size_t query_pos = path.find('?');
    std::string route_path = path.substr(0, query_pos);
    bool is_static = (route_path == "/admin/main.js" ||
                      route_path == "/admin/style.css" ||
                      route_path == "/favicon.ico");

    if (!info.is_authorized && !is_static)
    {
        Logger::debug("[SERVER] Unauthorized admin access: " + path);
        send_unauthorized(client_fd, loop);
        return true;
    }

    // 3. Handle specific routes
    if (is_favicon)
    {
        std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send_response(loop, client_fd, std::move(resp));
        return true;
    }

    if (is_api)
    {
        if (route_path == "/api/status")
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
            status["stats"]["active_clients"] = stats.getActiveClients();
            status["clients"] = loop->get_all_clients_info();
            
            send_json_response(client_fd, status, loop);
            return true;
        }
        
        if (route_path == "/api/logs")
        {
            json response;
            response["logs"] = Logger::getRecentLogs();
            response["level"] = (int)Logger::getLogLevel();
            send_json_response(client_fd, response, loop);
            return true;
        }

        std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send_response(loop, client_fd, std::move(resp));
        return true;
    }

    if (is_admin)
    {
        serve_admin_file(client_fd, info, loop);
        return true;
    }

    return false;
}

void ApiHandle::serve_admin_file(int client_fd, const RequestInfo &info, EpollLoop *loop)
{
    std::string clean_path = info.clean_uri;
    size_t query_pos = clean_path.find('?');
    if (query_pos != std::string::npos)
        clean_path.erase(query_pos);

    if (clean_path == "/admin")
    {
        // Redirect from the raw URI so the token survives the hop
        std::string query;
        size_t raw_query_pos = info.raw_uri.find('?');
        if (raw_query_pos != std::string::npos)
            query = info.raw_uri.substr(raw_query_pos);

        std::string response = "HTTP/1.1 301 Moved Permanently\r\n"
                               "Location: /admin/" + query + "\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        send_response(loop, client_fd, std::move(response));
        return;
    }

    if (clean_path == "/admin/") clean_path = "/admin/index.html";

    std::filesystem::path relative_path(clean_path.substr(7));
    if (relative_path.empty() || relative_path.is_absolute())
    {
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_response(loop, client_fd, std::move(response));
        return;
    }
    for (const auto &component : relative_path)
    {
        if (component == ".." || component == ".")
        {
            Logger::warn("[SERVER] Rejected unsafe admin path: " + clean_path);
            std::string response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_response(loop, client_fd, std::move(response));
            return;
        }
    }
    
    // Path resolution
    std::string rel_path = (std::filesystem::path("webui") / relative_path).string();
    std::string sys_path = (std::filesystem::path("/usr/share/rtsproxy/www") / relative_path).string();
    std::string local_path = (access(rel_path.c_str(), F_OK) == 0) ? rel_path : sys_path;
    
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open())
    {
        Logger::error("[SERVER] Admin file not found: " + local_path);
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send_response(loop, client_fd, std::move(response));
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
    send_response(loop, client_fd, header + content);
}

void ApiHandle::send_json_response(int client_fd, const json &j, EpollLoop *loop)
{
    std::string body;
    try
    {
        body = j.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    catch (const std::exception &e)
    {
        Logger::error("[SERVER] Failed to dump JSON: " + std::string(e.what()));
        body = "{\"error\":\"Internal Server Error: JSON serialization failed\"}";
    }
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
                         "Connection: close\r\n"
                         "\r\n";
    send_response(loop, client_fd, header + body);
}

void ApiHandle::send_unauthorized(int client_fd, EpollLoop *loop)
{
    std::string body =
                           "<html><head><title>401 Unauthorized</title></head>"
                           "<body style=\"font-family:sans-serif;text-align:center;padding-top:50px;\">"
                           "<h1>401 Unauthorized</h1>"
                           "<p>Invalid or missing access token.</p>"
                           "<p>Usage: <code>/admin/?token=YOUR_TOKEN</code></p>"
                           "</body></html>";
    std::string response = "HTTP/1.1 401 Unauthorized\r\n"
                           "Content-Type: text/html\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                           "Connection: close\r\n"
                           "\r\n" + body;
    send_response(loop, client_fd, std::move(response));
}

std::string ApiHandle::get_mime_type(const std::string &path)
{
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".ico") != std::string::npos) return "image/x-icon";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}
