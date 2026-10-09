#ifdef _WIN32

#include "media_capture_session.hpp"
#include "openzoom/capture/capture_texture.hpp"
#include "openzoom/capture/capture_buffer.hpp"
#include "openzoom/capture/capture_color.hpp"

#include <QDebug>

#include <d3d10_1.h>
#include <dxgi1_6.h>
#include <mferror.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <sstream>

namespace openzoom {

namespace {

std::wstring GuidStableText(const GUID& guid)
{
    wchar_t buffer[40]{};
    if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return L"{unknown}";
    }
    return buffer;
}

std::wstring MakeFormatStableId(const GUID& subtype,
                                UINT width,
                                UINT height,
                                UINT numerator,
                                UINT denominator)
{
    std::wostringstream stream;
    stream << GuidStableText(subtype) << L"_" << width << L"x" << height
           << L"@" << numerator << L"/" << denominator;
    return stream.str();
}

std::string FormatDescription(const VideoFormat& format)
{
    std::ostringstream stream;
    stream << format.width << "x" << format.height << " @ ";
    if (format.denominator == 0) {
        stream << "?";
    } else {
        stream << static_cast<double>(format.numerator) /
                      static_cast<double>(format.denominator);
    }
    stream << " fps";
    return stream.str();
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> SelectHighPerformanceAdapter()
{
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(
            0, IID_PPV_ARGS(factory.GetAddressOf())))) {
        return {};
    }

    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(adapter.GetAddressOf()));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result) || !adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            return adapter;
        }
    }
    return {};
}

std::string FormatHResult(HRESULT hr)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));

    LPSTR systemMessage = nullptr;
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                            FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        static_cast<DWORD>(hr),
                                        0,
                                        reinterpret_cast<LPSTR>(&systemMessage),
                                        0,
                                        nullptr);

    std::string result = std::string("hr=") + code;
    if (length != 0 && systemMessage) {
        std::string detail(systemMessage, length);
        while (!detail.empty() && std::isspace(static_cast<unsigned char>(detail.back()))) {
            detail.pop_back();
        }
        if (!detail.empty()) {
            result += ": " + detail;
        }
    }
    if (systemMessage) {
        LocalFree(systemMessage);
    }
    return result;
}

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(message) + " (" + FormatHResult(hr) + ")");
    }
}

void SafeShutdown(IMFMediaSource* source)
{
    if (source) {
        source->Shutdown();
    }
}

class ActivationShutdownGuard {
public:
    explicit ActivationShutdownGuard(IMFActivate* activation)
        : activation_(activation)
    {
    }

    ~ActivationShutdownGuard()
    {
        if (activation_) {
            const HRESULT hr = activation_->ShutdownObject();
            if (FAILED(hr)) {
                qWarning() << "IMFActivate::ShutdownObject failed:"
                           << QString::fromStdString(FormatHResult(hr));
            }
        }
    }

    void Dismiss() { activation_ = nullptr; }

private:
    IMFActivate* activation_{};
};

static const GUID kPreferredSubtypes[] = {
    MFVideoFormat_NV12,
    MFVideoFormat_YUY2,
    MFVideoFormat_ARGB32,
    MFVideoFormat_RGB32,
};

std::size_t PackedRowBytes(const GUID& subtype, UINT width)
{
    if (IsEqualGUID(subtype, MFVideoFormat_ARGB32) ||
        IsEqualGUID(subtype, MFVideoFormat_RGB32)) {
        return static_cast<std::size_t>(width) * 4u;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) {
        return static_cast<std::size_t>(width) * 2u;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        return width;
    }
    return 0;
}

UINT PackedRowCount(const GUID& subtype, UINT height)
{
    return IsEqualGUID(subtype, MFVideoFormat_NV12)
               ? height + ((height + 1u) / 2u)
               : height;
}

double FrameSpatialRange(const MediaFrame& frame)
{
    if (frame.data.empty() || frame.width == 0 || frame.height == 0 ||
        frame.stride <= 0) {
        return 0.0;
    }
    const bool bgra =
        IsEqualGUID(frame.subtype, MFVideoFormat_ARGB32) ||
        IsEqualGUID(frame.subtype, MFVideoFormat_RGB32);
    const bool nv12 = IsEqualGUID(frame.subtype, MFVideoFormat_NV12);
    const bool yuy2 = IsEqualGUID(frame.subtype, MFVideoFormat_YUY2);
    if (!bgra && !nv12 && !yuy2) {
        return 0.0;
    }

    const UINT stepX = std::max(1u, frame.width / 32u);
    const UINT stepY = std::max(1u, frame.height / 18u);
    double minimum = 255.0;
    double maximum = 0.0;
    for (UINT y = 0; y < frame.height; y += stepY) {
        const auto* row =
            frame.data.data() + static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(frame.stride);
        for (UINT x = 0; x < frame.width; x += stepX) {
            double luma = 0.0;
            if (bgra) {
                const auto* pixel = row + static_cast<std::size_t>(x) * 4u;
                luma = 0.0722 * pixel[0] + 0.7152 * pixel[1] +
                       0.2126 * pixel[2];
            } else if (nv12) {
                luma = row[x];
            } else {
                luma = row[static_cast<std::size_t>(x) * 2u];
            }
            minimum = std::min(minimum, luma);
            maximum = std::max(maximum, luma);
        }
    }
    return maximum - minimum;
}

CameraFailureKind ClassifyCameraFailure(HRESULT hr)
{
    if (hr == E_ACCESSDENIED) {
        return CameraFailureKind::AccessDenied;
    }
    if (hr == MF_E_HW_MFT_FAILED_START_STREAMING ||
        hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED ||
        hr == HRESULT_FROM_WIN32(ERROR_BUSY) ||
        hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        return CameraFailureKind::DeviceBusy;
    }
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED ||
        hr == MF_E_SHUTDOWN ||
        hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ||
        hr == HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED) ||
        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return CameraFailureKind::DeviceMissing;
    }
    return CameraFailureKind::Other;
}

// Plain-language message shown directly to the user; technical detail kept in
// parentheses for support/logging.
std::string DescribeCameraFailure(CameraFailureKind kind, HRESULT hr, const char* stage)
{
    switch (kind) {
    case CameraFailureKind::DeviceBusy:
        return std::string("The camera is in use by another app or not streaming yet. "
                           "If this is a phone camera, open Phone Link and make sure the phone is awake. (") +
               stage + ", " + FormatHResult(hr) + ")";
    case CameraFailureKind::DeviceMissing:
        return std::string("The camera was disconnected or could not be found. "
                           "Check that it is plugged in and connected, then try again. (") +
               stage + ", " + FormatHResult(hr) + ")";
    case CameraFailureKind::AccessDenied:
        return std::string("Windows blocked access to the camera. Allow camera access for "
                           "desktop apps in Settings > Privacy & security > Camera. (") +
               stage + ", " + FormatHResult(hr) + ")";
    default:
        return std::string(stage) + " failed (" + FormatHResult(hr) + ")";
    }
}

} // namespace

MediaCaptureSession::MediaCaptureSession() = default;

MediaCaptureSession::~MediaCaptureSession() = default;

