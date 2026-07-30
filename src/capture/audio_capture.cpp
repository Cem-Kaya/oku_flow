#ifdef _WIN32

#include "openzoom/capture/audio_capture.hpp"

#include "openzoom/common/recording_contract.hpp"

#include <QDebug>

#include <mferror.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace openzoom {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT32 kAudioSampleRate = 48000;
constexpr UINT32 kAudioChannels = 1;
constexpr UINT32 kAudioBitsPerSample = 16;

std::string HResultText(HRESULT result)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX",
                  static_cast<unsigned long>(result));
    LPSTR message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0,
        reinterpret_cast<LPSTR>(&message), 0, nullptr);
    std::string text = code;
    if (length != 0 && message) {
        std::string detail(message, length);
        while (!detail.empty() &&
               std::isspace(
                   static_cast<unsigned char>(detail.back()))) {
            detail.pop_back();
        }
        if (!detail.empty()) {
            text += ": " + detail;
        }
    }
    if (message) {
        LocalFree(message);
    }
    return text;
}

void ThrowIfFailed(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(operation) + " (" +
            HResultText(result) + ")");
    }
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
        kMediaFoundationTicksPerSecond /
        static_cast<long double>(frequency.QuadPart));
}

void ShutdownSource(IMFMediaSource* source)
{
    if (source) {
        source->Shutdown();
    }
}

std::wstring DefaultCaptureEndpointId()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(
            eCapture, eMultimedia, device.GetAddressOf()))) {
        return {};
    }
    LPWSTR endpointId = nullptr;
    if (FAILED(device->GetId(&endpointId)) || !endpointId) {
        return {};
    }
    std::wstring result(endpointId);
    CoTaskMemFree(endpointId);
    return result;
}

} // namespace

AudioCapture::~AudioCapture()
{
    Stop();
}

std::vector<AudioDeviceDescriptor> AudioCapture::EnumerateDevices()
{
    std::vector<AudioDeviceDescriptor> devices;
    lastError_.clear();
    const std::wstring defaultEndpointId =
        DefaultCaptureEndpointId();

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(
        attributes.GetAddressOf(), 1);
    if (FAILED(result)) {
        lastError_ =
            "Could not create microphone attributes (" +
            HResultText(result) + ")";
        return devices;
    }
    attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);

    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    result = MFEnumDeviceSources(
        attributes.Get(), &activations, &count);
    if (FAILED(result)) {
        lastError_ =
            "Could not enumerate microphones (" +
            HResultText(result) + ")";
        return devices;
    }

    for (UINT32 index = 0; index < count; ++index) {
        AudioDeviceDescriptor descriptor;
        WCHAR* value = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                &value, &length))) {
            descriptor.name.assign(value, value + length);
            CoTaskMemFree(value);
        }
        value = nullptr;
        length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
                &value, &length))) {
            descriptor.endpointId.assign(value, value + length);
            CoTaskMemFree(value);
        }
        descriptor.isDefault =
            !defaultEndpointId.empty() &&
            _wcsicmp(descriptor.endpointId.c_str(),
                     defaultEndpointId.c_str()) == 0;
        descriptor.activation = activations[index];
        devices.push_back(std::move(descriptor));
        activations[index]->Release();
    }
    CoTaskMemFree(activations);

    std::sort(
        devices.begin(), devices.end(),
        [](const AudioDeviceDescriptor& left,
           const AudioDeviceDescriptor& right) {
            if (left.isDefault != right.isDefault) {
                return left.isDefault;
            }
            return _wcsicmp(
                       left.name.c_str(), right.name.c_str()) < 0;
        });
    return devices;
}

bool AudioCapture::ConfigureReader(IMFSourceReader* reader)
{
    if (!reader) {
        return false;
    }
    ThrowIfFailed(
        reader->SetStreamSelection(
            MF_SOURCE_READER_ALL_STREAMS, FALSE),
        "Disable default microphone streams");
    ThrowIfFailed(
        reader->SetStreamSelection(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE),
        "Enable microphone stream");

    ComPtr<IMFMediaType> type;
    ThrowIfFailed(
        MFCreateMediaType(type.GetAddressOf()),
        "Create microphone PCM type");
    ThrowIfFailed(
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio),
        "Set microphone major type");
    ThrowIfFailed(
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM),
        "Set microphone PCM subtype");
    ThrowIfFailed(
        type->SetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels),
        "Set microphone channels");
    ThrowIfFailed(
        type->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioSampleRate),
        "Set microphone sample rate");
    ThrowIfFailed(
        type->SetUINT32(
            MF_MT_AUDIO_BITS_PER_SAMPLE, kAudioBitsPerSample),
        "Set microphone bits per sample");
    const UINT32 blockAlignment =
        kAudioChannels * kAudioBitsPerSample / 8u;
    ThrowIfFailed(
        type->SetUINT32(
            MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlignment),
        "Set microphone block alignment");
    ThrowIfFailed(
        type->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            kAudioSampleRate * blockAlignment),
        "Set microphone byte rate");

    return SUCCEEDED(reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get()));
}

