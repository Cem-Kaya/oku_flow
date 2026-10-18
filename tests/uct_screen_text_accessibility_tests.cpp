// QAccessible contract tests for Ultra Clear Text on-screen text. They run
// under Qt offscreen and verify Qt's accessibility model only; native UI
// Automation, Narrator, NVDA and Magnifier acceptance is a separate serial
// desktop pass (docs/uct_screen_text_accessibility.md).

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/view_transform.hpp"
#include "okuflow/ui/uct_screen_text_accessible.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#include "ultra_clear_text_test_support.hpp"

#include <QAccessible>
#include <QCryptographicHash>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QWidget>
#include <QtTest>

#include <algorithm>
#include <limits>
#include <memory>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

// A host with its own class name, so the host factory owns it regardless of
// factory installation order (production RenderWidget is the same case).
class TestViewport final : public QWidget {
    Q_OBJECT
public:
    explicit TestViewport(QWidget* parent) : QWidget(parent)
    {
        setProperty(kAccessibleVirtualChildHostProperty, true);
    }
};

namespace {

constexpr int kViewWidth = 960;
constexpr int kViewHeight = 540;
constexpr int kSceneWidth = 1600;
constexpr int kSceneHeight = 900;

QVector<QAccessible::Event> g_events;

// Records events about object-less (virtual) elements only: the UCT root
// and lines. Ordinary widget events (shows, names) are not ours to judge.
void RecordEvent(QAccessibleEvent* event)
{
    if (!event->object()) g_events.push_back(event->type());
}

int ForbiddenEvents()
{
    // Anything that could make a screen reader speak, move focus or the
    // caret. Only ObjectCreated/ObjectDestroyed bookkeeping is allowed.
    return static_cast<int>(std::count_if(g_events.begin(), g_events.end(), [](QAccessible::Event type) {
        return type != QAccessible::ObjectCreated && type != QAccessible::ObjectDestroyed;
    }));
}

ViewTransform Transform(float zoom, float fx, float fy)
{
    return ComputeViewTransform(kSceneWidth, kSceneHeight, kViewWidth, kViewHeight, zoom, fx, fy,
                                ViewportFitMode::kFill);
}

SourceStamp Stamp(std::int64_t timestamp, const ViewTransform& t)
{
    SourceStamp stamp;
    stamp.sourceEpoch = 1;
    stamp.geometryGeneration = 1;
    stamp.timestampMs = timestamp;
    stamp.viewTransform = t;
    stamp.viewportWidth = kViewWidth;
    stamp.viewportHeight = kViewHeight;
    return stamp;
}

RecognitionResult ResultFor(const SyntheticPage& page, const ViewTransform& t, const SourceStamp& stamp)
{
    RecognitionResult result;
    result.stamp = stamp;
    if (result.stamp.frameSeq == 0)
        result.stamp.frameSeq = static_cast<std::uint64_t>(std::max<std::int64_t>(0, stamp.timestampMs));
    const QImage view = IndependentFramePixels(RenderView(page.image, t, kViewWidth, kViewHeight),
                                               result.stamp.frameSeq);
    result.sourcePixelDigest = SourcePixelsDigest(view);
    result.output.ok = true;
    for (const PageWord& word : page.words) {
        const QRectF px = SceneRectToView(t, kViewWidth, kViewHeight, word.sceneRect);
        if (result.output.lines.isEmpty() ||
            std::abs(result.output.lines.last().words.first().boundsPx.top() - px.top()) > px.height() * 0.5) {
            result.output.lines.push_back({});
            result.lineColors.push_back({});
        }
        result.output.lines.last().words.push_back({word.text, px});
    }
    result.reference = MakeProbeFrame(view.constBits(), view.width(), view.height(),
                                      static_cast<int>(view.bytesPerLine()), 2, result.stamp);
    return result;
}

// Two agreeing recognitions of genuinely distinct synthetic observations:
// the real admission path, no hand-made lines or fabricated pixel hashes.
void AdmitTwice(OverlayModel& model, const SyntheticPage& page, const ViewTransform& t)
{
    model.Admit(ResultFor(page, t, Stamp(100, t)), Stamp(0, t), nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(200, t)), Stamp(0, t), nullptr, 210);
}

uct::ScreenLine Scripted(std::uint64_t id, const QString& text, QRectF sceneRect = {0.1, 0.1, 0.5, 0.05})
{
    uct::ScreenLine line;
    line.lineId = id;
    line.text = text;
    line.sceneRect = sceneRect;
    int start = 0;
    int wordIndex = 0;
    for (const QString& piece : text.split(QLatin1Char(' '))) {
        line.tokens.push_back({piece == QStringLiteral("\u2026") ? -1 : wordIndex++, start,
                               static_cast<int>(piece.size())});
        start += static_cast<int>(piece.size()) + 1;
    }
    return line;
}

// One shown window: an owner, the viewport host and the production overlay.
struct Harness {
    explicit Harness(bool attach = true)
    {
        window.move(137, 91); // never at the global origin
        window.resize(kViewWidth + 40, kViewHeight + 60);
        viewport = new TestViewport(&window);
        viewport->setGeometry(20, 30, kViewWidth, kViewHeight);
        overlay = new UltraClearTextOverlay(viewport, &window);
        if (attach) {
            a11y = new UctScreenTextAccessibility(
                overlay, viewport, [this] { return Snapshot(); }, [this] { return now; });
        }
        window.show();
    }
    ~Harness() { delete a11y.data(); }

    UctScreenTextSnapshot Snapshot()
    {
        UctScreenTextSnapshot snapshot;
        snapshot.enabled = enabled;
        snapshot.languageTag = QStringLiteral("en-US");
        if (!enabled) return snapshot;
        snapshot.lines = useScripted ? scripted : model.ScreenLines(now, frameSeq);
        return snapshot;
    }

    // Drives one real overlay paint. repaint() alone cannot reach
    // paintEvent before the tool window's queued expose event is delivered
    // (the backing store keeps the dirty region until then), so spin the
    // event loop, as the production presentation loop does between frames,
    // until a new paint plan lands. Bounded; if no paint arrives the
    // callers' PaintedGeometryCurrent/PaintedWord assertions fail visibly.
    void DrivePaint()
    {
        const std::uint64_t before = overlay->PaintedGeneration();
        overlay->repaint();
        for (int i = 0; i < 200 && overlay->PaintedGeneration() == before; ++i) QTest::qWait(1);
    }