std::vector<CameraDescriptor> MediaCaptureSession::EnumerateCameras()
{
    std::vector<CameraDescriptor> cameras;

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1))) {
        return cameras;
    }

    HRESULT hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        return cameras;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    if (FAILED(hr)) {
        return cameras;
    }

    cameras.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        CameraDescriptor descriptor;

        WCHAR* friendlyName = nullptr;
        UINT32 friendlyLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                     &friendlyName,
                                                     &friendlyLen))) {
            descriptor.name.assign(friendlyName, friendlyName + friendlyLen);
            CoTaskMemFree(friendlyName);
        }

        WCHAR* symbolicLink = nullptr;
        UINT32 linkLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                     &symbolicLink,
                                                     &linkLen))) {
            descriptor.symbolicLink.assign(symbolicLink, symbolicLink + linkLen);
            CoTaskMemFree(symbolicLink);
        }

        descriptor.activation = devices[i];
        cameras.push_back(std::move(descriptor));
    }

    for (UINT32 i = 0; i < count; ++i) {
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    std::sort(
        cameras.begin(),
        cameras.end(),
        [](const CameraDescriptor& a, const CameraDescriptor& b) {
            const int nameOrder =
                _wcsicmp(a.name.c_str(), b.name.c_str());
            if (nameOrder != 0) {
                return nameOrder < 0;
            }
            return _wcsicmp(
                       a.symbolicLink.c_str(),
                       b.symbolicLink.c_str()) < 0;
        });

    return cameras;
}

std::vector<VideoFormat> MediaCaptureSession::EnumerateFormats(const CameraDescriptor& descriptor)
{
    lastError_.clear();
    std::vector<VideoFormat> formats;

    if (!descriptor.activation) {
        lastError_ = "Invalid camera activation";
        return formats;
    }

    try {
        Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource;
        ThrowIfFailed(descriptor.activation->ActivateObject(__uuidof(IMFMediaSource),
                                                            reinterpret_cast<void**>(mediaSource.GetAddressOf())),
                      "ActivateObject");
        ActivationShutdownGuard activationGuard(descriptor.activation.Get());

        Microsoft::WRL::ComPtr<IMFAttributes> readerAttributes;
        ThrowIfFailed(MFCreateAttributes(readerAttributes.GetAddressOf(), 2),
                      "Create reader attributes");
        ThrowIfFailed(readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
                      "Enable video processing");
        ThrowIfFailed(readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE),
                      "Allow converters");

        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaSource.Get(),
                                                          readerAttributes.Get(),
                                                          reader.GetAddressOf()),
                      "Create source reader");

        formats = ExtractFormats(reader.Get());
    } catch (const std::exception& e) {
        lastError_ = e.what();
    } catch (...) {
        lastError_ = "Unknown exception while enumerating formats";
    }

    return formats;
}

bool MediaCaptureSession::StartCapture(const CameraDescriptor& descriptor,
                                const VideoFormat* requestedFormat,
                                FrameCallback callback,
                                GUID preferredSubtype,
                                CaptureErrorCallback errorCallback,
                                CaptureAccelerationMode accelerationMode,
                                const std::wstring& requestedStableId)
{
    lastError_.clear();
    formatNotice_.clear();
    negotiatedFormat_ = {};
    nativeFormats_.clear();
    lastFailureKind_.store(CameraFailureKind::None);
    deviceLost_.store(false);
    accelerationValidated_.store(false);
    accelerationRejected_.store(false);
    accelerationMode_ = accelerationMode;
    startupValidationFrames_ = 0;
    startupMaximumSpatialRange_ = 0.0;
    startupPreviousTimestamp_ = -1;
    startupAdvancingTimestamps_ = 0;
    startupValidationComplete_ = false;

    if (!descriptor.activation) {
        lastError_ = "Invalid camera activation";
        lastFailureKind_.store(CameraFailureKind::DeviceMissing);
        return false;
    }

    try {
        Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource;
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;

        const char* failedStage = "ActivateObject";
        if (accelerationMode == CaptureAccelerationMode::Accelerated &&
            !CreateAccelerationDeviceManager()) {
            accelerationRejected_.store(true);
            return false;
        }

        HRESULT hr = TryOpenDevice(
            descriptor, mediaSource, reader, failedStage, accelerationMode);

        if (FAILED(hr)) {
            const CameraFailureKind kind = ClassifyCameraFailure(hr);
            lastFailureKind_.store(kind);
            lastError_ = DescribeCameraFailure(kind, hr, failedStage);
            return false;
        }

        ActivationShutdownGuard activationGuard(descriptor.activation.Get());

        // Resolve persisted selection on the reader we will stream from. A
        // separate EnumerateFormats probe would activate, shut down, and reopen
        // this same camera before it could deliver its first frame.
        nativeFormats_ = ExtractFormats(reader.Get());
        if (!requestedFormat && !requestedStableId.empty()) {
            const auto requested = std::find_if(
                nativeFormats_.begin(), nativeFormats_.end(),
                [&requestedStableId](const VideoFormat& candidate) {
                    return candidate.stableId == requestedStableId;
                });
            if (requested != nativeFormats_.end()) requestedFormat = &*requested;
        }

        FrameFormat format;
        if (!ConfigureReader(reader.Get(), preferredSubtype, requestedFormat, format)) {
            lastError_ = "ConfigureReader failed to select format";
            lastFailureKind_.store(CameraFailureKind::Other);
            return false;
        }

        if (qEnvironmentVariableIsSet("OPENZOOM_CAPTURE_DIAGNOSTICS")) {
            Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation;
            DWORD streamCount = 0;
            if (SUCCEEDED(mediaSource->CreatePresentationDescriptor(&presentation)) &&
                SUCCEEDED(presentation->GetStreamDescriptorCount(&streamCount))) {
                for (DWORD index = 0; index < streamCount; ++index) {
                    BOOL selected = FALSE;
                    Microsoft::WRL::ComPtr<IMFStreamDescriptor> stream;
                    Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
                    Microsoft::WRL::ComPtr<IMFMediaType> nativeType;
                    GUID major = GUID_NULL;
                    if (FAILED(presentation->GetStreamDescriptorByIndex(index, &selected, &stream)) ||
                        FAILED(stream->GetMediaTypeHandler(&handler)) ||
                        FAILED(handler->GetMajorType(&major)) || major != MFMediaType_Video ||
                        FAILED(handler->GetCurrentMediaType(&nativeType))) continue;
                    UINT32 nativeWidth = 0, nativeHeight = 0, numerator = 0, denominator = 0;
                    MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);
                    MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
                    qInfo() << "Capture native source format:" << nativeWidth << "x" << nativeHeight
                            << "rate" << numerator << "/" << denominator
                            << "| reader output:" << format.width << "x" << format.height
                            << "rate" << format.frameRateNumerator << "/" << format.frameRateDenominator;
                }
            }
        }
        mediaSource_ = std::move(mediaSource);
        sourceReader_ = std::move(reader);
        activeActivation_ = descriptor.activation;
        currentFormat_ = format;
        negotiatedFormat_.subtype = format.subtype;
        negotiatedFormat_.width = format.width;
        negotiatedFormat_.height = format.height;
        negotiatedFormat_.numerator = format.frameRateNumerator;
        negotiatedFormat_.denominator = format.frameRateDenominator;
        negotiatedFormat_.stableId =
            MakeFormatStableId(format.subtype,
                               format.width,
                               format.height,
                               format.frameRateNumerator,
                               format.frameRateDenominator);
        if (requestedFormat &&
            (requestedFormat->width != negotiatedFormat_.width ||
             requestedFormat->height != negotiatedFormat_.height ||
             requestedFormat->numerator != negotiatedFormat_.numerator ||
             requestedFormat->denominator != negotiatedFormat_.denominator)) {
            formatNotice_ = "Requested " + FormatDescription(*requestedFormat) +
                            "; driver selected " +
                            FormatDescription(negotiatedFormat_) + ".";
        }
        frameRateNumerator_.store(format.frameRateNumerator);
        frameRateDenominator_.store(format.frameRateDenominator);
        lastSymbolicLink_ = descriptor.symbolicLink;
        activationGuard.Dismiss();

        running_ = true;
        captureThread_ = std::thread(
            [session = shared_from_this(), callback = std::move(callback),
             errorCallback = std::move(errorCallback)]() mutable {
                session->CaptureLoop(std::move(callback), std::move(errorCallback));
            });
        return true;
    } catch (const std::exception& e) {
        lastError_ = e.what();
        lastFailureKind_.store(CameraFailureKind::Other);
        qWarning() << "MediaCaptureSession::StartCapture exception:" << e.what();
    } catch (...) {
        lastError_ = "Unknown exception while starting capture";
        lastFailureKind_.store(CameraFailureKind::Other);
        qWarning() << "MediaCaptureSession::StartCapture unknown exception";
    }

    return false;
}

