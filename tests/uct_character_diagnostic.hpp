#pragma once

// Explicit, finite verifier baseline. This measures the existing matcher; a
// successful diagnostic is NOT a passing character-safety gate for OCR reuse.
#include "ultra_clear_text_test_support.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <QtTest>
#include <array>
#include <functional>

namespace uct_character_test {
using namespace okuflow::uct;

inline void Run(const std::function<ProbeFrame(const QImage&, int)>& prepare, bool resident)
{
    if (qEnvironmentVariable("OKUFLOW_UCT_CHARACTER_DIAGNOSTIC") != "1")
        QSKIP("Explicit character-change diagnostic not selected.");
    uct_test::RegisterFixtureFonts();
    const QString fontPath = "C:/Windows/Fonts/consola.ttf";
    QFile fontFile(fontPath);
    QVERIFY2(fontFile.open(QIODevice::ReadOnly), "Pinned local fixture font unavailable.");
    const auto fontHash = QCryptographicHash::hash(fontFile.readAll(), QCryptographicHash::Sha256).toHex();
    const int fontId = QFontDatabase::addApplicationFont(fontPath);
    QVERIFY(fontId >= 0);
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    QVERIFY(!families.isEmpty());
    const std::array<std::pair<QString, QString>, 12> edits{{
        {"+1", "-1"}, {"-1", " 1"}, {"1.0", "10 "}, {"0.1", "0,1"},
        {"<=", ">="}, {"==", "!="}, {"O", "0"}, {"l", "I"},
        {"1", "7"}, {"8", "9"}, {"rn", "m "}, {"c", "e"}}};
    const std::array<MatchTuning, 3> policies{{{}, kInputRegistrationTuning, {.94, .95, 2}}};
    const std::array<const char*, 3> policyNames{{"tracking", "exact-current", "paused-word"}};
    QJsonArray rows;
    std::array<int, 6> falseAccepts{}, falseHides{}, changedCounts{}, unchangedCounts{};
    int invalidReferences = 0;
    for (int edit = 0; edit < int(edits.size()); ++edit) {
        for (const int pixelSize : {14, 24, 44}) {
            QFont font(families.front()); font.setPixelSize(pixelSize);
            font.setStyleStrategy(QFont::PreferAntialias);
            const QFontMetricsF metrics(font);
            const QString prefix = "Reading ", suffix = " remains unchanged.";
            const qreal left = 32, top = 32, tokenX = left + metrics.horizontalAdvance(prefix);
            const qreal tokenWidth = std::max(metrics.horizontalAdvance(edits[edit].first),
                                               metrics.horizontalAdvance(edits[edit].second));
            const qreal suffixX = tokenX + tokenWidth + metrics.horizontalAdvance(' ');
            const auto render = [&](const QString& token, QPoint jitter) {
                QImage image(960, 128, QImage::Format_ARGB32); image.fill(QColor(246, 244, 236));
                QPainter painter(&image); painter.setFont(font); painter.setPen(QColor(18, 18, 22));
                painter.setRenderHint(QPainter::TextAntialiasing);
                painter.translate(jitter);
                const qreal baseline = top + metrics.ascent();
                painter.drawText(QPointF(left, baseline), prefix);
                painter.drawText(QPointF(tokenX, baseline), token);
                painter.drawText(QPointF(suffixX, baseline), suffix);
                painter.end(); return image;
            };
            const auto before = render(edits[edit].first, {});
            const auto normalized = [](const QRectF& box) {
                return QRectF(box.x()/960, box.y()/128, box.width()/960, box.height()/128);
            };
            const QVector<QRectF> boxes{
                normalized({left, top, suffixX + metrics.horizontalAdvance(suffix) - left, metrics.height()}),
                normalized({tokenX, top, tokenWidth, metrics.height()})};
            for (const int decimation : {1, 2, 4}) {
                const auto reference = prepare(before, decimation);
                QVERIFY(reference.IsValid());
                QCOMPARE(reference.Width(), before.width()/decimation);
                QCOMPARE(reference.Height(), before.height()/decimation);
                QVector<ReferencePatch> patches;
                if (resident) {
                    QVERIFY(reference.resident);
                    QVERIFY(reference.luma.isNull());
                    patches = reference.resident->References(reference.stamp, boxes);
                } else for (const auto& box : boxes) patches.push_back(BuildReferencePatch(reference, box));
                QCOMPARE(patches.size(), 2);
                for (const auto& patch : patches) {
                    invalidReferences += !patch.valid;
                    if (resident) QVERIFY(patch.values.isEmpty());
                }
                for (int phase = 0; phase < 4; ++phase) {
                    const bool changed = phase >= 2;
                    const QPoint jitter = phase % 2 ? QPoint(2, 1) : QPoint();
                    const auto current = render(changed ? edits[edit].second : edits[edit].first, jitter);
                    const auto probe = prepare(current, decimation);
                    QVERIFY(probe.IsValid());
                    QCOMPARE(probe.Width(), current.width()/decimation);
                    QCOMPARE(probe.Height(), current.height()/decimation);
                    if (resident) { QVERIFY(probe.resident); QVERIFY(probe.luma.isNull()); }
                    const auto digest = QString::fromLatin1(uct_test::SourcePixelsDigest(current).toHex());
                    for (int policy = 0; policy < int(policies.size()); ++policy) {
                        QVector<PatchMatch> matches;
                        if (resident) matches = probe.resident->Matches(probe.stamp, patches, policies[policy], {});
                        else for (const auto& patch : patches) matches.push_back(MatchPatch(patch, probe, policies[policy]));
                        QCOMPARE(matches.size(), 2);
                        for (int extent = 0; extent < 2; ++extent) {
                            const auto& match = matches[extent]; const int group = policy*2+extent;
                            if (changed) { ++changedCounts[group]; falseAccepts[group] += match.matched; }
                            else { ++unchangedCounts[group]; falseHides[group] += !match.matched; }
                            rows.push_back(QJsonObject{{"edit", edit}, {"before", edits[edit].first},
                                {"after", changed ? edits[edit].second : edits[edit].first}, {"changed", changed},
                                {"pixelSize", pixelSize}, {"decimation", decimation},
                                {"probeWidth", probe.Width()}, {"probeHeight", probe.Height()},
                                {"jitterPx", QJsonArray{jitter.x(), jitter.y()}},
                                {"extent", extent ? "edited-token" : "whole-line"}, {"policy", policyNames[policy]},
                                {"referenceValid", patches[extent].valid}, {"visible", match.visible},
                                {"matched", match.matched}, {"score", match.score},
                                {"offset", QJsonArray{match.sceneOffset.x(), match.sceneOffset.y()}},
                                {"currentActiveRowSha256", digest}});
                        }
                    }
                }
            }
        }
    }
    QJsonArray totals;
    for (int policy = 0; policy < 3; ++policy) for (int extent = 0; extent < 2; ++extent) {
        const int group = policy*2+extent;
        totals.push_back(QJsonObject{{"policy", policyNames[policy]},
            {"extent", extent ? "edited-token" : "whole-line"},
            {"changed", changedCounts[group]}, {"unchanged", unchangedCounts[group]},
            {"falseAccepts", falseAccepts[group]}, {"falseHides", falseHides[group]}});
    }
    const auto outputDir = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/uct-character-" +
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(outputDir));
    const QJsonObject report{{"resident", resident}, {"fontPath", fontPath},
        {"sourceWidth", 960}, {"sourceHeight", 128},
        {"fontSha256", QString::fromLatin1(fontHash)}, {"fontFamily", families.front()},
        {"jitterIsSyntheticRasterTranslation", true}, {"physicalCameraAcceptance", false},
        {"notAnOcrAccuracyBenchmark", true}, {"regionReuseAuthorized", false},
        {"invalidReferenceCount", invalidReferences}, {"summary", totals}, {"rows", rows}};
    QSaveFile file(QDir(outputDir).filePath("character-baseline.json"));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.commit());
    qInfo().noquote() << "UCT_CHARACTER_BASELINE" << outputDir << QJsonDocument(QJsonObject{{"summary", totals}}).toJson(QJsonDocument::Compact);
}
} // namespace uct_character_test