    // Overlay words exactly as the controller converts SceneWords.
    void ShowModelWords(const ViewTransform& t)
    {
        QVector<UltraClearTextOverlay::SceneWord> words;
        for (const SceneWord& word : model.SceneWords(now, frameSeq)) {
            words.push_back({word.text, word.sceneRect, word.colors, word.lineId, word.protectedSceneRects,
                             word.wordIndex});
        }
        overlay->SetViewTransform(t);
        overlay->SetWords(words);
        DrivePaint();
    }

    void Pan(const ViewTransform& t)
    {
        overlay->SetViewTransform(t);
        DrivePaint();
    }

    QAccessibleInterface* Root() const { return a11y ? a11y->RootInterface() : nullptr; }
    QAccessibleInterface* Line(int index) const { return Root() ? Root()->child(index) : nullptr; }
    QRect Viewport() const { return QRect(viewport->mapToGlobal(QPoint(0, 0)), viewport->size()); }

    QWidget window;
    TestViewport* viewport{};
    UltraClearTextOverlay* overlay{};
    QPointer<UctScreenTextAccessibility> a11y;
    OverlayModel model;
    std::int64_t now{210};
    std::uint64_t frameSeq{};
    bool enabled{true};
    bool useScripted{};
    QVector<uct::ScreenLine> scripted;
};

const QStringList kLines{QStringLiteral("Reading glasses help"), QStringLiteral("people see printed pages"),
                         QStringLiteral("clearly every morning")};

bool InViewport(const QRect& viewport, const QRect& rect)
{
    // Zero-size rects are positions; the far edges are included.
    const auto inside = [&](const QPoint& p) {
        return p.x() >= viewport.left() && p.x() <= viewport.left() + viewport.width() &&
               p.y() >= viewport.top() && p.y() <= viewport.top() + viewport.height();
    };
    if (rect.isEmpty()) return inside(rect.topLeft());
    return viewport.contains(rect);
}

} // namespace

class UctScreenTextAccessibilityTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void inactiveAccessibilityCreatesNothing();
    void projectionIsDrawnTokensWithCollapsedOmissions();
    void readingOnlyEvidenceIsNeverExposed();
    void projectionHonoursBoundsAndSharedReadingOrder();
    void treeShapeRolesAndStates();
    void wordAndLineUnitsMatchTokens();
    void geometryMatchesPaintedGlyphPixels_data();
    void geometryMatchesPaintedGlyphPixels();
    void panAndZoomMoveGeometryWithoutEventsOrWork();
    void geometryIsNeverOriginNorOutsideViewport();
    void hitTestingInvertsCharacterRects();
    void graphemeClustersShareOneRect();
    void strokeWeightChangeKeepsLastPaintFontAuthority();
    void offscreenRenderDoesNotReplacePaintedAuthority();
    void rtlOriginalInkOmissionAndHitTesting();
    void fullyHiddenLineHasZeroAreaQtRangeRect();
    void additionsFollowCadenceAndBoundedReadingHold();
    void everyHardRevocationIsSynchronous();
    void identicalReRecognitionRebindsIdentity();
    void resourceBoundsHoldUnderChurn();
    void emptySnapshotIsEmptyRoot();
};

void UctScreenTextAccessibilityTests::initTestCase()
{
    RegisterFixtureFonts();
    QAccessible::installUpdateHandler(&RecordEvent);
}

void UctScreenTextAccessibilityTests::init()
{
    QAccessible::setActive(true);
    QVERIFY2(QAccessible::isActive(), "the platform cannot activate QAccessible; contract untestable");
    g_events.clear();
}

void UctScreenTextAccessibilityTests::cleanupTestCase()
{
    QAccessible::installUpdateHandler(nullptr);
}

void UctScreenTextAccessibilityTests::inactiveAccessibilityCreatesNothing()
{
    QAccessible::setActive(false);
    QVERIFY(!QAccessible::isActive());
    {
        Harness h;
        AdmitTwice(h.model, MakePage(kLines), Transform(1.0f, 0.5f, 0.5f));
        h.frameSeq = h.model.Lines().first().validatedFrameSeq;
        h.a11y->SetEnabled(true);
        for (int i = 0; i < 100; ++i) h.a11y->ContentChanged();
        h.a11y->RevokeAll();
        h.a11y->ProcessDueWork();
        QCOMPARE(h.a11y->SnapshotsTaken(), std::uint64_t{0});
        QVERIFY(!h.a11y->RootInterface());
        QCOMPARE(h.a11y->ExposedLineCount(), 0);
        QVERIFY(!h.a11y->findChild<QTimer*>()->isActive());

        // Activation builds the current state at once.
        QAccessible::setActive(true);
        QVERIFY(h.a11y->IsActive());
        QVERIFY(h.a11y->RootInterface());
        QCOMPARE(h.a11y->ExposedLineCount(), 3);
        // Deactivation tears everything down again.
        const QAccessible::Id root = QAccessible::uniqueId(h.a11y->RootInterface());
        QAccessible::setActive(false);
        QVERIFY(!QAccessible::accessibleInterface(root));
        QCOMPARE(h.a11y->ExposedLineCount(), 0);
    }
    QAccessible::setActive(true);
}

void UctScreenTextAccessibilityTests::projectionIsDrawnTokensWithCollapsedOmissions()
{
    const auto t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    AdmitTwice(model, MakePage({QStringLiteral("Total is 3.14 and l0O ok")}), t);
    const std::uint64_t frame = model.Lines().first().validatedFrameSeq;
    const QVector<ScreenLine> lines = model.ScreenLines(210, frame);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines[0].text, QStringLiteral("Total is 3.14 and \u2026 ok"));
    QVERIFY(!lines[0].text.contains(QStringLiteral("l0O")));
    QVERIFY(!lines[0].text.contains(QLatin1Char('\n')));
    // Every glyph token is exactly a word the overlay draws.
    const QVector<SceneWord> drawn = model.SceneWords(210, frame);
    int glyphs = 0;
    for (const ScreenToken& token : lines[0].tokens) {
        const QString piece = lines[0].text.mid(token.start, token.length);
        if (token.wordIndex < 0) {
            QCOMPARE(piece, QStringLiteral("\u2026"));
            continue;
        }
        ++glyphs;
        QVERIFY(std::any_of(drawn.begin(), drawn.end(), [&](const SceneWord& word) {
            return word.lineId == lines[0].lineId && word.wordIndex == token.wordIndex && word.text == piece;
        }));
    }
    QCOMPARE(glyphs, drawn.size());
    // Single spaces separate tokens, so Qt word units equal tokens.
    for (int i = 1; i < lines[0].tokens.size(); ++i)
        QCOMPARE(lines[0].tokens[i].start, lines[0].tokens[i - 1].start + lines[0].tokens[i - 1].length + 1);
    // The reading pane is unchanged by the shared reading order.
    QCOMPARE(model.ReadingText(210, frame), QStringLiteral("Total is 3.14 and \u2026 ok"));
}

