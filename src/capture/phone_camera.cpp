#ifdef _WIN32
#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/capture/capture_buffer.hpp"
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QtEndian>
#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>

namespace okuflow {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr int kControlLimit = 64 * 1024;
constexpr int kVideoLimit = 16 * 1024 * 1024;
struct Packet { quint16 type{}, flags{}; QByteArray data; };

void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(hr) + ")");
}
void CheckCanceled(const std::atomic<bool>* canceled)
{
    if (canceled && canceled->load()) throw std::runtime_error("Phone camera canceled");
}
QByteArray Read(QTcpSocket& socket, int size, int timeout, const std::atomic<bool>* canceled = nullptr)
{
    QByteArray result;
    result.reserve(size);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    while (result.size() < size) {
        CheckCanceled(canceled);
        const auto bytes = socket.read(size - result.size());
        result.append(bytes);
        if (result.size() == size) break;
        if (socket.state() != QAbstractSocket::ConnectedState) throw std::runtime_error("Phone USB connection closed");
        if (Clock::now() >= deadline) throw std::runtime_error("Phone USB response timed out");
        socket.waitForReadyRead(20);
    }
    return result;
}
void Write(QTcpSocket& socket, const QByteArray& bytes, const std::atomic<bool>* canceled = nullptr)
{
    if (socket.write(bytes) != bytes.size()) throw std::runtime_error("Phone USB write failed");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (socket.bytesToWrite()) {
        CheckCanceled(canceled);
        if (socket.state() != QAbstractSocket::ConnectedState || Clock::now() >= deadline)
            throw std::runtime_error("Phone USB write timed out");
        socket.waitForBytesWritten(20);
    }
}
void Connect(QTcpSocket& socket, int timeout)
{
    socket.setReadBufferSize(32 * 1024 * 1024);
    socket.connectToHost(QHostAddress::LocalHost, 5037);
    if (!socket.waitForConnected(timeout)) throw std::runtime_error("The local adb server is unavailable");
}
void AdbRequest(QTcpSocket& socket, const QByteArray& service, int timeout = 3000,
                const std::atomic<bool>* canceled = nullptr)
{
    if (service.size() > 65535) throw std::runtime_error("Invalid adb request");
    Write(socket, QByteArray::number(service.size(), 16).rightJustified(4, '0') + service, canceled);
    const auto status = Read(socket, 4, timeout, canceled);
    if (status == "FAIL") {
        bool valid = false;
        const int size = Read(socket, 4, timeout, canceled).toInt(&valid, 16);
        if (!valid || size < 0 || size > kControlLimit) throw std::runtime_error("Invalid adb error");
        throw std::runtime_error(Read(socket, size, timeout, canceled).toStdString());
    }
    if (status != "OKAY") throw std::runtime_error("Invalid adb response");
}
void Transport(QTcpSocket& socket, const std::string& serial, const QByteArray& service,
               const std::atomic<bool>* canceled = nullptr, int timeout = 3000, std::uint64_t transportId = 0)
{
    Connect(socket, 300);
    AdbRequest(socket, transportId ? "host:transport-id:" + QByteArray::number(transportId) :
        "host:transport:" + QByteArray::fromStdString(serial), timeout, canceled);
    AdbRequest(socket, service, timeout, canceled);
}
Packet ReadPacket(QTcpSocket& socket, int limit, const std::atomic<bool>* canceled = nullptr)
{
    const auto header = Read(socket, 8, 3000, canceled);
    const auto size = qFromLittleEndian<quint32>(header.constData() + 4);
    if (size > static_cast<quint32>(limit)) throw std::runtime_error("Oversized phone packet");
    return {qFromLittleEndian<quint16>(header.constData()), qFromLittleEndian<quint16>(header.constData() + 2),
            Read(socket, static_cast<int>(size), 3000, canceled)};
}
QJsonObject Json(const Packet& packet)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(packet.data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Invalid phone JSON");
    return document.object();
}
void Hello(QTcpSocket& socket, quint16 role, const QJsonObject& json)
{
    const auto body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    QByteArray header(12, 0);
    std::memcpy(header.data(), "OKUF", 4);
    qToLittleEndian<quint16>(1, header.data() + 4);
    qToLittleEndian(role, header.data() + 6);
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), header.data() + 8);
    Write(socket, header + body);
}
void Command(QTcpSocket& socket, QJsonObject json, const std::atomic<bool>* canceled = nullptr)
{
    const auto body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (body.size() > kControlLimit) throw std::runtime_error("Oversized phone command");
    QByteArray header(8, 0);
    qToLittleEndian<quint16>(2, header.data());
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), header.data() + 4);
    Write(socket, header + body, canceled);
}
QJsonObject ControlHello(QTcpSocket& socket, int timeout = 3000, bool hevcSupported = false)
{
    QJsonArray codecs{"avc"};
    if (hevcSupported) codecs.append("hevc");
    Hello(socket, 1, {{"codecs", codecs}, {"maxWidth", 4096}, {"maxHeight", 2304},
                     {"maxPixelsPerSecond", 251658240}});
    const auto header = Read(socket, 8, timeout);
    const auto size = qFromLittleEndian<quint32>(header.constData() + 4);
    if (size > kControlLimit) throw std::runtime_error("Oversized phone acknowledgement");
    const Packet packet{qFromLittleEndian<quint16>(header.constData()), qFromLittleEndian<quint16>(header.constData() + 2),
        Read(socket, static_cast<int>(size), timeout)};
    if (packet.type != 1 || packet.flags) throw std::runtime_error("Missing phone acknowledgement");
    const auto json = Json(packet);
    if (json["protocol"].toInt() != 1 || json["token"].toString().isEmpty()) throw std::runtime_error("Unsupported phone protocol");
    return json;
}

