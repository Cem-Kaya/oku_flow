#ifdef _WIN32

// Exercises the GPU-fed recording submission path end to end without the
// application: a shared D3D12 BGRA texture and shared fence (allocated with
// exactly the production recording pool's parameters) are handed to
// VideoRecorder::StartGpu/AddGpuFrame, which open them through D3D11,
// convert to NV12 with the video processor, and feed DXGI samples to the
// hardware-enabled sink writer. This is the path where the 2026-07-30
// first-WriteSample wedge lived; the CTest timeout doubles as the wedge
// detector. Exit codes: 0 pass, 1 fail, 77 clean skip (no D3D12 device or
// no GPU-fed writer support on this machine).

#include "okuflow/common/media_writer.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 320;
constexpr UINT kHeight = 180;
constexpr int kSkipExitCode = 77;

std::filesystem::path TemporaryMp4Path()
{
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(directory), directory);
    wchar_t file[MAX_PATH]{};
    GetTempFileNameW(directory, L"ozg", 0, file);
    std::filesystem::path path(file);
    std::filesystem::remove(path);
    path.replace_extension(L".mp4");
    return path;
}

bool ContainsAudioStream(const std::filesystem::path& path)
{
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(
            path.c_str(), nullptr, reader.GetAddressOf()))) {
        return false;
    }
    for (DWORD stream = 0; stream < 8; ++stream) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(
                stream, 0, type.GetAddressOf()))) {
            break;
        }
        GUID major{};
        if (SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
            major == MFMediaType_Audio) {
            return true;
        }
    }
    return false;
}

std::size_t CountMoofBoxes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    std::size_t count = 0;
    for (std::size_t at = bytes.find("moof");
         at != std::string::npos;
         at = bytes.find("moof", at + 4)) {
        ++count;
    }
    return count;
}

// Keeps the D3D12 objects behind the shared handles alive for as long as
// any queued Media Foundation sample references the frame — the same
// contract the production recording pool provides via its lease.
struct SharedFrameKeepalive {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Fence> fence;
};

struct SharedFrameContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Fence> fence;
    HANDLE textureHandle{nullptr};
    HANDLE fenceHandle{nullptr};
    okuflow::GpuVideoFrame frame;

    ~SharedFrameContext()
    {
        if (textureHandle) {
            CloseHandle(textureHandle);
        }
        if (fenceHandle) {
            CloseHandle(fenceHandle);
        }
    }
};

// Builds the shared BGRA frame with exactly the production recording pool's
// allocation parameters (presenter.cpp RecordingFramePoolState): SHARED
// heap, ALLOW_RENDER_TARGET | ALLOW_SIMULTANEOUS_ACCESS, COMMON state.
// Returns false when this machine cannot create or share the resources —
// the caller turns that into a clean skip.
bool CreateSharedFrame(SharedFrameContext& context)
{
    if (FAILED(D3D12CreateDevice(nullptr,
                                 D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&context.device)))) {
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(context.device->CreateCommandQueue(
            &queueDesc, IID_PPV_ARGS(&context.queue)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = kWidth;
    textureDesc.Height = kHeight;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = textureDesc.Format;
    clearValue.Color[3] = 1.0f;
    if (FAILED(context.device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_SHARED,
            &textureDesc,
            D3D12_RESOURCE_STATE_COMMON,
            &clearValue,
            IID_PPV_ARGS(&context.texture)))) {
        return false;
    }

    // Upload a deterministic gradient once; simultaneous-access textures
    // promote from COMMON for copies, so no explicit barriers are needed.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 uploadBytes = 0;
    context.device->GetCopyableFootprints(
        &textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &uploadBytes);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(context.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload)))) {
        return false;
    }
    std::uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr,
                           reinterpret_cast<void**>(&mapped)))) {
        return false;
    }
    for (UINT row = 0; row < rows; ++row) {
        std::uint8_t* line = mapped + footprint.Offset +
                             static_cast<std::size_t>(row) *
                                 footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kWidth; ++x) {
            line[x * 4 + 0] = static_cast<std::uint8_t>((x * 3 + row) % 255);
            line[x * 4 + 1] = static_cast<std::uint8_t>((row * 5) % 255);
            line[x * 4 + 2] = 96;
            line[x * 4 + 3] = 255;
        }
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(context.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(context.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = context.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    context.queue->ExecuteCommandLists(1, lists);

    if (FAILED(context.device->CreateFence(
            0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&context.fence)))) {
        return false;
    }
    if (FAILED(context.queue->Signal(context.fence.Get(), 1))) {
        return false;
    }
    const HANDLE completion =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion) {
        return false;
    }
    context.fence->SetEventOnCompletion(1, completion);
    WaitForSingleObject(completion, 10'000);
    CloseHandle(completion);

    if (FAILED(context.device->CreateSharedHandle(
            context.texture.Get(), nullptr, GENERIC_ALL, nullptr,
            &context.textureHandle)) ||
        FAILED(context.device->CreateSharedHandle(
            context.fence.Get(), nullptr, GENERIC_ALL, nullptr,
            &context.fenceHandle))) {
        return false;
    }

    auto keepalive = std::make_shared<SharedFrameKeepalive>();
    keepalive->device = context.device;
    keepalive->texture = context.texture;
    keepalive->fence = context.fence;
    context.frame.textureSharedHandle = context.textureHandle;
    context.frame.fenceSharedHandle = context.fenceHandle;
    context.frame.readyFenceValue = 1;
    context.frame.width = kWidth;
    context.frame.height = kHeight;
    context.frame.lifetime = keepalive;
    return context.frame.IsValid();
}