void UctScreenTextAccessibilityTests::readingOnlyEvidenceIsNeverExposed()
{
    // A single immutable recognition is readable in the pane but never
    // drawn; screen text must not expose its guessed words.
    const auto page = MakePage({QStringLiteral("Read page")});
    const auto t = Transform(1.0f, 0.5f, 0.5f);
    auto stamp = Stamp(100, t);
    stamp.frameSeq = 50;
    auto selected = ResultFor(page, t, stamp);
    const QImage pixels = IndependentFramePixels(RenderView(page.image, t, kViewWidth, kViewHeight),
                                                 selected.stamp.frameSeq);
    selected.sourcePixelDigest = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(pixels.constBits()), pixels.sizeInBytes()),
        QCryptographicHash::Sha256);
    ModelTuning tuning;
    tuning.admitMatchBudgetMs = 1000;
    OverlayModel model(tuning);
    constexpr std::int64_t hour = 60 * 60 * 1000;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, hour), AdmitVerdict::kPending);
    QCOMPARE(model.ReadingText(hour, 50), QStringLiteral("\u2026 Read page"));
    QVERIFY(model.SceneWords(hour, 50).isEmpty());
    QVERIFY(model.ScreenLines(hour, 50).isEmpty());
}

void UctScreenTextAccessibilityTests::projectionHonoursBoundsAndSharedReadingOrder()
{
    const auto t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    AdmitTwice(model, MakePage(kLines), t);
    const std::uint64_t frame = model.Lines().first().validatedFrameSeq;
    const QVector<ScreenLine> all = model.ScreenLines(210, frame);
    QCOMPARE(all.size(), 3);
    QStringList texts;
    for (const ScreenLine& line : all) texts << line.text;
    QCOMPARE(texts.join(QLatin1Char('\n')), model.ReadingText(210, frame));

    ScreenTextLimits limits;
    limits.maxLineUtf16 = 12;
    const QVector<ScreenLine> cut = model.ScreenLines(210, frame, limits);
    QCOMPARE(cut[0].text, QStringLiteral("Reading \u2026"));
    QVERIFY(cut[0].truncated);
    for (const ScreenLine& line : cut) QVERIFY(line.text.size() <= 12);

    limits = {};
    limits.maxLines = 2;
    QCOMPARE(model.ScreenLines(210, frame, limits).size(), 2);
    limits = {};
    limits.maxTotalUtf16 = static_cast<int>(all[0].text.size() + all[1].text.size());
    const QVector<ScreenLine> capped = model.ScreenLines(210, frame, limits);
    QCOMPARE(capped.size(), 2); // cut at the end of reading order
    QCOMPARE(capped[1].text, all[1].text);
}

void UctScreenTextAccessibilityTests::treeShapeRolesAndStates()
{
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    const auto t = Transform(1.0f, 0.5f, 0.5f);
    AdmitTwice(h.model, MakePage(kLines), t);
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.ShowModelWords(t);
    h.a11y->SetEnabled(true);

    QAccessibleInterface* host = QAccessible::queryAccessibleInterface(h.viewport);
    QVERIFY(host);
    QAccessibleInterface* root = h.Root();
    QVERIFY(root);
    QCOMPARE(host->child(host->childCount() - 1), root);
    QCOMPARE(host->indexOfChild(root), host->childCount() - 1);
    QCOMPARE(root->parent(), host);
    QCOMPARE(root->role(), QAccessible::Grouping);
    QCOMPARE(root->text(QAccessible::Name), QStringLiteral("Ultra Clear Text on this view"));
    QCOMPARE(root->text(QAccessible::Description), QStringLiteral("\u2026 marks text left as the original image."));
    QCOMPARE(root->rect(), h.Viewport());
    QVERIFY(!root->textInterface());
    QVERIFY(!root->object());

    const QVector<ScreenLine> lines = h.model.ScreenLines(h.now, h.frameSeq);
    QCOMPARE(root->childCount(), lines.size());
    for (int i = 0; i < root->childCount(); ++i) {
        QAccessibleInterface* line = root->child(i);
        QVERIFY(line && line->isValid());
        QCOMPARE(line->parent(), root);
        QCOMPARE(root->indexOfChild(line), i);
        QCOMPARE(line->role(), QAccessible::StaticText); // UIA Text: no ValuePattern
        QCOMPARE(line->text(QAccessible::Name), lines[i].text);
        QVERIFY(line->state().readOnly);
        QVERIFY(!line->state().focusable);
        QVERIFY(!line->state().focused);
        QVERIFY(!line->state().invisible);
        QVERIFY(!line->state().offscreen);
        QCOMPARE(line->childCount(), 0);
        QVERIFY(line->textInterface());
        QCOMPARE(line->textInterface()->text(0, -1), lines[i].text);
        QCOMPARE(line->textInterface()->selectionCount(), 0);
        QVERIFY(line->attributesInterface());
        QCOMPARE(line->attributesInterface()->attributeValue(QAccessible::Attribute::Locale).toLocale(),
                 QLocale(QStringLiteral("en-US")));
        QCOMPARE(line->window(), host->window());
    }
    QCOMPARE(ForbiddenEvents(), 0);
}

