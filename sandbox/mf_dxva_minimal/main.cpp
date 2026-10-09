#ifdef _WIN32

#include <windows.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "interop_stress.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kDefaultTimeoutMs = 10000;
constexpr int kRequiredFrames = 45;

struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ComScope()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct MfScope {
    HRESULT result{MFStartup(MF_VERSION, MFSTARTUP_FULL)};
    ~MfScope()
    {
        if (SUCCEEDED(result)) {
            MFShutdown();
        }
    }
};

struct ProbeResult {
    std::string status{"error"};
    std::string detail;
    std::string camera;
    bool accelerated{};
    bool dxgiSamples{};
    int frames{};
    int timestampsAdvanced{};
    double averageReadMs{};
    double averageLuma{};
    double spatialRange{};
};

std::string EscapeJson(const std::string& text);

std::string ToJson(const InteropStressResult& result,
                   const std::string& camera)
{
    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(3);
    json << "{"
         << "\"status\":\"" << EscapeJson(result.status) << "\","
         << "\"detail\":\"" << EscapeJson(result.detail) << "\","
         << "\"camera\":\"" << EscapeJson(camera) << "\","
         << "\"path\":\"" << EscapeJson(result.path) << "\","
         << "\"iterationsRequested\":" << result.iterationsRequested << ","
         << "\"iterationsCompleted\":" << result.iterationsCompleted << ","
         << "\"averageIterationMs\":" << result.averageIterationMs
         << "}";
    return json.str();
}

std::string EscapeJson(const std::string& text)
{
    std::ostringstream output;
    for (const unsigned char value : text) {
        switch (value) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (value < 0x20) {
                char encoded[7]{};
                std::snprintf(encoded, sizeof(encoded), "\\u%04x", value);
                output << encoded;
            } else {
                output << static_cast<char>(value);
            }
            break;
        }
    }
    return output.str();
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int required =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr);
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::string HrText(HRESULT result)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(result));
    return buffer;
}

std::string ToJson(const ProbeResult& result)
{
    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(3);
    json << "{"
         << "\"status\":\"" << EscapeJson(result.status) << "\","
         << "\"detail\":\"" << EscapeJson(result.detail) << "\","
         << "\"camera\":\"" << EscapeJson(result.camera) << "\","
         << "\"mode\":\""
         << (result.accelerated ? "accelerated" : "compatibility") << "\","
         << "\"dxgiSamples\":" << (result.dxgiSamples ? "true" : "false")
         << ",\"frames\":" << result.frames
         << ",\"timestampsAdvanced\":" << result.timestampsAdvanced
         << ",\"averageReadMs\":" << result.averageReadMs
         << ",\"averageLuma\":" << result.averageLuma
         << ",\"spatialRange\":" << result.spatialRange
         << "}";
    return json.str();
}

std::wstring CameraName(IMFActivate* activation);
std::wstring CameraSymbolicLink(IMFActivate* activation);

std::vector<ComPtr<IMFActivate>> EnumerateCameras()
{
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
        FAILED(attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return {};
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count))) {
        return {};
    }

    std::vector<ComPtr<IMFActivate>> result;
    result.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        result.emplace_back(devices[index]);
        devices[index]->Release();
    }
    CoTaskMemFree(devices);
    std::sort(
        result.begin(),
        result.end(),
        [](const ComPtr<IMFActivate>& a,
           const ComPtr<IMFActivate>& b) {
            const std::wstring aName = CameraName(a.Get());
            const std::wstring bName = CameraName(b.Get());
            const int nameOrder =
                _wcsicmp(aName.c_str(), bName.c_str());
            if (nameOrder != 0) {
                return nameOrder < 0;
            }
            const std::wstring aLink =
                CameraSymbolicLink(a.Get());
            const std::wstring bLink =
                CameraSymbolicLink(b.Get());
            return _wcsicmp(
                       aLink.c_str(),
                       bLink.c_str()) < 0;
        });
    return result;
}

std::wstring CameraName(IMFActivate* activation)
{
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (!activation ||
        FAILED(activation->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &value, &length))) {
        return L"Unknown camera";
    }
    std::wstring name(value, value + length);
    CoTaskMemFree(value);
    return name;
}

