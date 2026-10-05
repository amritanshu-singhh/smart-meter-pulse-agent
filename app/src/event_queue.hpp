// event_queue.hpp - bounded thread-safe queue (mutex + condition variable)
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

template <typename T>
class EventQueue {
public:
    explicit EventQueue(std::size_t cap = 4096) : cap_(cap) {}

    void push(const T& v) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() >= cap_) { q_.pop_front(); ++dropped_; }  // drop oldest
            q_.push_back(v);
        }
        cv_.notify_one();
    }
    bool pop_for(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty(); })) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    bool empty() const { std::lock_guard<std::mutex> lk(m_); return q_.empty(); }
    std::size_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
    std::size_t cap_, dropped_ = 0;
};