void UctScreenTextAccessibilityTests::wordAndLineUnitsMatchTokens()
{
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    const auto t = Transform(1.0f, 0.5f, 0.5f);
    AdmitTwice(h.model, MakePage({QStringLiteral("Total is 3.14 and l0O ok")}), t);
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.ShowModelWords(t);
    h.a11y->SetEnabled(true);
    QAccessibleTextInterface* text = h.Line(0)->textInterface();
    const ScreenLine line = h.model.ScreenLines(h.now, h.frameSeq).first();
    for (const ScreenToken& token : line.tokens) {
        // Qt's default word finder treats punctuation as a gap, so only
        // glyph words are units here; UIA itself splits on whitespace.
        if (token.wordIndex < 0) continue;
        int start = -1, end = -1;
        const QString word = text->textAtOffset(token.start, QAccessible::WordBoundary, &start, &end);
        QCOMPARE(word, line.text.mid(token.start, token.length));
        QCOMPARE(start, token.start);
        QCOMPARE(end, token.start + token.length);
    }
    int start = -1, end = -1;
    QCOMPARE(text->textAtOffset(3, QAccessible::LineBoundary, &start, &end), line.text);
    QCOMPARE(start, 0);
    QCOMPARE(end, static_cast<int>(line.text.size()));
}

void UctScreenTextAccessibilityTests::geometryMatchesPaintedGlyphPixels_data()
{
    QTest::addColumn<float>("zoom");
    QTest::addColumn<float>("focusX");
    QTest::addColumn<float>("focusY");
    QTest::newRow("fit") << 1.0f << 0.5f << 0.5f;
    QTest::newRow("zoom2-left") << 2.0f << 0.25f << 0.2f;
    QTest::newRow("zoom3-centre") << 3.0f << 0.35f << 0.25f;
}

void UctScreenTextAccessibilityTests::geometryMatchesPaintedGlyphPixels()
{
    QFETCH(float, zoom);
    QFETCH(float, focusX);
    QFETCH(float, focusY);
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    AdmitTwice(h.model, MakePage(kLines), Transform(1.0f, 0.5f, 0.5f));
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.ShowModelWords(Transform(zoom, focusX, focusY));
    h.a11y->SetEnabled(true);
    QVERIFY(h.overlay->PaintedGeometryCurrent());

    // Pixels the overlay actually painted, in logical overlay coordinates.
    const QImage painted = h.overlay->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = painted.devicePixelRatio();
    const QPoint origin = h.Viewport().topLeft();
    QVERIFY(h.overlay->PaintedGeometryCurrent()); // grab repainted the same plan
    int checkedWords = 0;
    for (int i = 0; i < h.Root()->childCount(); ++i) {
        QAccessibleInterface* line = h.Line(i);
        QAccessibleTextInterface* text = line->textInterface();
        const ScreenLine content = h.model.ScreenLines(h.now, h.frameSeq)[i];
        for (const ScreenToken& token : content.tokens) {
            if (token.wordIndex < 0) continue;
            UltraClearTextOverlay::PaintedWordGeometry word;
            QVERIFY(h.overlay->PaintedWord(content.lineId, token.wordIndex,
                                           content.text.mid(token.start, token.length), word));
            QRect unionRect;
            int previousLeft = std::numeric_limits<int>::min();
            for (int u = token.start; u < token.start + token.length; ++u) {
                const QRect r = text->characterRect(u);
                QVERIFY(!r.isNull() || r.topLeft() != QPoint(0, 0));
                if (!r.isEmpty()) {
                    QVERIFY2(r.left() >= previousLeft, "character rects must run left to right");
                    previousLeft = r.left();
                    unionRect = unionRect.united(r);
                }
            }
            if (!word.drawn) continue;
            // Dark glyph ink inside this word's ink box, from the real pixels.
            const QRect ink = word.box.toAlignedRect().adjusted(-2, -2, 2, 2);
            QRect inkPixels;
            for (int y = std::max(0, ink.top()); y <= std::min(h.overlay->height() - 1, ink.bottom()); ++y) {
                for (int x = std::max(0, ink.left()); x <= std::min(h.overlay->width() - 1, ink.right()); ++x) {
                    const QRgb px = painted.pixel(int(x * dpr), int(y * dpr));
                    if (qAlpha(px) > 200 && qGray(px) < 96) inkPixels = inkPixels.united(QRect(x, y, 1, 1));
                }
            }
            if (inkPixels.isEmpty()) continue; // fully clipped by the viewport edge
            const QRect inkGlobal = inkPixels.translated(origin);
            QVERIFY2(unionRect.adjusted(-2, -2, 2, 2).contains(inkGlobal),
                     qPrintable(QStringLiteral("%1: ink %2,%3 %4x%5 outside chars %6,%7 %8x%9")
                                    .arg(content.text.mid(token.start, token.length))
                                    .arg(inkGlobal.x()).arg(inkGlobal.y()).arg(inkGlobal.width()).arg(inkGlobal.height())
                                    .arg(unionRect.x()).arg(unionRect.y()).arg(unionRect.width()).arg(unionRect.height())));
            ++checkedWords;
        }
    }
    QVERIFY(checkedWords > 0);
}

void UctScreenTextAccessibilityTests::panAndZoomMoveGeometryWithoutEventsOrWork()
{
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    AdmitTwice(h.model, MakePage(kLines), Transform(1.0f, 0.5f, 0.5f));
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.ShowModelWords(Transform(1.0f, 0.5f, 0.5f));
    h.a11y->SetEnabled(true);
    QVector<QAccessible::Id> ids;
    for (int i = 0; i < h.Root()->childCount(); ++i) ids << QAccessible::uniqueId(h.Line(i));
    const std::uint64_t snapshots = h.a11y->SnapshotsTaken();
    g_events.clear();
    QRect previous = h.Line(0)->textInterface()->characterRect(0);
    int moved = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const float zoom = 1.0f + 0.01f * (frame % 100);
        h.Pan(Transform(zoom, 0.3f + 0.001f * frame, 0.3f));
        const QRect now = h.Line(0)->textInterface()->characterRect(0);
        if (now != previous) ++moved;
        previous = now;
        // The same rect the overlay painted for this frame.
        UltraClearTextOverlay::PaintedWordGeometry word;
        const ScreenLine content = h.model.ScreenLines(h.now, h.frameSeq).first();
        QVERIFY(h.overlay->PaintedWord(content.lineId, 0, content.text.mid(0, content.tokens[0].length), word));
    }
    QVERIFY(moved > 100);
    QCOMPARE(g_events.size(), 0);
    QCOMPARE(h.a11y->SnapshotsTaken(), snapshots); // pan/zoom never reaches the model
    for (int i = 0; i < ids.size(); ++i) QCOMPARE(QAccessible::uniqueId(h.Line(i)), ids[i]);
}