std::wstring CameraSymbolicLink(IMFActivate* activation)
{
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (!activation ||
        FAILED(activation->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &value, &length))) {
        return {};
    }
    std::wstring link(value, value + length);
    CoTaskMemFree(value);
    return link;
}

bool CreateD3DManager(ComPtr<ID3D11Device>& device,
                      ComPtr<ID3D11DeviceContext>& context,
                      ComPtr<IMFDXGIDeviceManager>& manager,
                      UINT& resetToken,
                      std::string& error)
{
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
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        device.GetAddressOf(), &selected, context.GetAddressOf());
    if (FAILED(result)) {
        error = "D3D11CreateDevice failed (" + HrText(result) + ")";
        return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    result = MFCreateDXGIDeviceManager(
        &resetToken, manager.GetAddressOf());
    if (FAILED(result)) {
        error = "MFCreateDXGIDeviceManager failed (" + HrText(result) + ")";
        return false;
    }
    result = manager->ResetDevice(device.Get(), resetToken);
    if (FAILED(result)) {
        error = "IMFDXGIDeviceManager::ResetDevice failed (" +
                HrText(result) + ")";
        return false;
    }
    return true;
}

bool ReadCurrentProbeFormat(IMFSourceReader* reader,
                            UINT& width,
                            UINT& height,
                            GUID& subtype,
                            std::string& error)
{
    ComPtr<IMFMediaType> currentType;
    HRESULT result = reader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        currentType.GetAddressOf());
    if (FAILED(result)) {
        error = "GetCurrentMediaType failed (" + HrText(result) + ")";
        return false;
    }
    if (FAILED(MFGetAttributeSize(
            currentType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        FAILED(currentType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        error = "The selected camera format is incomplete";
        return false;
    }
    return width != 0 && height != 0;
}

bool SelectProbeOutput(IMFSourceReader* reader,
                       bool accelerated,
                       UINT& width,
                       UINT& height,
                       GUID& subtype,
                       std::string& error)
{
    HRESULT result = reader->SetStreamSelection(
        MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(result) ||
        FAILED(reader->SetStreamSelection(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE))) {
        error = "Could not select the camera video stream";
        return false;
    }

    if (!accelerated) {
        // Match MediaCapture::ConfigureReader: request the same raw formats
        // without pinning a size, then inspect what the driver negotiated.
        static const GUID preferredSubtypes[] = {
            MFVideoFormat_NV12,
            MFVideoFormat_YUY2,
            MFVideoFormat_ARGB32,
            MFVideoFormat_RGB32,
        };
        HRESULT selectionResult = MF_E_INVALIDMEDIATYPE;
        for (const GUID& candidate : preferredSubtypes) {
            ComPtr<IMFMediaType> outputType;
            if (FAILED(MFCreateMediaType(outputType.GetAddressOf())) ||
                FAILED(outputType->SetGUID(
                    MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                FAILED(outputType->SetGUID(MF_MT_SUBTYPE, candidate))) {
                error = "Could not create a compatibility probe format";
                return false;
            }
            selectionResult = reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                nullptr,
                outputType.Get());
            if (SUCCEEDED(selectionResult)) {
                return ReadCurrentProbeFormat(
                    reader, width, height, subtype, error);
            }
        }
        error = "SetCurrentMediaType(compatibility formats) failed (" +
                HrText(selectionResult) + ")";
        return false;
    }

    ComPtr<IMFMediaType> nativeType;
    result = reader->GetNativeMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        nativeType.GetAddressOf());
    if (FAILED(result)) {
        error = "GetNativeMediaType failed (" + HrText(result) + ")";
        return false;
    }
    if (FAILED(MFGetAttributeSize(
            nativeType.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
        error = "The camera did not report a frame size";
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    if (FAILED(MFCreateMediaType(outputType.GetAddressOf())) ||
        FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32)) ||
        FAILED(MFSetAttributeSize(
            outputType.Get(), MF_MT_FRAME_SIZE, width, height))) {
        error = "Could not create the BGRA probe format";
        return false;
    }

    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(
            nativeType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
        numerator != 0 && denominator != 0) {
        MFSetAttributeRatio(
            outputType.Get(), MF_MT_FRAME_RATE, numerator, denominator);
    }

    result = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get());
    if (FAILED(result)) {
        error = "SetCurrentMediaType(BGRA) failed (" + HrText(result) + ")";
        return false;
    }
    return ReadCurrentProbeFormat(
        reader, width, height, subtype, error);
}

bool AnalyzePixels(const BYTE* data,
                   LONG stride,
                   UINT width,
                   UINT height,
                   const GUID& subtype,
                   double& mean,
                   double& range)
{
    if (!data || stride == 0 || width == 0 || height == 0) {
        return false;
    }
    const bool bgra =
        IsEqualGUID(subtype, MFVideoFormat_ARGB32) ||
        IsEqualGUID(subtype, MFVideoFormat_RGB32);
    const bool nv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
    const bool yuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
    if (!bgra && !nv12 && !yuy2) {
        return false;
    }
    const UINT stepX = std::max(1u, width / 32u);
    const UINT stepY = std::max(1u, height / 18u);
    double total = 0.0;
    double minimum = 255.0;
    double maximum = 0.0;
    std::uint64_t samples = 0;
    for (UINT y = 0; y < height; y += stepY) {
        const BYTE* row =
            stride > 0
                ? data + static_cast<std::size_t>(y) * stride
                : data + static_cast<std::size_t>(height - 1u - y) *
                             static_cast<std::size_t>(-stride);
        for (UINT x = 0; x < width; x += stepX) {
            double luma = 0.0;
            if (bgra) {
                const BYTE* pixel =
                    row + static_cast<std::size_t>(x) * 4u;
                luma =
                    0.0722 * pixel[0] +
                    0.7152 * pixel[1] +
                    0.2126 * pixel[2];
            } else if (nv12) {
                luma = row[x];
            } else {
                luma = row[static_cast<std::size_t>(x) * 2u];
            }
            total += luma;
            minimum = std::min(minimum, luma);
            maximum = std::max(maximum, luma);
            ++samples;
        }
    }
    if (samples == 0) {
        return false;
    }
    mean = total / static_cast<double>(samples);
    range = maximum - minimum;
    return true;
}

bool AnalyzeDxgiBuffer(IMFMediaBuffer* buffer,
                       ID3D11Device* device,
                       ID3D11DeviceContext* context,
                       UINT width,
                       UINT height,
                       const GUID& subtype,
                       double& mean,
                       double& range,
                       std::string& error)
{
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer->QueryInterface(IID_PPV_ARGS(dxgiBuffer.GetAddressOf())))) {
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    HRESULT result = dxgiBuffer->GetResource(
        IID_PPV_ARGS(texture.GetAddressOf()));
    if (FAILED(result)) {
        error = "IMFDXGIBuffer::GetResource failed (" +
                HrText(result) + ")";
        return false;
    }
    UINT subresource = 0;
    dxgiBuffer->GetSubresourceIndex(&subresource);

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Width = width;
    stagingDescription.Height = height;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    result = device->CreateTexture2D(
        &stagingDescription, nullptr, staging.GetAddressOf());
    if (FAILED(result)) {
        error = "CreateTexture2D(readback) failed (" + HrText(result) + ")";
        return false;
    }
    context->CopySubresourceRegion(
        staging.Get(), 0, 0, 0, 0, texture.Get(), subresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        error = "Map(readback) failed (" + HrText(result) + ")";
        return false;
    }
    const bool analyzed = AnalyzePixels(
        static_cast<const BYTE*>(mapped.pData),
        static_cast<LONG>(mapped.RowPitch),
        width, height, subtype, mean, range);
    context->Unmap(staging.Get(), 0);
    return analyzed;
}

bool AnalyzeSystemBuffer(IMFMediaBuffer* buffer,
                         UINT width,
                         UINT height,
                         const GUID& subtype,
                         double& mean,
                         double& range)
{
    ComPtr<IMF2DBuffer> twoDimensional;
    if (SUCCEEDED(buffer->QueryInterface(
            IID_PPV_ARGS(twoDimensional.GetAddressOf())))) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(twoDimensional->Lock2D(&scanline, &pitch))) {
            const bool analyzed =
                AnalyzePixels(
                    scanline, pitch, width, height, subtype, mean, range);
            twoDimensional->Unlock2D();
            return analyzed;
        }
    }

    BYTE* bytes = nullptr;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&bytes, nullptr, &currentLength)) || !bytes) {
        return false;
    }
    std::size_t bytesPerRow = 0;
    if (IsEqualGUID(subtype, MFVideoFormat_ARGB32) ||
        IsEqualGUID(subtype, MFVideoFormat_RGB32)) {
        bytesPerRow = static_cast<std::size_t>(width) * 4u;
    } else if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        bytesPerRow = width;
    } else if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) {
        bytesPerRow = static_cast<std::size_t>(width) * 2u;
    }
    const bool analyzed =
        bytesPerRow != 0 &&
        currentLength >=
            static_cast<DWORD>(bytesPerRow * height) &&
        AnalyzePixels(bytes, static_cast<LONG>(bytesPerRow),
                      width, height, subtype, mean, range);
    buffer->Unlock();
    return analyzed;
}

