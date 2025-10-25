#include "../include/rtsp_client.h"
#include "../include/epoll_loop.h"
#include "../include/common/socket_ctx.h"
#include "../include/logger.h"
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
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
    {
        Logger::error("epoll_ctl EPOLL_CTL_DEL failed");
    }
    else
    {
        // Logger::debug("Remove socket form epoll success");
    }
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