void UctScreenTextAccessibilityTests::geometryIsNeverOriginNorOutsideViewport()
{
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    AdmitTwice(h.model, MakePage(kLines), Transform(1.0f, 0.5f, 0.5f));
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.a11y->SetEnabled(true); // published before any paint: geometry not current
    const QRect viewport = h.Viewport();
    QVERIFY(viewport.topLeft() != QPoint(0, 0));
    const auto checkAll = [&] {
        for (int i = 0; i < h.Root()->childCount(); ++i) {
            QAccessibleInterface* line = h.Line(i);
            QVERIFY(line->rect() != QRect());
            QVERIFY(InViewport(viewport, line->rect()));
            QAccessibleTextInterface* text = line->textInterface();
            for (int u = -1; u <= text->characterCount(); ++u) {
                const QRect r = text->characterRect(u);
                QVERIFY(r != QRect());
                QVERIFY2(InViewport(viewport, r), qPrintable(QStringLiteral("unit %1 at %2,%3")
                                                                 .arg(u).arg(r.x()).arg(r.y())));
            }
        }
    };
    checkAll();
    QRandomGenerator random(20261018);
    h.ShowModelWords(Transform(1.0f, 0.5f, 0.5f));
    for (int round = 0; round < 60; ++round) {
        const float zoom = 0.5f + 7.5f * float(random.generateDouble());
        // Include focus points that push all text off the viewport.
        const float fx = -0.5f + 2.0f * float(random.generateDouble());
        const float fy = -0.5f + 2.0f * float(random.generateDouble());
        h.Pan(Transform(zoom, fx, fy));
        checkAll();
    }
    // The overlay moving away from the viewport fails closed until repaint.
    h.overlay->move(h.overlay->pos() + QPoint(5, 0));
    QVERIFY(!h.overlay->PaintedGeometryCurrent());
    checkAll();
    QVERIFY(h.Line(0)->state().offscreen);
}

void UctScreenTextAccessibilityTests::hitTestingInvertsCharacterRects()
{
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    AdmitTwice(h.model, MakePage(kLines), Transform(1.0f, 0.5f, 0.5f));
    h.frameSeq = h.model.Lines().first().validatedFrameSeq;
    h.ShowModelWords(Transform(1.5f, 0.4f, 0.3f));
    h.a11y->SetEnabled(true);
    QAccessibleInterface* host = QAccessible::queryAccessibleInterface(h.viewport);
    int checked = 0;
    for (int i = 0; i < h.Root()->childCount(); ++i) {
        QAccessibleInterface* line = h.Line(i);
        QAccessibleTextInterface* text = line->textInterface();
        for (int u = 0; u < text->characterCount(); ++u) {
            const QRect r = text->characterRect(u);
            if (r.width() < 3 || r.height() < 3) continue;
            const QPoint centre = r.center();
            const int hit = text->offsetAtPoint(centre);
            QVERIFY(hit >= 0);
            QVERIFY(text->characterRect(hit).contains(centre));
            QCOMPARE(h.Root()->childAt(centre.x(), centre.y()), line);
            QCOMPARE(host->childAt(centre.x(), centre.y()), h.Root());
            ++checked;
        }
    }
    QVERIFY(checked > 20);
    const QPoint outside = h.Viewport().bottomRight() + QPoint(50, 50);
    QCOMPARE(h.Line(0)->textInterface()->offsetAtPoint(outside), -1);
    QVERIFY(!h.Root()->childAt(outside.x(), outside.y()));
}

void UctScreenTextAccessibilityTests::graphemeClustersShareOneRect()
{
    Harness h(false);
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    const QString word = QStringLiteral("e\u0301x\U0001F600b");
    UltraClearTextOverlay::SceneWord scene;
    scene.text = word;
    scene.sceneRect = QRectF(0.2, 0.3, 0.3, 0.08);
    scene.lineId = 7;
    scene.wordIndex = 0;
    h.overlay->SetViewTransform(Transform(1.0f, 0.5f, 0.5f));
    h.overlay->SetWords({scene});
    h.DrivePaint();
    UltraClearTextOverlay::PaintedWordGeometry geometry;
    QVERIFY(h.overlay->PaintedWord(7, 0, word, geometry));
    QCOMPARE(geometry.characters.size(), word.size());
    QCOMPARE(geometry.characters[0], geometry.characters[1]); // e + combining acute
    QCOMPARE(geometry.characters[3], geometry.characters[4]); // surrogate pair
    QVERIFY(geometry.characters[2] != geometry.characters[0]);
    QVERIFY(!h.overlay->PaintedWord(7, 0, QStringLiteral("other"), geometry)); // text mismatch fails
    QVERIFY(!h.overlay->PaintedWord(8, 0, word, geometry));
}

void UctScreenTextAccessibilityTests::strokeWeightChangeKeepsLastPaintFontAuthority()
{
    Harness h(false);
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    const QString word = QStringLiteral("Widening");
    UltraClearTextOverlay::SceneWord scene;
    scene.text = word;
    scene.sceneRect = QRectF(0.2, 0.3, 0.35, 0.08);
    scene.lineId = 11;
    scene.wordIndex = 0;
    h.overlay->SetStrokeWeight(-3);
    h.overlay->SetViewTransform(Transform(1.0f, 0.5f, 0.5f));
    h.overlay->SetWords({scene});
    h.DrivePaint();
    QVERIFY(h.overlay->PaintedGeometryCurrent());
    UltraClearTextOverlay::PaintedWordGeometry before;
    QVERIFY(h.overlay->PaintedWord(11, 0, word, before)); // first query shapes lazily
    QVERIFY(before.drawn);
    const std::uint64_t generation = h.overlay->PaintedGeneration();

    // The setter swaps the live reference font and clears the caret cache
    // and queues (but does not deliver) a repaint. Without processing any
    // paint, a query must still answer for the pixels on screen: exactly
    // the geometry shaped with the painted font, not the new font laid
    // over the old plan's origin and scale.
    h.overlay->SetStrokeWeight(3);
    QCOMPARE(h.overlay->PaintedGeneration(), generation);
    UltraClearTextOverlay::PaintedWordGeometry after;
    QVERIFY(h.overlay->PaintedWord(11, 0, word, after));
    QCOMPARE(after.drawn, before.drawn);
    QCOMPARE(after.box, before.box);
    QVERIFY(after.characters == before.characters);
}