ProbeResult RunChildProbe(std::size_t cameraIndex, bool accelerated)
{
    ProbeResult probe;
    probe.accelerated = accelerated;
    const auto cameras = EnumerateCameras();
    if (cameraIndex >= cameras.size()) {
        probe.status = "error";
        probe.detail = "Camera index is out of range";
        return probe;
    }
    probe.camera = Narrow(CameraName(cameras[cameraIndex].Get()));

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    if (accelerated &&
        !CreateD3DManager(
            device, context, manager, resetToken, probe.detail)) {
        return probe;
    }

    ComPtr<IMFMediaSource> source;
    HRESULT result = cameras[cameraIndex]->ActivateObject(
        IID_PPV_ARGS(source.GetAddressOf()));
    if (FAILED(result)) {
        probe.detail = "ActivateObject failed (" + HrText(result) + ")";
        return probe;
    }

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.GetAddressOf(), 8);
    // Basic and advanced source-reader video processing are mutually
    // exclusive. Advanced processing is the DXVA-aware path; compatibility
    // mode mirrors OkuFlow's established software-conversion reader.
    attributes->SetUINT32(
        accelerated
            ? MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
            : MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
        TRUE);
    attributes->SetUINT32(
        MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
        accelerated ? TRUE : FALSE);
    attributes->SetUINT32(
        MF_SOURCE_READER_DISABLE_DXVA,
        accelerated ? FALSE : TRUE);
    attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    if (accelerated) {
        attributes->SetUnknown(
            MF_SOURCE_READER_D3D_MANAGER, manager.Get());
    }

    ComPtr<IMFSourceReader> reader;
    result = MFCreateSourceReaderFromMediaSource(
        source.Get(), attributes.Get(), reader.GetAddressOf());
    if (FAILED(result)) {
        probe.detail =
            "MFCreateSourceReaderFromMediaSource failed (" +
            HrText(result) + ")";
        source->Shutdown();
        cameras[cameraIndex]->ShutdownObject();
        return probe;
    }

    UINT width = 0;
    UINT height = 0;
    GUID subtype = GUID_NULL;
    if (!SelectProbeOutput(
            reader.Get(), accelerated, width, height,
            subtype, probe.detail)) {
        source->Shutdown();
        cameras[cameraIndex]->ShutdownObject();
        return probe;
    }

    double totalReadMs = 0.0;
    double totalLuma = 0.0;
    double maximumRange = 0.0;
    LONGLONG previousTimestamp = -1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (probe.frames < kRequiredFrames &&
           std::chrono::steady_clock::now() < deadline) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const auto start = std::chrono::steady_clock::now();
        result = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &streamIndex, &flags, &timestamp, sample.GetAddressOf());
        const auto end = std::chrono::steady_clock::now();
        totalReadMs +=
            std::chrono::duration<double, std::milli>(end - start).count();
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            probe.detail =
                "ReadSample failed (" + HrText(result) + ")";
            break;
        }
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(
                0, buffer.GetAddressOf())) || !buffer) {
            continue;
        }
        double luma = 0.0;
        double spatialRange = 0.0;
        std::string analysisError;
        bool analyzed = false;
        if (accelerated) {
            analyzed = AnalyzeDxgiBuffer(
                buffer.Get(), device.Get(), context.Get(),
                width, height, subtype,
                luma, spatialRange, analysisError);
            if (analyzed) {
                probe.dxgiSamples = true;
            }
        }
        if (!analyzed) {
            analyzed = AnalyzeSystemBuffer(
                buffer.Get(), width, height, subtype,
                luma, spatialRange);
        }
        if (!analyzed) {
            probe.detail =
                analysisError.empty()
                    ? "The sample could not be inspected"
                    : analysisError;
            break;
        }

        if (previousTimestamp >= 0 && timestamp > previousTimestamp) {
            ++probe.timestampsAdvanced;
        }
        previousTimestamp = timestamp;
        totalLuma += luma;
        maximumRange = std::max(maximumRange, spatialRange);
        ++probe.frames;
    }

    reader.Reset();
    source->Shutdown();
    cameras[cameraIndex]->ShutdownObject();

    if (probe.frames > 0) {
        probe.averageReadMs =
            totalReadMs / static_cast<double>(probe.frames);
        probe.averageLuma =
            totalLuma / static_cast<double>(probe.frames);
        probe.spatialRange = maximumRange;
    }
    if (probe.frames < kRequiredFrames) {
        if (probe.detail.empty()) {
            probe.detail = "Camera did not deliver enough frames";
        }
        probe.status = "error";
    } else if (probe.timestampsAdvanced < kRequiredFrames - 2) {
        probe.status = "error";
        probe.detail = "Camera timestamps did not advance";
    } else if (probe.averageLuma < 1.0 ||
               probe.averageLuma > 254.0 ||
               probe.spatialRange < 2.0) {
        probe.status = "black";
        probe.detail =
            "Frames were constant or nearly blank; point the camera at a "
            "detailed scene and retry";
    } else {
        probe.status = "ok";
        probe.detail =
            accelerated && !probe.dxgiSamples
                ? "Healthy frames, but this driver returned system-memory "
                  "samples under accelerated configuration"
                : "Healthy frames";
    }
    return probe;
}