struct HevcDecoderError : std::runtime_error { using std::runtime_error::runtime_error; };

ComPtr<IMFTransform> CreatePhoneDecoder(PhoneVideoCodec codec)
{
    ComPtr<IMFTransform> transform;
    if (codec == PhoneVideoCodec::Avc) {
        Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(transform.GetAddressOf())), "Create phone H.264 decoder");
        return transform;
    }
    if (codec != PhoneVideoCodec::Hevc) throw std::runtime_error("Unsupported phone codec");
    struct Activations {
        IMFActivate** items = nullptr;
        UINT32 count = 0;
        ~Activations() { for (UINT32 i = 0; i < count; ++i) items[i]->Release(); CoTaskMemFree(items); }
    } activations;
    const MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_HEVC};
    const MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_NV12};
    // The synchronous system MFT can internally use DXVA with our DXGI manager.
    // Asynchronous hardware-proxy MFTs need a different event-driven pump.
    Check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activations.items, &activations.count), "Enumerate phone HEVC decoders");
    for (UINT32 i = 0; i < activations.count; ++i) {
        if (SUCCEEDED(activations.items[i]->ActivateObject(IID_PPV_ARGS(transform.ReleaseAndGetAddressOf())))) {
            activations.items[i]->DetachObject();
            return transform;
        }
    }
    throw std::runtime_error("No available Windows HEVC decoder");
}

// System H.264/HEVC MFTs accept Annex B and can share the capture session's DXGI manager.
class Decoder {
public:
    Decoder(UINT width, UINT height, UINT fps, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
    {
        transform_ = CreatePhoneDecoder(codec);
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_->GetAttributes(&attributes))) {
            attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            UINT32 aware = 0;
            if (manager && SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware)
                transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
        }
        ComPtr<IMFMediaType> input;
        Check(MFCreateMediaType(&input), "Create phone input type");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, codec == PhoneVideoCodec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        if (codec == PhoneVideoCodec::Hevc) input->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        Check(transform_->SetInputType(0, input.Get(), 0), "Set phone input type");
        SelectOutput();
        Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin phone decoder");
        Check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start phone decoder");
    }
    void Input(const QByteArray& data, LONGLONG timestamp, UINT fps, const FrameCallback& callback)
    {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateSample(&sample), "Create compressed sample");
        Check(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buffer), "Allocate compressed sample");
        BYTE* memory = nullptr;
        Check(buffer->Lock(&memory, nullptr, nullptr), "Lock compressed sample");
        std::memcpy(memory, data.constData(), static_cast<size_t>(data.size()));
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(data.size()));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(10'000'000 / std::max(1u, fps));
        HRESULT result = transform_->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING) { Drain(fps, callback); result = transform_->ProcessInput(0, sample.Get(), 0); }
        Check(result, "Decode phone input");
        Drain(fps, callback);
    }
