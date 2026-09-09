#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : buffer_(capacity){}
    bool push(const T& item){
        const size_t cur_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (cur_tail+1)%buffer_.size();
        if(next_tail==head_.load(std::memory_order_acquire)){
            return false;
        }
        buffer_[cur_tail] = item;                          // 1. 先写数据
        tail_.store(next_tail, std::memory_order_release); // 2. 再发布 tail
        return true;
    }


    bool pop(T& item){
        const size_t cur_head=head_.load(std::memory_order_relaxed);
        if(cur_head==tail_.load(std::memory_order_acquire)){
            return false;
        }
        item=buffer_[cur_head];
        head_.store((cur_head+1)%buffer_.size(),std::memory_order_release);
        return true;
    }
    size_t capacity() const{return buffer_.size();}
private:
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};