// Same contract as the CPU-fed codec pass: ~3 s of frames, audio muxed,
// playable output, exact sample accounting, and at least two completed
// fMP4 fragments (the crash-survivability cadence) — but submitted through
// StartGpu/AddGpuFrame. Sets *skipped when this codec's GPU-fed writer is
// unavailable here.
bool RunGpuCodecPass(okuflow::VideoRecorder::Codec codec,
                     const okuflow::GpuVideoFrame& frame,
                     bool* skipped)
{
    *skipped = false;
    const char* codecName = okuflow::VideoRecorder::CodecName(codec);
    const std::filesystem::path output = TemporaryMp4Path();
    okuflow::VideoRecorder recorder;
    // Self-diagnosing watcher: if any writer call wedges (the 2026-07-30
    // incident class), this names the exact blocked stage on stderr once a
    // second instead of leaving a silent hang for the CTest timeout.
    std::atomic<bool> watching{true};
    std::atomic<int> lastFrame{-1};
    std::thread watcher([&recorder, &watching, &lastFrame, codecName]() {
        int quietTicks = 0;
        int previousFrame = -2;
        while (watching.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!watching.load()) {
                return;
            }
            const int frame = lastFrame.load();
            quietTicks = frame == previousFrame ? quietTicks + 1 : 0;
            previousFrame = frame;
            if (quietTicks >= 1) {
                std::cerr << "[watch] " << codecName << " frame " << frame
                          << " stage: "
                          << okuflow::VideoRecorder::StageName(
                                 recorder.ActiveStage())
                          << std::endl;
            }
        }
    });
    struct WatcherJoin {
        std::atomic<bool>& flag;
        std::thread& thread;
        ~WatcherJoin()
        {
            flag.store(false);
            if (thread.joinable()) {
                thread.join();
            }
        }
    } watcherJoin{watching, watcher};

    std::cerr << "[test] " << codecName << " StartGpu..." << std::endl;
    const okuflow::VideoRecorder::AudioFormat audioFormat{};
    bool passed = recorder.StartGpu(
        output.wstring(), kWidth, kHeight, 30, 1, codec, frame,
        &audioFormat);
    std::cerr << "[test] " << codecName << " StartGpu -> "
              << (passed ? "ok" : "unavailable") << std::endl;
    if (!passed) {
        std::cerr << "GPU-fed " << codecName
                  << " writer unavailable on this machine: "
                  << recorder.LastError() << '\n';
        *skipped = true;
        std::error_code cleanupError;
        std::filesystem::remove(output, cleanupError);
        return true;
    }

    std::vector<std::int16_t> audio(1600u, 0);
    constexpr std::int64_t frameDuration100ns = 333'333;
    constexpr double pi = 3.14159265358979323846;
    constexpr int kFrameCount = 90;
    int framesSubmitted = 0;
    for (int index = 0; passed && index < kFrameCount; ++index) {
        for (std::size_t sample = 0; sample < audio.size(); ++sample) {
            const double phase =
                2.0 * pi * 440.0 *
                (index * audio.size() + sample) / 48'000.0;
            audio[sample] =
                static_cast<std::int16_t>(std::sin(phase) * 6000.0);
        }
        okuflow::RecordingFrameIdentity identity;
        identity.captureTimestamp100ns =
            static_cast<std::int64_t>(index) * frameDuration100ns;
        identity.captureClock100ns = identity.captureTimestamp100ns;
        identity.sequenceNumber = static_cast<std::uint64_t>(index);
        identity.frameRateNumerator = 30;
        identity.frameRateDenominator = 1;
        lastFrame.store(index);
        passed = recorder.AddGpuFrame(frame, identity);
        if (passed) {
            ++framesSubmitted;
        }
        passed = passed && recorder.AddAudioFrame(
            reinterpret_cast<const std::uint8_t*>(audio.data()),
            audio.size() * sizeof(std::int16_t),
            identity.captureTimestamp100ns,
            frameDuration100ns);
    }

    std::cerr << "[test] " << codecName << " finalizing..." << std::endl;
    const okuflow::VideoRecorder::FinalizeResult finalized =
        recorder.Stop();
    std::cerr << "[test] " << codecName << " finalized" << std::endl;
    passed = passed && finalized.HasPlayableVideo() &&
             finalized.videoSamplesWritten == kFrameCount &&
             std::filesystem::exists(output) &&
             std::filesystem::file_size(output) > 1024u &&
             ContainsAudioStream(output);
    if (!passed) {
        std::cerr << "GPU-fed " << codecName
                  << "/AAC MP4 validation failed: "
                  << recorder.LastError()
                  << " | submitted=" << framesSubmitted
                  << " | finalized samples=" << finalized.videoSamplesWritten
                  << " | disposition=" << static_cast<int>(finalized.disposition)
                  << " | HRESULT=" << finalized.hresult
                  << " | duration=" << finalized.playableSeconds
                  << " | exists=" << std::filesystem::exists(output);
        if (std::filesystem::exists(output)) {
            std::cerr << " | bytes=" << std::filesystem::file_size(output)
                      << " | audio=" << ContainsAudioStream(output);
        }
        std::cerr << '\n';
    }
    if (passed) {
        const std::size_t moofCount = CountMoofBoxes(output);
        if (moofCount < 2) {
            passed = false;
            std::cerr << "GPU-fed " << codecName
                      << ": expected at least two fMP4 fragments for a "
                         "3 s recording; found "
                      << moofCount << '\n';
        } else {
            std::cout << "GPU-fed " << codecName << ": " << moofCount
                      << " fragments across 3 s\n";
        }
    }
    std::error_code removeError;
    std::filesystem::remove(output, removeError);
    return passed;
}

} // namespace

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        std::cerr << "COM initialization failed\n";
        return 1;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        std::cerr << "Media Foundation initialization failed\n";
        return 1;
    }

    int exitCode = 0;
    {
        SharedFrameContext context;
        if (!CreateSharedFrame(context)) {
            std::cout << "SKIP: no D3D12 device with shared-resource "
                         "support on this machine\n";
            exitCode = kSkipExitCode;
        } else {
            bool h264Skipped = false;
            bool passed = RunGpuCodecPass(
                okuflow::VideoRecorder::Codec::H264,
                context.frame,
                &h264Skipped);
            if (h264Skipped) {
                // GPU-fed writing is hardware/driver dependent even for
                // H.264 (shared-handle open, video processor). Absence is
                // a clean skip, unlike the CPU-fed suite.
                std::cout << "SKIP: GPU-fed writer unavailable\n";
                exitCode = kSkipExitCode;
            } else {
                bool av1Skipped = false;
                const bool av1Passed = RunGpuCodecPass(
                    okuflow::VideoRecorder::Codec::Av1,
                    context.frame,
                    &av1Skipped);
                std::cout << "GPU-fed AV1 leg: "
                          << (av1Skipped
                                  ? "SKIPPED (no hardware AV1 encoder)"
                                  : (av1Passed ? "PASSED" : "FAILED"))
                          << '\n';
                passed = passed && av1Passed;
                exitCode = passed ? 0 : 1;
            }
        }
    }

    MFShutdown();
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return exitCode;
}

#endif // _WIN32