void UctScreenTextAccessibilityTests::offscreenRenderDoesNotReplacePaintedAuthority()
{
    // SaveSelectedVideoFrame renders the overlay into an export image via
    // QWidget::render. That paint is not on screen, so it must draw the
    // pending view into the image without promoting its plan over the
    // last on-screen paint.
    Harness h(false);
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    const QString word = QStringLiteral("Exported");
    UltraClearTextOverlay::SceneWord scene;
    scene.text = word;
    scene.sceneRect = QRectF(0.2, 0.3, 0.35, 0.08);
    scene.lineId = 12;
    scene.wordIndex = 0;
    h.overlay->SetViewTransform(Transform(1.0f, 0.5f, 0.5f));
    h.overlay->SetWords({scene});
    h.DrivePaint();
    QVERIFY(h.overlay->PaintedGeometryCurrent());
    UltraClearTextOverlay::PaintedWordGeometry before;
    QVERIFY(h.overlay->PaintedWord(12, 0, word, before));
    const std::uint64_t generation = h.overlay->PaintedGeneration();

    h.overlay->SetViewTransform(Transform(2.0f, 0.3f, 0.3f)); // queued, not painted
    QImage exported(h.overlay->size(), QImage::Format_ARGB32);
    exported.fill(Qt::transparent);
    h.overlay->render(&exported);
    QCOMPARE(h.overlay->PaintedGeneration(), generation);
    UltraClearTextOverlay::PaintedWordGeometry after;
    QVERIFY(h.overlay->PaintedWord(12, 0, word, after));
    QCOMPARE(after.box, before.box);
    QVERIFY(after.characters == before.characters);

    // The next real paint does become the authority.
    h.DrivePaint();
    QVERIFY(h.overlay->PaintedGeneration() > generation);
    UltraClearTextOverlay::PaintedWordGeometry painted;
    QVERIFY(h.overlay->PaintedWord(12, 0, word, painted));
    QVERIFY(painted.box != before.box);
}

void UctScreenTextAccessibilityTests::rtlOriginalInkOmissionAndHitTesting()
{
    // Suppressed (original-ink) Hebrew words on one descending line, plus
    // a suppressed Arabic word checked at the overlay level. Logical
    // order is rightmost first; proportional source-box cells, the "…"
    // gap and the separating-space gap must all follow that, not assume
    // the next word is to the right.
    const QString hebrew0 = QStringLiteral("\u05d0\u05d1\u05d2"); // אבג
    const QString hebrew1 = QStringLiteral("\u05d3\u05d4\u05d5"); // דהו
    const QString hebrew2 = QStringLiteral("\u05d6\u05d7\u05d8"); // זחט
    const QString arabic = QStringLiteral("\u0633\u0644\u0627\u0645"); // سلام
    const QString lineText = hebrew0 + QLatin1Char(' ') + hebrew1 + QStringLiteral(" \u2026 ") + hebrew2;

    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    h.useScripted = true;
    h.scripted = {Scripted(42, lineText, {0.08, 0.30, 0.78, 0.008})};

    const auto suppressed = [](const QString& text, std::uint64_t lineId, int index, const QRectF& rect) {
        UltraClearTextOverlay::SceneWord scene;
        scene.text = text;
        scene.lineId = lineId;
        scene.wordIndex = index;
        scene.sceneRect = rect; // projected height < 6 px: never drawn
        return scene;
    };
    h.overlay->SetViewTransform(Transform(1.0f, 0.5f, 0.5f));
    h.overlay->SetWords({suppressed(hebrew0, 42, 0, {0.68, 0.30, 0.18, 0.008}),
                         suppressed(hebrew1, 42, 1, {0.44, 0.30, 0.18, 0.008}),
                         suppressed(hebrew2, 42, 2, {0.08, 0.30, 0.18, 0.008}),
                         suppressed(arabic, 43, 0, {0.20, 0.50, 0.25, 0.008})});
    h.DrivePaint();
    QVERIFY(h.overlay->PaintedGeometryCurrent());

    // Overlay level: the first logical character is the rightmost cell.
    UltraClearTextOverlay::PaintedWordGeometry word0;
    QVERIFY(h.overlay->PaintedWord(42, 0, hebrew0, word0));
    QVERIFY(!word0.drawn);
    QCOMPARE(word0.characters.size(), hebrew0.size());
    for (int u = 1; u < word0.characters.size(); ++u)
        QVERIFY(word0.characters[u - 1].center().x() > word0.characters[u].center().x());
    UltraClearTextOverlay::PaintedWordGeometry arabicWord;
    QVERIFY(h.overlay->PaintedWord(43, 0, arabic, arabicWord));
    QVERIFY(!arabicWord.drawn);
    QCOMPARE(arabicWord.characters.size(), arabic.size());
    QVERIFY(arabicWord.characters.first().center().x() > arabicWord.characters.last().center().x());
    for (int u = 1; u < arabicWord.characters.size(); ++u)
        QVERIFY(arabicWord.characters[u - 1].center().x() >= arabicWord.characters[u].center().x() - 0.001);

    // Accessible level: offsets, gaps and hit testing follow suit.
    h.a11y->SetEnabled(true);
    QCOMPARE(h.a11y->ExposedLineCount(), 1);
    QAccessibleTextInterface* text = h.Line(0)->textInterface();
    QVERIFY(text);
    const int space0 = static_cast<int>(hebrew0.size());
    const int word1 = space0 + 1;
    const int omission = word1 + static_cast<int>(hebrew1.size()) + 1;
    const int word2 = omission + 2;
    const QRect first = text->characterRect(0);
    const QRect middle = text->characterRect(word1);
    const QRect lastWord = text->characterRect(word2);
    QVERIFY(!first.isEmpty() && !middle.isEmpty() && !lastWord.isEmpty());
    QVERIFY(first.center().x() > middle.center().x());
    QVERIFY(middle.center().x() > lastWord.center().x());
    for (int u = 1; u < hebrew0.size(); ++u)
        QVERIFY(text->characterRect(u - 1).left() >= text->characterRect(u).left());

    const QRect spaceRect = text->characterRect(space0);
    QVERIFY2(spaceRect.width() > 0, "space gap between descending boxes must have width");
    QVERIFY(spaceRect.left() >= middle.right() - 2 && spaceRect.right() <= first.left() + 2);

    const QRect omissionRect = text->characterRect(omission);
    QVERIFY2(omissionRect.width() > 0, "omission gap between descending boxes must have width");
    QVERIFY(omissionRect.left() >= lastWord.right() - 2 && omissionRect.right() <= middle.left() + 2);

    QCOMPARE(text->offsetAtPoint(first.center()), 0);
    QCOMPARE(text->offsetAtPoint(omissionRect.center()), omission);
    QCOMPARE(text->offsetAtPoint(lastWord.center()), word2);
    QCOMPARE(h.Root()->childAt(first.center().x(), first.center().y()), h.Line(0));
}

