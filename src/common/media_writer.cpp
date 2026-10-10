#ifdef _WIN32

#include "okuflow/common/media_writer.hpp"

#include <mferror.h>
#include <propvarutil.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace okuflow {

namespace {

std::string HrToString(HRESULT hr)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "hr=0x%08lx", static_cast<unsigned long>(hr));
    return std::string(buffer);
}

void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) + " (" + HrToString(hr) + ")");
    }
}

constexpr ULONGLONG kMinFreeBytesToStart = 500ull * 1024ull * 1024ull;
constexpr ULONGLONG kMinFreeBytesWhileRecording = 200ull * 1024ull * 1024ull;
constexpr ULONGLONG kSpaceCheckIntervalMs = 5000;

const char kDiskFullMessage[] = "Recording stopped: disk almost full. The recording so far was saved.";

// Keeps the D3D12 allocation, descriptors, and command allocator alive until
// Media Foundation releases the sample. Sink writers may consume samples
// asynchronously after WriteSample returns.
class FrameLifetimeHolder final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<
              Microsoft::WRL::ClassicCom>,
          IUnknown> {
public:
    explicit FrameLifetimeHolder(std::shared_ptr<void> lifetime)
        : lifetime_(std::move(lifetime))
    {
    }

private:
    std::shared_ptr<void> lifetime_;
};

constexpr GUID kOkuFlowFrameLifetimeAttribute{
    0x0d433dd5,
    0xc1ba,
    0x4698,
    {0xaa, 0x5a, 0x78, 0x4d, 0xa5, 0x60, 0x09, 0xcf}};

bool QueryFreeBytesForPath(const std::wstring& filePath, ULONGLONG& outFreeBytes)
{
    std::wstring directory = filePath;
    const size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        directory.clear();
    } else {
        directory.erase(separator + 1);
    }

    ULARGE_INTEGER freeBytesAvailable{};
    if (!GetDiskFreeSpaceExW(directory.empty() ? nullptr : directory.c_str(),
                             &freeBytesAvailable,
                             nullptr,
                             nullptr)) {
        return false;
    }
    outFreeBytes = freeBytesAvailable.QuadPart;
    return true;
}

bool IsDiskFullError(HRESULT hr)
{
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
           hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) ||
           hr == STG_E_MEDIUMFULL;
}

// Resets the observable stage to kIdle when a public recorder entry point
// returns by any path. Inner code only ever *stores* stages; nesting is safe
// because the guard lives at the outermost entry.
class StageResetGuard {
public:
    explicit StageResetGuard(std::atomic<VideoRecorder::WriterStage>& stage)
        : stage_(stage)
    {
    }
    ~StageResetGuard()
    {
        stage_.store(VideoRecorder::WriterStage::kIdle,
                     std::memory_order_relaxed);
    }

private:
    std::atomic<VideoRecorder::WriterStage>& stage_;
};

} // namespace

const char* VideoRecorder::StageName(WriterStage stage)
{
    switch (stage) {
    case WriterStage::kIdle:
        return "idle";
    case WriterStage::kOpenSharedTexture:
        return "opening the shared recording texture";
    case WriterStage::kOpenSharedFence:
        return "opening the shared recording fence";
    case WriterStage::kGpuSync:
        return "waiting for the GPU frame fence";
    case WriterStage::kReadbackMap:
        return "reading the frame back from the GPU";
    case WriterStage::kCreateEncoderTexture:
        return "creating the encoder texture";
    case WriterStage::kConvertNv12:
        return "converting the frame for the encoder";
    case WriterStage::kSubmitGpuWork:
        return "submitting GPU conversion work";
    case WriterStage::kCreateSampleBuffer:
        return "wrapping the frame for Media Foundation";
    case WriterStage::kWriteVideoSample:
        return "writing a video sample to the encoder";
    case WriterStage::kWriteAudioSample:
        return "writing an audio sample to the encoder";
    case WriterStage::kFinalize:
        return "finalizing the file";
    }
    return "unknown";
}

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder()
{
    if (abandoned_.load()) {
        // A wedged worker thread may still be inside a synchronous call on
        // these COM objects. Releasing them here could destroy an object
        // under that thread; leaking them is safe — process exit reclaims
        // everything (same rationale as the plan-28/30 driver workarounds).
        LeakComForProcessExit();
        return;
    }
    Stop();
}

void VideoRecorder::LeakComForProcessExit()
{
    (void)sinkWriter_.Detach();
    (void)pendingSample_.Detach();
    (void)gpuDevice_.Detach();
    (void)gpuContext_.Detach();
    (void)gpuDeviceManager_.Detach();
    (void)gpuFence_.Detach();
    (void)gpuReadbackTexture_.Detach();
    (void)gpuVideoDevice_.Detach();
    (void)gpuVideoContext_.Detach();
    (void)gpuVideoProcessorEnumerator_.Detach();
    (void)gpuVideoProcessor_.Detach();
    recording_ = false;
}

