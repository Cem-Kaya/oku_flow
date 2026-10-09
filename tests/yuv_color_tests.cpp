#include "okuflow/capture/capture_color.hpp"
#include "okuflow/common/frame_pipeline.hpp"
#include "okuflow/common/image_processing.hpp"
#include "yuv_color_reference.hpp"

#include <QtTest/QtTest>
#include <wrl/client.h>

class YuvColorTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(MFStartup(MF_VERSION))); }
    void cleanupTestCase() { QVERIFY(SUCCEEDED(MFShutdown())); }

    void metadataDefaultsAndExplicitValues()
    {
        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        QVERIFY(SUCCEEDED(MFCreateAttributes(attributes.GetAddressOf(), 2)));
        okuflow::YuvColorInfo color{okuflow::YuvMatrix::Bt709, okuflow::YuvRange::Full};
        QVERIFY(okuflow::ReadCaptureYuvColor(attributes.Get(), color));
        QVERIFY(color.matrix == okuflow::YuvMatrix::Bt601);
        QVERIFY(color.range == okuflow::YuvRange::Limited);
        for (bool bt709 : {false, true}) {
            for (bool full : {false, true}) {
                QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_YUV_MATRIX,
                    bt709 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601)));
                QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,
                    full ? MFNominalRange_0_255 : MFNominalRange_16_235)));
                QVERIFY(okuflow::ReadCaptureYuvColor(attributes.Get(), color));
                QCOMPARE(color.matrix == okuflow::YuvMatrix::Bt709, bt709);
                QCOMPARE(color.range == okuflow::YuvRange::Full, full);
                const auto d3d = okuflow::CaptureVideoColorSpace(color);
                QCOMPARE(UINT(d3d.YCbCr_Matrix), bt709 ? 1u : 0u);
                QCOMPARE(UINT(d3d.Nominal_Range), UINT(full
                    ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                    : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235));
                QCOMPARE(UINT(d3d.RGB_Range), 0u);
            }
        }
        QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown)));
        QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown)));
        QVERIFY(okuflow::ReadCaptureYuvColor(attributes.Get(), color));
        QVERIFY(color.matrix == okuflow::YuvMatrix::Bt601);
        QVERIFY(color.range == okuflow::YuvRange::Limited);
        QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_YUV_MATRIX, 999)));
        QVERIFY(!okuflow::ReadCaptureYuvColor(attributes.Get(), color));
        QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601)));
        QVERIFY(SUCCEEDED(attributes->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_48_208)));
        QVERIFY(!okuflow::ReadCaptureYuvColor(attributes.Get(), color));
    }

    void bothFormatsMatchReferenceAcrossRangeBoundaries()
    {
        for (bool bt709 : {false, true}) {
            for (bool full : {false, true}) {
                const okuflow::YuvColorInfo color{
                    bt709 ? okuflow::YuvMatrix::Bt709 : okuflow::YuvMatrix::Bt601,
                    full ? okuflow::YuvRange::Full : okuflow::YuvRange::Limited};
                for (int y : {0, 15, 16, 17, 64, 128, 234, 235, 236, 255}) {
                    for (int u : {0, 16, 90, 128, 240, 255}) {
                        for (int v : {0, 16, 90, 128, 240, 255}) {
                            const auto yy = static_cast<std::uint8_t>(y);
                            const auto uu = static_cast<std::uint8_t>(u);
                            const auto vv = static_cast<std::uint8_t>(v);
                            const std::uint8_t nv12[]{yy, yy, yy, yy, uu, vv};
                            const std::uint8_t yuy2[]{yy, uu, yy, vv};
                            std::vector<std::uint8_t> a, b;
                            QVERIFY(okuflow::processing::ConvertNv12ToBgra(nv12, 6, 2, 2, 2, a, color));
                            QVERIFY(okuflow::processing::ConvertYuy2ToBgra(yuy2, 4, 4, 2, 1, b, color));
                            const auto rgb = ReferenceYuvRgb(y, u, v, bt709, full);
                            for (int pixel = 0; pixel < 4; ++pixel) {
                                QVERIFY(std::abs(int(a[pixel * 4]) - rgb[2]) <= 1);
                                QVERIFY(std::abs(int(a[pixel * 4 + 1]) - rgb[1]) <= 1);
                                QVERIFY(std::abs(int(a[pixel * 4 + 2]) - rgb[0]) <= 1);
                                QCOMPARE(a[pixel * 4 + 3], std::uint8_t(255));
                            }
                            for (int i = 0; i < 8; ++i) QCOMPARE(a[i], b[i]);
                        }
                    }
                }
            }
        }
    }

    void taggedPipelineKeepsRangeAndMatrix()
    {
        // Quantized full-range BT.709 red. Treating it as 601 limited gives
        // a visibly different result, so this tests propagation, not parity.
        const std::vector<std::uint8_t> frame{54, 99, 54, 255};
        okuflow::processing::CpuFramePipeline pipeline;
        QVERIFY(pipeline.ConvertFrameToBgra(frame, MFVideoFormat_YUY2, 2, 1, 4, 4,
            {okuflow::YuvMatrix::Bt709, okuflow::YuvRange::Full}));
        const auto& pixels = pipeline.StageRaw();
        QVERIFY(pixels[0] <= 1);
        QVERIFY(pixels[1] <= 1);
        QVERIFY(pixels[2] >= 253);
        QCOMPARE(pixels[3], std::uint8_t(255));
    }
};

QTEST_APPLESS_MAIN(YuvColorTests)
#include "yuv_color_tests.moc"
