#pragma once
#include "okuflow/common/uct_ink_outline.hpp"
#include <QScopeGuard>

#if OKUFLOW_ENABLE_CUDA
namespace uct_ink_outline_test
{
inline void Parity()
{
    int devices{};
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices < 1)
        QSKIP("CUDA device unavailable.");
    using namespace okuflow::uct;
    for (int variant = 0; variant < 7; ++variant)
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
        if (variant == 4)
        {
            unsigned value = 19;
            for (int y = 24; y < 104; ++y)
                for (int x = 24; x < 168; ++x)
                {
                    value = value * 1664525u + 1013904223u;
                    const int shade = 65 + int(value >> 24) * 155 / 255;
                    image.setPixelColor(x, y, QColor(shade, shade, shade));
                }
        }
        if (variant == 5 || variant == 6)
        {
            image.fill(QColor(40, 40, 40));
            for (int y = 20; y < 110; y += variant == 5 ? 5 : 3)
                for (int x = 20; x < 172; x += variant == 5 ? 5 : 3)
                    image.setPixelColor(x, y, QColor(220, 220, 220));
        }
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
                QVERIFY(received.counts[k] >= 0);
                if (received.counts[k] > ink::MaximumSegments)
                {
                    data[k].resize(ink::MaximumSegments + 1);
                    continue;
                }
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
inline void ResidentHost()
{
    using namespace okuflow::uct;
    int devices{};
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices < 1)
        QSKIP("CUDA device unavailable.");
    const auto prior = qgetenv("OKUFLOW_UCT_INK_OUTLINE");
    const bool existed = qEnvironmentVariableIsSet("OKUFLOW_UCT_INK_OUTLINE");
    const auto restore = qScopeGuard([&] {
        if (existed)
            qputenv("OKUFLOW_UCT_INK_OUTLINE", prior);
        else
            qunsetenv("OKUFLOW_UCT_INK_OUTLINE");
    });
    qputenv("OKUFLOW_UCT_INK_OUTLINE", "1");
    QImage image(640, 128, QImage::Format_ARGB32);
    image.fill(QColor(40, 40, 40));
    QPainter painter(&image);
    for (const int x : {16, 336})
    {
        painter.fillRect(QRect(x + 20, 36, 80, 10), QColor(220, 220, 220));
        painter.fillRect(QRect(x + 90, 75, 10, 8), QColor(220, 220, 220));
    }
    painter.end();
    FixtureStream stream;
    GpuBuffer<unsigned char> pixels(image.sizeInBytes());
    QCOMPARE(cudaMemcpyAsync(pixels.data, image.constBits(), image.sizeInBytes(), cudaMemcpyHostToDevice,
                             stream.value),
             cudaSuccess);
    AnalysisPool pool;
    auto owned = pool.Capture(pixels.data, image.bytesPerLine(), image.width(), image.height(), stream.value,
                              7, 17, 3);
    QVERIFY(owned);
    std::atomic<bool> cancel{false};
    QString error;
    RecognitionResult result;
    result.stamp.sourceEpoch = 7;
    result.stamp.geometryGeneration = 3;
    result.stamp.frameSeq = 17;
    result.stamp.viewportWidth = 640;
    result.stamp.viewportHeight = 128;
    result.stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
    for (const QRectF bounds : {QRectF(16, 16, 160, 96), QRectF(336, 16, 160, 96), QRectF(0, 0, 640, 128)})
    {
        RecognizedWord word;
        word.boundsPx = word.protectedBoundsPx = bounds;
        word.disposition = TokenDisposition::kInvalidGeometry;
        word.colors = LineColors{QColor(220, 220, 220), QColor(40, 40, 40)};
        result.output.lines.push_back({{word}, {}});
    }
    const auto planned = PlanInkOutlines(result);
    QCOMPARE(planned.size(), 2);
    QVERIFY2(owned->PrepareResidentInkOutlines(result, cancel, error), qPrintable(error));
    std::uint64_t bytes{};
    for (int i = 0; i < 2; ++i)
    {
        QCOMPARE(result.output.lines[i].inkOutlines.size(), 1);
        const auto &outline = result.output.lines[i].inkOutlines.front();
        QVERIFY2(outline.geometry, ink::Name(outline.refusal));
        const auto cpu = ink::Build(image, planned[i].region);
        QVERIFY(cpu.geometry);
        QCOMPARE(outline.geometry->loops, cpu.geometry->loops);
        bytes += outline.transferBytes;
    }
    QVERIFY(result.output.lines[2].inkOutlines.isEmpty());
    QCOMPARE(result.inkOutlineBytes, bytes);
    QVERIFY(bytes > 0 && bytes < 4194656);
    auto stopped = result;
    for (auto &line : stopped.output.lines)
        line.inkOutlines.clear();
    cancel = true;
    QVERIFY(!owned->PrepareResidentInkOutlines(stopped, cancel, error));
    for (const auto &line : stopped.output.lines)
        QVERIFY(line.inkOutlines.isEmpty());
}
} // namespace uct_ink_outline_test
#endif