HRESULT MediaCaptureSession::TryOpenDevice(const CameraDescriptor& descriptor,
                                    Microsoft::WRL::ComPtr<IMFMediaSource>& outSource,
                                    Microsoft::WRL::ComPtr<IMFSourceReader>& outReader,
                                    const char*& failedStage,
                                    CaptureAccelerationMode accelerationMode)
{
    outSource.Reset();
    outReader.Reset();

    Microsoft::WRL::ComPtr<IMFMediaSource> mediaSource;
    HRESULT hr = descriptor.activation->ActivateObject(__uuidof(IMFMediaSource),
                                                       reinterpret_cast<void**>(mediaSource.GetAddressOf()));
    if (FAILED(hr)) {
        failedStage = "ActivateObject";
        return hr;
    }
    ActivationShutdownGuard activationGuard(descriptor.activation.Get());

    Microsoft::WRL::ComPtr<IMFAttributes> readerAttributes;
    ThrowIfFailed(MFCreateAttributes(readerAttributes.GetAddressOf(), 8),
                  "Create reader attributes");
    ThrowIfFailed(readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE),
                  "Allow converters");
    if (accelerationMode == CaptureAccelerationMode::Accelerated) {
        ThrowIfFailed(
            readerAttributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE),
            "Enable advanced video processing");
        ThrowIfFailed(
            readerAttributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
            "Enable hardware transforms");
        ThrowIfFailed(
            readerAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE),
            "Enable DXVA");
        ThrowIfFailed(
            readerAttributes->SetUnknown(
                MF_SOURCE_READER_D3D_MANAGER, dxgiDeviceManager_.Get()),
            "Attach DXGI device manager");
    } else {
        ThrowIfFailed(
            readerAttributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
            "Enable video processing");
        ThrowIfFailed(
            readerAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE),
            "Disable DXVA");
    }

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(mediaSource.Get(),
                                             readerAttributes.Get(),
                                             reader.GetAddressOf());
    if (FAILED(hr)) {
        failedStage = "Create source reader";
        // The guard shuts the activation down so a retry can reopen the device.
        return hr;
    }

    activationGuard.Dismiss();
    outSource = std::move(mediaSource);
    outReader = std::move(reader);
    return S_OK;
}

bool MediaCaptureSession::CreateAccelerationDeviceManager()
{
    ReleaseAccelerationResources();
    d3d11Device_.Reset();
    d3d11Context_.Reset();
    dxgiDeviceManager_.Reset();
    dxgiReadbackTexture_.Reset();
    dxgiResetToken_ = 0;

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected{};
    const UINT flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter =
        SelectHighPerformanceAdapter();
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        d3d11Device_.GetAddressOf(),
        &selected,
        d3d11Context_.GetAddressOf());
    if (FAILED(hr)) {
        lastError_ =
            "Hardware camera acceleration could not create a D3D11 device (" +
            FormatHResult(hr) + ")";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(d3d11Device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    hr = MFCreateDXGIDeviceManager(
        &dxgiResetToken_, dxgiDeviceManager_.GetAddressOf());
    if (FAILED(hr)) {
        lastError_ =
            "Hardware camera acceleration could not create its DXGI manager (" +
            FormatHResult(hr) + ")";
        return false;
    }
    hr = dxgiDeviceManager_->ResetDevice(
        d3d11Device_.Get(), dxgiResetToken_);
    if (FAILED(hr)) {
        lastError_ =
            "Hardware camera acceleration could not attach the GPU (" +
            FormatHResult(hr) + ")";
        return false;
    }
    return true;
}

bool MediaCaptureSession::ConsumeDeviceLost()
{
    return deviceLost_.exchange(false);
}

bool MediaCaptureSession::ConsumeAccelerationValidated()
{
    return accelerationValidated_.exchange(false);
}

bool MediaCaptureSession::ConsumeAccelerationRejected()
{
    return accelerationRejected_.exchange(false);
}

std::int64_t QueryClock100ns()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0) {
        return -1;
    }
    return static_cast<std::int64_t>(
        static_cast<long double>(counter.QuadPart) *
        10'000'000.0L /
        static_cast<long double>(frequency.QuadPart));
}

std::uint64_t ThreadCpuTime100ns()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(
            GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    ULARGE_INTEGER kernelTime{};
    kernelTime.LowPart = kernel.dwLowDateTime;
    kernelTime.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTime{};
    userTime.LowPart = user.dwLowDateTime;
    userTime.HighPart = user.dwHighDateTime;
    return kernelTime.QuadPart + userTime.QuadPart;
}

double MediaCaptureSession::CurrentFrameRate() const
{
    const UINT denominator = frameRateDenominator_.load();
    return denominator == 0
               ? 0.0
               : static_cast<double>(frameRateNumerator_.load()) /
                     static_cast<double>(denominator);
}

void MediaCaptureSession::Quiesce()
{
    running_ = false;

    if (sourceReader_) {
        sourceReader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
    }

    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    PrepareAccelerationInteropRelease();
}

