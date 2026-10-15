#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace okuflow {

// The worker owns the exact resource closure; it never references its caller.
// A failed driver cleanup retains that closure until process exit.
class AsyncCleanup final : public std::enable_shared_from_this<AsyncCleanup> {
public:
    enum class Status { Running, Completed, Failed };
    static std::shared_ptr<AsyncCleanup> Start(std::function<bool()> work) {
        auto cleanup = std::shared_ptr<AsyncCleanup>(new AsyncCleanup(std::move(work)));
        try {
            std::thread([cleanup] {
                bool success = false;
                try { success = cleanup->work_(); } catch (...) {}
                if (success) cleanup->work_ = {};
                else cleanup->retained_ = cleanup;
                cleanup->status_.store(success ? Status::Completed : Status::Failed,
                                       std::memory_order_release);
            }).detach();
        } catch (...) {
            cleanup->retained_ = cleanup;
            cleanup->status_.store(Status::Failed,std::memory_order_release);
        }
        return cleanup;
    }
    Status Poll() const noexcept { return status_.load(std::memory_order_acquire); }
private:
    explicit AsyncCleanup(std::function<bool()> work) : work_(std::move(work)) {}
    std::function<bool()> work_;
    std::shared_ptr<AsyncCleanup> retained_;
    std::atomic<Status> status_{Status::Running};
};

} // namespace okuflow