InteropStressResult RunInteropStressChild(
    std::size_t cameraIndex,
    InteropStressMode mode,
    int iterations,
    std::string& cameraName)
{
    InteropStressResult probe;
    probe.path =
        mode == InteropStressMode::ExternalMemory
            ? "d3d11D3d12CudaExternalMemory"
            : "legacyD3D11Registration";
    probe.iterationsRequested = iterations;
    const auto cameras = EnumerateCameras();
    if (cameraIndex >= cameras.size()) {
        probe.detail = "Camera index is out of range";
        return probe;
    }
    cameraName = Narrow(CameraName(cameras[cameraIndex].Get()));

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    if (!CreateD3DManager(
            device, context, manager, resetToken, probe.detail)) {
        return probe;
    }

    ComPtr<IMFMediaSource> source;
    HRESULT result = cameras[cameraIndex]->ActivateObject(
        IID_PPV_ARGS(source.GetAddressOf()));
    if (FAILED(result)) {
        probe.detail = "ActivateObject failed (" + HrText(result) + ")";
        return probe;
    }

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.GetAddressOf(), 8);
    attributes->SetUINT32(
        MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    attributes->SetUnknown(
        MF_SOURCE_READER_D3D_MANAGER, manager.Get());

    ComPtr<IMFSourceReader> reader;
    result = MFCreateSourceReaderFromMediaSource(
        source.Get(), attributes.Get(), reader.GetAddressOf());
    if (FAILED(result)) {
        probe.detail =
            "MFCreateSourceReaderFromMediaSource failed (" +
            HrText(result) + ")";
        source->Shutdown();
        cameras[cameraIndex]->ShutdownObject();
        return probe;
    }

    UINT width = 0;
    UINT height = 0;
    GUID subtype = GUID_NULL;
    if (!SelectProbeOutput(
            reader.Get(), true, width, height, subtype, probe.detail)) {
        reader.Reset();
        source->Shutdown();
        cameras[cameraIndex]->ShutdownObject();
        return probe;
    }

    ComPtr<ID3D11Texture2D> ownedTexture;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ownedTexture &&
           std::chrono::steady_clock::now() < deadline) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &streamIndex, &flags, &timestamp, sample.GetAddressOf());
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            probe.detail =
                "ReadSample failed (" + HrText(result) + ")";
            break;
        }
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        ComPtr<ID3D11Texture2D> sampleTexture;
        if (FAILED(sample->GetBufferByIndex(
                0, buffer.GetAddressOf())) ||
            FAILED(buffer.As(&dxgiBuffer)) ||
            FAILED(dxgiBuffer->GetResource(
                IID_PPV_ARGS(sampleTexture.GetAddressOf())))) {
            probe.detail =
                "Accelerated camera did not return a DXGI texture";
            break;
        }
        UINT subresource = 0;
        dxgiBuffer->GetSubresourceIndex(&subresource);

        D3D11_TEXTURE2D_DESC description{};
        sampleTexture->GetDesc(&description);
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = 0;
        description.MiscFlags =
            mode == InteropStressMode::ExternalMemory
                ? D3D11_RESOURCE_MISC_SHARED |
                      D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                : 0;
        result = device->CreateTexture2D(
            &description, nullptr, ownedTexture.GetAddressOf());
        if (FAILED(result)) {
            probe.detail =
                "CreateTexture2D(stress target) failed (" +
                HrText(result) + ")";
            break;
        }
        context->CopySubresourceRegion(
            ownedTexture.Get(), 0, 0, 0, 0,
            sampleTexture.Get(), subresource, nullptr);

        D3D11_QUERY_DESC queryDescription{};
        queryDescription.Query = D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> readyQuery;
        result = device->CreateQuery(
            &queryDescription, readyQuery.GetAddressOf());
        if (FAILED(result)) {
            probe.detail =
                "CreateQuery(stress target) failed (" +
                HrText(result) + ")";
            ownedTexture.Reset();
            break;
        }
        context->End(readyQuery.Get());
        context->Flush();
        const auto copyDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            result = context->GetData(
                readyQuery.Get(), nullptr, 0, 0);
            if (result == S_OK) {
                break;
            }
            if (FAILED(result)) {
                probe.detail =
                    "D3D11 stress copy query failed (" +
                    HrText(result) + ")";
                ownedTexture.Reset();
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < copyDeadline);
        if (result != S_OK) {
            if (probe.detail.empty()) {
                probe.detail =
                    "D3D11 stress copy did not complete before timeout";
            }
            ownedTexture.Reset();
        }
    }

    reader.Reset();
    source->Shutdown();
    cameras[cameraIndex]->ShutdownObject();

    if (!ownedTexture) {
        if (probe.detail.empty()) {
            probe.detail =
                "Camera did not deliver a usable GPU texture";
        }
        return probe;
    }
    return RunCudaInteropStress(
        device.Get(), context.Get(), ownedTexture.Get(), mode, iterations);
}