void VideoRecorder::PollGpuReadLeases(bool allowFlush)
{
    if (abandoned_.load()) return;
    for (auto it = pendingGpuReads_.begin(); it != pendingGpuReads_.end();) {
        const auto& backing = it->backing;
        const auto result = it->retirement->PollCompletion([&] {
            if (!backing->queryRecorded) {
                SetError("GPU recording reader completion is unknown after an interrupted submission.");
                return GpuReadCompletion::Unknown;
            }
            const HRESULT deviceHr = backing->device->GetDeviceRemovedReason();
            lastReaderDeviceHr_ = deviceHr;
            lastReaderProducerCompleted_ = backing->producerFence->GetCompletedValue();
            lastReaderProducerRequired_ = backing->producerValue;
            if (FAILED(deviceHr)) {
                SetError("GPU recording device was removed while retiring a reader (" + HrToString(deviceHr) + ").");
                return GpuReadCompletion::Unknown;
            }
            BOOL complete = FALSE;
            const HRESULT hr = backing->context->GetData(
                backing->query.Get(), &complete, sizeof(complete),
                allowFlush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
            lastReaderQueryHr_ = hr;
            lastReaderQueryComplete_ = complete;
            if (FAILED(hr)) {
                SetError("GPU recording reader completion query failed (" + HrToString(hr) + ").");
                return GpuReadCompletion::Unknown;
            }
            return hr == S_OK && complete
                ? GpuReadCompletion::Complete : GpuReadCompletion::Pending;
        });
        if (result == GpuReadCompletion::Complete) it = pendingGpuReads_.erase(it);
        else {
            if (result == GpuReadCompletion::Unknown) gpuReadFaulted_ = true;
            ++it;
        }
    }
}

void VideoRecorder::SetError(const std::string& err)
{
    lastError_ = err;
}

const char* VideoRecorder::CodecName(Codec codec)
{
    return codec == Codec::Av1 ? "AV1" : "H.264";
}

bool VideoRecorder::Start(const std::wstring& filePath,
                          UINT width,
                          UINT height,
                          UINT frameRateNumerator,
                          UINT frameRateDenominator,
                          Codec codec,
                          const AudioFormat* audioFormat)
{
    if (abandoned_.load()) {
        // Poisoned: a wedged worker may still be inside these COM
        // objects. Every public mutating entry fails fast so a
        // resumed (previously blocked) caller can never re-enter
        // the sink writer or start a new stream.
        return false;
    }
    Stop();

    ULONGLONG freeBytes = 0;
    terminalResult_.Reset();
    writeFailureHr_ = S_OK;
    if (GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetError("Recording output path already exists; it was preserved.");
        return false;
    }
    if (QueryFreeBytesForPath(filePath, freeBytes) && freeBytes < kMinFreeBytesToStart) {
        SetError("Not enough free disk space to start recording (less than 500 MB available). "
                 "Free up some space and try again.");
        return false;
    }

    try {
        if (!InitializeSink(filePath,
                            width,
                            height,
                            frameRateNumerator,
                            frameRateDenominator,
                            codec,
                            audioFormat,
                            nullptr,
                            MFVideoFormat_ARGB32)) {
            return false;
        }
        frameWidth_ = width;
        frameHeight_ = height;
        frameRateNumerator_ = std::max(1u, frameRateNumerator);
        frameRateDenominator_ = std::max(1u, frameRateDenominator);
        timeline_.Reset(frameRateNumerator_, frameRateDenominator_);
        pendingSample_.Reset();
        pendingSampleTime100ns_ = -1;
        timelineEnd100ns_ = 0;
        videoSamplesWritten_ = 0;
        targetPath_ = filePath;
        lastSpaceCheckTicks_ = GetTickCount64();
        stopReason_ = StopReason::None;
        activeCodec_ = codec;
        audioEnabled_ =
            audioFormat != nullptr && audioFormat->IsValid();
        gpuInputEnabled_ = false;
        gpuCompatibilityReadback_ = false;
        if (audioEnabled_) {
            audioFormat_ = *audioFormat;
        }
        recording_ = true;
        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        SetError(e.what());
    } catch (...) {
        SetError("Unknown exception starting recorder");
    }
    Stop();
    return false;
}

bool VideoRecorder::InitializeGpuDevice(HANDLE probeTextureHandle)
{
    gpuDevice_.Reset();
    gpuContext_.Reset();
    gpuDeviceManager_.Reset();
    gpuFence_.Reset();
    gpuReadbackTexture_.Reset();
    ResetGpuVideoProcessor();
    gpuResetToken_ = 0;
    if (!probeTextureHandle) {
        SetError("GPU recording did not receive a shareable probe texture.");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        SetError("GPU recording could not create a DXGI factory.");
        return false;
    }
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, adapter.GetAddressOf()) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL selectedLevel{};
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        const HRESULT createHr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &selectedLevel,
            context.GetAddressOf());
        if (FAILED(createHr)) {
            continue;
        }

        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> probe;
        if (FAILED(device.As(&device1)) ||
            FAILED(device1->OpenSharedResource1(
                probeTextureHandle, IID_PPV_ARGS(&probe)))) {
            continue;
        }

        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
        if (FAILED(device.As(&device5)) ||
            FAILED(context.As(&context4))) {
            continue;
        }

        // Mandatory for any device handed to a DXGI Device Manager: the
        // hardware encoder MFT drives this device from its own worker
        // threads while the recording worker runs VideoProcessorBlt/Flush
        // on the immediate context. Without multithread protection those
        // calls race inside the driver — the reproduced failure was a
        // permanent wedge in Flush() around the third submitted frame,
        // exactly when the encoder's async threads spin up (the 2026-07-30
        // production stop hang).
        Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
        if (FAILED(context.As(&multithread))) {
            continue;
        }
        multithread->SetMultithreadProtected(TRUE);

        Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager;
        UINT resetToken = 0;
        if (FAILED(MFCreateDXGIDeviceManager(
                &resetToken, manager.GetAddressOf())) ||
            FAILED(manager->ResetDevice(device.Get(), resetToken))) {
            continue;
        }
        gpuDevice_ = std::move(device);
        gpuContext_ = std::move(context);
        gpuDeviceManager_ = std::move(manager);
        gpuResetToken_ = resetToken;
        return true;
    }

    SetError(
        "GPU recording could not open the processed texture on a matching "
        "D3D11 adapter.");
    return false;
}

void VideoRecorder::ResetGpuVideoProcessor()
{
    gpuVideoProcessor_.Reset();
    gpuVideoProcessorEnumerator_.Reset();
    gpuVideoContext_.Reset();
    gpuVideoDevice_.Reset();
}

