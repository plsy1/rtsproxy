#pragma once

#include <sys/epoll.h>
#include <vector>
#include <queue>
#include <mutex>
#include <map>
#include <memory>
#include <functional>

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
    void set(std::unique_ptr<SocketCtx> ctx, int fd, uint32_t events);
    void remove(int fd);
    void loop(int timeout_ms = -1);

    static void set_non_blocking(int fd);

    RTSPClient *get_client_from_map(int client_fd);

    void add_client_to_map(int client_fd, std::unique_ptr<RTSPClient> client);

    void remove_client_from_map(int client_fd);

private:
    int epfd_;
    int max_events_;
    std::vector<struct epoll_event> events_;
    std::unordered_map<int, std::unique_ptr<SocketCtx>> ctx_ptr_map;
    std::unordered_map<int, std::unique_ptr<RTSPClient>> client_ptr_map;

    // Queue to hold tasks
    std::queue<std::function<void()>> task_queue_;
    std::mutex task_queue_mutex_; // Mutex to protect task queue
};