private:
    void SelectOutput()
    {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT result = transform_->GetOutputAvailableType(0, index, &type);
            if (result == MF_E_NO_MORE_TYPES) break;
            Check(result, "Query phone decoder output");
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            Check(transform_->SetOutputType(0, type.Get(), 0), "Set phone NV12 output");
            Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width_, &height_), "Read phone dimensions");
            if (!width_ || !height_ || width_ > 4096 || height_ > 2304) throw std::runtime_error("Unsupported phone dimensions");
            UINT32 pitch = width_;
            type->GetUINT32(MF_MT_DEFAULT_STRIDE, &pitch);
            stride_ = static_cast<LONG>(pitch);
            return;
        }
        throw std::runtime_error("Phone decoder has no NV12 output");
    }
    void Drain(UINT fps, const FrameCallback& callback)
    {
        for (int attempt = 0; attempt < 32; ++attempt) {
            MFT_OUTPUT_STREAM_INFO info{};
            Check(transform_->GetOutputStreamInfo(0, &info), "Query decoder buffer");
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> supplied;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buffer;
                Check(MFCreateSample(&supplied), "Create decoded sample");
                const DWORD size = std::max<DWORD>(info.cbSize, width_ * height_ * 3 / 2);
                if (size > 64 * 1024 * 1024) throw std::runtime_error("Oversized decoded phone buffer");
                Check(MFCreateAlignedMemoryBuffer(size, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Allocate decoded buffer");
                supplied->AddBuffer(buffer.Get());
                output.pSample = supplied.Get();
            }
            DWORD status = 0;
            const HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
            ComPtr<IMFCollection> events;
            events.Attach(output.pEvents);
            ComPtr<IMFSample> decoded = supplied;
            if (output.pSample && output.pSample != supplied.Get()) decoded.Attach(output.pSample);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) { SelectOutput(); continue; }
            Check(result, "Read decoded phone frame");
            if (!decoded) continue;
            MediaFrame frame;
            frame.width = width_; frame.height = height_; frame.stride = stride_;
            frame.subtype = MFVideoFormat_NV12;
            frame.yuvColor = {YuvMatrix::Bt709, YuvRange::Limited};
            frame.frameRateNumerator = fps; frame.frameRateDenominator = 1;
            decoded->GetSampleTime(&frame.captureTimestamp100ns);
            frame.captureClock100ns = MFGetSystemTime(); // Host arrival clock; never claimed as phone sensor latency.
            frame.sequenceNumber = sequence_++;
            ComPtr<IMFMediaBuffer> buffer;
            Check(decoded->GetBufferByIndex(0, &buffer), "Read decoded buffer");
            ComPtr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(buffer.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(frame.gpuTexture.GetAddressOf())))) {
                dxgi->GetSubresourceIndex(&frame.gpuSubresource);
                D3D11_TEXTURE2D_DESC description{};
                frame.gpuTexture->GetDesc(&description);
                frame.gpuFormat = description.Format;
                frame.gpuSample = decoded; // Pin the decoder's reusable surface until all consumers release it.
            } else if (!CopyCaptureBuffer(buffer.Get(), frame.subtype, width_, height_, stride_, frame.data, frame.stride)) {
                throw std::runtime_error("Invalid decoded phone buffer layout");
            }
            frame.dataSize = frame.data.empty() ? static_cast<size_t>(width_) * height_ * 3 / 2 : frame.data.size();
            if (callback) callback(std::move(frame));
        }
        throw std::runtime_error("Phone decoder output burst exceeded its bound");
    }
    ComPtr<IMFTransform> transform_;
    UINT width_{}, height_{};
    LONG stride_{};
    std::uint64_t sequence_{};
};
} // namespace