bool VideoRecorder::InitializeGpuVideoProcessor(
    UINT width,
    UINT height,
    UINT frameRateNumerator,
    UINT frameRateDenominator)
{
    ResetGpuVideoProcessor();
    if (!gpuDevice_ || !gpuContext_ || width == 0 || height == 0) {
        SetError("GPU recording video processor is unavailable.");
        return false;
    }
    if (FAILED(gpuDevice_.As(&gpuVideoDevice_)) ||
        FAILED(gpuContext_.As(&gpuVideoContext_))) {
        SetError(
            "GPU recording could not access the D3D11 video processor.");
        ResetGpuVideoProcessor();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator =
        std::max(1u, frameRateNumerator);
    content.InputFrameRate.Denominator =
        std::max(1u, frameRateDenominator);
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT result = gpuVideoDevice_->CreateVideoProcessorEnumerator(
        &content, gpuVideoProcessorEnumerator_.GetAddressOf());
    UINT bgraSupport = 0;
    UINT nv12Support = 0;
    if (FAILED(result) ||
        FAILED(gpuVideoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &bgraSupport)) ||
        (bgraSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        FAILED(gpuVideoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_NV12, &nv12Support)) ||
        (nv12Support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0 ||
        FAILED(gpuVideoDevice_->CreateVideoProcessor(
            gpuVideoProcessorEnumerator_.Get(),
            0,
            gpuVideoProcessor_.GetAddressOf()))) {
        SetError(
            "GPU recording cannot convert the BGRA canvas to encoder-native "
            "NV12 on this adapter.");
        ResetGpuVideoProcessor();
        return false;
    }
    return true;
}

bool VideoRecorder::StartGpu(
    const std::wstring& filePath,
    UINT width,
    UINT height,
    UINT frameRateNumerator,
    UINT frameRateDenominator,
    Codec codec,
    const GpuVideoFrame& probeFrame,
    const AudioFormat* audioFormat)
{
    if (abandoned_.load()) {
        // Poisoned: a wedged worker may still be inside these COM
        // objects. Every public mutating entry fails fast so a
        // resumed (previously blocked) caller can never re-enter
        // the sink writer or start a new stream.
        return false;
    }
    Stop();
    terminalResult_.Reset();
    writeFailureHr_ = S_OK;
    if (GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetError("Recording output path already exists; it was preserved.");
        return false;
    }
    if (!probeFrame.IsValid() ||
        probeFrame.width != width || probeFrame.height != height) {
        SetError("GPU recording probe dimensions do not match the output.");
        return false;
    }

    ULONGLONG freeBytes = 0;
    if (QueryFreeBytesForPath(filePath, freeBytes) &&
        freeBytes < kMinFreeBytesToStart) {
        SetError(
            "Not enough free disk space to start recording (less than "
            "500 MB available). Free up some space and try again.");
        return false;
    }

    try {
        if (!InitializeGpuDevice(probeFrame.textureSharedHandle)) {
            return false;
        }
        bool useCompatibilityReadback =
            !InitializeGpuVideoProcessor(
                width,
                height,
                frameRateNumerator,
                frameRateDenominator);
        try {
            InitializeSink(filePath,
                           width,
                           height,
                           frameRateNumerator,
                           frameRateDenominator,
                           codec,
                           audioFormat,
                           useCompatibilityReadback
                               ? nullptr
                               : gpuDeviceManager_.Get(),
                           useCompatibilityReadback
                               ? MFVideoFormat_ARGB32
                               : MFVideoFormat_NV12);
        } catch (...) {
            // Some encoder MFTs reject a DXGI-backed input type even though
            // the adapter can open the shared texture. Keep the readback on
            // the recording worker instead of failing the recording or
            // blocking the UI/camera thread.
            sinkWriter_.Reset();
            pendingSample_.Reset();
            ResetGpuVideoProcessor();
            WIN32_FILE_ATTRIBUTE_DATA failedOutput{};
            if (GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard,
                                     &failedOutput)) {
                if (failedOutput.nFileSizeHigh != 0 ||
                    failedOutput.nFileSizeLow != 0 ||
                    (failedOutput.dwFileAttributes &
                     (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                    throw; // Preserve nonempty/unknown output; do not retry.
                }
                if (!DeleteFileW(filePath.c_str())) {
                    throw;
                }
            }
            InitializeSink(filePath,
                           width,
                           height,
                           frameRateNumerator,
                           frameRateDenominator,
                           codec,
                           audioFormat,
                           nullptr,
                           MFVideoFormat_ARGB32);
            useCompatibilityReadback = true;
        }
        frameWidth_ = width;
        frameHeight_ = height;
        frameRateNumerator_ = std::max(1u, frameRateNumerator);
        frameRateDenominator_ = std::max(1u, frameRateDenominator);
        timeline_.Reset(frameRateNumerator_, frameRateDenominator_);
        pendingSample_.Reset();
        pendingSampleTime100ns_ = -1;
        timelineEnd100ns_ = 0;
        videoSamplesWritten_ = 0;
        targetPath_ = filePath;
        lastSpaceCheckTicks_ = GetTickCount64();
        stopReason_ = StopReason::None;
        activeCodec_ = codec;
        audioEnabled_ =
            audioFormat != nullptr && audioFormat->IsValid();
        if (audioEnabled_) {
            audioFormat_ = *audioFormat;
        }
        gpuInputEnabled_ = !useCompatibilityReadback;
        gpuCompatibilityReadback_ = useCompatibilityReadback;
        recording_ = true;
        lastError_.clear();
        return true;
    } catch (const std::exception& error) {
        SetError(error.what());
    } catch (...) {
        SetError("Unknown exception starting GPU recorder");
    }
    Stop();
    return false;
}

VideoRecorder::FinalizeResult VideoRecorder::Stop()
{
    return FinalizeAndStop(StopReason::Manual);
}

VideoRecorder::FinalizeResult VideoRecorder::FinalizeAndStop(StopReason reason)
{
    if (abandoned_.load()) {
        // Never touch the sink writer again — the wedged worker may still
        // be inside WriteSample on it, and Finalize alongside WriteSample
        // on one sink writer is forbidden.
        FinalizeResult abandoned;
        abandoned.videoSamplesWritten = videoSamplesWritten_;
        recording_ = false;
        return abandoned;
    }
    StageResetGuard stageGuard(stage_);
    const bool hadSink = sinkWriter_ != nullptr;
    FinalizeResult result = terminalResult_.Last();
    if (sinkWriter_) {
        HRESULT trailingWriteHr = S_OK;
        try {
            if (pendingSample_ &&
                !WritePendingSample(timeline_.NominalDuration100ns())) {
                trailingWriteHr =
                    HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
            }
        } catch (const std::exception& error) {
            trailingWriteHr = E_FAIL;
            SetError(error.what());
        } catch (...) {
            trailingWriteHr = E_FAIL;
            SetError("Unknown error writing the final video sample");
        }
        // Finalize flushes the trailing fragment; already-written fragments
        // remain valid on disk regardless of either result.
        stage_.store(WriterStage::kFinalize, std::memory_order_relaxed);
        const HRESULT finalizeHr = sinkWriter_->Finalize();
        stage_.store(WriterStage::kIdle, std::memory_order_relaxed);
        const HRESULT outcomeHr = FAILED(writeFailureHr_)
            ? static_cast<HRESULT>(writeFailureHr_)
            : (reason == StopReason::WriteFailed ? E_FAIL
               : (FAILED(trailingWriteHr) ? trailingWriteHr : finalizeHr));
        result.hresult = static_cast<long>(finalizeHr);
        result.disposition =
            SUCCEEDED(outcomeHr)
                ? FinalizeDisposition::Completed
                : FinalizeDisposition::CompletedTruncated;
        result.hresult = static_cast<long>(outcomeHr);
        if (FAILED(outcomeHr) && lastError_.empty()) {
            SetError(
                "The recording's final fragment could not be completed (" +
                HrToString(outcomeHr) +
                "). The file was retained; playback has not been verified.");
        }
    }
    if (hadSink) {
        result.playableSeconds = DurationSeconds();
        result.videoSamplesWritten = videoSamplesWritten_;
    }
    sinkWriter_.Reset();
    pendingSample_.Reset();
    pendingSampleTime100ns_ = -1;
    if (recording_) {
        stopReason_ = reason;
    }
    recording_ = false;
    audioEnabled_ = false;
    gpuInputEnabled_ = false;
    gpuCompatibilityReadback_ = false;
    PollGpuReadLeases();
    // A rejected last sample may not have reached the encoder. Give already
    // flushed conversion queries a short bounded retirement window on this
    // worker; never wait indefinitely, submit another copy, or signal a fence.
    const ULONGLONG readerDeadline = GetTickCount64() + 100;
    while (!pendingGpuReads_.empty() && !gpuReadFaulted_ &&
           !abandoned_.load() && GetTickCount64() < readerDeadline) {
        Sleep(1);
        PollGpuReadLeases(true);
    }
    // Finalization normally retires these reads. Any still pending/unknown
    // owns a self-retaining graph, independent of encoder sample lifetime.
    pendingGpuReads_.clear();
    gpuReadFaulted_ = false;
    gpuFence_.Reset();
    gpuReadbackTexture_.Reset();
    ResetGpuVideoProcessor();
    gpuDeviceManager_.Reset();
    gpuContext_.Reset();
    gpuDevice_.Reset();
    gpuResetToken_ = 0;
    videoSamplesWritten_ = 0;
    terminalResult_.Remember(result);
    return result;
}

bool VideoRecorder::InitializeSink(const std::wstring& filePath,
                                   UINT width,
                                   UINT height,
                                   UINT frameRateNumerator,
                                   UINT frameRateDenominator,
                                   Codec codec,
                                   const AudioFormat* audioFormat,
                                   IUnknown* d3dManager,
                                   const GUID& inputSubtype)
{
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    ThrowIfFailed(MFCreateAttributes(attrs.GetAddressOf(), 4), "Create sink attributes");
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (d3dManager) {
        ThrowIfFailed(
            attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, d3dManager),
            "Set sink-writer D3D manager");
    }
    // Fragmented MP4: the file is playable up to the last completed fragment
    // even if the process dies mid-recording (no moov finalize needed).
    ThrowIfFailed(attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4),
                  "Set fragmented MP4 container");

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    ThrowIfFailed(MFCreateSinkWriterFromURL(filePath.c_str(), nullptr, attrs.Get(), writer.GetAddressOf()),
                  "Create sink writer");

    // Output type. The caller probes AV1 first and falls back to H.264 when
    // the installed Media Foundation encoder/container path rejects AV1.
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    ThrowIfFailed(MFCreateMediaType(outType.GetAddressOf()), "Create output type");
    ThrowIfFailed(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set out major type");
    const GUID& outputSubtype = codec == Codec::Av1 ? MFVideoFormat_AV1 : MFVideoFormat_H264;
    ThrowIfFailed(outType->SetGUID(MF_MT_SUBTYPE, outputSubtype), "Set out subtype");
    ThrowIfFailed(outType->SetUINT32(MF_MT_AVG_BITRATE, width * height * 5), "Set bitrate"); // rough default
    ThrowIfFailed(outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set interlace");
    ThrowIfFailed(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height), "Set out frame size");
    ThrowIfFailed(MFSetAttributeRatio(outType.Get(),
                                      MF_MT_FRAME_RATE,
                                      frameRateNumerator,
                                      frameRateDenominator),
                  "Set out frame rate");
    // Crash survivability: fragmented MP4 only protects COMPLETED fragments,
    // and the container cuts fragments on keyframe boundaries. Without an
    // explicit bound the fragment cadence rides on encoder GOP defaults; pin
    // the keyframe interval to ~2 seconds so a process crash can lose at
    // most ~2 s of encoded media (plus the OS write-cache tail on power
    // loss).
    const UINT32 keyframeSpacingFrames = std::max<UINT32>(
        1u,
        (2u * frameRateNumerator + frameRateDenominator - 1u) /
            std::max<UINT32>(1u, frameRateDenominator));
    ThrowIfFailed(outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING,
                                     keyframeSpacingFrames),
                  "Set keyframe spacing");
    ThrowIfFailed(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set PAR");

    ThrowIfFailed(writer->AddStream(outType.Get(), &streamIndex_), "Add stream");

    // Compatibility input uses system-memory BGRA. The GPU-fed path converts
    // the shareable BGRA recording canvas to NV12 with VideoProcessorBlt
    // before wrapping it in an IMF DXGI buffer. This avoids inserting a
    // D3D-unaware BGRA converter ahead of the hardware encoder.
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    ThrowIfFailed(MFCreateMediaType(inType.GetAddressOf()), "Create input type");
    ThrowIfFailed(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set in major type");
    ThrowIfFailed(inType->SetGUID(MF_MT_SUBTYPE, inputSubtype), "Set in subtype");
    ThrowIfFailed(inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set in interlace");
    ThrowIfFailed(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height), "Set in frame size");
    ThrowIfFailed(MFSetAttributeRatio(inType.Get(),
                                      MF_MT_FRAME_RATE,
                                      frameRateNumerator,
                                      frameRateDenominator),
                  "Set in frame rate");
    ThrowIfFailed(MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set PAR");

    ThrowIfFailed(writer->SetInputMediaType(streamIndex_, inType.Get(), nullptr), "Set input media type");

    if (audioFormat && audioFormat->IsValid()) {
        const UINT32 blockAlignment =
            audioFormat->channels *
            audioFormat->bitsPerSample / 8u;

        Microsoft::WRL::ComPtr<IMFMediaType> audioOutput;
        ThrowIfFailed(
            MFCreateMediaType(audioOutput.GetAddressOf()),
            "Create AAC output type");
        ThrowIfFailed(
            audioOutput->SetGUID(
                MF_MT_MAJOR_TYPE, MFMediaType_Audio),
            "Set AAC output major type");
        ThrowIfFailed(
            audioOutput->SetGUID(
                MF_MT_SUBTYPE, MFAudioFormat_AAC),
            "Set AAC output subtype");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AUDIO_NUM_CHANNELS,
                audioFormat->channels),
            "Set AAC channels");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AUDIO_SAMPLES_PER_SECOND,
                audioFormat->sampleRate),
            "Set AAC sample rate");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AUDIO_BITS_PER_SAMPLE,
                audioFormat->bitsPerSample),
            "Set AAC bits per sample");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000),
            "Set AAC bitrate");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AAC_PAYLOAD_TYPE, 0),
            "Set AAC payload type");
        ThrowIfFailed(
            audioOutput->SetUINT32(
                MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29),
            "Set AAC profile");
        ThrowIfFailed(
            writer->AddStream(
                audioOutput.Get(), &audioStreamIndex_),
            "Add AAC stream");

        Microsoft::WRL::ComPtr<IMFMediaType> audioInput;
        ThrowIfFailed(
            MFCreateMediaType(audioInput.GetAddressOf()),
            "Create PCM input type");
        ThrowIfFailed(
            audioInput->SetGUID(
                MF_MT_MAJOR_TYPE, MFMediaType_Audio),
            "Set PCM input major type");
        ThrowIfFailed(
            audioInput->SetGUID(
                MF_MT_SUBTYPE, MFAudioFormat_PCM),
            "Set PCM input subtype");
        ThrowIfFailed(
            audioInput->SetUINT32(
                MF_MT_AUDIO_NUM_CHANNELS,
                audioFormat->channels),
            "Set PCM channels");
        ThrowIfFailed(
            audioInput->SetUINT32(
                MF_MT_AUDIO_SAMPLES_PER_SECOND,
                audioFormat->sampleRate),
            "Set PCM sample rate");
        ThrowIfFailed(
            audioInput->SetUINT32(
                MF_MT_AUDIO_BITS_PER_SAMPLE,
                audioFormat->bitsPerSample),
            "Set PCM bits per sample");
        ThrowIfFailed(
            audioInput->SetUINT32(
                MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlignment),
            "Set PCM block alignment");
        ThrowIfFailed(
            audioInput->SetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                audioFormat->sampleRate * blockAlignment),
            "Set PCM byte rate");
        ThrowIfFailed(
            writer->SetInputMediaType(
                audioStreamIndex_, audioInput.Get(), nullptr),
            "Set PCM input type");
    }

    ThrowIfFailed(writer->BeginWriting(), "BeginWriting");

    sinkWriter_ = writer;
    return true;
}

