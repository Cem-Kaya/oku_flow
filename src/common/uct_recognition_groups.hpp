#pragma once

#include <array>
#include <stdexcept>
#include <utility>

namespace okuflow::uct::detail {

// Host bookkeeping only, for the existing two pinned slots. This owns no CUDA
// resources: any exception still retires the complete enclosing inference State.
class RecognitionGroups final {
public:
    void Queue(int slot) {
        Require(slot, Phase::Free);
        phases_[slot] = Phase::Queued; // before the first operation that can enqueue
    }
    void Completed(int slot) {
        Require(slot, Phase::Queued);
        phases_[slot] = Phase::Reading; // only after this slot's event succeeds
    }
    void Consumed(int slot) {
        Require(slot, Phase::Reading);
        phases_[slot] = Phase::Free; // event alone cannot permit pinned reuse
    }
    template<class Drain, class Release> void Cancel(Drain&& drain, Release&& release) {
        if (phases_[0] == Phase::Queued || phases_[1] == Phase::Queued)
            std::forward<Drain>(drain)(); // failure must escape without releasing input
        std::forward<Release>(release)();
        phases_ = {};
    }
private:
    enum class Phase { Free, Queued, Reading };
    void Require(int slot, Phase expected) const {
        if (slot < 0 || slot >= int(phases_.size()) || phases_[slot] != expected)
            throw std::logic_error("Recognition metadata slot is still in use.");
    }
    std::array<Phase, 2> phases_{};
};

} // namespace okuflow::uct::detail