struct PhoneVideoDecoder::Impl {
    Decoder decoder;
    UINT fps;
    Impl(UINT width, UINT height, UINT rate, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
        : decoder(width, height, rate, codec, manager), fps(rate) {}
};
PhoneVideoDecoder::PhoneVideoDecoder(UINT width, UINT height, UINT fps, PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
    : impl_(std::make_unique<Impl>(width, height, fps, codec, manager)) {}
PhoneVideoDecoder::~PhoneVideoDecoder() = default;
bool PhoneVideoDecoder::IsSupported(PhoneVideoCodec codec, IMFDXGIDeviceManager* manager)
{
    try { PhoneVideoDecoder probe(1920, 1080, 60, codec, manager); return true; }
    catch (const std::exception&) { return false; }
}
void PhoneVideoDecoder::Decode(std::span<const std::uint8_t> data, LONGLONG timestamp, const FrameCallback& callback)
{
    if (data.empty() || data.size() > kVideoLimit) throw std::runtime_error("Invalid compressed phone frame size");
    impl_->decoder.Input(QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())), timestamp, impl_->fps, callback);
}

std::vector<CameraDescriptor> ParseAdbUsbDevices(std::span<const std::uint8_t> protobuf)
{
    struct Reader {
        std::span<const std::uint8_t> bytes;
        size_t position{};
        std::uint64_t Varint() {
            std::uint64_t result = 0;
            for (int shift = 0; shift <= 63; shift += 7) {
                if (position >= bytes.size()) throw std::runtime_error("Truncated adb protobuf varint");
                const auto byte = bytes[position++];
                if (shift == 63 && byte > 1) throw std::runtime_error("Overflowing adb protobuf varint");
                result |= static_cast<std::uint64_t>(byte & 127) << shift;
                if (byte < 128) return result;
            }
            throw std::runtime_error("Invalid adb protobuf varint");
        }
        std::span<const std::uint8_t> Bytes(std::uint64_t size) {
            if (size > bytes.size() - position) throw std::runtime_error("Truncated adb protobuf field");
            auto result = bytes.subspan(position, static_cast<size_t>(size));
            position += static_cast<size_t>(size);
            return result;
        }
        void Skip(unsigned wire) {
            if (wire == 0) Varint();
            else if (wire == 1) Bytes(8);
            else if (wire == 2) { const auto size = Varint(); Bytes(size); }
            else if (wire == 5) Bytes(4);
            else throw std::runtime_error("Unsupported adb protobuf field");
        }
    } root{protobuf};
    if (protobuf.size() > 65535) throw std::runtime_error("Oversized adb device listing");
    std::vector<CameraDescriptor> devices;
    while (root.position < root.bytes.size()) {
        const auto tag = root.Varint();
        if (!(tag >> 3)) throw std::runtime_error("Invalid adb protobuf field");
        if (tag != 10) { root.Skip(static_cast<unsigned>(tag & 7)); continue; }
        const auto size = root.Varint();
        Reader device{root.Bytes(size)};
        std::string serial, model;
        std::uint64_t state = 0, type = 0, transportId = 0;
        while (device.position < device.bytes.size()) {
            const auto field = device.Varint();
            if (!(field >> 3)) throw std::runtime_error("Invalid adb device field");
            if (field == 10 || field == 42) {
                const auto length = device.Varint();
                const auto text = device.Bytes(length);
                if (length > 512) throw std::runtime_error("Oversized adb device name");
                (field == 10 ? serial : model).assign(reinterpret_cast<const char*>(text.data()), text.size());
            } else if (field == 16) state = device.Varint();
            else if (field == 56) type = device.Varint();
            else if (field == 80) transportId = device.Varint();
            else device.Skip(static_cast<unsigned>(field & 7));
        }
        if (type != 1 || state != 8 || serial.empty() || serial.starts_with("emulator-")) continue;
        CameraDescriptor descriptor;
        descriptor.phoneSerial = serial;
        descriptor.phoneTransportId = transportId;
        descriptor.symbolicLink = QString("okuflow-phone:%1").arg(QString::fromStdString(serial)).toStdWString();
        descriptor.name = QString("OkuFlow Camera · %1").arg(QString::fromStdString(model.empty() ? serial : model).replace('_', ' ')).toStdWString();
        devices.push_back(std::move(descriptor));
        if (devices.size() > 128) throw std::runtime_error("Too many USB devices");
    }
    return devices;
}