void MediaCaptureSession::ReleaseResources()
{
    // CUDA releases the consumer lease only after its copy-completion event
    // succeeds. A nonblocking import reset may leave that copy outstanding;
    // never clear producer backing merely because its own D3D work quiesced.
    while (!activeCudaLease_.expired()) {
        if (shutdown_->IsAbandoned()) return;
        Sleep(1);
    }
    if (shutdown_->IsAbandoned()) return;
    sourceReader_.Reset();
    if (activeActivation_) {
        const HRESULT hr = activeActivation_->ShutdownObject();
        if (FAILED(hr)) {
            qWarning() << "IMFActivate::ShutdownObject failed:"
                       << QString::fromStdString(FormatHResult(hr));
            SafeShutdown(mediaSource_.Get());
        }
    } else {
        SafeShutdown(mediaSource_.Get());
    }
    mediaSource_.Reset();
    activeActivation_.Reset();
    ReleaseAccelerationResources();
    dxgiDeviceManager_.Reset();
    d3d11Context_.Reset();
    d3d11Device_.Reset();
    dxgiResetToken_ = 0;
    currentFormat_ = FrameFormat{};
    frameRateNumerator_.store(0);
    frameRateDenominator_.store(0);
}

void MediaCaptureSession::PrepareAccelerationInteropRelease()
{
    std::scoped_lock lock(d3d11Mutex_);
    if (!d3d11Device_ || !d3d11Context_) {
        return;
    }

    // This executes only on the independently owned stop coordinator. A stuck
    // driver may hold it indefinitely; the facade's deadline retains the entire
    // session rather than treating a timeout as successful GPU completion.
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    D3D11_QUERY_DESC queryDescription{};
    queryDescription.Query = D3D11_QUERY_EVENT;
    ThrowIfFailed(d3d11Device_->CreateQuery(
        &queryDescription, completion.GetAddressOf()), "Create capture shutdown query");
    d3d11Context_->End(completion.Get());
    d3d11Context_->Flush();
    HRESULT completionResult;
    do {
        completionResult = d3d11Context_->GetData(completion.Get(), nullptr, 0, 0);
        if (completionResult == S_FALSE) Sleep(1);
    } while (completionResult == S_FALSE);
    ThrowIfFailed(completionResult, "Wait for capture producer completion");

    // The capture thread is joined and producer work has completed. Keep the
    // shared BGRA allocation itself until the caller releases its CUDA import.
    d3d11CudaQueryPending_ = false;
    d3d11CudaPendingSequence_ = 0;
    d3d11CudaReadyQuery_.Reset();
    d3d11CudaOutputView_.Reset();
    d3d11VideoProcessorOutputTexture_.Reset();
    d3d11VideoProcessor_.Reset();
    d3d11VideoProcessorEnumerator_.Reset();
    d3d11VideoContext_.Reset();
    d3d11VideoDevice_.Reset();
    d3d11Context_->ClearState();
}

bool MediaCaptureSession::ConfigureReader(IMFSourceReader* reader,
                                   GUID preferredSubtype,
                                   const VideoFormat* requestedFormat,
                                   FrameFormat& outFormat)
{
    ThrowIfFailed(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
                  "Disable default streams");
    ThrowIfFailed(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE),
                  "Enable video stream");

    Microsoft::WRL::ComPtr<IMFMediaType> desiredType;
    ThrowIfFailed(MFCreateMediaType(desiredType.GetAddressOf()), "Create media type");
    ThrowIfFailed(desiredType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                  "Set major type");
    ThrowIfFailed(desiredType->SetGUID(MF_MT_SUBTYPE, preferredSubtype),
                  "Set subtype");
    if (requestedFormat) {
        ThrowIfFailed(MFSetAttributeSize(desiredType.Get(),
                                         MF_MT_FRAME_SIZE,
                                         requestedFormat->width,
                                         requestedFormat->height),
                      "Set requested frame size");
        if (requestedFormat->numerator != 0 &&
            requestedFormat->denominator != 0) {
            ThrowIfFailed(MFSetAttributeRatio(desiredType.Get(),
                                              MF_MT_FRAME_RATE,
                                              requestedFormat->numerator,
                                              requestedFormat->denominator),
                          "Set requested frame rate");
        }
    }

    HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             nullptr,
                                             desiredType.Get());
    bool typeSelected = SUCCEEDED(hr);

    if (!typeSelected) {
        for (const GUID& fallback : kPreferredSubtypes) {
            if (IsEqualGUID(fallback, preferredSubtype)) {
                continue;
            }
            Microsoft::WRL::ComPtr<IMFMediaType> fallbackType;
            ThrowIfFailed(MFCreateMediaType(fallbackType.GetAddressOf()), "Create fallback media type");
            ThrowIfFailed(fallbackType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                          "Set fallback major type");
            ThrowIfFailed(fallbackType->SetGUID(MF_MT_SUBTYPE, fallback),
                          "Set fallback subtype");
            if (requestedFormat) {
                ThrowIfFailed(MFSetAttributeSize(fallbackType.Get(),
                                                 MF_MT_FRAME_SIZE,
                                                 requestedFormat->width,
                                                 requestedFormat->height),
                              "Set fallback requested frame size");
                if (requestedFormat->numerator != 0 &&
                    requestedFormat->denominator != 0) {
                    ThrowIfFailed(MFSetAttributeRatio(fallbackType.Get(),
                                                      MF_MT_FRAME_RATE,
                                                      requestedFormat->numerator,
                                                      requestedFormat->denominator),
                                  "Set fallback requested frame rate");
                }
            }

            if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                      nullptr,
                                                      fallbackType.Get()))) {
                typeSelected = true;
                break;
            }
        }
    }

    return typeSelected && ReadCurrentFormat(reader, outFormat);
}

bool MediaCaptureSession::ReadCurrentFormat(IMFSourceReader* reader, FrameFormat& outFormat)
{
    Microsoft::WRL::ComPtr<IMFMediaType> currentType;
    ThrowIfFailed(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              currentType.GetAddressOf()),
                  "Get current media type");

    GUID subtype = GUID_NULL;
    ThrowIfFailed(currentType->GetGUID(MF_MT_SUBTYPE, &subtype), "Get subtype");

    UINT32 width = 0;
    UINT32 height = 0;
    ThrowIfFailed(MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height),
                  "Get frame size");

    LONG rawStride = 0;
    UINT32 storedStride = 0;
    if (SUCCEEDED(currentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &storedStride))) {
        // MF stores the signed stride as UINT32, including bottom-up RGB.
        std::memcpy(&rawStride, &storedStride, sizeof(rawStride));
    } else if (FAILED(MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &rawStride))) {
        if (IsEqualGUID(subtype, MFVideoFormat_ARGB32) || IsEqualGUID(subtype, MFVideoFormat_RGB32)) {
            rawStride = static_cast<LONG>(width * 4);
        } else if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
            rawStride = static_cast<LONG>(width);
        } else if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) {
            rawStride = static_cast<LONG>(width * 2);
        } else {
            return false;
        }
    }

    outFormat.subtype = subtype;
    outFormat.yuvColor = {};
    if ((IsEqualGUID(subtype, MFVideoFormat_NV12) ||
         IsEqualGUID(subtype, MFVideoFormat_YUY2)) &&
        !ReadCaptureYuvColor(currentType.Get(), outFormat.yuvColor)) {
        qWarning() << "Camera supplied an unsupported YUV matrix or nominal range";
        return false;
    }
    outFormat.width = width;
    outFormat.height = height;
    outFormat.stride = rawStride;
    UINT32 frameRateNumerator = 0;
    UINT32 frameRateDenominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(currentType.Get(),
                                      MF_MT_FRAME_RATE,
                                      &frameRateNumerator,
                                      &frameRateDenominator))) {
        outFormat.frameRateNumerator = frameRateNumerator;
        outFormat.frameRateDenominator = frameRateDenominator;
    }
    return true;
}

