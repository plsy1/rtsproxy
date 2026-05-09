#include "core/iclient.h"
#include "core/epoll_loop.h"
#include "common/socket_ctx.h"
#include "core/logger.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

EpollLoop::EpollLoop(int max_events) : max_events_(max_events)
{
    epfd_ = epoll_create1(0);
    if (epfd_ < 0)
    {
        Logger::error("epoll_create1 failed");
        exit(1);
    }
    events_.resize(max_events_);
}

EpollLoop::~EpollLoop()
{
    if (epfd_ >= 0)
        close(epfd_);
}

void EpollLoop::add_task(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    task_queue_.push(task);
}

void EpollLoop::process_tasks()
{
    std::lock_guard<std::mutex> lock(task_queue_mutex_);

    while (!task_queue_.empty())
    {
        auto task = task_queue_.front();
        task_queue_.pop();
        task();
    }
}

void EpollLoop::set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollLoop::remove(int fd)
{
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT)
    {
        Logger::error("epoll_ctl EPOLL_CTL_DEL failed: " + std::string(strerror(errno)));
    }

    ctx_ptr_map.erase(fd);
}

void EpollLoop::set(SocketCtx *ctx, int fd, uint32_t events)
{

    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = ctx;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        if (errno == EEXIST)
        {
            if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0)
            {
                Logger::error(std::string("epoll_ctl EPOLL_CTL_MOD failed: ") + strerror(errno));
            }
        }
        else
        {
            Logger::error(std::string("epoll_ctl EPOLL_CTL_ADD failed: ") + strerror(errno));
        }
    }
}

void EpollLoop::set(std::unique_ptr<SocketCtx> ctx, int fd, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = ctx.get();

    ctx_ptr_map[fd] = std::move(ctx);

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        if (errno == EEXIST)
        {
            if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0)
            {
                Logger::error(std::string("epoll_ctl EPOLL_CTL_MOD failed: ") + strerror(errno));
            }
        }
        else
        {
            Logger::error(std::string("epoll_ctl EPOLL_CTL_ADD failed: ") + strerror(errno));
        }
    }
}

void EpollLoop::loop(int timeout_ms)
{
    while (true)
    {
        int n = epoll_wait(epfd_, events_.data(), max_events_, timeout_ms);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            Logger::error("epoll_wait failed");
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            SocketCtx *ctx = static_cast<SocketCtx *>(events_[i].data.ptr);
            if (!ctx)
                continue;
            ctx->handler(events_[i].events);
        }
        process_tasks();
    }
}

IClient *EpollLoop::get_client_from_map(int client_fd)
{
    auto it = client_ptr_map.find(client_fd);
    if (it != client_ptr_map.end())
    {
        return it->second.get();
    }
    return nullptr;
}

void EpollLoop::add_client_to_map(int client_fd, std::unique_ptr<IClient> client)
{
    auto it = client_ptr_map.find(client_fd);
    if (it != client_ptr_map.end())
    {
        Logger::warn("Client FD already exists, skipping add: " + std::to_string(client_fd));
        return;
    }

    client_ptr_map[client_fd] = std::move(client);
}

void EpollLoop::remove_client_from_map(int client_fd)
{
    client_ptr_map.erase(client_fd);
}
json EpollLoop::get_all_clients_info() const
{
    json clients = json::array();
    for (auto const& [fd, client] : client_ptr_map) {
        if (client && !client->is_closed()) {
            clients.push_back(client->get_info());
        }
    }
    return clients;
}
