#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace okuflow {

enum class GpuCopyCompletion { Pending, Complete, Failed };

// One reusable producer allocation may be borrowed by at most one copy. Only
// observed completion releases its lease; errors and deadline expiry retain it.
// The owner must retain this entire state on failed/unfinished GPU teardown.
class GpuCopyLease {
public:
    bool Begin(std::shared_ptr<void> lease, std::uint64_t nowMs)
    {
        if (pending_ || failed_ || !lease) return false;
        lease_ = std::move(lease);
        startedMs_ = nowMs;
        pending_ = true;
        return true;
    }
    bool Poll(GpuCopyCompletion completion, std::uint64_t nowMs)
    {
        if (failed_) return false;
        if (!pending_) return true;
        if (completion == GpuCopyCompletion::Complete) {
            pending_ = false;
            lease_.reset();
            return true;
        }
        if (completion == GpuCopyCompletion::Failed || nowMs - startedMs_ >= 1000)
            failed_ = true;
        return false;
    }
    bool Pending() const { return pending_; }
    bool Failed() const { return failed_; }
private:
    std::shared_ptr<void> lease_;
    std::uint64_t startedMs_{};
    bool pending_{};
    bool failed_{};
};

} // namespace okuflow
