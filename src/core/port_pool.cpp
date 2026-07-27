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

void PortPool::purge_expired_marks()
{
    auto now = std::chrono::steady_clock::now();
    for (auto it = occupied_marks_.begin(); it != occupied_marks_.end();)
    {
        if (now - it->second < kOccupiedTtl)
        {
            ++it;
            continue;
        }

        // Never un-block a port that is now part of a live allocation: the mark
        // and the allocation are independent, and dropping it from used_ports_
        // here would let the pair be handed out a second time.
        uint16_t base = static_cast<uint16_t>(it->first & ~1u);
        if (allocated_bases_.find(base) == allocated_bases_.end())
            used_ports_.erase(it->first);

        it = occupied_marks_.erase(it);
    }
}

uint16_t PortPool::acquire_pair()
{
    std::lock_guard<std::mutex> lock(mutex_);

    purge_expired_marks();

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

    // A half of this pair may also be blocked because a foreign process holds
    // it. Releasing the allocation must not silently clear that block.
    auto now = std::chrono::steady_clock::now();
    for (uint16_t p : {port, static_cast<uint16_t>(port + 1)})
    {
        auto it = occupied_marks_.find(p);
        if (it == occupied_marks_.end())
            continue;
        if (now - it->second < kOccupiedTtl)
            used_ports_.insert(p);
        else
            occupied_marks_.erase(it);
    }
}

void PortPool::mark_occupied(uint16_t port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    used_ports_.insert(port);
    // Re-stamping on every failed bind keeps a port that is still genuinely
    // taken blocked, while one that was only briefly busy ages out.
    occupied_marks_[port] = std::chrono::steady_clock::now();
}
