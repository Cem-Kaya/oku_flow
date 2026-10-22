#pragma once

#include "okuflow/common/uct_paddle.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace okuflow::uct::baseline8f02
{
std::vector<float> PaddleNormalize(const QImage&, QSize, bool);
std::vector<float> PaddleRectify(const QImage&, const PaddleQuad&, int, int);
}

namespace uct_cpu_sampling_test
{
using namespace okuflow::uct;

inline bool SameBytes(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

inline QByteArray Digest(const std::vector<float>& values)
{
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(values.data()),
        qsizetype(values.size() * sizeof(float))), QCryptographicHash::Sha256).toHex();
}

inline PaddleQuad Quad(double x, double y, double w, double h, double tilt)
{
    PaddleQuad quad;
    quad.points = {QPointF(x, y), QPointF(x + w, y + tilt),
                   QPointF(x + w + 3, y + h + tilt), QPointF(x - 2, y + h)};
    quad.score = .9f;
    return quad;
}

inline void ExactRegression()
{
    // External storage supplies deliberately padded/noncontiguous source rows.
    for (const QSize size : {QSize(1, 1), QSize(2, 3), QSize(17, 9), QSize(57, 31)}) {
        const int stride = size.width() * 4 + 28;
        QByteArray storage(stride * size.height(), char(0xcd));
        QImage image(reinterpret_cast<uchar*>(storage.data()), size.width(), size.height(),
                     stride, QImage::Format_ARGB32);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x)
                image.setPixel(x, y, qRgba((x * 73 + y * 19) % 256,
                    (x * 31 + y * 97) % 256, (x * 11 + y * 43) % 256, (x * 17 + y * 13) % 256));
        for (const QImage::Format format : {QImage::Format_ARGB32, QImage::Format_RGB888,
                                          QImage::Format_Grayscale8, QImage::Format_ARGB32_Premultiplied}) {
            const QImage source = format == image.format() ? image : image.convertToFormat(format);
            for (const QSize target : {QSize(1, 1), QSize(31, 19), QSize(128, 48)})
                for (bool detection : {false, true})
                    QVERIFY2(SameBytes(PaddleNormalize(source, target, detection),
                        baseline8f02::PaddleNormalize(source, target, detection)), "Normalization tensor differs from 8f02");
            for (double tilt : {-17., 0., 13.})
                for (int width : {32, 128, 512, 1024, 2048})
                    for (int valid : {1, 13, width, width + 17}) {
                        const auto quad = Quad(-5, -3, size.width() + 11, size.height() + 7, tilt);
                        QVERIFY2(SameBytes(PaddleRectify(source, quad, width, valid),
                            baseline8f02::PaddleRectify(source, quad, width, valid)), "Rectified tensor differs from 8f02");
                    }
        }
    }
    QImage source(257, 129, QImage::Format_ARGB32);
    source.fill(QColor(7, 81, 237));
    for (const QSize target : {QSize(480, 256), QSize(704, 384), QSize(960, 544)})
        for (bool detection : {false, true})
            QVERIFY(SameBytes(PaddleNormalize(source, target, detection),
                             baseline8f02::PaddleNormalize(source, target, detection)));
    for (const QSize invalid : {QSize(), QSize(4097, 1), QSize(1, 4097)}) {
        QVERIFY_EXCEPTION_THROWN(PaddleNormalize(source, invalid, true), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(baseline8f02::PaddleNormalize(source, invalid, true), std::runtime_error);
    }
    QVERIFY_EXCEPTION_THROWN(PaddleNormalize({}, QSize(32, 32), true), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(baseline8f02::PaddleNormalize({}, QSize(32, 32), true), std::runtime_error);
    for (int width : {0, 31, 2049}) {
        QVERIFY(PaddleRectify(source, Quad(0, 0, 20, 10, 0), width, 1).empty());
        QVERIFY(baseline8f02::PaddleRectify(source, Quad(0, 0, 20, 10, 0), width, 1).empty());
    }
    QVERIFY(PaddleRectify(source, Quad(0, 0, 20, 10, 0), 1024, 0).empty());
    QVERIFY(baseline8f02::PaddleRectify(source, Quad(0, 0, 20, 10, 0), 1024, 0).empty());
    QVERIFY(PaddleRectify({}, Quad(0, 0, 20, 10, 0), 1024, 1).empty());
    QVERIFY(baseline8f02::PaddleRectify({}, Quad(0, 0, 20, 10, 0), 1024, 1).empty());
}

inline void Benchmark()
{
    const auto frameDirectory = qEnvironmentVariable("OKUFLOW_UCT_SAMPLING_FRAMES");
    const auto output = qEnvironmentVariable("OKUFLOW_UCT_SAMPLING_REPORT");
    if (frameDirectory.isEmpty() || output.isEmpty())
        QSKIP("Explicit CPU sampling diagnostic paths not supplied; no inference or GPU work.");
    QJsonObject report{{"schema", 1}, {"baseline", "8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d"},
        {"clock", "QElapsedTimer CPU wall nanoseconds"}, {"inference", false},
        {"gpu", false}, {"warmups_per_variant", 5}, {"abba_blocks", 31},
        {"timed_scope", "call, input format conversion, coordinate planning, output allocation and sampling; excludes correctness/hashing"}};
    QJsonArray cases;
    // Persist each completed workload, so a later failure retains earlier raw data.
    const auto save = [&] {
        report["cases"] = cases;
        QFile file(output);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        return file.write(bytes) == bytes.size();
    };
    for (const QString& name : {QStringLiteral("menu_7000.png"), QStringLiteral("menu_7033.png")}) {
        const auto path = QDir(frameDirectory).filePath(name);
        QFile input(path);
        QVERIFY2(input.open(QIODevice::ReadOnly), qPrintable(path));
        const auto inputHash = QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex();
        const QImage image(path);
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(1280, 720));
        for (int workload = 0; workload < 9; ++workload) {
            const bool normalize = workload < 3;
            const QSize target = normalize ? QVector<QSize>{{480, 256}, {704, 384}, {960, 544}}[workload] : QSize();
            const int crops = normalize ? 0 : QVector<int>{1, 10, 128, 1, 10, 128}[workload - 3];
            const bool region = workload >= 6;
            const QImage source = region ? image.copy(QRect(250, 130, 703, 401)) : image;
            QVector<PaddleQuad> quads;
            QJsonArray geometry;
            for (int i = 0; i < crops; ++i) {
                const int x = (i * 79) % (source.width() - 200) - (i % 11 == 0 ? 9 : 0);
                const int y = (i * 43) % (source.height() - 65);
                quads.push_back(Quad(x, y, 180 + i % 9, 26 + i % 7, (i % 3 - 1) * 13));
                QJsonArray points;
                for (const auto& p : quads.back().points) points.push_back(QJsonArray{p.x(), p.y()});
                geometry.push_back(points);
            }
            const auto run = [&](bool candidate) {
                if (normalize) return candidate ? PaddleNormalize(source, target, true)
                                               : baseline8f02::PaddleNormalize(source, target, true);
                std::vector<float> result;
                result.reserve(std::size_t(crops) * 1024 * 48 * 3);
                for (int i = 0; i < crops; ++i) {
                    // Fixed1024 is the baseline canvas; varying useful width retains padding work.
                    const int valid = 160 + (i * 47) % 865;
                    auto crop = candidate ? PaddleRectify(source, quads[i], 1024, valid)
                                          : baseline8f02::PaddleRectify(source, quads[i], 1024, valid);
                    result.insert(result.end(), crop.begin(), crop.end());
                }
                return result;
            };
            QElapsedTimer timer;
            timer.start();
            const auto expected = run(false);
            const qint64 firstBaseline = timer.nsecsElapsed();
            timer.restart();
            auto first = run(true);
            const qint64 firstCandidate = timer.nsecsElapsed();
            QVERIFY2(SameBytes(expected, first), "Full benchmark tensor byte mismatch");
            first.clear(); first.shrink_to_fit();
            for (int warm = 0; warm < 5; ++warm)
                for (bool candidate : {false, true}) QVERIFY(SameBytes(expected, run(candidate)));
            QJsonArray raw, aTimes, bTimes;
            for (int block = 0; block < 31; ++block)
                for (int position = 0; position < 4; ++position) {
                    const bool candidate = (position == 1 || position == 2) != (block % 2 != 0);
                    timer.restart();
                    const auto actual = run(candidate);
                    const auto elapsed = timer.nsecsElapsed();
                    QVERIFY2(SameBytes(expected, actual), "Raw timed execution tensor byte mismatch");
                    raw.push_back(QJsonObject{{"block", block}, {"position", position},
                        {"candidate", candidate}, {"wall_ns", double(elapsed)}});
                    (candidate ? bTimes : aTimes).push_back(double(elapsed));
                }
            const auto quantile = [](QJsonArray array, double q) {
                QVector<double> sorted;
                for (auto value : array) sorted.push_back(value.toDouble());
                std::sort(sorted.begin(), sorted.end());
                return sorted[std::max(0, int(std::ceil(q * sorted.size())) - 1)] / 1e6;
            };
            QJsonObject item{{"frame", name}, {"frame_sha256", QString::fromLatin1(inputHash)},
                {"source_width", source.width()}, {"source_height", source.height()},
                {"source_format", int(source.format())}, {"source_stride", source.bytesPerLine()},
                {"operation", normalize ? "normalize" : "rectify"}, {"region", region},
                {"target_width", normalize ? target.width() : 1024},
                {"target_height", normalize ? target.height() : 48}, {"synthetic_crop_count", crops},
                {"ocr_word_count", QJsonValue::Null}, {"quads", geometry},
                {"tensor_bytes", double(expected.size() * sizeof(float))},
                {"tensor_sha256", QString::fromLatin1(Digest(expected))},
                {"all_tensor_bytes_equal", true}, {"raw", raw},
                {"first_baseline_ms", firstBaseline / 1e6}, {"first_candidate_ms", firstCandidate / 1e6},
                {"baseline_p50_ms", quantile(aTimes, .5)}, {"baseline_p95_ms", quantile(aTimes, .95)},
                {"candidate_p50_ms", quantile(bTimes, .5)}, {"candidate_p95_ms", quantile(bTimes, .95)}};
            cases.push_back(item);
            QVERIFY2(save(), "Cannot persist sampling benchmark raw report");
            qInfo().noquote() << name << workload << "p50/p95 ms" << quantile(aTimes, .5) << quantile(aTimes, .95)
                << "->" << quantile(bTimes, .5) << quantile(bTimes, .95);
        }
    }
    report["complete"] = true;
    QVERIFY(save());
}
} // namespace uct_cpu_sampling_test
