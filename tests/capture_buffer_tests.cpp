#include "okuflow/capture/capture_buffer.hpp"
#include "okuflow/capture/capture_texture.hpp"
#include "okuflow/capture/media_capture.hpp"

#include <QtTest/QtTest>
#include <wrl/client.h>
#include <climits>
#include <cstring>

using Microsoft::WRL::ComPtr;

class CaptureBufferTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(MFStartup(MF_VERSION))); }
    void cleanupTestCase() { QVERIFY(SUCCEEDED(MFShutdown())); }

    void captureBurstKeepsJitterAndCatchesUpOnOverflow()
    {
        std::deque<okuflow::MediaFrame> pending, retired;
        const auto enqueue = [&pending, &retired](std::uint64_t sequence, bool burst) {
            okuflow::MediaFrame frame;
            frame.sequenceNumber = sequence;
            return okuflow::EnqueueCaptureFrame(pending, std::move(frame), burst, 6, retired);
        };
        for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
            QCOMPARE(enqueue(sequence, true), std::size_t(0));
        }
        QCOMPARE(pending.size(), std::size_t(6));
        QCOMPARE(pending.front().sequenceNumber, std::uint64_t(1));
        QCOMPARE(enqueue(7, true), std::size_t(6));
        QCOMPARE(pending.size(), std::size_t(1));
        QCOMPARE(pending.front().sequenceNumber, std::uint64_t(7));
        QCOMPARE(retired.size(), std::size_t(6));
        QCOMPARE(retired.front().sequenceNumber, std::uint64_t(1));
        QCOMPARE(retired.back().sequenceNumber, std::uint64_t(6));
        QCOMPARE(enqueue(8, false), std::size_t(1));
        QCOMPARE(pending.front().sequenceNumber, std::uint64_t(8));
        QCOMPARE(retired.size(), std::size_t(7));
    }

    void bgraGpuCopyPreservesChannelsFromCroppedArrayMip()
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
            D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf())));

        D3D11_TEXTURE2D_DESC sourceDescription{};
        sourceDescription.Width = 8;
        sourceDescription.Height = 6;
        sourceDescription.MipLevels = 2;
        sourceDescription.ArraySize = 2;
        sourceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sourceDescription.SampleDesc.Count = 1;
        sourceDescription.Usage = D3D11_USAGE_DEFAULT;
        std::vector<BYTE> pixels[4];
        D3D11_SUBRESOURCE_DATA initial[4]{};
        for (UINT slice = 0; slice < 2; ++slice) {
            for (UINT mip = 0; mip < 2; ++mip) {
                const UINT subresource = D3D11CalcSubresource(mip, slice, 2);
                const UINT width = sourceDescription.Width >> mip;
                const UINT height = sourceDescription.Height >> mip;
                pixels[subresource].resize(width * height * 4);
                for (UINT y = 0; y < height; ++y) {
                    for (UINT x = 0; x < width; ++x) {
                        BYTE* pixel = pixels[subresource].data() + (y * width + x) * 4;
                        pixel[0] = static_cast<BYTE>(10 + subresource * 20 + x);
                        pixel[1] = static_cast<BYTE>(30 + y);
                        pixel[2] = static_cast<BYTE>(90 + x + y);
                        pixel[3] = static_cast<BYTE>(180 + subresource);
                    }
                }
                initial[subresource].pSysMem = pixels[subresource].data();
                initial[subresource].SysMemPitch = width * 4;
            }
        }
        ComPtr<ID3D11Texture2D> source;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&sourceDescription, initial, source.GetAddressOf())));
        D3D11_TEXTURE2D_DESC destinationDescription = sourceDescription;
        destinationDescription.Width = 3;
        destinationDescription.Height = 2;
        destinationDescription.ArraySize = 1;
        destinationDescription.MipLevels = 1;
        destinationDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        destinationDescription.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> destination;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&destinationDescription, nullptr, destination.GetAddressOf())));
        QVERIFY(okuflow::CopyBgraCaptureTexture(context.Get(), source.Get(),
            D3D11CalcSubresource(1, 1, 2), destination.Get(), 3, 2));

        auto stagingDescription = destinationDescription;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.MiscFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf())));
        context->CopyResource(staging.Get(), destination.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        QVERIFY(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        bool pixelsMatch = true;
        for (UINT y = 0; y < 2; ++y) {
            pixelsMatch = pixelsMatch && std::memcmp(
                static_cast<BYTE*>(mapped.pData) + y * mapped.RowPitch,
                pixels[3].data() + y * 4 * 4, 3 * 4) == 0;
        }
        context->Unmap(staging.Get(), 0);
        QVERIFY(pixelsMatch);

        QVERIFY(!okuflow::CopyBgraCaptureTexture(context.Get(), source.Get(), 4,
            destination.Get(), 3, 2)); // nonexistent array slice
        QVERIFY(!okuflow::CopyBgraCaptureTexture(context.Get(), source.Get(), 3,
            destination.Get(), 5, 2)); // outside source mip/destination extent
        sourceDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11Texture2D> rgbaSource;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&sourceDescription, initial, rgbaSource.GetAddressOf())));
        QVERIFY(!okuflow::CopyBgraCaptureTexture(context.Get(), rgbaSource.Get(), 3,
            destination.Get(), 3, 2)); // channel conversion must use the video processor
    }

    void paddedNv12PlacesChromaAfterPitchedLuma()
    {
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreateMemoryBuffer(24, buffer.GetAddressOf())));
        BYTE* data = nullptr;
        QVERIFY(SUCCEEDED(buffer->Lock(&data, nullptr, nullptr)));
        std::memset(data, 0xee, 24);
        const BYTE rows[3][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {91, 92, 93, 94}};
        for (int row = 0; row < 3; ++row) std::memcpy(data + row * 8, rows[row], 4);
        QVERIFY(SUCCEEDED(buffer->Unlock()));
        QVERIFY(SUCCEEDED(buffer->SetCurrentLength(20))); // final padding is unnecessary
        std::vector<std::uint8_t> result;
        LONG stride = 0;
        QVERIFY(okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_NV12,
                                             4, 2, 8, result, stride));
        QCOMPARE(stride, LONG(4));
        QCOMPARE(result.size(), std::size_t(12));
        QVERIFY(std::memcmp(result.data(), rows, sizeof(rows)) == 0);
        QVERIFY(SUCCEEDED(buffer->SetCurrentLength(19)));
        const auto previous = result;
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_NV12,
                                              4, 2, 8, result, stride));
        QVERIFY(result == previous);
    }

    void mappedGpuPackingUsesAllocationPlanesAndChannels_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("padded-nv12") << int(DXGI_FORMAT_NV12);
        QTest::newRow("rgba") << int(DXGI_FORMAT_R8G8B8A8_UNORM);
        QTest::newRow("bgra") << int(DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    void mappedGpuPackingUsesAllocationPlanesAndChannels()
    {
        QFETCH(int, format);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            device.GetAddressOf(), nullptr, context.GetAddressOf())));
        const bool nv12 = format == int(DXGI_FORMAT_NV12);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 8;
        description.Height = 8;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = static_cast<DXGI_FORMAT>(format);
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Texture2D> texture;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&description, nullptr, texture.GetAddressOf())));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        QVERIFY(SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)));
        auto* bytes = static_cast<BYTE*>(mapped.pData);
        for (UINT row = 0; row < (nv12 ? 12u : 8u); ++row) {
            std::memset(bytes + row * mapped.RowPitch, row >= 8 ? 91 : row < 4 ? 17 : 0xee, nv12 ? 8 : 32);
            if (!nv12) for (UINT x = 0; x < 8; ++x) {
                BYTE* pixel = bytes + row * mapped.RowPitch + x * 4;
                pixel[0] = 11; pixel[1] = 37; pixel[2] = 93; pixel[3] = 177;
            }
        }
        context->Unmap(texture.Get(), 0);
        QVERIFY(SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        std::vector<std::uint8_t> packed;
        LONG stride = 0;
        GUID subtype{};
        const bool copied = okuflow::CopyMappedCaptureTexture(mapped, description.Format,
            8, 8, 4, 4, packed, stride, subtype);
        context->Unmap(texture.Get(), 0);
        QVERIFY(copied);
        QCOMPARE(stride, LONG(nv12 ? 4 : 16));
        if (nv12) {
            QCOMPARE(packed.size(), std::size_t(24));
            for (int i = 0; i < 16; ++i) QCOMPARE(packed[i], std::uint8_t(17));
            for (int i = 16; i < 24; ++i) QCOMPARE(packed[i], std::uint8_t(91));
        } else {
            QCOMPARE(packed[0], std::uint8_t(format == int(DXGI_FORMAT_R8G8B8A8_UNORM) ? 93 : 11));
            QCOMPARE(packed[1], std::uint8_t(37));
            QCOMPARE(packed[2], std::uint8_t(format == int(DXGI_FORMAT_R8G8B8A8_UNORM) ? 11 : 93));
            QCOMPARE(packed[3], std::uint8_t(177));
        }
    }

    void bottomUpLinearRgbBecomesTopDown()
    {
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreateMemoryBuffer(24, buffer.GetAddressOf())));
        BYTE* data = nullptr;
        QVERIFY(SUCCEEDED(buffer->Lock(&data, nullptr, nullptr)));
        std::memset(data, 0xee, 24);
        std::memset(data, 22, 8); // bottom row is first in memory
        std::memset(data + 12, 11, 8);
        QVERIFY(SUCCEEDED(buffer->Unlock()));
        QVERIFY(SUCCEEDED(buffer->SetCurrentLength(20)));
        std::vector<std::uint8_t> result;
        LONG stride = 0;
        QVERIFY(okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_RGB32,
                                             2, 2, -12, result, stride));
        QCOMPARE(stride, LONG(8));
        QCOMPARE(result.size(), std::size_t(16));
        for (int x = 0; x < 8; ++x) {
            QCOMPARE(result[x], std::uint8_t(11));
            QCOMPARE(result[8 + x], std::uint8_t(22));
        }
    }

    void paddedYuy2PreservesPackedPixelPairs()
    {
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreateMemoryBuffer(24, buffer.GetAddressOf())));
        BYTE* data = nullptr;
        QVERIFY(SUCCEEDED(buffer->Lock(&data, nullptr, nullptr)));
        const BYTE rows[2][8] = {{16, 128, 32, 130, 64, 90, 80, 100},
                                 {100, 80, 120, 90, 140, 130, 160, 128}};
        std::memset(data, 0xee, 24);
        std::memcpy(data, rows[0], 8);
        std::memcpy(data + 12, rows[1], 8);
        QVERIFY(SUCCEEDED(buffer->Unlock()));
        QVERIFY(SUCCEEDED(buffer->SetCurrentLength(20)));
        std::vector<std::uint8_t> result;
        LONG stride = 0;
        QVERIFY(okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_YUY2,
                                             4, 2, 12, result, stride));
        QCOMPARE(stride, LONG(8));
        QCOMPARE(result.size(), std::size_t(16));
        QVERIFY(std::memcmp(result.data(), rows, sizeof(rows)) == 0);
    }

    void native2dPitchOverridesNegotiatedStride_data()
    {
        QTest::addColumn<bool>("bottomUp");
        QTest::newRow("top-down") << false;
        QTest::newRow("bottom-up") << true;
    }

    void native2dPitchOverridesNegotiatedStride()
    {
        QFETCH(bool, bottomUp);
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreate2DMediaBuffer(6, 3, MFVideoFormat_RGB32.Data1,
                                               bottomUp, buffer.GetAddressOf())));
        ComPtr<IMF2DBuffer> surface;
        QVERIFY(SUCCEEDED(buffer.As(&surface)));
        BYTE* top = nullptr;
        LONG pitch = 0;
        QVERIFY(SUCCEEDED(surface->Lock2D(&top, &pitch)));
        for (int row = 0; row < 3; ++row) {
            std::memset(top + row * pitch, row + 1, 24);
        }
        QVERIFY(SUCCEEDED(surface->Unlock2D()));
        QVERIFY(bottomUp ? pitch < 0 : pitch > 0);
        std::vector<std::uint8_t> result;
        LONG stride = 0;
        // Deliberately wrong metadata must not override the actual 2D layout.
        QVERIFY(okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_RGB32,
                                             6, 3, 4, result, stride));
        QCOMPARE(stride, LONG(24));
        QCOMPARE(result.size(), std::size_t(72));
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 24; ++col) {
                QCOMPARE(result[row * 24 + col], std::uint8_t(row + 1));
            }
        }
        // The copy always releases its lock, including on validation failures.
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_RGB32,
                                              600, 3, 4, result, stride));
        QVERIFY(SUCCEEDED(surface->Lock2D(&top, &pitch)));
        QVERIFY(SUCCEEDED(surface->Unlock2D()));
    }

    void nativeNv12PitchIncludesBothPlanes()
    {
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreate2DMediaBuffer(6, 4, MFVideoFormat_NV12.Data1,
                                               FALSE, buffer.GetAddressOf())));
        ComPtr<IMF2DBuffer> surface;
        QVERIFY(SUCCEEDED(buffer.As(&surface)));
        BYTE* top = nullptr;
        LONG pitch = 0;
        QVERIFY(SUCCEEDED(surface->Lock2D(&top, &pitch)));
        for (int row = 0; row < 6; ++row) std::memset(top + row * pitch, row + 20, 6);
        QVERIFY(SUCCEEDED(surface->Unlock2D()));
        std::vector<std::uint8_t> result;
        LONG stride = 0;
        QVERIFY(okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_NV12,
                                             6, 4, 6, result, stride));
        QCOMPARE(stride, LONG(6));
        QCOMPARE(result.size(), std::size_t(36));
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col) {
                QCOMPARE(result[row * 6 + col], std::uint8_t(row + 20));
            }
        }
    }

    void invalidLayoutsAreRejected()
    {
        QVERIFY(!okuflow::IsSupportedCaptureLayout(MFVideoFormat_RGB24, 4, 2, 12));
        QVERIFY(!okuflow::IsSupportedCaptureLayout(MFVideoFormat_NV12, 4, 3, 4));
        QVERIFY(okuflow::IsSupportedCaptureLayout(MFVideoFormat_RGB32, 4, 2, -16));
        ComPtr<IMFMediaBuffer> buffer;
        QVERIFY(SUCCEEDED(MFCreateMemoryBuffer(64, buffer.GetAddressOf())));
        QVERIFY(SUCCEEDED(buffer->SetCurrentLength(64)));
        std::vector<std::uint8_t> result{77};
        LONG stride = 99;
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_NV12, 4, 2, -4, result, stride));
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_NV12, 4, 3, 4, result, stride));
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_YUY2, 3, 2, 8, result, stride));
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_RGB32, 4, 2, 8, result, stride));
        QVERIFY(!okuflow::CopyCaptureBuffer(buffer.Get(), MFVideoFormat_RGB32, UINT_MAX, UINT_MAX, 4, result, stride));
        QCOMPARE(stride, LONG(99));
        QCOMPARE(result.size(), std::size_t(1));
        QCOMPARE(result[0], std::uint8_t(77));
    }
};

QTEST_APPLESS_MAIN(CaptureBufferTests)
#include "capture_buffer_tests.moc"