bool AudioCapture::Start(
    const AudioDeviceDescriptor& descriptor,
    AudioFrameCallback callback,
    AudioErrorCallback errorCallback)
{
    Stop();
    lastError_.clear();
    if (!descriptor.activation) {
        lastError_ = "Invalid microphone";
        return false;
    }

    try {
        ComPtr<IMFMediaSource> source;
        ThrowIfFailed(
            descriptor.activation->ActivateObject(
                IID_PPV_ARGS(source.GetAddressOf())),
            "Open microphone");

        ComPtr<IMFAttributes> attributes;
        ThrowIfFailed(
            MFCreateAttributes(attributes.GetAddressOf(), 2),
            "Create microphone reader attributes");
        ThrowIfFailed(
            attributes->SetUINT32(
                MF_READWRITE_DISABLE_CONVERTERS, FALSE),
            "Enable microphone converters");

        ComPtr<IMFSourceReader> reader;
        ThrowIfFailed(
            MFCreateSourceReaderFromMediaSource(
                source.Get(), attributes.Get(),
                reader.GetAddressOf()),
            "Create microphone reader");
        if (!ConfigureReader(reader.Get())) {
            ShutdownSource(source.Get());
            descriptor.activation->ShutdownObject();
            lastError_ =
                "This microphone could not provide 48 kHz audio";
            return false;
        }

        mediaSource_ = std::move(source);
        sourceReader_ = std::move(reader);
        activeActivation_ = descriptor.activation;
        activeEndpointId_ = descriptor.endpointId;
        running_ = true;
        captureThread_ = std::thread(
            &AudioCapture::CaptureLoop, this,
            std::move(callback), std::move(errorCallback));
        return true;
    } catch (const std::exception& error) {
        lastError_ = error.what();
    } catch (...) {
        lastError_ = "Unknown error opening microphone";
    }
    Stop();
    return false;
}

void AudioCapture::Stop()
{
    running_ = false;
    if (sourceReader_) {
        sourceReader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
    }
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    sourceReader_.Reset();
    ShutdownSource(mediaSource_.Get());
    mediaSource_.Reset();
    if (activeActivation_) {
        activeActivation_->ShutdownObject();
    }
    activeActivation_.Reset();
    activeEndpointId_.clear();
}

void AudioCapture::CaptureLoop(
    AudioFrameCallback callback,
    AudioErrorCallback errorCallback)
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    ComPtr<IMFSourceReader> reader = sourceReader_;
    while (running_ && reader) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG sourceTimestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT result = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
            &streamIndex, &flags, &sourceTimestamp,
            sample.GetAddressOf());
        if (!running_) {
            break;
        }
        if (FAILED(result) ||
            (flags & MF_SOURCE_READERF_ERROR) != 0) {
            running_ = false;
            const std::string message =
                "Microphone capture stopped (" +
                HResultText(result) + ")";
            if (errorCallback) {
                errorCallback(message);
            }
            break;
        }
        if (!sample ||
            (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(
                buffer.GetAddressOf())) || !buffer) {
            continue;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(
                &data, nullptr, &length)) || !data) {
            continue;
        }
        AudioFrame frame;
        frame.pcm.assign(data, data + length);
        frame.duration100ns =
            static_cast<std::int64_t>(
                static_cast<long double>(length) *
                kMediaFoundationTicksPerSecond /
                (kAudioSampleRate * kAudioChannels *
                 (kAudioBitsPerSample / 8u)));
        // ReadSample returns after the block has accumulated. Timestamp the
        // first PCM sample, not callback completion, so audio and video share
        // one QPC time origin without a one-block audio delay.
        frame.captureClock100ns =
            QueryClock100ns() - frame.duration100ns;
        buffer->Unlock();
        if (callback) {
            callback(std::move(frame));
        }
    }
    if (uninitializeCom) {
        CoUninitialize();
    }
}

} // namespace openzoom

#endif // _WIN32
