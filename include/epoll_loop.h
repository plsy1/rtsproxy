#pragma once

#include <sys/epoll.h>
#include <vector>
#include <functional>
#include <queue>
#include <mutex>

class RTSPClient;
class SocketCtx;

class EpollLoop
{
public:
    EpollLoop(int max_events = 64);
    ~EpollLoop();
    void add_task(std::function<void()> task);
    void process_tasks();
    void set(SocketCtx *ctx, int fd, uint32_t events);
    void remove(int fd);
    void loop(int timeout_ms = -1);

    static void set_non_blocking(int fd);

private:
    int epfd_;
    int max_events_;
    std::vector<struct epoll_event> events_;

    // Queue to hold tasks
    std::queue<std::function<void()>> task_queue_;
    std::mutex task_queue_mutex_; // Mutex to protect task queue
};