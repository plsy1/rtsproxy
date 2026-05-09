#pragma once

#include <sys/epoll.h>
#include <vector>
#include <queue>
#include <mutex>
#include <map>
#include <memory>
#include <functional>
#include "core/iclient.h"

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

    /**
     * Defer deletion of a SocketCtx until the end of the current event loop cycle.
     * This prevents bad_function_call crashes if the context is being processed.
     */
    void defer_delete(std::unique_ptr<SocketCtx> ctx);

    static void set_non_blocking(int fd);

    IClient *get_client_from_map(int client_fd);

    void add_client_to_map(int client_fd, std::unique_ptr<IClient> client);

    void remove_client_from_map(int client_fd);
    size_t get_client_count() const { return client_ptr_map.size(); }
    json get_all_clients_info() const;

private:
    int epfd_;
    int max_events_;
    std::vector<struct epoll_event> events_;
    std::unordered_map<int, std::unique_ptr<SocketCtx>> ctx_ptr_map;
    std::unordered_map<int, std::unique_ptr<IClient>> client_ptr_map;
    std::vector<std::unique_ptr<SocketCtx>> deferred_delete_ctx_;

    // Queue to hold tasks
    std::queue<std::function<void()>> task_queue_;
    std::mutex task_queue_mutex_; // Mutex to protect task queue
};