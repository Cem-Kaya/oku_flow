#pragma once
#include <memory>
namespace okuflow {
enum class GpuReadCompletion { Pending, Complete, Unknown };
// Arm before submitting the reader. Unknown completion deliberately preserves
// the backing until process exit, even if its recorder or sample is destroyed.
class GpuReadRetirement {
public:
    explicit GpuReadRetirement(std::shared_ptr<void> backing) : backing_(std::move(backing)) {}
    void Arm(const std::shared_ptr<GpuReadRetirement>& self) noexcept { retained_ = self; }
    template<class Poll> GpuReadCompletion PollCompletion(Poll poll) {
        if (unknown_) return GpuReadCompletion::Unknown;
        if (!retained_) return GpuReadCompletion::Complete;
        const auto result = poll();
        if (result == GpuReadCompletion::Complete) {
            backing_.reset(); retained_.reset();
        } else if (result == GpuReadCompletion::Unknown) {
            unknown_ = true;
        }
        return result;
    }
private:
    std::shared_ptr<void> backing_;
    std::shared_ptr<GpuReadRetirement> retained_;
    bool unknown_{};
};
} // namespace okuflow