bool VideoRecorder::AddAudioFrame(
    const std::uint8_t* pcmData,
    std::size_t byteCount,
    std::int64_t sampleTime100ns,
    std::int64_t duration100ns)
{
    if (abandoned_.load() || !recording_ || !sinkWriter_ || !audioEnabled_ ||
        !pcmData || byteCount == 0 || sampleTime100ns < 0) {
        return false;
    }
    try {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(
            MFCreateMemoryBuffer(
                static_cast<DWORD>(byteCount),
                buffer.GetAddressOf()),
            "Create audio buffer");
        BYTE* destination = nullptr;
        ThrowIfFailed(
            buffer->Lock(&destination, nullptr, nullptr),
            "Lock audio buffer");
        std::memcpy(destination, pcmData, byteCount);
        buffer->Unlock();
        ThrowIfFailed(
            buffer->SetCurrentLength(
                static_cast<DWORD>(byteCount)),
            "Set audio buffer length");

        Microsoft::WRL::ComPtr<IMFSample> sample;
        ThrowIfFailed(
            MFCreateSample(sample.GetAddressOf()),
            "Create audio sample");
        ThrowIfFailed(
            sample->AddBuffer(buffer.Get()),
            "Attach audio buffer");
        ThrowIfFailed(
            sample->SetSampleTime(sampleTime100ns),
            "Set audio sample time");
        ThrowIfFailed(
            sample->SetSampleDuration(
                std::max<std::int64_t>(1, duration100ns)),
            "Set audio sample duration");
        StageResetGuard stageGuard(stage_);
        stage_.store(WriterStage::kWriteAudioSample,
                     std::memory_order_relaxed);
        const HRESULT audioWriteHr =
            sinkWriter_->WriteSample(audioStreamIndex_, sample.Get());
        if (FAILED(audioWriteHr)) {
            writeFailureHr_ = static_cast<long>(audioWriteHr);
        }
        ThrowIfFailed(audioWriteHr, "Write audio sample");
        timelineEnd100ns_ =
            std::max(
                timelineEnd100ns_,
                sampleTime100ns +
                    std::max<std::int64_t>(1, duration100ns));
        return true;
    } catch (const std::exception& error) {
        SetError(error.what());
    } catch (...) {
        SetError("Unknown exception adding audio");
    }
    return false;
}