std::filesystem::path TemporaryJsonPath()
{
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(directory), directory);
    wchar_t name[MAX_PATH]{};
    GetTempFileNameW(directory, L"ozd", 0, name);
    return name;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string RunChildWithWatchdog(const std::wstring& executable,
                                 std::size_t cameraIndex,
                                 bool accelerated,
                                 DWORD timeoutMs)
{
    const std::filesystem::path outputPath = TemporaryJsonPath();
    std::wostringstream command;
    command << L'"' << executable << L"\" --child --camera "
            << cameraIndex << L" --mode "
            << (accelerated ? L"accelerated" : L"compatibility")
            << L" --json-file \"" << outputPath.wstring() << L'"';
    std::wstring mutableCommand = command.str();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process);
    if (!created) {
        std::filesystem::remove(outputPath);
        return "{\"status\":\"error\",\"detail\":\"Could not start probe child\"}";
    }
    const DWORD waitResult =
        WaitForSingleObject(process.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::filesystem::remove(outputPath);
        return std::string("{\"status\":\"hang\",\"detail\":\"Camera driver ")
             + "did not return before the watchdog timeout\",\"mode\":\""
             + (accelerated ? "accelerated" : "compatibility") + "\"}";
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::string result = ReadTextFile(outputPath);
    std::filesystem::remove(outputPath);
    if (result.empty()) {
        result =
            "{\"status\":\"error\",\"detail\":\"Probe child returned no verdict\"}";
    }
    return result;
}

