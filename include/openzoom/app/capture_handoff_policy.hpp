#pragma once

namespace openzoom {

enum class CaptureGpuPending { None, CudaCopy, D3D11Query };

// Only D3D11 conversion deadlines select a slower input rung. A pending CUDA
// copy still owns the producer and must resolve through its ownership deadline.
class CaptureHandoffPolicy {
public:
    bool RecordDeadline(CaptureGpuPending pending) {
        if (pending != CaptureGpuPending::D3D11Query || demoted_) return false;
        if (++consecutiveDeadlines_ < 3) return false;
        demoted_ = true;
        return true;
    }
    void RecordReady() { consecutiveDeadlines_ = 0; }
    void Reset() { consecutiveDeadlines_ = 0; demoted_ = false; }
    bool Demoted() const { return demoted_; }

private:
    unsigned int consecutiveDeadlines_{};
    bool demoted_{};
};

} // namespace openzoom