bool VideoRecorder::WritePendingSample(std::int64_t duration100ns)
{
    if (!pendingSample_ || !sinkWriter_) {
        return true;
    }
    const std::int64_t safeDuration =
        std::max<std::int64_t>(1, duration100ns);
    ThrowIfFailed(
        pendingSample_->SetSampleDuration(safeDuration),
        "SetSampleDuration");
    stage_.store(WriterStage::kWriteVideoSample, std::memory_order_relaxed);
    const HRESULT writeHr =
        sinkWriter_->WriteSample(streamIndex_, pendingSample_.Get());
    stage_.store(WriterStage::kIdle, std::memory_order_relaxed);
    if (FAILED(writeHr)) {
        // Never retry an already rejected sample during teardown. Preserve
        // the original HRESULT even if Finalize subsequently succeeds.
        writeFailureHr_ = static_cast<long>(writeHr);
        pendingSample_.Reset();
        pendingSampleTime100ns_ = -1;
    }
    if (IsDiskFullError(writeHr)) {
        SetError(kDiskFullMessage);
        return false;
    }
    ThrowIfFailed(writeHr, "WriteSample");
    ++videoSamplesWritten_;
    timelineEnd100ns_ =
        std::max(timelineEnd100ns_,
                 pendingSampleTime100ns_ + safeDuration);
    pendingSample_.Reset();
    pendingSampleTime100ns_ = -1;
    return true;
}

