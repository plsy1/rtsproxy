#include "handlers/master_handle.h"
#include "handlers/api_handle.h"
#include "handlers/rtsp_to_http_handle.h"
#include "handlers/rtsp_to_rtsp_handle.h"
#include "core/epoll_loop.h"
#include "core/logger.h"
#include "protocol/request_parser.h"
#include <sys/socket.h>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>

void MasterHandle::handle(int client_fd, sockaddr_in client_addr, EpollLoop *loop, BufferPool &pool)
{
    if (client_fd < 0) return;

    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close(client_fd);
        return;
    }

    buf[n] = 0;
    std::string req(buf, n);
    
    // 1. Parse the first request to identify protocol, path, and auth.
    auto info = RequestParser::parse(req);
    std::string client_host = std::string(inet_ntoa(client_addr.sin_addr)) + ":" + std::to_string(ntohs(client_addr.sin_port));

    // 2. Common Authorization Check (except for Admin paths which may have their own logic)
    // Actually, ApiHandle::dispatch handles its own auth for /api and /admin.
    
    // 3. Dispatch to specific handles
    
    // --- Case A: API / Admin / WebUI ---
    if (ApiHandle::dispatch(client_fd, info, loop, pool)) {
        return; 
    }

    // --- Common Auth Check for Streaming ---
    if (!info.is_authorized) {
        Logger::warn("[MASTER] Unauthorized request from " + client_host + ": " + info.clean_uri);
        std::string resp = info.is_http ? 
            "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n" :
            "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
        close(client_fd);
        return;
    }

    // --- Case B: RTSP-to-HTTP Streaming ---
    if (RtspToHttpHandle::dispatch(client_fd, client_addr, info, loop, pool)) {
        return;
    }

    // --- Case C: RTSP-to-RTSP Proxy ---
    if (RtspToRtspHandle::dispatch(client_fd, client_addr, info, req, loop, pool)) {
        return;
    }

    // --- No handler matched ---
    Logger::error("[MASTER] No handler found for " + client_host + " request: " + info.clean_uri);
    close(client_fd);
}
