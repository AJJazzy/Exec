#include "nexus/common/cancellation.hpp"

#include <thread>

namespace nexus {

bool CancellationState::requested() const {
    std::lock_guard lock(mutex_);
    return requested_;
}

void CancellationState::request() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(mutex_);
        if (requested_) return;
        requested_ = true;
        callbacks.swap(callbacks_);
    }
    cv_.notify_all();
    for (auto& cb : callbacks) cb();
}

void CancellationState::onCancel(std::function<void()> callback) {
    {
        std::lock_guard lock(mutex_);
        if (!requested_) {
            callbacks_.push_back(std::move(callback));
            return;
        }
    }
    callback();
}

bool CancellationState::waitUntil(std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, deadline, [&] { return requested_; });
}

bool CancellationToken::waitUntil(std::chrono::steady_clock::time_point deadline) const {
    if (state_) return state_->waitUntil(deadline);
    std::this_thread::sleep_until(deadline);
    return false;
}

} // namespace nexus
