#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace okuflow {

// Shared control plane for a capture session and its stop coordinator. No
// driver operation executes under this mutex. Cancellation denies new callback
// entries immediately; an already-entered callback must own its target state.
class CaptureShutdown {
public:
    struct Snapshot {
        bool quiescent{};
        bool finished{};
        bool abandoned{};
        std::chrono::steady_clock::time_point finishedAt{};
    };
    // A control-plane snapshot only: no condition-variable wait or driver call.
    Snapshot Poll()
    {
        std::lock_guard lock(mutex_);
        return {quiescent_ && deliveries_ == 0, finished_, abandoned_, finishedAt_};
    }
    class Delivery {
    public:
        Delivery() = default;
        explicit Delivery(CaptureShutdown* owner) : owner_(owner) {}
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;
        Delivery(Delivery&& other) noexcept : owner_(other.owner_) { other.owner_ = nullptr; }
        ~Delivery()
        {
            if (owner_) {
                std::lock_guard lock(owner_->mutex_);
                --owner_->deliveries_;
                owner_->changed_.notify_all();
            }
        }
        explicit operator bool() const { return owner_ != nullptr; }
    private:
        CaptureShutdown* owner_{};
    };

    Delivery TryEnterDelivery()
    {
        std::lock_guard lock(mutex_);
        if (canceled_) return {};
        ++deliveries_;
        return Delivery(this);
    }
    void Cancel()
    {
        std::lock_guard lock(mutex_);
        canceled_ = true;
        changed_.notify_all();
    }
    bool WaitQuiescent(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, deadline, [this] { return abandoned_ || (quiescent_ && deliveries_ == 0); });
        return !abandoned_ && quiescent_ && deliveries_ == 0;
    }
    void MarkQuiescent()
    {
        std::lock_guard lock(mutex_);
        quiescent_ = true;
        changed_.notify_all();
    }
    // The coordinator preserves backing resources until the UI has released
    // its CUDA imports. An abandoned session must retain that backing forever.
    bool WaitReleasePermission()
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return releaseAllowed_ || abandoned_; });
        return !abandoned_;
    }
    void AllowRelease()
    {
        std::lock_guard lock(mutex_);
        releaseAllowed_ = true;
        changed_.notify_all();
    }
    void Abandon()
    {
        std::lock_guard lock(mutex_);
        abandoned_ = true;
        changed_.notify_all();
    }
    bool IsAbandoned()
    {
        std::lock_guard lock(mutex_);
        return abandoned_;
    }
    void MarkFinished()
    {
        std::lock_guard lock(mutex_);
        finishedAt_ = std::chrono::steady_clock::now();
        finished_ = true;
        changed_.notify_all();
    }
    bool WaitFinished(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline, [this] { return finished_; });
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned deliveries_{};
    bool canceled_{};
    bool quiescent_{};
    bool releaseAllowed_{};
    bool abandoned_{};
    bool finished_{};
    std::chrono::steady_clock::time_point finishedAt_{};
};
// Outcome of PollWithDeadline below.
enum class BoundedPollOutcome { Completed, TimedOut, Abandoned };

// Polls a nonblocking completion query (true = complete, false = still
// pending) on a stop-coordinator thread until it completes, the deadline
// expires, or the session is abandoned. Poll exceptions propagate. This
// bounds shutdown query waits that would otherwise spin forever on a stuck
// driver while the facade has already retained the session for process exit.
template <typename Poll>
BoundedPollOutcome PollWithDeadline(Poll&& poll, CaptureShutdown& shutdown,
                                    std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        if (poll()) return BoundedPollOutcome::Completed;
        if (shutdown.IsAbandoned()) return BoundedPollOutcome::Abandoned;
        if (std::chrono::steady_clock::now() >= deadline) return BoundedPollOutcome::TimedOut;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace okuflow
