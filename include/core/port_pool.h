#pragma once

#include <stdint.h>
#include <mutex>
#include <set>

/**
 * PortPool manages a range of UDP ports to ensure that RTP/RTCP
 * port pairs (even, odd) are allocated without internal collisions.
 */
class PortPool
{
public:
    static PortPool& getInstance();

    /**
     * Acquire a pair of consecutive ports (even, even+1).
     * Returns the even port, or 0 if no ports are available.
     */
    uint16_t acquire_pair();

    /**
     * Release a pair of ports starting with 'port'.
     */
    void release_pair(uint16_t port);

    /**
     * Mark a port as externally occupied (failed to bind).
     * This port will be skipped for a while.
     */
    void mark_occupied(uint16_t port);

private:
    PortPool();
    ~PortPool() = default;

    PortPool(const PortPool&) = delete;
    PortPool& operator=(const PortPool&) = delete;

    std::mutex mutex_;
    uint16_t start_port_{20000};
    uint16_t end_port_{40000};
    uint16_t next_port_{20000};
    
    std::set<uint16_t> used_ports_;
};
