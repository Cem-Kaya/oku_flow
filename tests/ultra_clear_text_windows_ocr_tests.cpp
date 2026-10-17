// Optional acceptance of the real built-in Windows.Media.Ocr backend through
// the production scheduler. Desktop-free: no windows, camera or network.
// Skips (never passes vacuously) when Windows has no OCR language installed.

#include "okuflow/common/ultra_clear_text.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QGuiApplication>
#include <QPainter>
#include <QSet>
#include <QtTest>

#include <mutex>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

class UltraClearTextWindowsOcrTests final : public QObject {
    Q_OBJECT

private slots:
    void recognizesSyntheticPrintedPage_data();
    void recognizesSyntheticPrintedPage();
};

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
    RecognitionScheduler scheduler(
        CreateDefaultRecognizer(),
        [&](RecognitionResult r) {
            std::lock_guard lock(mutex);
            received = std::move(r);
        },
        [&](BackendStatus s) {
            std::lock_guard lock(mutex);
            status = s;
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