void UctScreenTextAccessibilityTests::fullyHiddenLineHasZeroAreaQtRangeRect()
{
    // Qt 6.12's QWindowsUiaTextRangeProvider::GetBoundingRectangles builds
    // a range rect from the first and last character rects by min/max and
    // ignores IsOffscreen/empty. A completely occluded two-word line
    // whose raw centres differ in both axes must still yield zero area:
    // every hidden character anchors on one shared point.
    Harness h;
    QVERIFY(QTest::qWaitForWindowExposed(&h.window));
    auto* occluder = new QWidget(&h.window);
    occluder->setGeometry(20, 30, kViewWidth, kViewHeight); // exactly the viewport
    occluder->show();
    h.overlay->SetExcludedWidget(occluder);
    h.useScripted = true;
    h.scripted = {Scripted(9, QStringLiteral("alpha beta"), {0.15, 0.30, 0.50, 0.13})};

    const auto sceneWord = [](const QString& text, int index, const QRectF& rect) {
        UltraClearTextOverlay::SceneWord scene;
        scene.text = text;
        scene.lineId = 9;
        scene.wordIndex = index;
        scene.sceneRect = rect;
        return scene;
    };
    h.overlay->SetViewTransform(Transform(1.0f, 0.5f, 0.5f));
    // Different vertical centres, so per-character raw fallbacks would
    // span a positive-area box from the first to the last character.
    h.overlay->SetWords({sceneWord(QStringLiteral("alpha"), 0, {0.15, 0.30, 0.20, 0.05}),
                         sceneWord(QStringLiteral("beta"), 1, {0.45, 0.38, 0.20, 0.05})});
    h.DrivePaint();
    QVERIFY(h.overlay->PaintedGeometryCurrent());
    UltraClearTextOverlay::PaintedWordGeometry firstWord, secondWord;
    QVERIFY(h.overlay->PaintedWord(9, 0, QStringLiteral("alpha"), firstWord));
    QVERIFY(h.overlay->PaintedWord(9, 1, QStringLiteral("beta"), secondWord));
    QVERIFY(std::abs(firstWord.characters.first().center().y() -
                     secondWord.characters.last().center().y()) > 5.0);
    QVERIFY(h.overlay->VisiblePart(firstWord.characters.first(), false).isEmpty());

    h.a11y->SetEnabled(true);
    QCOMPARE(h.a11y->ExposedLineCount(), 1);
    QAccessibleInterface* line = h.Line(0);
    QVERIFY(line->state().offscreen);
    QAccessibleTextInterface* text = line->textInterface();
    const int count = text->characterCount();
    const QRect first = text->characterRect(0);
    for (int u = 0; u < count; ++u) {
        QCOMPARE(text->characterRect(u), first); // one shared anchor
        QVERIFY(InViewport(h.Viewport(), text->characterRect(u)));
    }
    // Emulate Qt's endpoint aggregation exactly: min of lefts/tops, max
    // of rights/bottoms over the first and last character rects.
    const QRect last = text->characterRect(count - 1);
    const int rangeWidth = std::max(first.right(), last.right()) - std::min(first.left(), last.left());
    const int rangeHeight = std::max(first.bottom(), last.bottom()) - std::min(first.top(), last.top());
    QVERIFY2(rangeWidth <= 0 || rangeHeight <= 0, "fully hidden line must report a zero-area UIA range rect");
}

void UctScreenTextAccessibilityTests::additionsFollowCadenceAndBoundedReadingHold()
{
    Harness h;
    h.useScripted = true;
    h.now = 0;
    h.scripted = {Scripted(1, QStringLiteral("alpha"))};
    h.a11y->SetEnabled(true);
    QCOMPARE(h.a11y->ExposedLineCount(), 1); // first content after enable is immediate
    QCOMPARE(h.a11y->findChildren<QTimer*>().size(), 1); // one bounded cadence timer

    h.scripted << Scripted(2, QStringLiteral("beta"), {0.1, 0.3, 0.5, 0.05});
    h.a11y->ContentChanged();
    QCOMPARE(h.a11y->ExposedLineCount(), 1);
    QVERIFY(h.a11y->HasPendingAdditions());
    for (int i = 0; i < 50; ++i) h.a11y->ContentChanged(); // churn neither publishes nor resets
    h.now = 999;
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 1);
    h.now = 1000;
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 2);
    QCOMPARE(h.Line(1)->text(QAccessible::Name), QStringLiteral("beta"));

    // A reader querying text holds additions, never beyond 3 s.
    h.now = 2000;
    h.scripted << Scripted(3, QStringLiteral("gamma"), {0.1, 0.5, 0.5, 0.05});
    h.a11y->ContentChanged();
    h.now = 2500;
    QCOMPARE(h.Line(0)->text(QAccessible::Name), QStringLiteral("alpha"));
    h.now = 3000;
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 2);
    h.now = 4999;
    h.Line(0)->textInterface()->characterRect(0);
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 2);
    h.now = 5000;
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 3);

    // Without a reader, the cadence alone applies.
    h.now = 20000;
    h.scripted << Scripted(4, QStringLiteral("delta"), {0.1, 0.7, 0.5, 0.05});
    h.a11y->ContentChanged();
    h.now = 21000;
    h.a11y->ProcessDueWork();
    QCOMPARE(h.a11y->ExposedLineCount(), 4);
    QCOMPARE(ForbiddenEvents(), 0);
}