std::vector<VideoFormat> MediaCaptureSession::ExtractFormats(IMFSourceReader* reader)
{
    std::vector<VideoFormat> formats;
    if (!reader) {
        return formats;
    }

    std::set<std::tuple<std::wstring, UINT, UINT, UINT, UINT>> uniqueKeys;
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
        HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, mediaType.GetAddressOf());
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || !mediaType) {
            lastError_ = HrToString(hr);
            break;
        }

        UINT32 width = 0, height = 0;
        if (FAILED(MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
            continue;
        }

        UINT32 num = 0, den = 0;
        if (FAILED(MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &num, &den))) {
            num = 0;
            den = 0;
        }

        GUID subtype = GUID_NULL;
        if (FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }

        const std::wstring subtypeText = GuidStableText(subtype);
        auto key = std::make_tuple(subtypeText, width, height, num, den);
        if (uniqueKeys.insert(key).second) {
            VideoFormat fmt;
            fmt.subtype = subtype;
            fmt.width = width;
            fmt.height = height;
            fmt.numerator = num;
            fmt.denominator = den;
            fmt.stableId = MakeFormatStableId(subtype, width, height, num, den);
            formats.push_back(fmt);
        }
    }
    return formats;
}

std::string MediaCaptureSession::HrToString(HRESULT hr)
{
    return FormatHResult(hr);
}

void MediaCaptureSession::ReleaseAccelerationResources()
{
    std::scoped_lock lock(d3d11Mutex_);
    d3d11CudaQueryPending_ = false;
    d3d11CudaPendingSequence_ = 0;
    d3d11CudaReadyQuery_.Reset();
    d3d11CudaOutputView_.Reset();
    d3d11VideoProcessorOutputTexture_.Reset();
    d3d11CudaTexture_.Reset();
    d3d11VideoProcessor_.Reset();
    d3d11VideoProcessorEnumerator_.Reset();
    d3d11VideoContext_.Reset();
    d3d11VideoDevice_.Reset();
    dxgiReadbackTexture_.Reset();
    videoProcessorInputFormat_ = DXGI_FORMAT_UNKNOWN;
    videoProcessorWidth_ = 0;
    videoProcessorHeight_ = 0;
    d3d11CudaNeedsCopy_ = false;
}

bool MediaCaptureSession::AttachDxgiFrame(IMFMediaBuffer* buffer, MediaFrame& frame)
{
    if (!buffer) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer->QueryInterface(
            IID_PPV_ARGS(dxgiBuffer.GetAddressOf())))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dxgiBuffer->GetResource(
            IID_PPV_ARGS(texture.GetAddressOf()))) ||
        !texture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    UINT subresource = 0;
    if (FAILED(dxgiBuffer->GetSubresourceIndex(&subresource))) {
        subresource = 0;
    }
    frame.gpuTexture = std::move(texture);
    frame.gpuSubresource = subresource;
    frame.gpuFormat = description.Format;
    return true;
}

bool MediaCaptureSession::CopyGpuFrame(const FrameFormat& format, MediaFrame& frame)
{
    if (!frame.gpuTexture || !d3d11Device_ || !d3d11Context_) {
        return false;
    }

    std::scoped_lock lock(d3d11Mutex_);
    D3D11_TEXTURE2D_DESC sourceDescription{};
    frame.gpuTexture->GetDesc(&sourceDescription);
    GUID readbackSubtype = format.subtype;
    if (sourceDescription.Format == DXGI_FORMAT_NV12) {
        readbackSubtype = MFVideoFormat_NV12;
    } else if (sourceDescription.Format == DXGI_FORMAT_YUY2) {
        readbackSubtype = MFVideoFormat_YUY2;
    } else if (sourceDescription.Format ==
               DXGI_FORMAT_B8G8R8A8_UNORM) {
        readbackSubtype = MFVideoFormat_ARGB32;
    } else if (sourceDescription.Format ==
               DXGI_FORMAT_R8G8B8A8_UNORM) {
        readbackSubtype = MFVideoFormat_RGB32;
    }
    bool recreate = !dxgiReadbackTexture_;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC existing{};
        dxgiReadbackTexture_->GetDesc(&existing);
        recreate =
            existing.Width != sourceDescription.Width ||
            existing.Height != sourceDescription.Height ||
            existing.Format != sourceDescription.Format;
    }
    if (recreate) {
        D3D11_TEXTURE2D_DESC staging = sourceDescription;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        dxgiReadbackTexture_.Reset();
        const HRESULT createResult = d3d11Device_->CreateTexture2D(
            &staging, nullptr, dxgiReadbackTexture_.GetAddressOf());
        if (FAILED(createResult)) {
            return false;
        }
    }

    d3d11Context_->CopySubresourceRegion(
        dxgiReadbackTexture_.Get(), 0, 0, 0, 0,
        frame.gpuTexture.Get(), frame.gpuSubresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = d3d11Context_->Map(
        dxgiReadbackTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult) || !mapped.pData) {
        return false;
    }

    const std::size_t rowBytes =
        PackedRowBytes(readbackSubtype, format.width);
    const UINT rowCount =
        PackedRowCount(readbackSubtype, format.height);
    if (rowBytes == 0 || mapped.RowPitch < rowBytes) {
        d3d11Context_->Unmap(dxgiReadbackTexture_.Get(), 0);
        return false;
    }
    frame.data.resize(rowBytes * rowCount);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT row = 0; row < rowCount; ++row) {
        std::memcpy(
            frame.data.data() + static_cast<std::size_t>(row) * rowBytes,
            source + static_cast<std::size_t>(row) * mapped.RowPitch,
            rowBytes);
    }
    d3d11Context_->Unmap(dxgiReadbackTexture_.Get(), 0);
    frame.subtype = readbackSubtype;
    frame.stride = static_cast<LONG>(rowBytes);
    frame.dataSize = frame.data.size();
    return true;
}

bool MediaCaptureSession::ReadbackGpuFrame(MediaFrame& frame)
{
    if (!frame.IsGpuResident()) {
        return !frame.data.empty();
    }
    FrameFormat format{};
    format.subtype = frame.subtype;
    format.yuvColor = frame.yuvColor;
    format.width = frame.width;
    format.height = frame.height;
    format.stride = frame.stride;
    format.frameRateNumerator = frame.frameRateNumerator;
    format.frameRateDenominator = frame.frameRateDenominator;
    return CopyGpuFrame(format, frame);
}

