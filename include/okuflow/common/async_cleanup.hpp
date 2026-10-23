#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace okuflow {

#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
// Standalone, model-free fixture only. No allocator override or runtime hook
// is compiled into production targets.
namespace detail {
enum class AsyncCleanupSetupPoint { ClosureConversion, ObjectAllocation, ControlBlockAllocation, ThreadStart };
inline thread_local void (*asyncCleanupSetupCheckpoint)(AsyncCleanupSetupPoint) = nullptr;
inline void CheckAsyncCleanupSetup(AsyncCleanupSetupPoint point) {
    if (asyncCleanupSetupCheckpoint) asyncCleanupSetupCheckpoint(point);
}
template<class T> struct AsyncCleanupTestAllocator {
    using value_type = T;
    AsyncCleanupTestAllocator() = default;
    template<class U> AsyncCleanupTestAllocator(const AsyncCleanupTestAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        CheckAsyncCleanupSetup(AsyncCleanupSetupPoint::ControlBlockAllocation);
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>{}.deallocate(p, n); }
    template<class U> bool operator==(const AsyncCleanupTestAllocator<U>&) const noexcept { return true; }
    template<class U> bool operator!=(const AsyncCleanupTestAllocator<U>&) const noexcept { return false; }
};
} // namespace detail
#endif

// The worker owns the exact resource closure; it never references its caller.
// A failed driver cleanup retains that closure until process exit.
class AsyncCleanup final : public std::enable_shared_from_this<AsyncCleanup> {
public:
    enum class Status { Running, Completed, Failed };
    static std::shared_ptr<AsyncCleanup> Start(std::function<bool()> work) {
#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
        detail::CheckAsyncCleanupSetup(detail::AsyncCleanupSetupPoint::ObjectAllocation);
#endif
        auto cleanup = std::shared_ptr<AsyncCleanup>(new AsyncCleanup(std::move(work))
#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
            , std::default_delete<AsyncCleanup>{}, detail::AsyncCleanupTestAllocator<AsyncCleanup>{}
#endif
        );
        try {
#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
            detail::CheckAsyncCleanupSetup(detail::AsyncCleanupSetupPoint::ThreadStart);
#endif
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
#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
    // Fixture must first acquire a terminal Poll() result. The worker writes
    // retained_ before publishing that result and never changes it afterwards.
    bool TestHasSelfRetention() const noexcept { return retained_.get() == this; }
#endif
private:
    explicit AsyncCleanup(std::function<bool()> work) : work_(std::move(work)) {}
    std::function<bool()> work_;
    std::shared_ptr<AsyncCleanup> retained_;
    std::atomic<Status> status_{Status::Running};
};

} // namespace okuflow
