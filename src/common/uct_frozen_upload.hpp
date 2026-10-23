#pragma once

#include "okuflow/common/async_cleanup.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace okuflow::uct::detail {

// Final catch/destructor retirement cannot return reusable storage or throw.
// State reserves one empty shared_ptr<State> retirementQuarantine member at
// construction. Its noexcept copy assignment needs no new control block,
// allocation, thread or wait when even async handoff allocation is unavailable.
template<class State, class Retire>
std::shared_ptr<AsyncCleanup> RetireOwnedState(std::shared_ptr<State>& owner, Retire&& retire) noexcept {
    static_assert(noexcept(owner->retirementQuarantine = owner));
    if (!owner) return {};
    try {
        // Keep the caller's exact owner until cleanup has accepted its closure.
        auto receipt = std::forward<Retire>(retire)(owner);
        if (receipt) {
            owner.reset();
            return receipt;
        }
    } catch (...) {}
    // Completion is unknown. Deliberate process-lifetime quarantine matches
    // AsyncCleanup's failed-work self-retention; never fabricate a receipt.
    owner->retirementQuarantine = owner;
    owner.reset();
    return {};
}

// One RecognizeDevice call's unresolved frozen descriptor upload. The guard
// adds no storage ownership until cancellation transfers the exact State to
// the existing async retirement callback. Normal/zero-crop paths stay cheap.
class FrozenUploadRetirement final {
public:
    void Queued() noexcept { pending_ = true; }
    // Call only after the existing group wait succeeds, never after enqueue.
    void Completed() noexcept { pending_ = false; }

    template<class State, class Retire>
    std::shared_ptr<AsyncCleanup> Cancel(std::shared_ptr<State>& owner, Retire&& retire) const {
        if (!owner) return {};
        if (!pending_) {
            owner->inFlight.reset();
            return {}; // same engines/pinned storage remain reusable
        }
        // Keep the caller's owner until retirement has accepted the entire
        // closure. Allocation/handoff failure cannot release pending storage.
        auto receipt = std::forward<Retire>(retire)(owner);
        if (!receipt) throw std::runtime_error("Frozen upload retirement did not retain its State.");
        owner.reset();
        return receipt;
    }
private:
    bool pending_{};
};

} // namespace okuflow::uct::detail