bool MediaCaptureSession::EnsureVideoProcessor(const MediaFrame& frame)
{
    if (!frame.gpuTexture || !d3d11Device_ || !d3d11Context_ ||
        frame.width == 0 || frame.height == 0) {
        return false;
    }
    const bool directBgraCopy = frame.gpuFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
    if ((directBgraCopy || (d3d11VideoProcessor_ &&
                           d3d11VideoProcessorOutputTexture_ && d3d11CudaOutputView_)) &&
        d3d11CudaTexture_ &&
        d3d11CudaReadyQuery_ &&
        videoProcessorInputFormat_ == frame.gpuFormat &&
        videoProcessorWidth_ == frame.width &&
        videoProcessorHeight_ == frame.height) {
        return true;
    }

    d3d11CudaQueryPending_ = false;
    d3d11CudaPendingSequence_ = 0;
    d3d11CudaReadyQuery_.Reset();
    d3d11CudaOutputView_.Reset();
    d3d11VideoProcessorOutputTexture_.Reset();
    d3d11CudaTexture_.Reset();
    d3d11VideoProcessor_.Reset();
    d3d11VideoProcessorEnumerator_.Reset();
    d3d11VideoContext_.Reset();
    d3d11VideoDevice_.Reset();

    if (directBgraCopy) {
        // MF has already converted this sample to the exact CUDA handoff
        // format. A bit-preserving GPU copy needs neither video processing nor
        // another color conversion (and avoids its driver scheduling path).
        D3D11_TEXTURE2D_DESC output{};
        output.Width = frame.width;
        output.Height = frame.height;
        output.MipLevels = 1;
        output.ArraySize = 1;
        output.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        output.SampleDesc.Count = 1;
        output.Usage = D3D11_USAGE_DEFAULT;
        output.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        output.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(d3d11Device_->CreateTexture2D(
                &output, nullptr, d3d11CudaTexture_.GetAddressOf()))) return false;
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        if (FAILED(d3d11Device_->CreateQuery(
                &query, d3d11CudaReadyQuery_.GetAddressOf()))) return false;
        d3d11CudaNeedsCopy_ = false;
        videoProcessorInputFormat_ = frame.gpuFormat;
        videoProcessorWidth_ = frame.width;
        videoProcessorHeight_ = frame.height;
        return true;
    }

    if (FAILED(d3d11Device_.As(&d3d11VideoDevice_)) ||
        FAILED(d3d11Context_.As(&d3d11VideoContext_))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator =
        frame.frameRateNumerator == 0 ? 30 : frame.frameRateNumerator;
    content.InputFrameRate.Denominator =
        frame.frameRateDenominator == 0 ? 1 : frame.frameRateDenominator;
    content.InputWidth = frame.width;
    content.InputHeight = frame.height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = frame.width;
    content.OutputHeight = frame.height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT result = d3d11VideoDevice_->CreateVideoProcessorEnumerator(
        &content, d3d11VideoProcessorEnumerator_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }

    UINT inputSupport = 0;
    UINT outputSupport = 0;
    if (FAILED(d3d11VideoProcessorEnumerator_->CheckVideoProcessorFormat(
            frame.gpuFormat, &inputSupport)) ||
        (inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        FAILED(d3d11VideoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport)) ||
        (outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        return false;
    }

    result = d3d11VideoDevice_->CreateVideoProcessor(
        d3d11VideoProcessorEnumerator_.Get(), 0,
        d3d11VideoProcessor_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = frame.width;
    outputDescription.Height = frame.height;
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    outputDescription.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    result = d3d11Device_->CreateTexture2D(
        &outputDescription, nullptr, d3d11CudaTexture_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDescription{};
    outputViewDescription.ViewDimension =
        D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDescription.Texture2D.MipSlice = 0;
    result = d3d11VideoDevice_->CreateVideoProcessorOutputView(
        d3d11CudaTexture_.Get(),
        d3d11VideoProcessorEnumerator_.Get(),
        &outputViewDescription,
        d3d11CudaOutputView_.GetAddressOf());
    if (FAILED(result)) {
        // A few drivers reject a shareable allocation as a VideoProcessor
        // target. Convert into a private texture, then issue one GPU copy into
        // the shared allocation. This remains a zero-CPU-copy path.
        d3d11CudaOutputView_.Reset();
        d3d11CudaTexture_.Reset();
        outputDescription.MiscFlags = 0;
        result = d3d11Device_->CreateTexture2D(
            &outputDescription,
            nullptr,
            d3d11VideoProcessorOutputTexture_.GetAddressOf());
        if (FAILED(result)) {
            return false;
        }
        result = d3d11VideoDevice_->CreateVideoProcessorOutputView(
            d3d11VideoProcessorOutputTexture_.Get(),
            d3d11VideoProcessorEnumerator_.Get(),
            &outputViewDescription,
            d3d11CudaOutputView_.GetAddressOf());
        if (FAILED(result)) {
            return false;
        }
        outputDescription.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED |
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        result = d3d11Device_->CreateTexture2D(
            &outputDescription, nullptr, d3d11CudaTexture_.GetAddressOf());
        if (FAILED(result)) {
            return false;
        }
        d3d11CudaNeedsCopy_ = true;
    } else {
        d3d11VideoProcessorOutputTexture_ = d3d11CudaTexture_;
        d3d11CudaNeedsCopy_ = false;
    }

    D3D11_QUERY_DESC queryDescription{};
    queryDescription.Query = D3D11_QUERY_EVENT;
    result = d3d11Device_->CreateQuery(
        &queryDescription, d3d11CudaReadyQuery_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }

    videoProcessorInputFormat_ = frame.gpuFormat;
    videoProcessorWidth_ = frame.width;
    videoProcessorHeight_ = frame.height;
    return true;
}

GpuFramePreparationResult MediaCaptureSession::PrepareGpuFrameForCuda(
    const MediaFrame& frame,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture,
    std::shared_ptr<void>& outLease)
{
    outTexture.Reset();
    outLease.reset();
    if (!frame.IsGpuResident()) {
        return GpuFramePreparationResult::Unsupported;
    }

    std::scoped_lock lock(d3d11Mutex_);
    if (!activeCudaLease_.expired()) {
        return GpuFramePreparationResult::Retry;
    }
    const auto leaseReadyTexture = [&]() {
        // A distinct control block makes weak expiration track the consumer,
        // while the contained session reference retains all producer backing.
        outLease = std::make_shared<std::shared_ptr<MediaCaptureSession>>(shared_from_this());
        activeCudaLease_ = outLease;
        outTexture = d3d11CudaTexture_;
    };
    if (d3d11CudaQueryPending_) {
        const std::uint64_t pendingSequence =
            d3d11CudaPendingSequence_;
        BOOL complete = FALSE;
        const HRESULT pendingResult = d3d11Context_->GetData(
            d3d11CudaReadyQuery_.Get(),
            &complete,
            sizeof(complete),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (pendingResult == S_FALSE) {
            return GpuFramePreparationResult::Retry;
        }
        if (FAILED(pendingResult) || !complete) {
            d3d11CudaQueryPending_ = false;
            qWarning() << "D3D11 camera conversion completion query failed";
            return GpuFramePreparationResult::Unsupported;
        }
        d3d11CudaQueryPending_ = false;
        d3d11CudaPendingSequence_ = 0;
        if (frame.sequenceNumber == pendingSequence) {
            leaseReadyTexture();
            return GpuFramePreparationResult::Ready;
        }
    }
    if (!EnsureVideoProcessor(frame)) {
        return GpuFramePreparationResult::Unsupported;
    }

    if (frame.gpuFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (!CopyBgraCaptureTexture(d3d11Context_.Get(), frame.gpuTexture.Get(),
                frame.gpuSubresource, d3d11CudaTexture_.Get(), frame.width, frame.height)) {
            return GpuFramePreparationResult::Unsupported;
        }
    } else {
    const bool yuvInput = frame.gpuFormat == DXGI_FORMAT_NV12 ||
                          frame.gpuFormat == DXGI_FORMAT_YUY2;
    if (yuvInput && frame.yuvColor.range == YuvRange::Full) {
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        if (FAILED(d3d11VideoProcessorEnumerator_->GetVideoProcessorCaps(&caps)) ||
            (caps.DeviceCaps & D3D11_VIDEO_PROCESSOR_DEVICE_CAPS_NOMINAL_RANGE) == 0) {
            // A driver without nominal-range support would silently expand
            // full-range YUV as limited. Let the raw CPU/CUDA rung convert it.
            return GpuFramePreparationResult::Unsupported;
        }
    }
    const auto inputColor = CaptureVideoColorSpace(yuvInput ? frame.yuvColor
        : YuvColorInfo{YuvMatrix::Bt709, YuvRange::Full});
    const auto outputColor = CaptureVideoColorSpace(
        {YuvMatrix::Bt709, YuvRange::Full});
    // Apply every frame: a media-type change need not recreate the processor
    // when only color metadata changes. RGB output always uses full range.
    d3d11VideoContext_->VideoProcessorSetStreamColorSpace(
        d3d11VideoProcessor_.Get(), 0, &inputColor);
    d3d11VideoContext_->VideoProcessorSetOutputColorSpace(
        d3d11VideoProcessor_.Get(), &outputColor);

    D3D11_TEXTURE2D_DESC inputDescription{};
    frame.gpuTexture->GetDesc(&inputDescription);
    const UINT mipLevels = std::max(1u, inputDescription.MipLevels);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDescription{};
    inputViewDescription.FourCC = 0;
    inputViewDescription.ViewDimension =
        D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDescription.Texture2D.MipSlice =
        frame.gpuSubresource % mipLevels;
    inputViewDescription.Texture2D.ArraySlice =
        frame.gpuSubresource / mipLevels;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT result = d3d11VideoDevice_->CreateVideoProcessorInputView(
        frame.gpuTexture.Get(),
        d3d11VideoProcessorEnumerator_.Get(),
        &inputViewDescription,
        inputView.GetAddressOf());
    if (FAILED(result)) {
        return GpuFramePreparationResult::Unsupported;
    }

    const RECT fullFrame{
        0, 0,
        static_cast<LONG>(frame.width),
        static_cast<LONG>(frame.height)};
    d3d11VideoContext_->VideoProcessorSetOutputTargetRect(
        d3d11VideoProcessor_.Get(), TRUE, &fullFrame);
    d3d11VideoContext_->VideoProcessorSetStreamSourceRect(
        d3d11VideoProcessor_.Get(), 0, TRUE, &fullFrame);
    d3d11VideoContext_->VideoProcessorSetStreamDestRect(
        d3d11VideoProcessor_.Get(), 0, TRUE, &fullFrame);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = inputView.Get();
    result = d3d11VideoContext_->VideoProcessorBlt(
        d3d11VideoProcessor_.Get(),
        d3d11CudaOutputView_.Get(),
        0, 1, &stream);
    if (FAILED(result)) {
        return GpuFramePreparationResult::Unsupported;
    }
    if (d3d11CudaNeedsCopy_) {
        d3d11Context_->CopyResource(
            d3d11CudaTexture_.Get(),
            d3d11VideoProcessorOutputTexture_.Get());
    }
    }

    // D3D11 and CUDA use separate API timelines for the same allocation.
    // Complete the direct copy or VideoProcessor write before CUDA
    // imports/reads it. UploadD3D11Frame performs the reciprocal CUDA drain
    // before this reusable texture can be written again.
    d3d11Context_->End(d3d11CudaReadyQuery_.Get());
    d3d11Context_->Flush();
    d3d11CudaQueryPending_ = true;
    d3d11CudaPendingSequence_ = frame.sequenceNumber;
    // Never sleep on the viewport thread waiting for this separate GPU
    // timeline. Pending work retains this exact MediaFrame for the app's 1 ms
    // event-loop retries and existing 25 ms safe-copy fallback deadline.
    BOOL complete = FALSE;
    const HRESULT completionResult = d3d11Context_->GetData(
                d3d11CudaReadyQuery_.Get(),
                &complete,
                sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (completionResult == S_FALSE) {
        return GpuFramePreparationResult::Retry;
    }
    d3d11CudaQueryPending_ = false;
    d3d11CudaPendingSequence_ = 0;
    if (FAILED(completionResult) || !complete) {
        qWarning() << "D3D11 camera conversion completion query failed";
        return GpuFramePreparationResult::Unsupported;
    }

    leaseReadyTexture();
    return GpuFramePreparationResult::Ready;
}

bool MediaCaptureSession::ValidateStartupFrame(const MediaFrame& frame)
{
    if (accelerationMode_ != CaptureAccelerationMode::Accelerated ||
        startupValidationComplete_ || accelerationRejected_.load()) {
        return true;
    }

    ++startupValidationFrames_;
    startupMaximumSpatialRange_ =
        std::max(startupMaximumSpatialRange_, FrameSpatialRange(frame));
    if (startupPreviousTimestamp_ >= 0 &&
        frame.captureTimestamp100ns > startupPreviousTimestamp_) {
        ++startupAdvancingTimestamps_;
    }
    startupPreviousTimestamp_ = frame.captureTimestamp100ns;

    constexpr UINT kValidationFrames = 30;
    if (startupValidationFrames_ < kValidationFrames) {
        return true;
    }
    const bool healthy =
        startupAdvancingTimestamps_ >= kValidationFrames / 2 &&
        startupMaximumSpatialRange_ >= 2.0;
    qInfo() << "Camera acceleration startup validation:"
            << (healthy ? "passed" : "rejected")
            << "| advancing timestamps" << startupAdvancingTimestamps_
            << "of" << startupValidationFrames_
            << "| spatial range" << startupMaximumSpatialRange_;
    if (healthy) {
        startupValidationComplete_ = true;
        accelerationValidated_.store(true);
        return true;
    }

    accelerationRejected_.store(true);
    return false;
}

void MediaCaptureSession::CaptureLoop(FrameCallback callback, CaptureErrorCallback errorCallback)
{
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(coInit);
    auto reportFailure = [this, &errorCallback](const std::string& message) {
        qWarning() << "Camera capture stopped:" << QString::fromStdString(message);
        auto delivery = shutdown_->TryEnterDelivery();
        if (errorCallback && delivery) {
            try {
                errorCallback(message);
            } catch (const std::exception& e) {
                qWarning() << "Capture error callback failed:" << e.what();
            } catch (...) {
                qWarning() << "Capture error callback failed with an unknown exception";
            }
        }
    };

    if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE) {
        running_ = false;
        reportFailure(std::string("Capture thread COM initialization failed (") +
                      FormatHResult(coInit) + ")");
        return;
    }

    Microsoft::WRL::ComPtr<IMFSourceReader> reader = sourceReader_;
    if (!reader) {
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return;
    }

    FrameFormat format = currentFormat_;
    std::uint64_t sequenceNumber = 0;
    const bool diagnosticsEnabled =
        qEnvironmentVariableIsSet("OPENZOOM_CAPTURE_DIAGNOSTICS");
    std::int64_t diagnosticsStartClock = QueryClock100ns();
    std::uint64_t diagnosticsStartCpu = ThreadCpuTime100ns();
    std::uint64_t diagnosticsFrames = 0;
    std::uint64_t diagnosticsBytes = 0;
    std::int64_t diagnosticsWork100ns = 0;

    while (running_) {
        const std::int64_t frameWorkStart = QueryClock100ns();
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;

        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                        0,
                                        &streamIndex,
                                        &flags,
                                        &timestamp,
                                        &sample);

        if (!running_) {
            break;
        }

        if (FAILED(hr)) {
            running_ = false;
            // Classify and flag device loss so the app's frame tick can poll
            // ConsumeDeviceLost() and drive reconnection.
            const CameraFailureKind kind = ClassifyCameraFailure(hr);
            lastFailureKind_.store(kind);
            deviceLost_.store(true);
            reportFailure(DescribeCameraFailure(kind, hr, "ReadSample"));
            break;
        }

        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            running_ = false;
            lastFailureKind_.store(CameraFailureKind::DeviceMissing);
            deviceLost_.store(true);
            reportFailure("The camera reported a stream error and stopped delivering frames");
            break;
        }

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            running_ = false;
            lastFailureKind_.store(CameraFailureKind::DeviceMissing);
            deviceLost_.store(true);
            reportFailure("The camera stream ended unexpectedly");
            break;
        }

        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            // Mid-stream format change (S7): re-query the current media type and
            // refresh the cached format so we never copy frames with a stale stride.
            try {
                FrameFormat updatedFormat;
                if (!ReadCurrentFormat(reader.Get(), updatedFormat)) {
                    running_ = false;
                    reportFailure("The camera changed to an unsupported media format");
                    break;
                }
                format = updatedFormat;
                currentFormat_ = updatedFormat;
                frameRateNumerator_.store(updatedFormat.frameRateNumerator);
                frameRateDenominator_.store(updatedFormat.frameRateDenominator);
            } catch (const std::exception& e) {
                running_ = false;
                reportFailure(std::string("Failed to read the camera's changed media format: ") + e.what());
                break;
            }
        }

        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0 || !sample) {
            continue;
        }

        MediaFrame frame;
        frame.subtype = format.subtype;
        frame.yuvColor = format.yuvColor;
        frame.width = format.width;
        frame.height = format.height;
        frame.captureTimestamp100ns = timestamp;
        frame.captureClock100ns = QueryClock100ns();
        frame.sequenceNumber = sequenceNumber++;
        frame.frameRateNumerator = format.frameRateNumerator;
        frame.frameRateDenominator = format.frameRateDenominator;
        frame.stride = format.stride;
        DWORD length = 0;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        if (accelerationMode_ == CaptureAccelerationMode::Accelerated) {
            sample->GetBufferByIndex(0, buffer.GetAddressOf());
        }
        const bool gpuAttached =
            buffer && AttachDxgiFrame(buffer.Get(), frame);
        if (gpuAttached) {
            // Pin the sample for as long as the frame is retained anywhere
            // (six-frame recording burst queue, deferred-conversion retry).
            // Without this the reader recycles the pooled texture on sample
            // release and overwrites queued frames' pixels — silent content
            // corruption that no drop counter can see. Retention is bounded
            // by the queue depth; if the reader's pool runs dry, ReadSample
            // paces the capture thread instead of corrupting frames.
            frame.gpuSample = sample;
        }
        // The first frames retain a small CPU readback so the existing
        // black/frozen-frame startup validator still sees pixels. Once the
        // accelerated source is validated, steady-state preview frames leave
        // the capture thread as retained D3D11 textures only.
        bool frameReady = gpuAttached && startupValidationComplete_;
        if (gpuAttached && !startupValidationComplete_) {
            frameReady = CopyGpuFrame(format, frame);
        }
        if (!frameReady) {
            buffer.Reset();
            DWORD bufferCount = 0;
            hr = sample->GetBufferCount(&bufferCount);
            if (SUCCEEDED(hr)) {
                // Preserve the native 2D interface/pitch for single-buffer video.
                hr = bufferCount == 1
                    ? sample->GetBufferByIndex(0, buffer.GetAddressOf())
                    : sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
            }
            if (FAILED(hr) || !buffer) {
                continue;
            }

            if (!CopyCaptureBuffer(buffer.Get(), format.subtype,
                                   format.width, format.height, format.stride,
                                   frame.data, frame.stride)) {
                continue;
            }
            frame.dataSize = frame.data.size();
            frameReady = true;
        }
        if (!frameReady) {
            continue;
        }
        frame.dataSize = frame.data.size();
        length = static_cast<DWORD>(
            std::min<std::size_t>(
                frame.dataSize,
                std::numeric_limits<DWORD>::max()));

        if (!ValidateStartupFrame(frame)) {
            running_ = false;
            reportFailure(
                "Hardware camera acceleration did not produce a usable image; "
                "retrying in compatibility mode");
            break;
        }

        auto delivery = shutdown_->TryEnterDelivery();
        if (callback && delivery) {
            try {
                callback(std::move(frame));
            } catch (const std::exception& e) {
                running_ = false;
                reportFailure(std::string("Frame callback failed: ") + e.what());
                break;
            } catch (...) {
                running_ = false;
                reportFailure("Frame callback failed with an unknown exception");
                break;
            }
        }
        if (diagnosticsEnabled) {
            ++diagnosticsFrames;
            diagnosticsBytes += length;
            const std::int64_t now = QueryClock100ns();
            diagnosticsWork100ns +=
                std::max<std::int64_t>(0, now - frameWorkStart);
            const std::int64_t elapsed =
                now - diagnosticsStartClock;
            if (elapsed >= 5 * 10'000'000LL) {
                const std::uint64_t cpuNow = ThreadCpuTime100ns();
                const double seconds =
                    static_cast<double>(elapsed) / 10'000'000.0;
                const double fps =
                    diagnosticsFrames / std::max(0.001, seconds);
                const double averageBytes =
                    diagnosticsFrames == 0
                        ? 0.0
                        : static_cast<double>(diagnosticsBytes) /
                              diagnosticsFrames;
                const double averageWorkMs =
                    diagnosticsFrames == 0
                        ? 0.0
                        : static_cast<double>(diagnosticsWork100ns) /
                              diagnosticsFrames / 10'000.0;
                const double cpuPercent =
                    100.0 *
                    static_cast<double>(
                        cpuNow - diagnosticsStartCpu) /
                    std::max<double>(1.0, elapsed);
                qInfo().nospace()
                    << "Capture diagnostics: " << fps
                    << " FPS | " << averageBytes
                    << " CPU bytes/frame | capture work "
                    << averageWorkMs << " ms | thread CPU "
                    << cpuPercent << "%";
                diagnosticsStartClock = now;
                diagnosticsStartCpu = cpuNow;
                diagnosticsFrames = 0;
                diagnosticsBytes = 0;
                diagnosticsWork100ns = 0;
            }
        }
    }

    if (shouldUninitializeCom) {
        CoUninitialize();
    }
}

} // namespace openzoom

#endif // _WIN32
