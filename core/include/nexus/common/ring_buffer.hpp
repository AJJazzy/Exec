#pragma once
// Fixed-capacity FIFO used for console output, logs and history so that memory stays bounded no
// matter how much a script prints. Each element receives a monotonically increasing sequence
// number, letting consumers fetch "everything after N" without copying the whole buffer.

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace nexus {

template <class T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // Returns the sequence number assigned to the element (starting at 1).
    uint64_t push(T value) {
        if (items_.size() == capacity_) {
            items_.pop_front();
            ++dropped_;
        }
        items_.emplace_back(++lastSeq_, std::move(value));
        return lastSeq_;
    }

    // Elements with sequence > afterSeq, oldest first (at most `limit`, newest kept when limiting).
    std::vector<std::pair<uint64_t, T>> since(uint64_t afterSeq, size_t limit = SIZE_MAX) const {
        std::vector<std::pair<uint64_t, T>> out;
        if (items_.empty() || afterSeq >= lastSeq_) return out;
        // Sequence numbers are consecutive, so the start index can be computed directly.
        uint64_t firstSeq = items_.front().first;
        size_t start = afterSeq + 1 > firstSeq ? static_cast<size_t>(afterSeq + 1 - firstSeq) : 0;
        size_t count = items_.size() - start;
        if (count > limit) start += count - limit;
        out.reserve(items_.size() - start);
        for (size_t i = start; i < items_.size(); ++i) out.push_back(items_[i]);
        return out;
    }

    std::vector<T> values() const {
        std::vector<T> out;
        out.reserve(items_.size());
        for (const auto& item : items_) out.push_back(item.second);
        return out;
    }

    void clear() { items_.clear(); }
    void setCapacity(size_t capacity) {
        capacity_ = capacity == 0 ? 1 : capacity;
        while (items_.size() > capacity_) {
            items_.pop_front();
            ++dropped_;
        }
    }

    size_t size() const { return items_.size(); }
    size_t capacity() const { return capacity_; }
    uint64_t lastSequence() const { return lastSeq_; }
    uint64_t droppedCount() const { return dropped_; }
    bool empty() const { return items_.empty(); }

private:
    size_t capacity_;
    std::deque<std::pair<uint64_t, T>> items_;
    uint64_t lastSeq_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace nexus
