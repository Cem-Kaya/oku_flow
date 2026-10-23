#pragma once

#include "okuflow/common/async_cleanup.hpp"
#include <utility>

namespace okuflow::detail {

// Terminal handoff for a worker-owned resource with possibly submitted work.
// Copy the existing control block into the resource itself BEFORE removing a
// reusable owner or constructing the allocating cleanup closure. The detached
// closure owns that exact resource, its anchor member identity and drain code;
// it borrows neither the cache nor caller storage. Only a confirmed drain may
// break the cycle. Setup failure or unknown completion retains it until exit.
// Call once per retired resource, on its owning worker, with a non-null owner.
template<class Resource>
std::shared_ptr<AsyncCleanup> StartRetainedCleanup(
    std::shared_ptr<Resource> owner, std::shared_ptr<Resource>& cache,
    std::shared_ptr<Resource> Resource::* anchor, bool (*drain)(Resource&)) noexcept {
    static_assert(noexcept((owner.get()->*anchor) = owner));
    (owner.get()->*anchor) = owner; // noexcept; no allocation/control-block creation
    if (cache == owner) cache.reset(); // owner also protects a cache-aliased argument
    try {
        auto work = [owner, anchor, drain] {
            if (!drain(*owner)) return false;
            (owner.get()->*anchor).reset(); // closure still owns it through return
            return true;
        };
#if defined(OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS)
        CheckAsyncCleanupSetup(AsyncCleanupSetupPoint::ClosureConversion);
#endif
        return AsyncCleanup::Start(std::function<bool()>(std::move(work)));
    } catch (...) {
        // The pre-established self owner survives destruction of every local
        // and temporary, including std::function/new/control-block failures.
        return {};
    }
}

} // namespace okuflow::detail