std::vector<CameraDescriptor> PhoneCameraTransport::Enumerate()
{
    std::vector<CameraDescriptor> devices;
    try {
        const auto listing = [](const QByteArray& service) {
            QTcpSocket socket;
            Connect(socket, 100);
            AdbRequest(socket, service, 150);
            bool valid = false;
            const int size = Read(socket, 4, 150).toInt(&valid, 16);
            if (!valid || size < 0 || size > 65535) throw std::runtime_error("Invalid adb listing length");
            return Read(socket, size, 150);
        };
        std::vector<CameraDescriptor> attached;
        try {
            const auto data = listing("host:track-devices-proto-binary");
            attached = ParseAdbUsbDevices(std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<size_t>(data.size())));
        } catch (...) {
          for (const auto& line : listing("host:devices-l").split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 3 || fields[1] != "device" || fields[0].startsWith("emulator-")) continue;
            bool usb = false;
            QByteArray model = fields[0];
            for (const auto& field : fields) {
                if (field.startsWith("usb:")) usb = true;
                if (field.startsWith("model:")) model = field.mid(6);
            }
            if (!usb || attached.size() >= 8) continue;
            CameraDescriptor descriptor;
            descriptor.phoneSerial = fields[0].toStdString();
            descriptor.symbolicLink = QString("okuflow-phone:%1").arg(QString::fromUtf8(fields[0])).toStdWString();
            descriptor.name = QString("OkuFlow Camera · %1").arg(QString::fromUtf8(model).replace('_', ' ')).toStdWString();
            attached.push_back(std::move(descriptor));
          }
        }
        for (const auto& descriptor : attached) {
            if (devices.size() >= 32) break;
            // Keep this identity even after lens discovery so a saved phone choice never falls back to a webcam.
            devices.push_back(descriptor);
            try {
                QTcpSocket capabilitiesSocket;
                Transport(capabilitiesSocket, descriptor.phoneSerial, "localabstract:okuflow_camera", nullptr, 150, descriptor.phoneTransportId);
                const auto capabilities = ControlHello(capabilitiesSocket, 150);
                devices.back().phoneAppReady = true;
                const auto cameras = capabilities["cameras"].toArray();
                if (!cameras.empty()) {
                    for (const auto& camera : cameras) {
                        const auto id = camera.toObject()["id"].toString();
                        if (id.isEmpty() || id.size() > 128) continue;
                        auto lens = descriptor;
                        lens.phoneAppReady = true;
                        lens.phoneCameraId = id.toStdString();
                        lens.symbolicLink += L"/" + id.toStdWString();
                        lens.name += L" · " + id.toStdWString();
                        devices.push_back(std::move(lens));
                    }
                    if (!devices.empty() && devices.back().phoneSerial == descriptor.phoneSerial) continue;
                }
            } catch (...) { /* App absent/busy: retain the saved-camera entry for later startup. */ }
        }
    } catch (...) { /* Optional companion discovery never makes webcam enumeration fail. */ }
    return devices;
}