std::string RunInteropChildWithWatchdog(
    const std::wstring& executable,
    std::size_t cameraIndex,
    InteropStressMode mode,
    int iterations,
    DWORD timeoutMs)
{
    const std::filesystem::path outputPath = TemporaryJsonPath();
    std::wostringstream command;
    command << L'"' << executable
            << L"\" --interop-child --camera " << cameraIndex
            << L" --interop "
            << (mode == InteropStressMode::ExternalMemory
                    ? L"external"
                    : L"legacy")
            << L" --iterations " << iterations
            << L" --json-file \"" << outputPath.wstring() << L'"';
    std::wstring mutableCommand = command.str();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process);
    if (!created) {
        std::filesystem::remove(outputPath);
        return "{\"status\":\"error\",\"detail\":\"Could not start "
               "interop stress child\"}";
    }

    const DWORD waitResult =
        WaitForSingleObject(process.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::filesystem::remove(outputPath);
        return std::string(
                   "{\"status\":\"hang\",\"detail\":\"Interop driver "
                   "operation did not return before the watchdog timeout\","
                   "\"path\":\"")
             + (mode == InteropStressMode::ExternalMemory
                    ? "d3d11D3d12CudaExternalMemory"
                    : "legacyD3D11Registration")
             + "\"}";
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::string verdict = ReadTextFile(outputPath);
    std::filesystem::remove(outputPath);
    if (verdict.empty()) {
        std::ostringstream failure;
        failure << "{\"status\":\"crash\",\"detail\":\"Interop stress child "
                   "returned no verdict\",\"path\":\""
                << (mode == InteropStressMode::ExternalMemory
                        ? "d3d11D3d12CudaExternalMemory"
                        : "legacyD3D11Registration")
                << "\",\"exitCode\":" << exitCode << "}";
        verdict = failure.str();
    }
    return verdict;
}

