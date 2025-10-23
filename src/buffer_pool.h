#pragma once

#include <vector>
#include <memory>
#include <cstring>

class BufferPool
{

public:
    BufferPool(size_t buf_size, size_t pool_size)
        : buf_size_(buf_size)
    {
        for (size_t i = 0; i < pool_size; ++i)
        {
            pool_.push_back(std::make_unique<uint8_t[]>(buf_size_));
        }
    }

    std::unique_ptr<uint8_t[]> acquire()
    {
        if (pool_.empty())
            return std::make_unique<uint8_t[]>(buf_size_);
        auto buf = std::move(pool_.back());
        pool_.pop_back();
        return buf;
    }

    void release(std::unique_ptr<uint8_t[]> buf)
    {
        pool_.push_back(std::move(buf));
    }

private:
    size_t buf_size_;
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