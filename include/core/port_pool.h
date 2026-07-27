#pragma once

#include <stdint.h>
#include <chrono>
#include <map>
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
     * 'port' must be the even port returned by a previous acquire_pair();
     * anything else (odd port, foreign port, double release) is ignored.
     */
    void release_pair(uint16_t port);

    /**
     * Mark a port as externally occupied (failed to bind).
     * The port is skipped until the mark expires, so a process that transiently
     * holds a port cannot permanently shrink the range.
     */
    void mark_occupied(uint16_t port);

private:
    PortPool();
    ~PortPool() = default;

    PortPool(const PortPool&) = delete;
    PortPool& operator=(const PortPool&) = delete;

    // Caller must hold mutex_.
    void purge_expired_marks();

    // Long enough to stop retrying a port that something else really is
    // sitting on, short enough that the range recovers on its own.
    static constexpr std::chrono::seconds kOccupiedTtl{300};

    std::mutex mutex_;
    uint16_t start_port_{20000};
    uint16_t end_port_{40000};
    uint16_t next_port_{20000};

    std::set<uint16_t> used_ports_;
    // Even ports handed out by acquire_pair() and not yet released.
    std::set<uint16_t> allocated_bases_;
    // Ports blocked because some other process holds them, and when the block
    // was placed. Distinct from allocated_bases_ so that expiry can never free
    // a port that this pool has genuinely handed out.
    std::map<uint16_t, std::chrono::steady_clock::time_point> occupied_marks_;
};
