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
#include <algorithm>
#include <cctype>

namespace
{
constexpr size_t kMaxInitialRequestSize = 64 * 1024;

void close_client(EpollLoop *loop, int client_fd)
{
    if (loop)
        loop->remove(client_fd);
    close(client_fd);
}

bool get_initial_message_size(const std::string &buffer, size_t header_end,
                              size_t &message_size)
{
    const size_t headers_size = header_end + 4;
    std::string headers = buffer.substr(0, headers_size);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    size_t body_size = 0;
    size_t pos = headers.find("content-length:");
    while (pos != std::string::npos &&
           pos != 0 &&
           !(pos >= 2 && headers[pos - 2] == '\r' && headers[pos - 1] == '\n'))
    {
        pos = headers.find("content-length:", pos + 1);
    }
    if (pos != std::string::npos)
    {
        pos += sizeof("content-length:") - 1;
        while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t'))
            ++pos;
        size_t end = headers.find_first_of("\r\n", pos);
        try
        {
            size_t parsed = 0;
            std::string value = headers.substr(pos, end - pos);
            body_size = std::stoull(value, &parsed);
            while (parsed < value.size() &&
                   (value[parsed] == ' ' || value[parsed] == '\t'))
                ++parsed;
            if (parsed != value.size())
                return false;
        }
        catch (...)
        {
            return false;
        }
    }

    if (headers_size > kMaxInitialRequestSize ||
        body_size > kMaxInitialRequestSize - headers_size)
        return false;

    message_size = headers_size + body_size;
    return true;
}
}

void MasterHandle::handle(int client_fd, sockaddr_in client_addr, EpollLoop *loop,
                          BufferPool &pool, std::string &request_buffer,
                          uint32_t events)
{
    if (client_fd < 0) return;

    char buf[4096];
    bool peer_closed = false;
    while (true)
    {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            request_buffer.append(buf, static_cast<size_t>(n));
            if (request_buffer.size() > kMaxInitialRequestSize)
            {
                Logger::warn("[MASTER] Initial request exceeds 64 KiB limit");
                close_client(loop, client_fd);
                return;
            }
            continue;
        }
        if (n == 0)
        {
            peer_closed = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;

        close_client(loop, client_fd);
        return;
    }

    size_t header_end = request_buffer.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        if (peer_closed || (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)))
            close_client(loop, client_fd);
        return;
    }

    size_t message_size = 0;
    if (!get_initial_message_size(request_buffer, header_end, message_size))
    {
        Logger::warn("[MASTER] Invalid Content-Length in initial request");
        close_client(loop, client_fd);
        return;
    }
    if (request_buffer.size() < message_size)
    {
        if (peer_closed)
            close_client(loop, client_fd);
        return;
    }

    std::string req = request_buffer;
    
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
        send(client_fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        close_client(loop, client_fd);
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
    Logger::debug("[MASTER] No handler found for " + client_host + " request: " + info.clean_uri);
    close_client(loop, client_fd);
}
