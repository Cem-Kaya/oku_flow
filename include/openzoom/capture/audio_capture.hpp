#pragma once

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace openzoom {

struct AudioDeviceDescriptor {
    std::wstring name;
    std::wstring endpointId;
    bool isDefault{false};
    Microsoft::WRL::ComPtr<IMFActivate> activation;
};

struct AudioFrame {
    std::vector<std::uint8_t> pcm;
    std::uint32_t sampleRate{48000};
    std::uint32_t channels{1};
    std::uint32_t bitsPerSample{16};
    std::int64_t captureClock100ns{-1};
    std::int64_t duration100ns{0};
};

using AudioFrameCallback = std::function<void(AudioFrame&& frame)>;
using AudioErrorCallback =
    std::function<void(const std::string& message)>;

// Media Foundation microphone capture normalized to signed 16-bit,
// 48 kHz PCM. The QPC-based timestamp shares a clock with camera-frame
// arrival timestamps, allowing RecordingManager to mux sound with both MP4s.
class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    std::vector<AudioDeviceDescriptor> EnumerateDevices();
    bool Start(const AudioDeviceDescriptor& descriptor,
               AudioFrameCallback callback,
               AudioErrorCallback errorCallback = {});
    void Stop();

    bool IsRunning() const { return running_.load(); }
    const std::string& LastError() const { return lastError_; }
    const std::wstring& ActiveEndpointId() const {
        return activeEndpointId_;
    }

private:
    void CaptureLoop(AudioFrameCallback callback,
                     AudioErrorCallback errorCallback);
    bool ConfigureReader(IMFSourceReader* reader);

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource_;
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader_;
    Microsoft::WRL::ComPtr<IMFActivate> activeActivation_;
    std::thread captureThread_;
    std::atomic<bool> running_{false};
    std::wstring activeEndpointId_;
    std::string lastError_;
};

} // namespace openzoom

#endif // _WIN32
