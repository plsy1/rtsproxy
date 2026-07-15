#pragma once

#include "protocol/request_parser.h"
#include "core/epoll_loop.h"
#include "core/buffer_pool.h"
#include <string>

class ApiHandle
{
public:
    /**
     * Centralized dispatcher for administrative tasks:
     * - paths under /api/
     * - paths under /admin/
     * - /favicon.ico
     * Also handles unauthorized access for these paths.
     * @return true if handled, false if it's a streaming request.
     */
    static bool dispatch(int client_fd, const RequestInfo &info, EpollLoop *loop, BufferPool &pool);

private:
    static void serve_admin_file(int client_fd, const RequestInfo &info, EpollLoop *loop);
    static void send_json_response(int client_fd, const nlohmann::json &j, EpollLoop *loop);
    static void send_unauthorized(int client_fd, EpollLoop *loop);
    static std::string get_mime_type(const std::string &path);
};
