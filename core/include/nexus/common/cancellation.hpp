#pragma once
// Cooperative cancellation shared between the UI thread, session workers and execution backends.
// std::stop_token/jthread are avoided because they are still unavailable in some supported libc++.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace nexus {

class CancellationState {
public:
    bool requested() const;
    void request();

    // Registers a callback that runs once when cancellation is requested (immediately if it already
    // was). Callbacks run on the thread that calls request() and must not block.
    void onCancel(std::function<void()> callback);

    // Blocks until cancellation is requested or the deadline passes. Returns true when cancelled.
    // Used instead of sleep() so waits stay responsive and nothing busy-polls.
    bool waitUntil(std::chrono::steady_clock::time_point deadline) const;
    bool waitFor(std::chrono::milliseconds duration) const { return waitUntil(std::chrono::steady_clock::now() + duration); }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool requested_ = false;
    std::vector<std::function<void()>> callbacks_;
};

// Cheap copyable handle; a default-constructed token can never be cancelled.
class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<CancellationState> state) : state_(std::move(state)) {}

    bool requested() const { return state_ && state_->requested(); }
    void onCancel(std::function<void()> callback) const {
        if (state_) state_->onCancel(std::move(callback));
    }
    bool waitUntil(std::chrono::steady_clock::time_point deadline) const;
    const CancellationState* state() const { return state_.get(); }

private:
    std::shared_ptr<CancellationState> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<CancellationState>()) {}
    CancellationToken token() const { return CancellationToken(state_); }
    void cancel() { state_->request(); }
    bool requested() const { return state_->requested(); }

private:
    std::shared_ptr<CancellationState> state_;
};

} // namespace nexus