std::wstring ExecutablePath()
{
    std::wstring path(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(),
                           static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

int ParseCameraIndex(int argc, wchar_t** argv, std::size_t& index)
{
    for (int argument = 1; argument + 1 < argc; ++argument) {
        if (std::wstring(argv[argument]) == L"--camera") {
            try {
                index =
                    static_cast<std::size_t>(
                        std::stoull(argv[argument + 1]));
                return 0;
            } catch (...) {
                return 2;
            }
        }
    }
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool child =
        std::find_if(argv + 1, argv + argc, [](const wchar_t* value) {
            return std::wstring(value) == L"--child";
        }) != argv + argc;
    const bool interopChild =
        std::find_if(argv + 1, argv + argc, [](const wchar_t* value) {
            return std::wstring(value) == L"--interop-child";
        }) != argv + argc;

    ComScope com;
    MfScope mediaFoundation;
    if ((FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) ||
        FAILED(mediaFoundation.result)) {
        std::cerr
            << "{\"status\":\"error\",\"detail\":\"COM or Media Foundation "
               "initialization failed\"}\n";
        return 3;
    }

    if (interopChild) {
        std::size_t cameraIndex = 0;
        if (ParseCameraIndex(argc, argv, cameraIndex) != 0) {
            return 2;
        }
        InteropStressMode mode = InteropStressMode::ExternalMemory;
        int iterations = 100;
        std::filesystem::path jsonFile;
        for (int argument = 1; argument + 1 < argc; ++argument) {
            const std::wstring name = argv[argument];
            if (name == L"--interop") {
                mode =
                    std::wstring(argv[argument + 1]) == L"legacy"
                        ? InteropStressMode::LegacyRegistration
                        : InteropStressMode::ExternalMemory;
            } else if (name == L"--iterations") {
                iterations = std::max(
                    1, std::stoi(argv[argument + 1]));
            } else if (name == L"--json-file") {
                jsonFile = argv[argument + 1];
            }
        }
        std::string cameraName;
        const std::string verdict =
            ToJson(
                RunInteropStressChild(
                    cameraIndex, mode, iterations, cameraName),
                cameraName);
        if (!jsonFile.empty()) {
            std::ofstream output(jsonFile, std::ios::binary);
            output << verdict;
        } else {
            std::cout << verdict << '\n';
        }
        return 0;
    }

    if (child) {
        std::size_t cameraIndex = 0;
        if (ParseCameraIndex(argc, argv, cameraIndex) != 0) {
            return 2;
        }
        bool accelerated = true;
        std::filesystem::path jsonFile;
        for (int argument = 1; argument + 1 < argc; ++argument) {
            const std::wstring name = argv[argument];
            if (name == L"--mode") {
                accelerated =
                    std::wstring(argv[argument + 1]) == L"accelerated";
            } else if (name == L"--json-file") {
                jsonFile = argv[argument + 1];
            }
        }
        const std::string verdict =
            ToJson(RunChildProbe(cameraIndex, accelerated));
        if (!jsonFile.empty()) {
            std::ofstream output(jsonFile, std::ios::binary);
            output << verdict;
        } else {
            std::cout << verdict << '\n';
        }
        return 0;
    }

    const auto cameras = EnumerateCameras();
    if (cameras.empty()) {
        std::cout
            << "{\"status\":\"skip\",\"reason\":\"no camera present\","
               "\"cameras\":[]}\n";
        return 77;
    }

    DWORD timeoutMs = kDefaultTimeoutMs;
    bool timeoutSpecified = false;
    std::size_t selectedCamera = static_cast<std::size_t>(-1);
    bool interopRequested = false;
    InteropStressMode interopMode =
        InteropStressMode::ExternalMemory;
    int interopIterations = 100;
    for (int argument = 1; argument + 1 < argc; ++argument) {
        const std::wstring name = argv[argument];
        if (name == L"--timeout-ms") {
            timeoutMs =
                static_cast<DWORD>(std::stoul(argv[argument + 1]));
            timeoutSpecified = true;
        } else if (name == L"--camera") {
            selectedCamera =
                static_cast<std::size_t>(
                    std::stoull(argv[argument + 1]));
        } else if (name == L"--interop") {
            interopRequested = true;
            interopMode =
                std::wstring(argv[argument + 1]) == L"legacy"
                    ? InteropStressMode::LegacyRegistration
                    : InteropStressMode::ExternalMemory;
        } else if (name == L"--iterations") {
            interopIterations =
                std::max(1, std::stoi(argv[argument + 1]));
        }
    }
    if (selectedCamera != static_cast<std::size_t>(-1) &&
        selectedCamera >= cameras.size()) {
        std::cerr
            << "{\"status\":\"error\",\"reason\":\"camera index out of range\"}\n";
        return 2;
    }

    if (interopRequested) {
        if (!timeoutSpecified) {
            timeoutMs = 60000;
        }
        std::cout
            << "{\"status\":\"complete\",\"stressPath\":\""
            << (interopMode == InteropStressMode::ExternalMemory
                    ? "d3d11D3d12CudaExternalMemory"
                    : "legacyD3D11Registration")
            << "\",\"cameras\":[";
        bool firstStress = true;
        for (std::size_t index = 0; index < cameras.size(); ++index) {
            if (selectedCamera != static_cast<std::size_t>(-1) &&
                index != selectedCamera) {
                continue;
            }
            if (!firstStress) {
                std::cout << ',';
            }
            firstStress = false;
            const std::string name =
                Narrow(CameraName(cameras[index].Get()));
            const std::string verdict =
                RunInteropChildWithWatchdog(
                    ExecutablePath(),
                    index,
                    interopMode,
                    interopIterations,
                    timeoutMs);
            std::cout << "{\"index\":" << index
                      << ",\"name\":\"" << EscapeJson(name) << "\","
                      << "\"stress\":" << verdict << '}';
        }
        std::cout << "]}\n";
        return 0;
    }

    std::cout << "{\"status\":\"complete\",\"cameras\":[";
    bool first = true;
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        if (selectedCamera != static_cast<std::size_t>(-1) &&
            index != selectedCamera) {
            continue;
        }
        if (!first) {
            std::cout << ',';
        }
        first = false;
        const std::string name = Narrow(CameraName(cameras[index].Get()));
        const std::string accelerated =
            RunChildWithWatchdog(
                ExecutablePath(), index, true, timeoutMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::string compatibility =
            RunChildWithWatchdog(
                ExecutablePath(), index, false, timeoutMs);
        std::cout << "{\"index\":" << index
                  << ",\"name\":\"" << EscapeJson(name) << "\","
                  << "\"accelerated\":" << accelerated << ','
                  << "\"compatibility\":" << compatibility << '}';
    }
    std::cout << "]}\n";
    return 0;
}

#endif // _WIN32
