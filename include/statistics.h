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

    void addBytes(size_t bytes) {
        total_bytes_ += bytes;
        current_period_bytes_ += bytes;
    }

    void setActiveClients(size_t count) {
        active_clients_ = count;
    }

    uint64_t getTotalBytes() const { return total_bytes_; }
    size_t getActiveClients() const { return active_clients_; }
    
    // Returns bandwidth in bytes per second
    double getBandwidth() {
        updateBandwidth();
        return current_bandwidth_;
    }

private:
    Statistics() : total_bytes_(0), current_period_bytes_(0), active_clients_(0), current_bandwidth_(0) {
        last_update_time_ = std::chrono::steady_clock::now();
    }

    void updateBandwidth() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time_).count();
        
        if (duration >= 1000) { // Update every second
            uint64_t bytes = current_period_bytes_.exchange(0);
            current_bandwidth_ = (double)bytes / (duration / 1000.0);
            last_update_time_ = now;
        }
    }

    std::atomic<uint64_t> total_bytes_;
    std::atomic<uint64_t> current_period_bytes_;
    std::atomic<size_t> active_clients_;
    
    double current_bandwidth_;
    std::chrono::steady_clock::time_point last_update_time_;
};
