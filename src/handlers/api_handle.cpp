#include "handlers/api_handle.h"
#include "core/statistics.h"
#include "core/logger.h"
#include "3rd/json.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

bool ApiHandle::dispatch(int client_fd, const RequestInfo &info, EpollLoop *loop, BufferPool &pool)
{
    const std::string &path = info.clean_uri;

    // 1. Route match checks
    bool is_api = (path.find("/api/") == 0);
    bool is_admin = (path.find("/admin") == 0);
    bool is_favicon = (path == "/favicon.ico");

    if (!is_api && !is_admin && !is_favicon) return false;

    // 2. Authorization check
    bool is_static = (path.find(".js") != std::string::npos || 
                      path.find(".css") != std::string::npos || 
                      path.find(".ico") != std::string::npos);

    if (!info.is_authorized && !is_static)
    {
        Logger::debug("[SERVER] Unauthorized admin access: " + path);
        send_unauthorized(client_fd);
        return true;
    }

    // 3. Handle specific routes
    if (is_favicon)
    {
        std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
        close(client_fd);
        return true;
    }

    if (is_api)
    {
        if (path.find("/api/status") == 0)
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
            
            send_json_response(client_fd, status);
            return true;
        }
        
        if (path.find("/api/logs") == 0)
        {
            json response;
            response["logs"] = Logger::getRecentLogs();
            response["level"] = (int)Logger::getLogLevel();
            send_json_response(client_fd, response);
            return true;
        }
    }

    if (is_admin)
    {
        serve_admin_file(client_fd, info);
        return true;
    }

    return false;
}

void ApiHandle::serve_admin_file(int client_fd, const RequestInfo &info)
{
    std::string clean_path = info.clean_uri;

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
    
    // Path resolution
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

void ApiHandle::send_json_response(int client_fd, const json &j)
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

void ApiHandle::send_unauthorized(int client_fd)
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

std::string ApiHandle::get_mime_type(const std::string &path)
{
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".ico") != std::string::npos) return "image/x-icon";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}
