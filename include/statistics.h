#pragma once

#include <atomic>
#include <chrono>
#include <string>

class Statistics {
public:
    static Statistics& getInstance() {
        static Statistics instance;
        return instance;
    }

    void addUpstreamBytes(size_t bytes) {
        total_upstream_bytes_ += bytes;
        upstream_period_bytes_ += bytes;
    }

    void addDownstreamBytes(size_t bytes) {
        total_downstream_bytes_ += bytes;
        downstream_period_bytes_ += bytes;
    }

    void setActiveClients(size_t count) {
        active_clients_ = count;
    }

    uint64_t getTotalUpstreamBytes() const { return total_upstream_bytes_; }
    uint64_t getTotalDownstreamBytes() const { return total_downstream_bytes_; }
    uint64_t getTotalBytes() const { return total_upstream_bytes_ + total_downstream_bytes_; }
    size_t getActiveClients() const { return active_clients_; }
    
    double getUpstreamBandwidth() {
        updateBandwidth();
        return upstream_bandwidth_;
    }

    double getDownstreamBandwidth() {
        updateBandwidth();
        return downstream_bandwidth_;
    }

private:
    Statistics() : total_upstream_bytes_(0), total_downstream_bytes_(0), 
                   upstream_period_bytes_(0), downstream_period_bytes_(0),
                   active_clients_(0), upstream_bandwidth_(0), downstream_bandwidth_(0) {
        last_update_time_ = std::chrono::steady_clock::now();
    }

    void updateBandwidth() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time_).count();
        
        if (duration >= 1000) { // Update every second
            uint64_t up_bytes = upstream_period_bytes_.exchange(0);
            uint64_t down_bytes = downstream_period_bytes_.exchange(0);
            
            upstream_bandwidth_ = (double)up_bytes / (duration / 1000.0);
            downstream_bandwidth_ = (double)down_bytes / (duration / 1000.0);
            
            last_update_time_ = now;
        }
    }

    std::atomic<uint64_t> total_upstream_bytes_;
    std::atomic<uint64_t> total_downstream_bytes_;
    std::atomic<uint64_t> upstream_period_bytes_;
    std::atomic<uint64_t> downstream_period_bytes_;
    std::atomic<size_t> active_clients_;
    
    double upstream_bandwidth_;
    double downstream_bandwidth_;
    std::chrono::steady_clock::time_point last_update_time_;
};
