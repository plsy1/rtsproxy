#pragma once

#include <vector>
#include <memory>
#include <cstring>

class BufferPool
{

public:
    BufferPool(size_t buf_size, size_t pool_size)
        : buf_size_(buf_size), total_allocated_(pool_size)
    {
        for (size_t i = 0; i < pool_size; ++i)
        {
            pool_.push_back(std::make_unique<uint8_t[]>(buf_size_));
        }
    }

    std::unique_ptr<uint8_t[]> acquire()
    {
        if (pool_.empty()) {
            total_allocated_++;
            size_t used = total_allocated_;
            if (used > peak_used_) peak_used_ = used;
            return std::make_unique<uint8_t[]>(buf_size_);
        }
        auto buf = std::move(pool_.back());
        pool_.pop_back();
        
        size_t used = total_allocated_ - pool_.size();
        if (used > peak_used_) peak_used_ = used;
        
        return buf;
    }

    void release(std::unique_ptr<uint8_t[]> buf)
    {
        pool_.push_back(std::move(buf));
    }

    size_t get_available_count() const { return pool_.size(); }
    size_t get_total_allocated() const { return total_allocated_; }
    size_t get_buffer_size() const { return buf_size_; }
    size_t get_peak_used() const { return peak_used_; }

private:
    size_t buf_size_;
    size_t total_allocated_;
    size_t peak_used_ = 0;
    std::vector<std::unique_ptr<uint8_t[]>> pool_;
};

struct Packet
{
    std::unique_ptr<uint8_t[]> data;
    size_t length;
    size_t offset = 0;

    Packet(std::vector<uint8_t> &&data, size_t length, size_t offset)
        : data(std::make_unique<uint8_t[]>(length)), length(length), offset(offset)
    {
        std::memcpy(this->data.get(), data.data(), length);
    }

    Packet(std::unique_ptr<uint8_t[]> &&data, size_t length, size_t offset)
        : data(std::move(data)), length(length), offset(offset) {}
};