PhoneCameraTransport::~PhoneCameraTransport() { Stop(); }
bool PhoneCameraTransport::Start(const CameraDescriptor& descriptor, IMFDXGIDeviceManager* manager,
                                 FrameCallback callback, CaptureErrorCallback errorCallback)
{
    ComPtr<IMFDXGIDeviceManager> retained = manager;
    worker_ = std::thread([this, descriptor, retained, callback = std::move(callback), errorCallback = std::move(errorCallback)] {
        Run(descriptor, retained, callback, errorCallback);
    });
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(12), [this] { return opened_ || failed_; })) {
        error_ = "Phone camera startup timed out. Open OkuFlow Camera and authorize USB debugging.";
        canceled_.store(true);
        return false;
    }
    return opened_ && !failed_;
}
void PhoneCameraTransport::Cancel() { canceled_.store(true); }
void PhoneCameraTransport::Stop() { Cancel(); if (worker_.joinable()) worker_.join(); }
void PhoneCameraTransport::SetZoom(float ratio)
{
    if (!std::isfinite(ratio) || ratio <= 0) return;
    std::lock_guard lock(mutex_); pendingZoom_ = ratio;
}
void PhoneCameraTransport::SetCamera(const std::string& id)
{ if (id.size() <= 128) { std::lock_guard lock(mutex_); pendingCamera_ = id; } }
void PhoneCameraTransport::SetTorch(bool enabled)
{ std::lock_guard lock(mutex_); pendingTorch_ = enabled; }
VideoFormat PhoneCameraTransport::Format() const { std::lock_guard lock(mutex_); return format_; }
std::string PhoneCameraTransport::Error() const { std::lock_guard lock(mutex_); return error_; }

