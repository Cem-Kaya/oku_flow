#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace openzoom {

// Shared control plane for a capture session and its stop coordinator. No
// driver operation executes under this mutex. Cancellation denies new callback
// entries immediately; an already-entered callback must own its target state.
class CaptureShutdown {
public:
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
        return changed_.wait_until(lock, deadline, [this] { return quiescent_ && deliveries_ == 0; });
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
};
} // namespace openzoom