bool VideoRecorder::CheckDiskSpace()
{
    const ULONGLONG nowTicks = GetTickCount64();
    if (nowTicks - lastSpaceCheckTicks_ < kSpaceCheckIntervalMs) {
        return true;
    }
    lastSpaceCheckTicks_ = nowTicks;
    ULONGLONG freeBytes = 0;
    if (!QueryFreeBytesForPath(targetPath_, freeBytes) ||
        freeBytes >= kMinFreeBytesWhileRecording) {
        return true;
    }
    FinalizeAndStop(StopReason::DiskFull);
    SetError(kDiskFullMessage);
    return false;
}

bool VideoRecorder::QueueVideoSample(
    Microsoft::WRL::ComPtr<IMFSample> sample,
    const RecordingFrameIdentity& identity)
{
    if (!sample || !CheckDiskSpace()) {
        return false;
    }
    const std::int64_t sampleTime =
        timeline_.MapTimestamp(identity.captureTimestamp100ns);
    ThrowIfFailed(sample->SetSampleTime(sampleTime), "SetSampleTime");
    if (pendingSample_ &&
        !WritePendingSample(sampleTime - pendingSampleTime100ns_)) {
        FinalizeAndStop(StopReason::DiskFull);
        return false;
    }
    pendingSample_ = std::move(sample);
    pendingSampleTime100ns_ = sampleTime;
    timelineEnd100ns_ = std::max(
        timelineEnd100ns_,
        sampleTime + timeline_.NominalDuration100ns());
    return true;
}

bool VideoRecorder::AddFrame(const uint8_t* bgraData,
                             size_t strideBytes,
                             const RecordingFrameIdentity& identity)
{
    if (abandoned_.load() || !recording_ || !sinkWriter_ || !bgraData) {
        return false;
    }

    StageResetGuard stageGuard(stage_);
    try {
        const size_t bufferSize = strideBytes * frameHeight_;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateMemoryBuffer(static_cast<DWORD>(bufferSize), buffer.GetAddressOf()),
                      "CreateMemoryBuffer");

        BYTE* dest = nullptr;
        DWORD maxLen = 0;
        ThrowIfFailed(buffer->Lock(&dest, &maxLen, nullptr), "Lock buffer");
        std::memcpy(dest, bgraData, bufferSize);
        ThrowIfFailed(buffer->Unlock(), "Unlock buffer");
        ThrowIfFailed(buffer->SetCurrentLength(static_cast<DWORD>(bufferSize)), "SetCurrentLength");

        Microsoft::WRL::ComPtr<IMFSample> sample;
        ThrowIfFailed(MFCreateSample(sample.GetAddressOf()), "CreateSample");
        ThrowIfFailed(sample->AddBuffer(buffer.Get()), "AddBuffer");
        return QueueVideoSample(std::move(sample), identity);
    } catch (const std::exception& e) {
        SetError(e.what());
    } catch (...) {
        SetError("Unknown exception adding frame");
    }
    FinalizeAndStop(StopReason::WriteFailed);
    return false;
}