void PhoneCameraTransport::Run(CameraDescriptor descriptor, ComPtr<IMFDXGIDeviceManager> manager,
                               FrameCallback callback, CaptureErrorCallback errorCallback)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(apartment, "Initialize phone decoder COM");
        const LONGLONG sessionClock = MFGetSystemTime();
        LONGLONG lastDecodedClock = -1;
        std::uint64_t outputSequence = 0;
        const auto stream = [&](bool hevcSupported) {
            // New/retried connections never replay an earlier light-on request.
            { std::lock_guard lock(mutex_); pendingTorch_.reset(); }
            torchAvailable_.store(false);
            torchEnabled_.store(false);
            // Revalidate positive USB evidence immediately before selecting a transport.
            const auto attached = Enumerate();
            const auto selected = std::find_if(attached.begin(), attached.end(), [&](const auto& item) { return item.phoneSerial == descriptor.phoneSerial; });
            if (selected == attached.end())
                throw std::runtime_error("The selected phone is not connected and authorized over USB");
            descriptor.phoneTransportId = selected->phoneTransportId;
            {
                QTcpSocket launcher;
                Transport(launcher, descriptor.phoneSerial, "shell:am start -n com.okuflow.camera/.MainActivity", &canceled_, 3000, descriptor.phoneTransportId);
                QByteArray response;
                const auto deadline = Clock::now() + std::chrono::seconds(3);
                while (launcher.state() == QAbstractSocket::ConnectedState && Clock::now() < deadline) {
                    CheckCanceled(&canceled_);
                    launcher.waitForReadyRead(20);
                    response += launcher.readAll();
                    if (response.size() > kControlLimit) throw std::runtime_error("Oversized app launch response");
                }
                if (response.contains("Error") || response.contains("Exception")) throw std::runtime_error("Install OkuFlow Camera on the selected USB phone first");
            }
            QTcpSocket control, video;
            const auto connectionDeadline = Clock::now() + std::chrono::seconds(5);
            for (;;) {
                CheckCanceled(&canceled_);
                try { Transport(control, descriptor.phoneSerial, "localabstract:okuflow_camera", &canceled_, 3000, descriptor.phoneTransportId); break; }
                catch (...) {
                    control.abort();
                    if (Clock::now() >= connectionDeadline) throw std::runtime_error("Open OkuFlow Camera, allow camera access and authorize USB debugging");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            const auto capabilities = ControlHello(control, 3000, hevcSupported);
            Transport(video, descriptor.phoneSerial, "localabstract:okuflow_camera", &canceled_, 3000, descriptor.phoneTransportId);
            Hello(video, 2, {{"token", capabilities["token"]}});
            const auto bound = ReadPacket(video, kControlLimit, &canceled_);
            if (bound.type != 1 || bound.flags || Json(bound)["channel"].toString() != "video") throw std::runtime_error("Phone video binding failed");
            if (!descriptor.phoneCameraId.empty()) Command(control, {{"op", "camera"}, {"cameraId", QString::fromStdString(descriptor.phoneCameraId)}}, &canceled_);
            Command(control, {{"op", "torch"}, {"enabled", false}}, &canceled_);
            Command(control, {{"op", "start"}}, &canceled_);
            std::map<quint32, QJsonObject> modes;
            std::unique_ptr<PhoneVideoDecoder> decoder;
            PhoneVideoCodec decoderCodec = PhoneVideoCodec::Avc;
            bool generationDecoded = false;
            auto generationStarted = Clock::now();
            quint32 decoderGeneration = 0;
            qint64 previousSequence = -1, firstTimestamp = -1, previousTimestamp = -1;
            auto lastHeartbeat = Clock::now();
            auto lastFrame = Clock::now();
            LONGLONG generationClock = 0;
            UINT fps = 30;
            std::optional<Packet> pending;
            while (!canceled_.load()) {
                if (decoder && decoderCodec == PhoneVideoCodec::Hevc && !generationDecoded &&
                    Clock::now() - generationStarted > std::chrono::seconds(4))
                    throw HevcDecoderError("HEVC decoder did not produce a frame within four seconds");
                if (Clock::now() - lastFrame > std::chrono::seconds(12)) throw std::runtime_error("Phone camera stopped delivering frames");
                while (control.bytesAvailable() || control.waitForReadyRead(1)) {
                    const auto packet = ReadPacket(control, kControlLimit, &canceled_);
                    if (packet.flags || (packet.type != 3 && packet.type != 4 && packet.type != 5)) throw std::runtime_error("Invalid phone control message");
                    const auto json = Json(packet);
                    if (packet.type == 5) throw std::runtime_error(json["error"].toString().toStdString());
                    if (packet.type == 4) {
                        torchAvailable_.store(json["torchAvailable"].toBool(false));
                        torchEnabled_.store(json["torchEnabled"].toBool(false));
                    }
                    if (packet.type == 3 && json["event"].toString() == "configured") {
                        const UINT width = json["width"].toInt(), height = json["height"].toInt();
                        fps = json["fps"].toInt();
                        const auto mime = json["mime"].toString();
                        if (!width || !height || width > 4096 || height > 2304 || !fps || fps > 60 ||
                            (mime != "video/avc" && !(hevcSupported && mime == "video/hevc")))
                            throw std::runtime_error("Phone negotiated an unsupported stream format");
                        qInfo() << "Phone negotiated" << mime << width << "x" << height << "@" << fps << "FPS";
                        const quint32 generation = static_cast<quint32>(json["generation"].toInt());
                        modes[generation] = json;
                        while (modes.size() > 4) modes.erase(modes.begin());
                        {
                            std::lock_guard lock(mutex_);
                            format_ = {MFVideoFormat_NV12, width, height, fps, 1, L"okuflow-phone-auto"};
                        }
                    }
                }
                if (Clock::now() - lastHeartbeat > std::chrono::seconds(5)) {
                    Command(control, {{"op", "ping"}}, &canceled_);
                    lastHeartbeat = Clock::now();
                }
                float zoom = -1;
                std::string camera;
                std::optional<bool> torch;
                { std::lock_guard lock(mutex_); std::swap(zoom, pendingZoom_); camera.swap(pendingCamera_); torch.swap(pendingTorch_); }
                if (zoom > 0) Command(control, {{"op", "zoom"}, {"ratio", zoom}}, &canceled_);
                if (!camera.empty()) Command(control, {{"op", "camera"}, {"cameraId", QString::fromStdString(camera)}}, &canceled_);
                if (torch.has_value()) Command(control, {{"op", "torch"}, {"enabled", *torch}}, &canceled_);
                if (control.state() != QAbstractSocket::ConnectedState || video.state() != QAbstractSocket::ConnectedState)
                    throw std::runtime_error("Phone USB connection lost");
                if (!pending && !video.bytesAvailable() && !video.waitForReadyRead(10)) {
                    if (Clock::now() - lastFrame > std::chrono::seconds(12)) throw std::runtime_error("Phone camera stopped delivering frames");
                    continue;
                }
                // Configure is sent before codec output; allow the independent control channel to catch up.
                if (modes.empty()) continue;
                auto packet = pending ? std::move(*pending) : ReadPacket(video, kVideoLimit, &canceled_);
                pending.reset();
                if (packet.type != 6 || packet.flags & ~3 || packet.data.size() <= 20) throw std::runtime_error("Invalid phone video packet");
                const qint64 timestamp = qFromLittleEndian<qint64>(packet.data.constData());
                const qint64 sequence = qFromLittleEndian<qint64>(packet.data.constData() + 8);
                const quint32 generation = qFromLittleEndian<quint32>(packet.data.constData() + 16);
                if (!modes.count(generation)) { pending = std::move(packet); continue; }
                if (!decoder || generation != decoderGeneration) {
                    const auto mode = modes.at(generation);
                    fps = mode["fps"].toInt();
                    decoderCodec = mode["mime"].toString() == "video/hevc" ? PhoneVideoCodec::Hevc : PhoneVideoCodec::Avc;
                    try {
                        decoder = std::make_unique<PhoneVideoDecoder>(mode["width"].toInt(), mode["height"].toInt(),
                            fps, decoderCodec, manager.Get());
                    } catch (const std::exception& exception) {
                        if (decoderCodec == PhoneVideoCodec::Hevc) throw HevcDecoderError(exception.what());
                        throw;
                    }
                    generationStarted = Clock::now();
                    generationDecoded = false;
                    decoderGeneration = generation;
                    previousSequence = -1; firstTimestamp = previousTimestamp = -1;
                    if (lastDecodedClock == std::numeric_limits<LONGLONG>::max()) throw std::runtime_error("Phone timestamp overflow");
                    generationClock = std::max(lastDecodedClock + 1, MFGetSystemTime() - sessionClock);
                    if (generationClock < 0) throw std::runtime_error("Invalid phone session clock");
                }
                if (sequence != previousSequence + 1) throw std::runtime_error("Missing or reordered phone packet");
                previousSequence = sequence;
                const auto data = packet.data.mid(20);
                if (!data.startsWith(QByteArray("\0\0\1", 3)) && !data.startsWith(QByteArray("\0\0\0\1", 4))) throw std::runtime_error("Phone video is not Annex B");
                if (!(packet.flags & 2)) {
                    if (timestamp < 0 || timestamp <= previousTimestamp) throw std::runtime_error("Invalid phone frame timestamp");
                    previousTimestamp = timestamp;
                    if (firstTimestamp < 0) firstTimestamp = timestamp;
                    if (timestamp - firstTimestamp > (std::numeric_limits<LONGLONG>::max() - generationClock) / 10) throw std::runtime_error("Phone timestamp overflow");
                }
                const auto normalized = (packet.flags & 2) ? generationClock : generationClock + (timestamp - firstTimestamp) * 10;
                try {
                    decoder->Decode(std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<size_t>(data.size())),
                    normalized, [&](MediaFrame&& frame) {
                        generationDecoded = true;
                        lastDecodedClock = frame.captureTimestamp100ns;
                        frame.sequenceNumber = outputSequence++;
                        { std::lock_guard lock(mutex_); opened_ = true; }
                        ready_.notify_all();
                        if (callback) callback(std::move(frame));
                    });
                } catch (const std::exception& exception) {
                    if (decoderCodec == PhoneVideoCodec::Hevc) throw HevcDecoderError(exception.what());
                    throw;
                }
                lastFrame = Clock::now();
            }
        };
        bool hevcSupported = PhoneVideoDecoder::IsSupported(PhoneVideoCodec::Hevc, manager.Get());
        for (;;) {
            try { stream(hevcSupported); break; }
            catch (const HevcDecoderError& exception) {
                if (!hevcSupported || canceled_.load()) throw;
                hevcSupported = false;
                qWarning() << "Phone HEVC decode failed; reconnecting once with H.264:" << exception.what();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
    } catch (const std::exception& exception) {
        {
            std::lock_guard lock(mutex_);
            failed_ = true;
            error_ = exception.what();
        }
        ready_.notify_all();
        if (!canceled_.load() && errorCallback) { try { errorCallback(exception.what()); } catch (...) {} }
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}
} // namespace okuflow
#endif
