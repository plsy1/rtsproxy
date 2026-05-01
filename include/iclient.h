#pragma once

#include <functional>
#include <chrono>
#include <atomic>
#include "3rd/json.hpp"

using json = nlohmann::json;

class BandwidthEstimator {
public:
    void addBytes(size_t bytes) {
        bytes_in_period_ += bytes;
    }

    double getBandwidth() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time_).count();
        if (duration >= 1000) {
            uint64_t bytes = bytes_in_period_.exchange(0);
            current_bandwidth_ = (double)bytes * 8.0 / (duration / 1000.0);
            last_update_time_ = now;
        }
        return current_bandwidth_;
    }

private:
    std::atomic<uint64_t> bytes_in_period_{0};
    double current_bandwidth_{0};
    std::chrono::steady_clock::time_point last_update_time_ = std::chrono::steady_clock::now();
};

/**
 * Common interface for all proxy client types (RTSPClient, RTSPMitmClient, …).
 * EpollLoop stores IClient pointers, avoiding a dependency on concrete types.
 */
class IClient
{
public:
    using ClosedCallback = std::function<void()>;

    virtual ~IClient() = default;
    virtual void set_on_closed_callback(ClosedCallback cb) = 0;
    virtual json get_info() const = 0;
    virtual bool is_closed() const = 0;
};
