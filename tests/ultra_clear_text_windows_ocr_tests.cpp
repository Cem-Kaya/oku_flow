// Optional acceptance of the real built-in Windows.Media.Ocr backend through
// the production scheduler. Desktop-free: no windows, camera or network.
// Skips (never passes vacuously) when Windows has no OCR language installed.

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_paddle.hpp"
#include "ultra_clear_text_test_support.hpp"
#include "../src/common/uct_windows_geometry.hpp"

#include <QGuiApplication>
#include <QLineF>
#include <QPainter>
#include <QSet>
#include <QTransform>
#include <QtTest>

#include <mutex>
#include <limits>
#include <array>
#include <cmath>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

class UltraClearTextWindowsOcrTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsBoxesUseImageCentreAndActualResize();
    void recognizesTiltedPrintedPage_data();
    void recognizesTiltedPrintedPage();
    void recognizesSyntheticPrintedPage_data();
    void recognizesSyntheticPrintedPage();
};

void UltraClearTextWindowsOcrTests::windowsBoxesUseImageCentreAndActualResize()
{
    using namespace windows_ocr;
    const Word word{L"Head", 80, 30, 40, 20};
    const QSize sampled(200, 100);
    const auto ordinary = SourceWordQuad(word, sampled, sampled, std::nullopt);
    QVERIFY(ordinary.valid);
    QCOMPARE(ordinary.Bounds(), QRectF(80, 30, 40, 20));
    QCOMPARE(SourceWordQuad(word, sampled, sampled, 0.0), ordinary);
    const auto clockwise = SourceWordQuad(word, sampled, sampled, 90.0);
    QVERIFY(clockwise.valid);
    const std::array<QPointF, 4> expected{{{120, 30}, {120, 70}, {100, 70}, {100, 30}}};
    for (int i = 0; i < 4; ++i) QVERIFY(QLineF(clockwise.points[i], expected[i]).length() < 1e-9);
    const auto counterclockwise = SourceWordQuad(word, sampled, sampled, -90.0);
    QVERIFY(QLineF(counterclockwise.points[0], QPointF(80, 70)).length() < 1e-9);
    QVERIFY(QLineF(counterclockwise.points[2], QPointF(100, 30)).length() < 1e-9);
    const auto halfTurn = SourceWordQuad(word, sampled, sampled, 180.0);
    QVERIFY(QLineF(halfTurn.points[0], QPointF(120, 70)).length() < 1e-9);
    // A rounded downscale may have different X/Y ratios. Rotate around the
    // sampled centre before restoring those axes, never around the word.
    const auto resized = SourceWordQuad(word, sampled, QSize(401, 203), 90.0);
    QVERIFY(QLineF(resized.points[0], QPointF(240.6, 60.9)).length() < 1e-9);
    QVERIFY(QLineF(resized.points[2], QPointF(200.5, 142.1)).length() < 1e-9);
    QVERIFY(!SourceWordQuad(word, {}, sampled, 1.0).valid);
    QVERIFY(!SourceWordQuad(word, sampled, {}, 1.0).valid);
    QVERIFY(!SourceWordQuad(word, sampled, sampled, std::numeric_limits<double>::quiet_NaN()).valid);
    QVERIFY(!SourceWordQuad(word, sampled, sampled, std::numeric_limits<double>::infinity()).valid);
    Word malformed = word;
    malformed.width = -1;
    QVERIFY(!SourceWordQuad(malformed, sampled, sampled, 0.0).valid);
    malformed = word;
    malformed.x = std::numeric_limits<float>::infinity();
    QVERIFY(!SourceWordQuad(malformed, sampled, sampled, 0.0).valid);
}

void UltraClearTextWindowsOcrTests::recognizesTiltedPrintedPage_data()
{
    QTest::addColumn<double>("degrees");
    QTest::newRow("clockwise six degrees") << 6.0;
    QTest::newRow("counterclockwise six degrees") << -6.0;
    QTest::newRow("clockwise three degrees") << 3.0;
    QTest::newRow("counterclockwise three degrees") << -3.0;
}

void UltraClearTextWindowsOcrTests::recognizesTiltedPrintedPage()
{
    QFETCH(double, degrees);
    auto recognizer = CreateWindowsRecognizer();
    const auto status = recognizer->Initialize();
    if (status.state != BackendState::kReady)
        QSKIP(qPrintable(QStringLiteral("Windows.Media.Ocr unavailable: ") + status.detail));
    const auto page = MakePage({QStringLiteral("MAGNIFIED PRINTED TEXT"),
                                QStringLiteral("READS CLEARLY TODAY")}, {}, 1600, 900, 72);
    QImage tilted(page.image.size(), QImage::Format_ARGB32);
    tilted.fill(Qt::white);
    QTransform rotation;
    rotation.translate(800, 450);
    rotation.rotate(degrees);
    rotation.translate(-800, -450);
    QPainter painter(&tilted);
    painter.setTransform(rotation);
    painter.drawImage(QPoint{}, page.image);
    painter.end();
    std::atomic<bool> cancel{};
    const auto output = recognizer->Recognize(tilted, cancel);
    QVERIFY2(output.ok, qPrintable(output.error));
    int matched = 0;
    for (const auto& expectedWord : page.words) {
        const QRectF source(expectedWord.sceneRect.x() * 1600, expectedWord.sceneRect.y() * 900,
                            expectedWord.sceneRect.width() * 1600, expectedWord.sceneRect.height() * 900);
        const QPointF expectedCentre = rotation.map(source.center());
        for (const auto& line : output.lines) for (const auto& word : line.words) {
            if (word.text.compare(expectedWord.text, Qt::CaseInsensitive) != 0) continue;
            QVERIFY(word.quadPx.valid);
            const double detected = QuadAngle(word.quadPx) * 180.0 / 3.14159265358979323846;
            qInfo("word=%s native-angle=%.4f expected-angle=%.1f centre-error=%.2f",
                  qPrintable(word.text), detected, degrees,
                  QLineF(word.boundsPx.center(), expectedCentre).length());
            QVERIFY(std::abs(detected - degrees) < 2.0);
            if (QLineF(word.boundsPx.center(), expectedCentre).length() <= 24) ++matched;
        }
    }
    QVERIFY2(matched * 4 >= page.words.size() * 3,
             qPrintable(QStringLiteral("%1 of %2 tilted words mapped to actual source ink")
                            .arg(matched).arg(page.words.size())));
}

