#pragma once

#include <cstdint>

namespace okuflow {

// Automatic finalization and a later user Stop refer to the same output.
// Reset only when beginning a new recording attempt, never during teardown.
template <typename Result>
class RecordingTerminalResult {
public:
    void Reset() { result_ = {}; }
    void Remember(const Result& result) { result_ = result; }
    const Result& Last() const { return result_; }

private:
    Result result_{};
};

// A failed encoder can leave recoverable bytes even when it accepted no
// samples. Unknown sizes and externally owned paths must also be retained.
constexpr bool CanRemoveEmptyRecording(bool sessionOwned,
                                       bool regularFile,
                                       bool symbolicLink,
                                       std::int64_t fileBytes,
                                       std::uint64_t acceptedVideoSamples)
{
    return sessionOwned && regularFile && !symbolicLink &&
           fileBytes == 0 && acceptedVideoSamples == 0;
}

} // namespace okuflow
