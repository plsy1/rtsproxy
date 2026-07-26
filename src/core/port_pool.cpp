#include "core/port_pool.h"
#include "core/logger.h"

PortPool& PortPool::getInstance()
{
    static PortPool instance;
    return instance;
}

PortPool::PortPool()
{
    // Initialize with standard range
    next_port_ = start_port_;
}

uint16_t PortPool::acquire_pair()
{
    std::lock_guard<std::mutex> lock(mutex_);

    uint16_t attempts = 0;
    uint16_t total_ports = end_port_ - start_port_;

    while (attempts < total_ports / 2)
    {
        uint16_t p = next_port_;
        
        // Ensure even
        if (p % 2 != 0) p++;
        if (p >= end_port_) p = start_port_;

        next_port_ = p + 2;
        if (next_port_ >= end_port_) next_port_ = start_port_;

        if (used_ports_.find(p) == used_ports_.end() && 
            used_ports_.find(p + 1) == used_ports_.end())
        {
            used_ports_.insert(p);
            used_ports_.insert(p + 1);
            allocated_bases_.insert(p);
            return p;
        }

        attempts++;
    }

    Logger::error("[PortPool] No available port pairs in range " + 
                 std::to_string(start_port_) + "-" + std::to_string(end_port_));
    return 0;
}

void PortPool::release_pair(uint16_t port)
{
    if (port == 0) return;
    
    std::lock_guard<std::mutex> lock(mutex_);

    // Only the even RTP port of a pair owns the pair. Releasing the odd half
    // would free one port of the pair below and one of the pair above, handing
    // a live pair to a second session.
    if (port % 2 != 0)
    {
        Logger::warn("[PortPool] Ignoring release of odd port " + std::to_string(port) +
                     ", expected the even RTP port of the pair");
        return;
    }

    if (allocated_bases_.erase(port) == 0)
    {
        Logger::warn("[PortPool] Ignoring release of port pair " + std::to_string(port) +
                     ", not allocated by this pool");
        return;
    }

    // port is even here, so port + 1 cannot exceed UINT16_MAX and wrap to 0.
    used_ports_.erase(port);
    used_ports_.erase(static_cast<uint16_t>(port + 1));
}

void PortPool::mark_occupied(uint16_t port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    used_ports_.insert(port);
    // We don't automatically release it, but it will be skipped.
    // In a real system, we might want a timer to clear these, 
    // but for now, we just skip it to avoid immediate re-collision.
}