bool VideoRecorder::AddGpuFrame(
    const GpuVideoFrame& frame,
    const RecordingFrameIdentity& identity)
{
    if (abandoned_.load() || !recording_ || !sinkWriter_ ||
        (!gpuInputEnabled_ && !gpuCompatibilityReadback_) ||
        !gpuDevice_ || !gpuContext_ || !frame.IsValid() ||
        frame.width != frameWidth_ || frame.height != frameHeight_) {
        return false;
    }

    PollGpuReadLeases();
    if (gpuReadFaulted_) return false;
    // Offline/bursty callers can fill this bounded reader ring faster than
    // already-flushed work retires. Backpressure belongs on the recorder
    // worker, never the UI/producer, and must not discard an unfinished lease.
    if (pendingGpuReads_.size() >= 48) {
        const ULONGLONG readerDeadline = GetTickCount64() + 100;
        while (pendingGpuReads_.size() >= 48 && !gpuReadFaulted_ &&
               !abandoned_.load() && GetTickCount64() < readerDeadline) {
            Sleep(1);
            PollGpuReadLeases(true);
        }
        if (abandoned_.load() || gpuReadFaulted_) return false;
        if (pendingGpuReads_.size() >= 48) {
            SetError("GPU recording reader completion exceeded the 100 ms admission deadline (48 pending reads retained; query " +
                     HrToString(lastReaderQueryHr_) + ", complete=" +
                     std::to_string(lastReaderQueryComplete_) + ", device " +
                     HrToString(lastReaderDeviceHr_) + ", producer=" +
                     std::to_string(lastReaderProducerCompleted_) + "/" +
                     std::to_string(lastReaderProducerRequired_) + ").");
            return false;
        }
    }

    StageResetGuard stageGuard(stage_);
    try {
        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        ThrowIfFailed(gpuDevice_.As(&device1), "Query D3D11 device1");
        stage_.store(WriterStage::kOpenSharedTexture,
                     std::memory_order_relaxed);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device1->OpenSharedResource1(
                frame.textureSharedHandle,
                IID_PPV_ARGS(&texture)),
            "Open shared recording texture");

        if (!gpuFence_) {
            Microsoft::WRL::ComPtr<ID3D11Device5> device5;
            ThrowIfFailed(gpuDevice_.As(&device5), "Query D3D11 device5");
            stage_.store(WriterStage::kOpenSharedFence,
                         std::memory_order_relaxed);
            ThrowIfFailed(
                device5->OpenSharedFence(
                    frame.fenceSharedHandle,
                    IID_PPV_ARGS(&gpuFence_)),
                "Open shared recording fence");
        }
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
        ThrowIfFailed(gpuContext_.As(&context4), "Query D3D11 context4");
        stage_.store(WriterStage::kGpuSync, std::memory_order_relaxed);
        ThrowIfFailed(context4->Wait(gpuFence_.Get(), frame.readyFenceValue),
                      "Queue recording producer fence wait");

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != frameWidth_ || desc.Height != frameHeight_ ||
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            throw std::runtime_error(
                "Shared recording texture has an incompatible format");
        }

        if (gpuCompatibilityReadback_) {
            bool recreateReadback = !gpuReadbackTexture_;
            if (gpuReadbackTexture_) {
                D3D11_TEXTURE2D_DESC existing{};
                gpuReadbackTexture_->GetDesc(&existing);
                recreateReadback =
                    existing.Width != desc.Width ||
                    existing.Height != desc.Height ||
                    existing.Format != desc.Format;
            }
            if (recreateReadback) {
                D3D11_TEXTURE2D_DESC readbackDesc = desc;
                readbackDesc.Usage = D3D11_USAGE_STAGING;
                readbackDesc.BindFlags = 0;
                readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                readbackDesc.MiscFlags = 0;
                ThrowIfFailed(
                    gpuDevice_->CreateTexture2D(
                        &readbackDesc,
                        nullptr,
                        gpuReadbackTexture_.ReleaseAndGetAddressOf()),
                    "Create worker recording readback texture");
            }

            auto reader = std::make_shared<GpuReadLease>();
            reader->device = gpuDevice_;
            reader->context = gpuContext_;
            reader->input = texture;
            reader->output = gpuReadbackTexture_;
            reader->source = frame.lifetime;
            reader->producerFence = gpuFence_;
            reader->producerValue = frame.readyFenceValue;
            const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
            ThrowIfFailed(gpuDevice_->CreateQuery(&queryDesc, &reader->query),
                          "Create recording copy completion query");
            auto retirement = std::make_shared<GpuReadRetirement>(reader);
            std::vector<std::uint8_t> compact(
                static_cast<std::size_t>(frameWidth_) *
                frameHeight_ * 4u);
            pendingGpuReads_.push_back({reader, retirement});
            retirement->Arm(retirement);
            gpuContext_->CopyResource(
                gpuReadbackTexture_.Get(), texture.Get());
            gpuContext_->End(reader->query.Get());
            reader->queryRecorded = true;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            stage_.store(WriterStage::kReadbackMap,
                         std::memory_order_relaxed);
            ThrowIfFailed(
                gpuContext_->Map(
                    gpuReadbackTexture_.Get(),
                    0,
                    D3D11_MAP_READ,
                    0,
                    &mapped),
                "Map worker recording readback texture");
            const std::size_t rowBytes =
                static_cast<std::size_t>(frameWidth_) * 4u;
            for (UINT row = 0; row < frameHeight_; ++row) {
                std::memcpy(
                    compact.data() +
                        static_cast<std::size_t>(row) * rowBytes,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    rowBytes);
            }
            gpuContext_->Unmap(gpuReadbackTexture_.Get(), 0);
            PollGpuReadLeases();
            return AddFrame(
                compact.data(), rowBytes, identity);
        }

        if (!gpuVideoDevice_ || !gpuVideoContext_ ||
            !gpuVideoProcessorEnumerator_ || !gpuVideoProcessor_) {
            throw std::runtime_error(
                "GPU recording NV12 converter is unavailable");
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDescription{};
        inputViewDescription.FourCC = 0;
        inputViewDescription.ViewDimension =
            D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDescription.Texture2D.MipSlice = 0;
        inputViewDescription.Texture2D.ArraySlice = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        ThrowIfFailed(
            gpuVideoDevice_->CreateVideoProcessorInputView(
                texture.Get(),
                gpuVideoProcessorEnumerator_.Get(),
                &inputViewDescription,
                inputView.GetAddressOf()),
            "Create recording BGRA input view");

        D3D11_TEXTURE2D_DESC encoderDescription{};
        encoderDescription.Width = frameWidth_;
        encoderDescription.Height = frameHeight_;
        encoderDescription.MipLevels = 1;
        encoderDescription.ArraySize = 1;
        encoderDescription.Format = DXGI_FORMAT_NV12;
        encoderDescription.SampleDesc.Count = 1;
        encoderDescription.Usage = D3D11_USAGE_DEFAULT;
        encoderDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> encoderTexture;
        stage_.store(WriterStage::kCreateEncoderTexture,
                     std::memory_order_relaxed);
        ThrowIfFailed(
            gpuDevice_->CreateTexture2D(
                &encoderDescription,
                nullptr,
                encoderTexture.GetAddressOf()),
            "Create recording NV12 encoder texture");

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDescription{};
        outputViewDescription.ViewDimension =
            D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputViewDescription.Texture2D.MipSlice = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        ThrowIfFailed(
            gpuVideoDevice_->CreateVideoProcessorOutputView(
                encoderTexture.Get(),
                gpuVideoProcessorEnumerator_.Get(),
                &outputViewDescription,
                outputView.GetAddressOf()),
            "Create recording NV12 output view");

        const RECT frameRect{
            0,
            0,
            static_cast<LONG>(frameWidth_),
            static_cast<LONG>(frameHeight_)};
        gpuVideoContext_->VideoProcessorSetOutputTargetRect(
            gpuVideoProcessor_.Get(), TRUE, &frameRect);
        gpuVideoContext_->VideoProcessorSetStreamSourceRect(
            gpuVideoProcessor_.Get(), 0, TRUE, &frameRect);
        gpuVideoContext_->VideoProcessorSetStreamDestRect(
            gpuVideoProcessor_.Get(), 0, TRUE, &frameRect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.pInputSurface = inputView.Get();
        auto reader = std::make_shared<GpuReadLease>();
        reader->device = gpuDevice_;
        reader->context = gpuContext_;
        reader->input = texture;
        reader->output = encoderTexture;
        reader->inputView = inputView;
        reader->outputView = outputView;
        reader->processor = gpuVideoProcessor_;
        reader->source = frame.lifetime;
        reader->producerFence = gpuFence_;
        reader->producerValue = frame.readyFenceValue;
        const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        ThrowIfFailed(gpuDevice_->CreateQuery(&queryDesc, &reader->query),
                      "Create recording reader completion query");
        auto retirement = std::make_shared<GpuReadRetirement>(reader);
        // Every allocation happens before submission. Arm before Blt: failure
        // can leave partial work queued, whose completion is then unknown.
        pendingGpuReads_.push_back({reader, retirement});
        retirement->Arm(retirement);
        stage_.store(WriterStage::kConvertNv12, std::memory_order_relaxed);
        ThrowIfFailed(
            gpuVideoContext_->VideoProcessorBlt(
                gpuVideoProcessor_.Get(),
                outputView.Get(),
                0,
                1,
                &stream),
            "Convert recording canvas to NV12");
        gpuContext_->End(reader->query.Get());
        reader->queryRecorded = true;
        // The NVIDIA encoder can consume the texture on a different internal
        // command path. Submit the video-processor work before handing the
        // texture to Media Foundation so the encoder never observes an
        // unsubmitted conversion.
        stage_.store(WriterStage::kSubmitGpuWork, std::memory_order_relaxed);
        gpuContext_->Flush();

        stage_.store(WriterStage::kCreateSampleBuffer,
                     std::memory_order_relaxed);
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(
            MFCreateDXGISurfaceBuffer(
                __uuidof(ID3D11Texture2D),
                encoderTexture.Get(),
                0,
                FALSE,
                buffer.GetAddressOf()),
            "Create NV12 DXGI video buffer");
        // MFCreateDXGISurfaceBuffer initially reports zero valid bytes. Some
        // hardware encoder MFTs reject that sample with E_INVALIDARG even
        // though the backing texture is complete. Mark the whole surface as
        // valid before it enters the sink writer.
        DWORD bufferLength = 0;
        ThrowIfFailed(
            buffer->GetMaxLength(&bufferLength),
            "Get NV12 DXGI video buffer length");
        ThrowIfFailed(
            buffer->SetCurrentLength(bufferLength),
            "Set NV12 DXGI video buffer length");
        Microsoft::WRL::ComPtr<IMFSample> sample;
        ThrowIfFailed(
            MFCreateSample(sample.GetAddressOf()),
            "Create GPU video sample");
        ThrowIfFailed(
            sample->AddBuffer(buffer.Get()),
            "Attach DXGI video buffer");
        auto lifetime = Microsoft::WRL::Make<FrameLifetimeHolder>(
            frame.lifetime);
        if (!lifetime) {
            throw std::runtime_error(
                "Could not retain the shared recording frame");
        }
        ThrowIfFailed(
            sample->SetUnknown(
                kOkuFlowFrameLifetimeAttribute, lifetime.Get()),
            "Retain shared recording frame");
        return QueueVideoSample(std::move(sample), identity);
    } catch (const std::exception& error) {
        SetError(error.what());
    } catch (...) {
        SetError("Unknown exception adding GPU video frame");
    }
    FinalizeAndStop(StopReason::WriteFailed);
    return false;
}

double VideoRecorder::DurationSeconds() const
{
    return static_cast<double>(timelineEnd100ns_) /
           static_cast<double>(kMediaFoundationTicksPerSecond);
}

} // namespace okuflow

#endif // _WIN32
