#pragma once

#ifdef _WIN32

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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

    bool IsRunning() const { return session_ && session_->running.load(); }
    // Sticky: a previous Stop() abandoned a wedged reader thread. Process-
    // global Media Foundation teardown (MFShutdown) is unsafe for the rest
    // of the process lifetime once this is set — the detached thread may
    // still be inside an MF call.
    bool WasAbandoned() const { return abandonedForExit_; }
    const std::string& LastError() const { return lastError_; }
    const std::wstring& ActiveEndpointId() const {
        return activeEndpointId_;
    }

private:
    // Owned by every thread that needs it (capture loop, stop-time flusher,
    // and this object), so a detached wedged thread never touches
    // AudioCapture members — AudioCapture is a value member of the app and
    // is destroyed with it. A fresh session per Start() also means a stale
    // thread can never observe a later session's flags.
    struct CaptureSession {
        std::atomic<bool> running{false};
        std::atomic<bool> loopDone{false};
        std::atomic<bool> flushDone{true};
    };

    static void CaptureLoop(std::shared_ptr<CaptureSession> session,
                            Microsoft::WRL::ComPtr<IMFSourceReader> reader,
                            AudioFrameCallback callback,
                            AudioErrorCallback errorCallback);
    bool ConfigureReader(IMFSourceReader* reader);

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource_;
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader_;
    Microsoft::WRL::ComPtr<IMFActivate> activeActivation_;
    std::thread captureThread_;
    std::shared_ptr<CaptureSession> session_;
    bool abandonedForExit_{false};
    std::wstring activeEndpointId_;
    std::string lastError_;
};

} // namespace openzoom

#endif // _WIN32
