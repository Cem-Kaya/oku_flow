#pragma once
#include "okuflow/common/uct_ink_outline.hpp"

#if OKUFLOW_ENABLE_CUDA
namespace uct_ink_outline_test
{
inline void Parity()
{
    int devices{};
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices < 1)
        QSKIP("CUDA device unavailable.");
    using namespace okuflow::uct;
    for (int variant = 0; variant < 4; ++variant)
    {
        QImage image(192, 128, QImage::Format_ARGB32);
        const bool inverted = variant == 1;
        image.fill(inverted ? QColor(220, 220, 220) : QColor(40, 40, 40));
        const QColor color = inverted ? QColor(40, 40, 40) : QColor(220, 220, 220);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 5));
        painter.drawLine(QPointF(35.25, 35.5), QPointF(35.25, 50.5));
        painter.drawLine(QPointF(28.5, 62.25), QPointF(55.5, 62.25));
        painter.drawLine(QPointF(35.25, 75.5), QPointF(50.25, 85.5));
        painter.drawEllipse(QRectF(90.25, 65.5, 22, 20));
        painter.fillRect(QRect(130, 35, 10, 8), color);
        if (variant == 2)
            painter.fillRect(QRect(16, 50, 20, 8), color);
        painter.end();
        ink::Region region{16,
                           16,
                           160,
                           96,
                           {color.red(), color.green(), color.blue()},
                           {inverted ? 220 : 40, inverted ? 220 : 40, inverted ? 220 : 40}};
        if (variant == 3)
            region.foreground[0] = region.foreground[1] = region.foreground[2] = 45;
        const auto cpu = ink::Build(image, region);
        const std::size_t pitch = image.width() * 4 + 128, cells = (region.w - 1) * (region.h - 1);
        GpuBuffer<unsigned char> pixels(pitch * image.height()), coverage(region.w * region.h);
        GpuBuffer<int> prefixes(ink::Levels * cells), rows(ink::Levels * (region.h - 1));
        GpuBuffer<ink::Summary> summary(1);
        GpuBuffer<ink::Segment> segments(ink::Levels * ink::MaximumSegments);
        FixtureStream stream;
        QVERIFY(cudaMemcpy2DAsync(pixels.data, pitch, image.constBits(), image.bytesPerLine(),
                                  image.width() * 4, image.height(), cudaMemcpyHostToDevice,
                                  stream.value) == cudaSuccess);
        gpu::InkOutline(pixels.data, pitch, image.width(), image.height(), region, coverage.data,
                        prefixes.data, rows.data, summary.data, segments.data, stream.value);
        ink::Summary received{};
        QVERIFY(cudaMemcpyAsync(&received, summary.data, sizeof(received), cudaMemcpyDeviceToHost,
                                stream.value) == cudaSuccess);
        QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);
        ink::Segments data;
        if (!received.invalid && !received.border)
            for (int k = 0; k < ink::Levels; ++k)
            {
                QVERIFY(received.counts[k] >= 0 && received.counts[k] <= ink::MaximumSegments);
                data[k].resize(received.counts[k]);
                if (!data[k].empty())
                    QVERIFY(cudaMemcpyAsync(data[k].data(), segments.data + k * ink::MaximumSegments,
                                            data[k].size() * sizeof(ink::Segment), cudaMemcpyDeviceToHost,
                                            stream.value) == cudaSuccess);
            }
        QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);
        const auto device = ink::Assemble(region, received, data);
        QCOMPARE(device.refusal, cpu.refusal);
        QCOMPARE(device.components, cpu.components);
        QCOMPARE(device.holes, cpu.holes);
        QCOMPARE(device.vertices, cpu.vertices);
        QCOMPARE(bool(device.geometry), bool(cpu.geometry));
        if (cpu.geometry)
        {
            QCOMPARE(device.geometry->loops, cpu.geometry->loops);
            QCOMPARE(device.geometry->path, cpu.geometry->path);
        }
    }
}
} // namespace uct_ink_outline_test
#endif