void UltraClearTextWindowsOcrTests::recognizesSyntheticPrintedPage_data()
{
    QTest::addColumn<bool>("lightOnDark");
    QTest::addColumn<bool>("oppositeSurround");
    QTest::newRow("dark print on light page") << false << false;
    QTest::newRow("light print on dark board") << true << false;
    QTest::newRow("dark print with dark surround") << false << true;
    QTest::newRow("light print with bright surround") << true << true;
}

void UltraClearTextWindowsOcrTests::recognizesSyntheticPrintedPage()
{
    QFETCH(bool, lightOnDark);
    QFETCH(bool, oppositeSurround);
    const QStringList lines{QStringLiteral("MAGNIFIED PRINTED TEXT"), QStringLiteral("READS CLEARLY TODAY")};
    SyntheticPage page = MakePage(lines, {}, 1600, 900, 72);
    if (lightOnDark) {
        page.image.invertPixels(QImage::InvertRgb);
    }
    if (oppositeSurround) {
        // The surrounding room dominates global luma, with the opposite
        // polarity from the printed region. Exercise the actual Windows
        // backend before assuming an inversion heuristic causes a failure.
        QImage framed(page.image.size(), QImage::Format_ARGB32);
        framed.fill(lightOnDark ? Qt::white : Qt::black);
        QPainter painter(&framed);
        painter.drawImage(QRect(320, 180, 960, 540), page.image);
        painter.end();
        page.image = std::move(framed);
        for (PageWord& word : page.words) {
            const QRectF original = word.sceneRect;
            word.sceneRect = QRectF(0.2 + 0.6 * original.x(), 0.2 + 0.6 * original.y(),
                                     0.6 * original.width(), 0.6 * original.height());
        }
    }
    const ViewTransform t =
        ComputeViewTransform(1600, 900, 1600, 900, 1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
    std::mutex mutex;
    std::optional<RecognitionResult> received;
    BackendStatus status;
    int statusUpdates = 0;
    RecognitionScheduler scheduler(
        CreateWindowsRecognizer(),
        [&](RecognitionResult r) {
            std::lock_guard lock(mutex);
            received = std::move(r);
        },
        [&](BackendStatus s) {
            std::lock_guard lock(mutex);
            status = s;
            ++statusUpdates;
        });
    QTRY_VERIFY_WITH_TIMEOUT(scheduler.Status().state != BackendState::kNotStarted, 15000);
    const BackendStatus initial = scheduler.Status();
    qInfo("backend=%s state=%d language=%s detail=%s", qPrintable(initial.name), static_cast<int>(initial.state),
          qPrintable(initial.languageTag), qPrintable(initial.detail));
    if (initial.state != BackendState::kReady) {
        QSKIP(qPrintable(QStringLiteral("Windows.Media.Ocr unavailable: ") + initial.detail));
    }
    SourceStamp stamp;
    stamp.sourceEpoch = 1;
    stamp.geometryGeneration = 1;
    stamp.viewTransform = t;
    stamp.viewportWidth = 1600;
    stamp.viewportHeight = 900;
    QVERIFY(scheduler.Submit(stamp, RenderView(page.image, t, 1600, 900)) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            std::lock_guard lock(mutex);
            return received.has_value();
        }(),
        30000);
    std::lock_guard lock(mutex);
    QVERIFY2(received->output.ok, qPrintable(received->output.error));
    // Runtime status must survive Initialize(), including when Windows is the
    // temporary bridge in an automatic chain whose initial status was GPU.
    QVERIFY(statusUpdates >= 2);
    QCOMPARE(status.state, BackendState::kReady);
    QCOMPARE(status.name, QStringLiteral("Windows.Media.Ocr"));
    QVERIFY(!status.usesGpu);
    qInfo("recognition took %.1f ms", received->durationMs);
    for (const RecognizedLine& line : received->output.lines) {
        for (const RecognizedWord& word : line.words) {
            qInfo("word '%s' at %.0f,%.0f %.0fx%.0f", qPrintable(word.text), word.boundsPx.x(),
                  word.boundsPx.y(), word.boundsPx.width(), word.boundsPx.height());
        }
    }
    int matched = 0;
    for (const PageWord& expected : page.words) {
        const QRectF expectedPx = SceneRectToView(t, 1600, 900, expected.sceneRect);
        for (const RecognizedLine& line : received->output.lines) {
            for (const RecognizedWord& word : line.words) {
                if (word.text.compare(expected.text, Qt::CaseInsensitive) == 0 &&
                    expectedPx.adjusted(-12, -12, 12, 12).contains(word.boundsPx.center())) {
                    ++matched;
                }
            }
        }
    }
    // Requires most words, at their printed positions; not an accuracy claim.
    QVERIFY2(matched * 4 >= page.words.size() * 3,
             qPrintable(QStringLiteral("%1 of %2 words matched").arg(matched).arg(page.words.size())));
    QVERIFY(received->reference.IsValid());
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    UltraClearTextWindowsOcrTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ultra_clear_text_windows_ocr_tests.moc"
