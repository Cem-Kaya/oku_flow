#pragma once
#ifdef _WIN32
#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace okuflow {
// Pixels are immutable while this lease exists. Texture storage and its final
// native release belong to the decoder's bounded off-thread resource owner.
struct VideoPlaybackGpuImage {
    ID3D11Texture2D* texture{};
    std::shared_ptr<void> storage;
    std::size_t bytes{};
};

struct VideoGpuFormat {
    UINT width{},height{},squareWidth{},rotation{};
    GUID subtype{};
};

// Genuine GPU device/format/conversion incompatibility or fault. Only this
// category may latch a document to the CPU compatibility decoder.
struct VideoGpuIncompatible : std::runtime_error { using std::runtime_error::runtime_error; };
// Bounded texture credit is retained by live consumers. This is ordinary
// backpressure, never evidence that the GPU path is incompatible.
struct VideoGpuBackpressure : std::runtime_error { using std::runtime_error::runtime_error; };

// Access only from the single playback worker. No native call or texture map is
// exposed to the UI. Decoder output is copied/converted into owned shareable
// BGRA textures before publication; it never borrows a reusable MF pool sample.
// While a conversion is pending the decoder owns the exact closure the GPU may
// still read or write: source reader, MF sample/buffer, source texture, both
// processor views, output storage and the completion query. If completion
// becomes unknown (deadline, failed Blt, failed query) the converter latches
// unsafe, that whole closure is quarantined until process exit, and no further
// conversion, query poll, seek or sample read may use it.
class VideoGpuDecoder final : public std::enable_shared_from_this<VideoGpuDecoder> {
public:
    using Clock=std::chrono::steady_clock;
    static std::shared_ptr<VideoGpuDecoder> Create(LUID adapter);
    // Private conversion target is also selectable for a deterministic driver-
    // compatibility fixture. Normal use first tries the shared target directly.
    static std::shared_ptr<VideoGpuDecoder> CreateFromDevice(ID3D11Device* device,
                                                           bool forcePrivateConversion = false);
    ~VideoGpuDecoder();
    void ConfigureReader(IMFAttributes* attributes);
    // Output subtypes offered for this coded type by the first installed
    // decoder that accepts it on this device, in that decoder's own
    // preference order (its native-precision output first). Empty when no
    // installed decoder accepts the type. Worker-thread only; the probe
    // transform is shut down before returning and never streams a sample.
    std::vector<GUID> DecoderPreferredOutputs(IMFMediaType* codedType);
    // The reader producing Convert samples. A pending conversion copies this
    // reference into its closure, so the reader outlives the exact sample.
    void AttachSourceReader(IMFSourceReader* reader);
    // Non-blocking and noexcept: drops a completed pending sample (then the
    // reader), or quarantines the full pending closure if completion is
    // unknown. Callers must invoke this before releasing their own reader.
    void DetachSourceReader() noexcept;
    bool HasUnsafePending() const;
    VideoGpuFormat SetFormat(IMFMediaType* format);
    std::shared_ptr<const VideoPlaybackGpuImage> Convert(IMFSample* sample,
        const std::function<bool()>& current,Clock::time_point deadline);
    static std::size_t RetainedBytes();
    bool HasConversionCredit();
    static constexpr std::size_t kTextureByteLimit=256u*1024*1024;
    static constexpr unsigned kTextureSlotLimit=8;
private:
    struct State;
    explicit VideoGpuDecoder(std::unique_ptr<State> state);
    bool PollPending();
    void DrainPending(Clock::time_point deadline);
    void ClearPending() noexcept;
    void MarkUnsafe() noexcept;
    std::unique_ptr<State> state_;
    std::shared_ptr<VideoGpuDecoder> quarantine_;
};
} // namespace okuflow
#endif