void UctScreenTextAccessibilityTests::everyHardRevocationIsSynchronous()
{
    Harness h;
    h.useScripted = true;
    h.scripted = {Scripted(1, QStringLiteral("alpha")), Scripted(2, QStringLiteral("beta"), {0.1, 0.3, 0.5, 0.05})};
    h.a11y->SetEnabled(true);
    const auto idsOf = [&] {
        QVector<QAccessible::Id> ids;
        for (int i = 0; i < h.Root()->childCount(); ++i) ids << QAccessible::uniqueId(h.Line(i));
        return ids;
    };
    QVector<QAccessible::Id> ids = idsOf();
    QCOMPARE(ids.size(), 2);

    // A line that failed validation disappears from the drawn set.
    h.scripted.removeFirst();
    h.a11y->ContentChanged();
    QVERIFY(!QAccessible::accessibleInterface(ids[0]));
    QVERIFY(QAccessible::accessibleInterface(ids[1]));

    // Text changing under one id is a removal now, an addition later.
    h.scripted = {Scripted(2, QStringLiteral("bet"), {0.1, 0.3, 0.5, 0.05})};
    h.a11y->ContentChanged();
    QVERIFY(!QAccessible::accessibleInterface(ids[1]));
    QCOMPARE(h.a11y->ExposedLineCount(), 0);
    QVERIFY(h.a11y->HasPendingAdditions());

    // Invalidate()/clear-branch path: everything, before any timer.
    h.now += 2000;
    h.a11y->ProcessDueWork();
    ids = idsOf();
    QCOMPARE(ids.size(), 1);
    h.a11y->RevokeAll();
    QVERIFY(!QAccessible::accessibleInterface(ids[0]));
    QVERIFY(h.Root());
    QCOMPARE(h.Root()->childCount(), 0);
    // The next content after a revocation is published at once (seek).
    h.a11y->ContentChanged();
    QCOMPARE(h.a11y->ExposedLineCount(), 1);

    // Provider reporting UCT off revokes lines; disabling removes the root.
    ids = idsOf();
    const QAccessible::Id root = QAccessible::uniqueId(h.Root());
    h.enabled = false;
    h.a11y->ContentChanged();
    QVERIFY(!QAccessible::accessibleInterface(ids[0]));
    h.a11y->SetEnabled(false);
    QVERIFY(!QAccessible::accessibleInterface(root));
    QVERIFY(!h.Root());

    // Explicit destruction deletes every element on this thread.
    h.enabled = true;
    h.a11y->SetEnabled(true);
    ids = idsOf();
    const QAccessible::Id root2 = QAccessible::uniqueId(h.Root());
    delete h.a11y.data();
    QVERIFY(!QAccessible::accessibleInterface(root2));
    for (QAccessible::Id id : ids) QVERIFY(!QAccessible::accessibleInterface(id));
    QCOMPARE(ForbiddenEvents(), 0);
}

void UctScreenTextAccessibilityTests::identicalReRecognitionRebindsIdentity()
{
    Harness h;
    h.useScripted = true;
    h.scripted = {Scripted(1, QStringLiteral("alpha beta"), {0.1, 0.1, 0.5, 0.05})};
    h.a11y->SetEnabled(true);
    const QAccessible::Id id = QAccessible::uniqueId(h.Line(0));
    g_events.clear();
    for (std::uint64_t round = 2; round < 300; ++round) {
        // New model id, same text, jittered box.
        h.scripted = {Scripted(round, QStringLiteral("alpha beta"),
                               {0.1 + 0.002 * (round % 5), 0.1, 0.5, 0.05})};
        h.a11y->ContentChanged();
        QCOMPARE(h.a11y->ExposedLineCount(), 1);
        QCOMPARE(QAccessible::uniqueId(h.Line(0)), id);
        QVERIFY(!h.a11y->HasPendingAdditions());
    }
    QCOMPARE(g_events.size(), 0);
    // Far away, the same words are a different line: revoke, add later.
    h.scripted = {Scripted(999, QStringLiteral("alpha beta"), {0.1, 0.8, 0.5, 0.05})};
    h.a11y->ContentChanged();
    QVERIFY(!QAccessible::accessibleInterface(id));
    QVERIFY(h.a11y->HasPendingAdditions());
}

void UctScreenTextAccessibilityTests::resourceBoundsHoldUnderChurn()
{
    Harness h;
    h.useScripted = true;
    QVector<uct::ScreenLine> many;
    for (int i = 0; i < 200; ++i)
        many << Scripted(i + 1, QStringLiteral("word%1 text").arg(i), {0.0, i / 200.0, 0.5, 0.004});
    h.scripted = many;
    h.a11y->SetEnabled(true);
    QCOMPARE(h.a11y->ExposedLineCount(), 128);
    // Oversized lines are cut at the end of reading order.
    QVector<uct::ScreenLine> wide;
    for (int i = 0; i < 40; ++i) wide << Scripted(1000 + i, QString(500, QLatin1Char('x')), {0.0, i / 40.0, 0.5, 0.02});
    h.scripted = wide;
    h.a11y->RevokeAll();
    h.a11y->ContentChanged();
    QVERIFY(h.a11y->ExposedLineCount() * 500 <= 8192);
    QCOMPARE(h.a11y->ExposedLineCount(), 16);

    // Churn: every round replaces all content; dead ids never stay alive.
    QVector<QAccessible::Id> seen;
    for (int round = 0; round < 10000; ++round) {
        h.scripted = {Scripted(5000 + round, QStringLiteral("r%1").arg(round), {0.0, 0.0, 0.2, 0.05}),
                      Scripted(20000 + round, QStringLiteral("s%1").arg(round), {0.0, 0.5, 0.2, 0.05})};
        h.a11y->RevokeAll();
        h.a11y->ContentChanged();
        QCOMPARE(h.a11y->ExposedLineCount(), 2);
        if (round % 1000 == 0) seen << QAccessible::uniqueId(h.Line(0)) << QAccessible::uniqueId(h.Line(1));
    }
    for (int i = 0; i + 2 < seen.size(); ++i) QVERIFY(!QAccessible::accessibleInterface(seen[i]));
    QVERIFY(h.a11y->Revocations() >= 19998);
}

void UctScreenTextAccessibilityTests::emptySnapshotIsEmptyRoot()
{
    // Show original, settling, composite pause, watchdog and an unusable
    // backend all reach the provider as "enabled, nothing drawn".
    Harness h;
    h.useScripted = true;
    h.a11y->SetEnabled(true);
    QVERIFY(h.Root());
    QCOMPARE(h.Root()->childCount(), 0);
    QCOMPARE(h.Root()->text(QAccessible::Name), QStringLiteral("Ultra Clear Text on this view"));
    QAccessibleInterface* host = QAccessible::queryAccessibleInterface(h.viewport);
    const QPoint centre = h.Viewport().center();
    QVERIFY(!host->childAt(centre.x(), centre.y())); // the viewport itself stays under the pointer
}

QTEST_MAIN(UctScreenTextAccessibilityTests)
#include "uct_screen_text_accessibility_tests.moc"
