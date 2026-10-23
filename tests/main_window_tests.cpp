#include "gui_test_support.hpp"
#include "okuflow/app/app.hpp"
#include "okuflow/app/language_manager.hpp"
#include "okuflow/common/visible_chat_session.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/agent_control_panel.hpp"
#include "okuflow/ui/collapsible_section.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/theme_contrast.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/render_widget.hpp"

#include <QApplication>
#include <QAccessible>
#include <QAbstractButton>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QAction>
#include <QPalette>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTextCursor>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyleHints>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <Windows.h>
#include <dwmapi.h>

namespace okuflow {

// MainWindow only calls these methods when setApp() has been given an
// OkuFlowApp. Keep its dependency at the UI seam; these definitions must never
// be exercised by the tests.
bool OkuFlowApp::HandlePanKey(int, bool) { qFatal("Unexpected app interaction"); return false; }
bool OkuFlowApp::HandlePanScroll(const QWheelEvent*) { qFatal("Unexpected app interaction"); return false; }
void OkuFlowApp::HandleZoomWheel(const QWheelEvent*) { qFatal("Unexpected app interaction"); }
void OkuFlowApp::HandleKeyboardZoom(float) { qFatal("Unexpected app interaction"); }
void OkuFlowApp::BeginMousePan(const QPointF&, const QSize&) { qFatal("Unexpected app interaction"); }
bool OkuFlowApp::UpdateMousePan(const QPointF&) { qFatal("Unexpected app interaction"); return false; }
void OkuFlowApp::EndMousePan() { qFatal("Unexpected app interaction"); }

namespace {

using test::ActivateFixtureWindow;

template <typename T>
T* Named(MainWindow& window, const char* name)
{
    return window.findChild<T*>(QString::fromLatin1(name));
}

QRect GlobalRect(QWidget* widget)
{
    return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
}

// Returns an empty string when the action buttons form a filled rectangle:
// every row shares the same outer edges and buttons in a row share a width.
QString ActionRowsProblem(MainWindow& window, int* rowCount)
{
    const QList<QPushButton*> actions{window.capturePhotoButton(), window.recordButton(),
                                      window.explainNowButton(),
                                      window.annotationButton()};
    QMap<int, QList<QRect>> rows;
    for (QPushButton* button : actions) {
        const QRect rect = GlobalRect(button);
        if (rect.height() < 58) {
            return QStringLiteral("%1 is only %2px tall").arg(button->text()).arg(rect.height());
        }
        rows[rect.y()].append(rect);
    }
    *rowCount = static_cast<int>(rows.size());
    if (*rowCount < 1 || *rowCount > 2) return QStringLiteral("Actions must form one or two rows");
    if (*rowCount == 2 && (rows.first().size() != 2 || rows.last().size() != 2))
        return QStringLiteral("Wrapped actions must form two above two");
    int left = 0;
    int right = 0;
    bool first = true;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        auto row = it.value();
        std::sort(row.begin(), row.end(),
                  [](const QRect& a, const QRect& b) { return a.x() < b.x(); });
        if (first) {
            left = row.first().left();
            right = row.last().right();
            first = false;
        }
        if (std::abs(row.first().left() - left) > 1 || std::abs(row.last().right() - right) > 1) {
            return QStringLiteral("Action row at y=%1 spans %2..%3, expected %4..%5")
                .arg(it.key()).arg(row.first().left()).arg(row.last().right())
                .arg(left).arg(right);
        }
        for (const QRect& rect : row) {
            if (std::abs(rect.width() - row.first().width()) > 4) {
                return QStringLiteral("Action row at y=%1 has unequal widths %2 and %3")
                    .arg(it.key()).arg(rect.width()).arg(row.first().width());
            }
        }
    }
    return {};
}

void AddQuickModes(MainWindow& window)
{
    auto* list = window.presetList();
    list->addItem(QStringLiteral("Read a page"));
    list->addItem(QStringLiteral("High contrast"));
    list->addItem(QStringLiteral("View a board"));
    list->setCurrentRow(0);
}

// Deterministic light/dark fixture palettes. Disabled roles start deliberately
// unreadable so the window's scheme-aware override is what makes them pass.
QPalette SchemePalette(bool light)
{
    QPalette palette = QApplication::palette();
    const auto set = [&palette](QPalette::ColorRole role, const char* hex) {
        palette.setColor(role, QColor(QString::fromLatin1(hex)));
    };
    if (light) {
        set(QPalette::Window, "#f3f3f3"); set(QPalette::WindowText, "#000000");
        set(QPalette::Base, "#ffffff"); set(QPalette::AlternateBase, "#e9e9e9");
        set(QPalette::Text, "#000000"); set(QPalette::Button, "#fbfbfb");
        set(QPalette::ButtonText, "#000000"); set(QPalette::Highlight, "#005da6");
        set(QPalette::HighlightedText, "#ffffff"); set(QPalette::Mid, "#8a8a8a");
        set(QPalette::Midlight, "#e3e3e3"); set(QPalette::Light, "#ffffff");
        set(QPalette::Dark, "#6d6d6d"); set(QPalette::Shadow, "#000000");
        set(QPalette::PlaceholderText, "#767676"); set(QPalette::BrightText, "#ffffff");
        set(QPalette::ToolTipBase, "#ffffff"); set(QPalette::ToolTipText, "#000000");
        set(QPalette::Link, "#005da6");
        const QColor faint(QStringLiteral("#c8c8c8"));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, faint);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, faint);
        palette.setColor(QPalette::Disabled, QPalette::Text, faint);
    } else {
        set(QPalette::Window, "#202020"); set(QPalette::WindowText, "#ffffff");
        set(QPalette::Base, "#2b2b2b"); set(QPalette::AlternateBase, "#353535");
        set(QPalette::Text, "#ffffff"); set(QPalette::Button, "#2b2b2b");
        set(QPalette::ButtonText, "#ffffff"); set(QPalette::Highlight, "#005da6");
        set(QPalette::HighlightedText, "#ffffff"); set(QPalette::Mid, "#5a5a5a");
        set(QPalette::Midlight, "#3a3a3a"); set(QPalette::Light, "#4a4a4a");
        set(QPalette::Dark, "#141414"); set(QPalette::Shadow, "#000000");
        set(QPalette::PlaceholderText, "#8a8a8a"); set(QPalette::BrightText, "#ffffff");
        set(QPalette::ToolTipBase, "#2b2b2b"); set(QPalette::ToolTipText, "#ffffff");
        set(QPalette::Link, "#4cc2ff");
        const QColor faint(QStringLiteral("#404040"));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, faint);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, faint);
        palette.setColor(QPalette::Disabled, QPalette::Text, faint);
    }
    return palette;
}

QRect ScaledWidgetRect(QWidget* widget, QWidget* window, const QImage& image,
                       int insetLogical, int leftTrimLogical = 0)
{
    const qreal ratio = image.devicePixelRatio();
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0));
    QRect rect(QPointF(topLeft.x() * ratio, topLeft.y() * ratio).toPoint(),
               QSizeF(widget->width() * ratio, widget->height() * ratio).toSize());
    const int inset = static_cast<int>(insetLogical * ratio);
    rect.adjust(inset + static_cast<int>(leftTrimLogical * ratio), inset, -inset, -inset);
    return rect.intersected(image.rect());
}

// Counts pixels readable (>= minimumRatio) against the region's majority
// color, so white text on a near-white surface scores zero.
int ReadablePixelCount(const QImage& image, const QRect& region, double minimumRatio)
{
    QMap<QRgb, int> histogram;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            ++histogram[image.pixel(x, y)];
        }
    }
    if (histogram.isEmpty()) return 0;
    QRgb background = histogram.firstKey();
    for (auto it = histogram.begin(); it != histogram.end(); ++it) {
        if (it.value() > histogram.value(background)) background = it.key();
    }
    int readable = 0;
    for (auto it = histogram.begin(); it != histogram.end(); ++it) {
        if (theme_contrast::ContrastRatio(QColor(it.key()), QColor(background)) >= minimumRatio) {
            readable += it.value();
        }
    }
    return readable;
}

QColor StyleSheetColor(const QString& styleSheet)
{
    const QRegularExpressionMatch match =
        QRegularExpression(QStringLiteral("color:\\s*(#[0-9a-fA-F]{6})")).match(styleSheet);
    return match.hasMatch() ? QColor(match.captured(1)) : QColor();
}

QColor IconInkColor(const QIcon& icon, int* inkPixels)
{
    const QImage image = icon.pixmap(QSize(48, 48)).toImage()
                             .convertToFormat(QImage::Format_ARGB32);
    long long red = 0, green = 0, blue = 0;
    *inkPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 200) {
                red += pixel.red(); green += pixel.green(); blue += pixel.blue();
                ++(*inkPixels);
            }
        }
    }
    if (*inkPixels == 0) return QColor();
    return QColor(static_cast<int>(red / *inkPixels),
                  static_cast<int>(green / *inkPixels),
                  static_cast<int>(blue / *inkPixels));
}

// These screenshots intentionally use a camera-free window. Mirror the app's
// initial language/Zoom state so the fixture does not depict inactive controls
// as enabled or display English as the selected language in translated images.
void SyncScreenshotState(MainWindow& window, const QString& languageCode)
{
    auto* languages = window.applicationLanguageCombo();
    const QSignalBlocker blocked(languages);
    languages->setCurrentIndex(languages->findData(languageCode));
    for (auto* slider : {window.zoomSlider(), window.zoomCenterXSlider(),
                         window.zoomCenterYSlider()}) {
        slider->setEnabled(window.zoomCheckbox()->isChecked());
    }
}

QList<QWidget*> CaptureWindows(QWidget& window)
{
    QList<QWidget*> result{&window};
    for (auto* peer : QApplication::topLevelWidgets()) {
        if (peer != &window && peer->isVisible() && window.isAncestorOf(peer))
            result.append(peer);
    }
    return result;
}

QRect CaptureBounds(QWidget& window)
{
    QRect result;
    for (auto* peer : CaptureWindows(window)) result = result.united(peer->frameGeometry());
    return result;
}

// Visual examples fit the real work area. Large geometry contracts below keep
// their original sizes and assertions, even when that size cannot be captured.
bool PrepareVisualWindow(QWidget& window)
{
    // Suppress only this fixture HWND's opening transition. A compositor flush
    // alone cannot prevent a genuine native grab from capturing its cross-fade.
    const BOOL disabled = TRUE;
    return SUCCEEDED(DwmSetWindowAttribute(reinterpret_cast<HWND>(window.winId()),
        DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled)));
}

QString FitVisualWindow(QWidget& window, const QSize& preferred)
{
    auto* screen = window.screen();
    if (!screen) return QStringLiteral("No native screen for visual fixture");
    const QRect available = screen->availableGeometry().adjusted(12, 12, -12, -12);
    const QSize frame = window.frameGeometry().size() - window.size();
    window.resize(preferred.boundedTo(available.size() - frame));
    window.move(available.topLeft());
    if (!QTest::qWaitFor([&] { return available.contains(CaptureBounds(window)); }, 1000))
        return QStringLiteral("Fixture minimum size does not fit native work area %1x%2")
            .arg(available.width()).arg(available.height());
    return {};
}

bool CaptureUnobstructed(QWidget& window)
{
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    if (foregroundProcess != GetCurrentProcessId()) return false;
    // Sample each visible native peer in physical coordinates, not Qt logical
    // coordinates. A foreign topmost window must not become screenshot evidence.
    for (auto* peer : CaptureWindows(window)) {
        const HWND handle = reinterpret_cast<HWND>(peer->winId());
        RECT client{};
        if (!IsWindowVisible(handle) || !GetClientRect(handle, &client)) return false;
        for (const int x : {1, 2, 3}) for (const int y : {1, 2, 3}) {
            POINT point{client.right * x / 4, client.bottom * y / 4};
            if (!ClientToScreen(handle, &point)) return false;
            DWORD owner = 0;
            GetWindowThreadProcessId(WindowFromPoint(point), &owner);
            if (owner != GetCurrentProcessId()) return false;
        }
    }
    return true;
}

bool NativeHitBelongsTo(QWidget* widget, QString* problem)
{
    auto* owner = widget->window();
    const HWND handle = reinterpret_cast<HWND>(owner->winId());
    const QPoint local = widget->mapTo(owner, widget->rect().center());
    const qreal ratio = owner->devicePixelRatioF();
    POINT point{qRound(local.x() * ratio), qRound(local.y() * ratio)};
    if (!ClientToScreen(handle, &point)) return false;
    const HWND hit = WindowFromPoint(point);
    const bool belongs = hit == handle || IsChild(handle, hit);
    if (!belongs) {
        RECT client{};
        GetClientRect(handle, &client);
        DWORD process = 0;
        GetWindowThreadProcessId(hit, &process);
        wchar_t name[128]{};
        GetClassNameW(hit, name, 128);
        *problem = QStringLiteral("Native footer %1 is covered/outside its owner: Qt owner %2x%3, "
            "native client %4x%5, local %6,%7, DPR %8, native point %9,%10, hit %11 PID %12")
            .arg(widget->objectName()).arg(owner->width()).arg(owner->height())
            .arg(client.right).arg(client.bottom).arg(local.x()).arg(local.y())
            .arg(ratio).arg(point.x).arg(point.y).arg(QString::fromWCharArray(name)).arg(process);
    }
    return belongs;
}

bool SaveScreenshot(QWidget& window, const QString& name)
{
    const QString root = qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS");
    if (root.isEmpty()) {
        return true;
    }
    // Native tool windows can be exposed before their first compositor frame.
    QTest::qWait(80);
    QCoreApplication::processEvents();
    QDir directory(root);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        qWarning().noquote() << "Cannot create screenshot directory:" << root;
        return false;
    }
    const QString path = directory.filePath(name + QStringLiteral(".png"));
    // Corner controls and the quick grid are native tool windows. Capture the
    // composed desktop region so the optional evidence includes those peers.
    QScreen* screen = window.screen();
    const QRect area = CaptureBounds(window);
    QString problem;
    QPixmap pixels;
    if (!screen || !screen->availableGeometry().contains(area))
        problem = QStringLiteral("window/native peers exceed available desktop");
    else if (!CaptureUnobstructed(window))
        problem = QStringLiteral("foreign foreground or obstructed native window");
    else {
        pixels = screen->grabWindow(0, area.x(), area.y(), area.width(), area.height());
        if (pixels.isNull()) problem = QStringLiteral("native grab failed");
        else if (CaptureBounds(window) != area || !CaptureUnobstructed(window))
            problem = QStringLiteral("native ownership/geometry changed during grab");
        else if (qAbs(pixels.width() - area.width() * pixels.devicePixelRatio()) > 1 ||
                 qAbs(pixels.height() - area.height() * pixels.devicePixelRatio()) > 1)
            problem = QStringLiteral("native grab dimensions do not cover requested bounds");
        else if (!pixels.save(path)) problem = QStringLiteral("cannot save native grab");
    }
    const auto rect = [](const QRect& value) {
        return QJsonArray{value.x(), value.y(), value.width(), value.height()};
    };
    const QJsonObject record{{"name", name}, {"saved", problem.isEmpty()}, {"problem", problem},
        {"available", screen ? rect(screen->availableGeometry()) : QJsonArray{}},
        {"bounds", rect(area)}, {"dpr", screen ? screen->devicePixelRatio() : 0.0},
        {"pixels", QJsonArray{pixels.width(), pixels.height()}}};
    QFile manifest(directory.filePath(QStringLiteral("native-captures.jsonl")));
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Append) ||
        manifest.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n') < 0) {
        qWarning().noquote() << "Cannot record native screenshot manifest:" << name;
        return false;
    }
    if (!problem.isEmpty()) qWarning().noquote() << "Refusing incomplete UI screenshot:" << name << problem;
    return problem.isEmpty();
}

} // namespace

class MainWindowTests final : public QObject {
    Q_OBJECT
private slots:
    void desktopVisualLayouts_data();
    void desktopVisualLayouts();
    void desktopVisualChat_data();
    void desktopVisualChat();
    void modesGridSearchAndGeometry_data();
    void modesGridSearchAndGeometry();
    void dependentControls();
    void desktopZoomSliderReachesSixteenX();
    void textClarityMasterPreservesRefinements();
    void ultraClearTextControlsHaveNativeGeometry();
    void ultraClearTextOverlayFollowsNativeViewport();
    void ultraClearTextOverlayYieldsToModalDialogs_data();
    void ultraClearTextOverlayYieldsToModalDialogs();
    void disabledSlidersHaveDistinctAppearance();
    void inspectorContrastAppearance_data();
    void inspectorContrastAppearance();
    void searchIconsStayInsideFields();
    void gridBrowsingRequiresDeliberateActivation();
    void carouselUsesAppliedModeDuringGridBrowse();
    void searchCanHandoffBetweenTabs();
    void activeSearchRecomputesAfterLanguageSwitch();
    void busyActionSurvivesGermanResize();
    void keyboardRegionsAndFindShortcut();
    void assistantInspectorTabTraversal();
    void floatingAssistantTabTraversal();
    void chromeFollowsNativeOwnerMove_data();
    void chromeFollowsNativeOwnerMove();
    void minimumViewportChromeFits_data();
    void minimumViewportChromeFits();
    void bottomChromeFormsRectangularGroups_data();
    void bottomChromeFormsRectangularGroups();
    void bottomChromeRetainsNativeSizeDuringRefresh_data();
    void bottomChromeRetainsNativeSizeDuringRefresh();
    void hideUiPreservesPriorModeAndStreaming_data();
    void hideUiPreservesPriorModeAndStreaming();
    void mouseFocusedChromeCanIdleFade();
    void unrelatedNativeInputCannotRevealChrome();
    void keyboardFocusedChromeSurvivesNativeHandoff();
    void simpleBackwardFocusCrossesPanels();
    void modalDialogKeepsTabAndNumberKeys();
    void streamingPreservesReadingState();
};

void MainWindowTests::desktopVisualLayouts_data()
{
    QTest::addColumn<int>("language");
    QTest::addColumn<QString>("code");
    QTest::newRow("english") << int(AppLanguage::English) << QStringLiteral("en");
    QTest::newRow("turkish") << int(AppLanguage::Turkish) << QStringLiteral("tr");
    QTest::newRow("german") << int(AppLanguage::German) << QStringLiteral("de");
}

void MainWindowTests::desktopVisualLayouts()
{
    QFETCH(int, language);
    QFETCH(QString, code);
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    QVERIFY(translations.SetLanguage(static_cast<AppLanguage>(language), false));
    MainWindow window;
    AddQuickModes(window);
    SyncScreenshotState(window, code);
    QVERIFY(PrepareVisualWindow(window));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QString fit = FitVisualWindow(window, QSize(1280, 900));
    QVERIFY2(fit.isEmpty(), qPrintable(fit));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    const QString prefix = QStringLiteral("desktop-%1-").arg(code);
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("simple")));

    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* grid = Named<QWidget>(window, "modeGridPopup");
    QVERIFY(gridButton && grid);
    QVERIFY2(ActivateFixtureWindow(gridButton), test::ActivationFailureMessage());
    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("grid")));
    QTest::keyClick(window.presetList(), Qt::Key_Escape);
    QTRY_VERIFY(!grid->isVisible());

    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("image")));
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* settings = Named<QWidget>(window, "settingsTabPage");
    QVERIFY(tabs && settings);
    tabs->setCurrentWidget(settings);
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("settings")));

    const QString narrowFit = FitVisualWindow(window, QSize(720, 900));
    QVERIFY2(narrowFit.isEmpty(), qPrintable(narrowFit));
    window.setAdvancedPanelWidth(360);
    QTRY_VERIFY(window.renderWidget()->width() >= 320);
    int rows = 0;
    QString problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("narrow")));
    if (!qEnvironmentVariableIsEmpty("OKUFLOW_UI_TEST_SCREENSHOTS")) {
        // An oversized geometry case must never produce a clipped evidence PNG.
        window.resize(window.screen()->availableGeometry().width() + 100, window.height());
        QTRY_VERIFY(!window.screen()->availableGeometry().contains(CaptureBounds(window)));
        const QString rejected = prefix + QStringLiteral("rejected-offscreen");
        QVERIFY(!SaveScreenshot(window, rejected));
        QVERIFY(!QFileInfo::exists(QDir(qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS"))
            .filePath(rejected + QStringLiteral(".png"))));
    }
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::desktopVisualChat_data()
{
    QTest::addColumn<bool>("light");
    QTest::newRow("light") << true;
    QTest::newRow("dark") << false;
}

void MainWindowTests::desktopVisualChat()
{
    QFETCH(bool, light);
    const QPalette original = QApplication::palette();
    auto* hints = QApplication::styleHints();
    const auto originalScheme = hints->colorScheme();
    const auto restore = qScopeGuard([&] {
        hints->setColorScheme(originalScheme);
        QApplication::setPalette(original);
    });
    // A palette alone does not switch native Windows checkbox/style assets.
    // This overrides only this test process, leaving the OS theme unchanged.
    const auto scheme = light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark;
    hints->setColorScheme(scheme);
    QTRY_COMPARE(hints->colorScheme(), scheme);
    QApplication::setPalette(SchemePalette(light));
    QCoreApplication::processEvents();
    MainWindow window;
    QVERIFY(PrepareVisualWindow(window));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QString fit = FitVisualWindow(window, QSize(1200, 900));
    QVERIFY2(fit.isEmpty(), qPrintable(fit));
    window.advancedModeButton()->click();
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* page = Named<QWidget>(window, "assistantTabPage");
    QVERIFY(tabs && page);
    tabs->setCurrentWidget(page);
    window.setAdvancedPanelWidth(440);
    AssistiveOverlay overlay(window.renderWidget());
    overlay.ShowChat(false);
    QVERIFY(PrepareVisualWindow(overlay));
    VisibleChatSession session;
    connect(&session, &VisibleChatSession::SnapshotChanged, window.assistantChatView(), &AssistantChatView::SetChatSnapshot);
    connect(&session, &VisibleChatSession::SnapshotChanged, &overlay, &AssistiveOverlay::SetChatSnapshot);
    const auto ticket = session.Admit(QStringLiteral("Explain this synthetic frame"), ChatBackendKind::Explain);
    QVERIFY(ticket);
    QImage frame(240, 120, QImage::Format_RGB32);
    frame.fill(Qt::green);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(frame.save(&buffer, "JPG"));
    QVERIFY(session.SetImagePreview(*ticket, jpeg));
    const QString answer = QStringLiteral(
        "This synthetic answer wraps across several lines. The native visual fixture checks the whole window, "
        "including its thumbnail, reading action, attachment switch and keyboard focus. ").repeated(3);
    QVERIFY(session.Complete(*ticket, answer));
    const QString prefix = QStringLiteral("desktop-chat-%1-").arg(light ? "light" : "dark");
    for (auto* view : {window.assistantChatView(), overlay.chatView()}) {
        if (view == overlay.chatView()) {
            overlay.ShowChat(true);
            QTRY_VERIFY(overlay.isVisible());
        }
        QVERIFY2(ActivateFixtureWindow(view), test::ActivationFailureMessage());
        QTRY_VERIFY(view->transcript()->toPlainText().contains(answer));
        // Text arrives synchronously, while the splitter/viewport resize
        // posts the width-dependent transcript render to the next event turn.
        // Wait for the same wrapping invariant after that layout completes.
        QTRY_COMPARE(view->transcript()->horizontalScrollBar()->maximum(), 0);
        for (QWidget* action : {static_cast<QWidget*>(view->readAloudButton()),
                                static_cast<QWidget*>(view->attachmentCheckbox()),
                                static_cast<QWidget*>(view->sendButton())}) {
            QVERIFY(action->isVisible());
            QVERIFY(view->rect().contains(QRect(action->mapTo(view, QPoint()), action->size())));
            // setAdvancedPanelWidth queues its splitter resize. Wait for the
            // resulting native layout, not just the updated text snapshot.
            QString hitProblem;
            const bool hit = QTest::qWaitFor([&] { return NativeHitBelongsTo(action, &hitProblem); }, 1000);
            if (!hit) SaveScreenshot(window, prefix + QStringLiteral("diagnostic-footer"));
            QVERIFY2(hit, qPrintable(hitProblem));
        }
        view->attachmentCheckbox()->setChecked(true);
        view->attachmentCheckbox()->setFocus();
        QTRY_VERIFY(view->attachmentCheckbox()->hasFocus());
        view->transcript()->verticalScrollBar()->setValue(0);
        QVERIFY(SaveScreenshot(window, prefix + (view == overlay.chatView() ? QStringLiteral("floating") : QStringLiteral("advanced"))));
        view->transcript()->verticalScrollBar()->setValue(view->transcript()->verticalScrollBar()->maximum());
        view->attachmentCheckbox()->setChecked(false);
        view->readAloudButton()->setFocus();
        QTRY_VERIFY(view->readAloudButton()->hasFocus());
        QVERIFY(SaveScreenshot(window, prefix + (view == overlay.chatView() ? QStringLiteral("floating-reading") : QStringLiteral("advanced-reading"))));
    }
    const QPoint oldFrame = window.frameGeometry().topLeft();
    const QPoint oldOverlay = overlay.frameGeometry().topLeft();
    window.move(oldFrame + QPoint(8, 8));
    QTRY_COMPARE(overlay.frameGeometry().topLeft() - oldOverlay, QPoint(8, 8));
    QVERIFY(SaveScreenshot(window, prefix + QStringLiteral("moved")));

    // Native Details evidence uses the production panel with synthetic data,
    // without any connected app service capable of approving a real action.
    QDialog dialog(&window);
    QVERIFY(PrepareVisualWindow(dialog));
    auto* layout = new QVBoxLayout(&dialog);
    auto* panel = new AgentControlPanel(&dialog);
    layout->addWidget(panel);
    QJsonObject changes;
    for (int i = 0; i < 40; ++i)
        changes.insert(QStringLiteral("field-%1").arg(i, 2, 10, QLatin1Char('0')), QString(80, QLatin1Char('x')));
    QJsonObject approval{{"approvalId", "visual-fixture"}, {"tool", "capture.photo.take"},
        {"kind", "action"}, {"targetIdentity", "synthetic-target"}, {"expiresInMs", 300000},
        {"normalizedChanges", changes}};
    panel->SetPendingApproval(approval);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const QString dialogFit = FitVisualWindow(dialog, QSize(600, 600));
    QVERIFY2(dialogFit.isEmpty(), qPrintable(dialogFit));
    QVERIFY2(ActivateFixtureWindow(&dialog), test::ActivationFailureMessage());
    auto* detailsButton = panel->findChild<QPushButton*>(QStringLiteral("agentApprovalDetails"));
    auto* details = panel->findChild<QPlainTextEdit*>(QStringLiteral("agentApprovalArguments"));
    QVERIFY(detailsButton && details);
    detailsButton->click();
    QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
    details->verticalScrollBar()->setValue(details->verticalScrollBar()->maximum() / 2);
    auto cursor = details->cursorForPosition(details->viewport()->rect().center());
    cursor.setPosition(std::min(cursor.position() + 30, details->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    details->setTextCursor(cursor);
    QVERIFY(!cursor.selectedText().isEmpty());
    details->setFocus();
    QTRY_VERIFY(details->hasFocus());
    const int scroll = details->verticalScrollBar()->value();
    QVERIFY(SaveScreenshot(dialog, prefix + QStringLiteral("approval-before")));
    approval["expiresInMs"] = 299000;
    panel->SetPendingApproval(approval);
    QCOMPARE(details->textCursor().anchor(), cursor.anchor());
    QCOMPARE(details->textCursor().position(), cursor.position());
    QCOMPARE(details->verticalScrollBar()->value(), scroll);
    QVERIFY(SaveScreenshot(dialog, prefix + QStringLiteral("approval-after")));
}

void MainWindowTests::streamingPreservesReadingState()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.advancedModeButton()->click();
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* assistantPage = Named<QWidget>(window, "assistantTabPage");
    QVERIFY(tabs && assistantPage);
    tabs->setCurrentWidget(assistantPage);
    AssistiveOverlay overlay(window.renderWidget());
    VisibleChatSession session;
    connect(&session, &VisibleChatSession::SnapshotChanged, window.assistantChatView(), &AssistantChatView::SetChatSnapshot);
    connect(&session, &VisibleChatSession::SnapshotChanged, &overlay, &AssistiveOverlay::SetChatSnapshot);
    const auto ticket = session.Admit(QStringLiteral("Explain this view"), ChatBackendKind::Explain);
    QVERIFY(ticket);
    overlay.ShowChat(true);
    const QString answer = QStringLiteral("A long answer line for reading.\n").repeated(100);
    QVERIFY(session.AppendDelta(*ticket, answer));
    const std::array<QTextBrowser*, 2> bodies{
        window.assistantChatView()->transcript(), overlay.chatView()->transcript()};
    QCoreApplication::processEvents();
    std::array<QString, 2> selections;
    for (size_t index = 0; index < bodies.size(); ++index) {
        auto* body = bodies[index];
        QVERIFY(body && body->isVisible());
        QTextCursor reading(body->document());
        const int start = body->toPlainText().indexOf(answer.left(20));
        QVERIFY(start >= 0);
        reading.setPosition(start + 3);
        reading.setPosition(start + 18, QTextCursor::KeepAnchor);
        body->setTextCursor(reading);
        selections[index] = reading.selectedText();
        body->verticalScrollBar()->setValue(0);
    }
    QVERIFY(session.AppendDelta(*ticket, QStringLiteral("New streamed text")));
    for (size_t index = 0; index < bodies.size(); ++index) {
        auto* body = bodies[index];
        QCOMPARE(body->textCursor().selectedText(), selections[index]);
        QCOMPARE(body->verticalScrollBar()->value(), 0);
        auto reading = body->textCursor();
        reading.clearSelection();
        body->setTextCursor(reading);
        body->verticalScrollBar()->setValue(body->verticalScrollBar()->maximum());
    }
    QVERIFY(session.AppendDelta(*ticket, QStringLiteral("\nMore text")));
    for (auto* body : bodies)
        QCOMPARE(body->verticalScrollBar()->value(), body->verticalScrollBar()->maximum());

    // Read has an independent result pane. Its stream preserves selection and
    // scrolling without replacing or hiding either shared Explain view.
    const QString chatText = bodies[1]->toPlainText();
    overlay.SetReadContent(QStringLiteral("Read"), answer, true);
    auto* readBody = overlay.ReadBody();
    QVERIFY(readBody);
    QTextCursor reading(readBody->document());
    reading.setPosition(3); reading.setPosition(18, QTextCursor::KeepAnchor);
    readBody->setTextCursor(reading);
    const QString readSelection = reading.selectedText();
    readBody->verticalScrollBar()->setValue(0);
    overlay.SetReadContent(QStringLiteral("Read"), answer + QStringLiteral("More reading"), true);
    QCOMPARE(readBody->textCursor().selectedText(), readSelection);
    QCOMPARE(readBody->verticalScrollBar()->value(), 0);
    QCOMPARE(bodies[1]->toPlainText(), chatText);
    QVERIFY(overlay.isVisible());
}

void MainWindowTests::desktopZoomSliderReachesSixteenX()
{
    MainWindow window;
    auto* slider = window.zoomSlider();
    QVERIFY(slider);
    QCOMPARE(slider->minimum(), 100);
    QCOMPARE(slider->maximum(), 1600);
    slider->setValue(1600);
    QCOMPARE(slider->value(), 1600);
    slider->setValue(1601);
    QCOMPARE(slider->value(), 1600);
    slider->setValue(1200);
    QCOMPARE(slider->value(), 1200);
}

void MainWindowTests::modesGridSearchAndGeometry_data()
{
    QTest::addColumn<int>("language");
    QTest::addColumn<QString>("code");
    QTest::newRow("english") << int(AppLanguage::English) << QStringLiteral("en");
    QTest::newRow("turkish") << int(AppLanguage::Turkish) << QStringLiteral("tr");
    QTest::newRow("german") << int(AppLanguage::German) << QStringLiteral("de");
}

void MainWindowTests::modesGridSearchAndGeometry()
{
    QFETCH(int, language);
    QFETCH(QString, code);
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    QVERIFY(translations.SetLanguage(static_cast<AppLanguage>(language), false));

    MainWindow window;
    window.resize(1280, 720);
    SyncScreenshotState(window, code);
    AddQuickModes(window);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QCoreApplication::processEvents();
    QVERIFY(window.isSimpleMode());
    QVERIFY(window.simpleModeButton()->isChecked());
    QVERIFY(!window.advancedModeButton()->isChecked());
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* grid = Named<QWidget>(window, "modeGridPopup");
    auto* current = Named<QPushButton>(window, "currentModeButton");
    QVERIFY(gridButton && grid && current);
    SaveScreenshot(window, code + QStringLiteral("-simple"));

    QVERIFY2(ActivateFixtureWindow(gridButton), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(gridButton->window()));
    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    QTRY_VERIFY(window.presetList()->hasFocus());
    QCOMPARE(window.presetList()->count(), 3);
    SaveScreenshot(window, code + QStringLiteral("-grid"));
    QTest::keyClick(window.presetList(), Qt::Key_Escape);
    QTRY_VERIFY(!grid->isVisible());
    const bool restoredFocus = QTest::qWaitFor([&] { return current->hasFocus(); }, 1500);
    QWidget* focus = QApplication::focusWidget();
    QWidget* active = QApplication::activeWindow();
    DWORD foregroundProcess = 0;
    const HWND foregroundWindow = GetForegroundWindow();
    GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
    wchar_t foregroundClass[128]{};
    GetClassNameW(foregroundWindow, foregroundClass, 128);
    const QString focusDetails = QStringLiteral(
        "Escape focus: widget=%1 class=%2 active=%3 foregroundPid=%4 ownPid=%5 nativeClass=%6")
        .arg(focus ? focus->objectName() : QStringLiteral("<none>"))
        .arg(focus ? QString::fromLatin1(focus->metaObject()->className())
                   : QStringLiteral("<none>"))
        .arg(active ? active->objectName() : QStringLiteral("<none>"))
        .arg(foregroundProcess).arg(GetCurrentProcessId())
        .arg(QString::fromWCharArray(foregroundClass));
    QVERIFY2(restoredFocus, qPrintable(focusDetails));

    QTest::keyClick(&window, Qt::Key_2);
    QCOMPARE(window.presetList()->currentRow(), 1);
    QCOMPARE(current->text(), QStringLiteral("High contrast"));
    Named<QPushButton>(window, "nextModeButton")->click();
    QCOMPARE(window.presetList()->currentRow(), 2);
    Named<QPushButton>(window, "previousModeButton")->click();
    QCOMPARE(window.presetList()->currentRow(), 1);

    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    QVERIFY(window.advancedModeButton()->isChecked());
    auto* splitter = Named<QSplitter>(window, "advancedContentSplitter");
    auto* advanced = Named<QTabWidget>(window, "advancedPage");
    auto* imagePage = Named<QWidget>(window, "imageTabPage");
    auto* assistantPage = Named<QWidget>(window, "assistantTabPage");
    auto* settingsPage = Named<QWidget>(window, "settingsTabPage");
    QVERIFY(splitter && advanced && imagePage && assistantPage && settingsPage);
    QTRY_VERIFY(advanced->isVisible());
    QCOMPARE(advanced->count(), 3);
    QCOMPARE(advanced->widget(0), imagePage);
    QCOMPARE(advanced->widget(1), assistantPage);
    QCOMPARE(advanced->widget(2), settingsPage);
    QTRY_VERIFY_WITH_TIMEOUT(advanced->width() >= 500, 1000);
    SaveScreenshot(window, code + QStringLiteral("-image-default"));
    for (int index = 0; index < advanced->count(); ++index) {
        // The tab style has 2 px horizontal padding on each side.
        const int requiredWidth = advanced->tabBar()->fontMetrics()
                                      .horizontalAdvance(advanced->tabText(index)) + 4;
        const int actualWidth = advanced->tabBar()->tabRect(index).width();
        const QString widthDetails = QStringLiteral(
            "Tab %1 (%2) clipped: rect %3 px, required %4 px, inspector %5 px, bar %6 px")
            .arg(index).arg(advanced->tabText(index)).arg(actualWidth)
            .arg(requiredWidth).arg(advanced->width())
            .arg(advanced->tabBar()->width());
        QVERIFY2(actualWidth >= requiredWidth, qPrintable(widthDetails));
    }
    QVERIFY(splitter->geometry().width() > window.renderWidget()->width());
    QVERIFY(imagePage->isAncestorOf(window.zoomCheckbox()));
    QVERIFY(imagePage->isAncestorOf(window.textClarityCheckbox()));
    QVERIFY(settingsPage->isAncestorOf(window.cameraCombo()));
    QVERIFY(settingsPage->isAncestorOf(window.phoneLightButton()));
    QVERIFY(!window.phoneLightButton()->isVisible());
    QVERIFY(!window.phoneLightButton()->isEnabled());
    QVERIFY(settingsPage->isAncestorOf(window.viewportFitCombo()));
    QVERIFY(settingsPage->isAncestorOf(window.recordingCanvasCombo()));
    QVERIFY(settingsPage->isAncestorOf(window.applicationLanguageCombo()));
    auto* imageSearch = Named<QLineEdit>(window, "imageSettingsSearch");
    auto* sharedSearch = Named<QLineEdit>(window, "sharedSettingsSearch");
    QVERIFY(imageSearch && sharedSearch);
    const auto initialStates = window.sectionStates();
    QVERIFY(!initialStates.isEmpty());
    auto collapsed = initialStates;
    collapsed[QStringLiteral("magnification")] = false;
    collapsed[QStringLiteral("device")] = false;
    window.setSectionStates(collapsed);
    QCOMPARE(window.sectionStates(), collapsed);
    imageSearch->setText(TranslateUi(QStringLiteral("Zoom")));
    QCoreApplication::processEvents();
    imageSearch->clear();
    QCoreApplication::processEvents();
    QCOMPARE(window.sectionStates(), collapsed);
    advanced->setCurrentWidget(settingsPage);
    sharedSearch->setText(TranslateUi(QStringLiteral("Viewport framing")));
    QCoreApplication::processEvents();
    QVERIFY(window.viewportFitCombo()->isVisible());
    sharedSearch->clear();
    QCoreApplication::processEvents();
    QCOMPARE(window.sectionStates(), collapsed);
    window.blackWhiteCheckbox()->setChecked(!window.blackWhiteCheckbox()->isChecked());
    QCoreApplication::processEvents();
    QCOMPARE(window.sectionStates(), collapsed);

    window.resize(960, 680);
    QCoreApplication::processEvents();
    SaveScreenshot(window, code + QStringLiteral("-settings-narrow"));
    advanced->setCurrentWidget(imagePage);
    QCoreApplication::processEvents();
    SaveScreenshot(window, code + QStringLiteral("-image-narrow"));
    QVERIFY(window.renderWidget()->width() > 0);
    QVERIFY(splitter->widget(1)->width() > 0);
    window.simpleModeButton()->click();
    QTRY_VERIFY(window.isSimpleMode());
    QVERIFY(!advanced->isVisible());
    QTRY_VERIFY(window.renderWidget()->width() > splitter->width() / 2);
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::dependentControls()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.setKeystoneTrackingControls(true, true, false, false, true, false, 1, 2);
    auto buttons = window.findChildren<QPushButton*>();
    bool enabledPause = false;
    bool disabledNext = false;
    for (auto* button : buttons) {
        if (button->accessibleName().contains(QStringLiteral("Stop Automatic Screen Correction"))) {
            enabledPause |= button->isEnabled();
        }
        if (button->accessibleName().contains(QStringLiteral("Next Screen Correction"))) {
            disabledNext |= !button->isEnabled();
        }
    }
    QVERIFY(enabledPause);
    QVERIFY(disabledNext);
    window.setKeystoneTrackingControls(true, true, true, true, true, false, 1, 2);
    bool enabledNext = false;
    for (auto* button : buttons) {
        if (button->accessibleName().contains(QStringLiteral("Next Screen Correction"))) {
            enabledNext |= button->isEnabled();
        }
    }
    QVERIFY(enabledNext);
}

void MainWindowTests::textClarityMasterPreservesRefinements()
{
    MainWindow window;
    auto* master = window.textClarityCheckbox();
    auto* flatten = window.backgroundFlattenCheckbox();
    auto* adaptive = window.adaptiveBinarizationCheckbox();
    auto* refinements = Named<QWidget>(window, "textClarityRefinements");
    auto* help = Named<QLabel>(window, "textClarityHelp");
    auto* imageScroll = Named<QScrollArea>(window, "imageSettingsScroll");
    QVERIFY(master && flatten && adaptive && refinements && help && imageScroll);
    auto* fine = qobject_cast<CollapsibleSection*>(refinements->findChild<CollapsibleSection*>());
    QVERIFY(fine);
    QCOMPARE(fine->persistKey(), QStringLiteral("textClarityFine"));
    QVERIFY(fine->headerWidget()->isEnabled());
    QVERIFY(refinements->isAncestorOf(flatten));
    QVERIFY(refinements->isAncestorOf(adaptive));
    QVERIFY(refinements->isAncestorOf(help));
    QVERIFY(help->wordWrap());
    QVERIFY(refinements->layout()->contentsMargins().left() >= 12);
    fine->setExpanded(true);

    master->setChecked(true);
    flatten->setChecked(true);
    adaptive->setChecked(true);
    QVERIFY(flatten->isEnabled() && adaptive->isEnabled());
    master->setChecked(false);
    QVERIFY(flatten->isChecked() && adaptive->isChecked());
    QVERIFY(!fine->contentWidget()->isEnabled());
    QVERIFY(!flatten->isEnabled() && !adaptive->isEnabled());
    QVERIFY(fine->headerWidget()->isEnabled());
    QCOMPARE(fine->contentWidget()->accessibleDescription(), master->toolTip());

    master->setChecked(true);
    QVERIFY(fine->contentWidget()->isEnabled());
    QVERIFY(flatten->isChecked() && adaptive->isChecked());
    QVERIFY(flatten->isEnabled() && adaptive->isEnabled());
    {
        const QSignalBlocker blocked(master);
        master->setChecked(false);
    }
    window.refreshTextClarityUi();
    QVERIFY(!fine->contentWidget()->isEnabled());
    QVERIFY(flatten->isChecked() && adaptive->isChecked());
    {
        const QSignalBlocker blocked(master);
        master->setChecked(true);
    }
    window.refreshTextClarityUi();
    QVERIFY(flatten->isEnabled() && adaptive->isEnabled());

    // Exercise the expanded nested group at its minimum inspector width
    // without opening or focusing a native desktop window.
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(720, 720);
    window.show();
    window.advancedModeButton()->click();
    window.setAdvancedPanelWidth(360);
    QTRY_VERIFY_WITH_TIMEOUT(imageScroll->viewport()->width() >= 300, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        imageScroll->widget()->width() <= imageScroll->viewport()->width() + 1, 1000);
    QVERIFY(fine->isExpanded());
    QVERIFY(fine->headerWidget()->isEnabled());
    QVERIFY(help->isVisible() && help->isEnabled());
    QVERIFY(help->width() <= imageScroll->viewport()->width());
}

void MainWindowTests::ultraClearTextOverlayFollowsNativeViewport()
{
    MainWindow window;
    QVERIFY(PrepareVisualWindow(window));
    window.show();
    const QString fit = FitVisualWindow(window, QSize(1100, 760));
    QVERIFY2(fit.isEmpty(), qPrintable(fit));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    auto* target = window.renderWidget();
    auto* overlay = window.ultraClearTextOverlay();
    QVERIFY(target && overlay);
    const auto transformFor = [&](float zoom, float centerX) {
        return ComputeViewTransform(1600, 900, target->width(), target->height(),
                                    zoom, centerX, 0.5f, ViewportFitMode::kFill);
    };
    overlay->SetViewTransform(transformFor(1.0f, 0.5f));
    // Known accepted words exercise the production native overlay. This
    // fixture deliberately does not open a camera or claim OCR accuracy.
    overlay->SetWords({{QStringLiteral("Reading"), QRectF(0.28, 0.35, 0.23, 0.07),
                         {QColor(Qt::black), QColor(Qt::white)}, 1},
                       {QStringLiteral("clearly"), QRectF(0.54, 0.35, 0.20, 0.07),
                         {QColor(Qt::black), QColor(Qt::white)}, 1}});
    QTRY_VERIFY(overlay->isVisible());
    QTRY_VERIFY(overlay->PaintCount() > 0);
    QCOMPARE(GlobalRect(overlay), GlobalRect(target));
    QVERIFY(SaveScreenshot(window, QStringLiteral("desktop-en-ultra-clear-overlay-before-pan")));
    const auto before = overlay->PaintCount();
    const ViewTransform zoomed = transformFor(1.4f, 0.48f);
    QVERIFY(zoomed.valid);
    QVERIFY(zoomed.sourceWidth < overlay->CurrentViewTransform().sourceWidth);
    overlay->SetViewTransform(zoomed);
    QTRY_VERIFY(overlay->PaintCount() > before);
    QCOMPARE(overlay->WordCount(), 2);
    const auto words = overlay->ProjectedWordRects();
    QCOMPARE(words.size(), 2);
    const QRectF expected = uct::SceneRectToView(zoomed, target->width(), target->height(),
                                                QRectF(0.28, 0.35, 0.23, 0.07));
    QVERIFY(QLineF(words.first().topLeft(), expected.topLeft()).length() < 0.5);
    window.resize(window.width() - 80, window.height() - 40);
    QTRY_COMPARE(GlobalRect(overlay), GlobalRect(target));
    overlay->SetViewTransform(transformFor(1.2f, 0.5f));
    QTRY_VERIFY(overlay->PaintCount() > before + 1);
    QVERIFY(SaveScreenshot(window, QStringLiteral("desktop-en-ultra-clear-overlay")));
    window.hide();
    QTRY_VERIFY(!overlay->isVisible());
}

void MainWindowTests::ultraClearTextOverlayYieldsToModalDialogs_data()
{
    QTest::addColumn<bool>("nativeFileDialog");
    QTest::newRow("qt-dialog") << false;
    QTest::newRow("native-open-video-dialog") << true;
}

void MainWindowTests::ultraClearTextOverlayYieldsToModalDialogs()
{
    QFETCH(bool, nativeFileDialog);
    MainWindow window;
    QVERIFY(PrepareVisualWindow(window));
    window.show();
    const QString fit = FitVisualWindow(window, QSize(1100, 760));
    QVERIFY2(fit.isEmpty(), qPrintable(fit));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    auto* overlay = window.ultraClearTextOverlay();
    QVERIFY(overlay);
    const QVector<UltraClearTextOverlay::SceneWord> words{
        {QStringLiteral("Reading clearly"), QRectF(.2, .35, .6, .1),
         {QColor(Qt::white), QColor(Qt::black)}, 1}};
    auto* target = window.renderWidget();
    overlay->SetViewTransform(ComputeViewTransform(1600, 900, target->width(), target->height(),
        1.0f, .5f, .5f, ViewportFitMode::kFill));
    overlay->SetWords(words);
    QTRY_VERIFY(overlay->isVisible());
    QTRY_VERIFY(overlay->PaintedGeometryCurrent());

    std::unique_ptr<QDialog> dialog;
    if (nativeFileDialog) {
        auto picker = std::make_unique<QFileDialog>(&window);
        picker->setWindowTitle(QStringLiteral("Open video"));
        picker->setDirectory(QDir::tempPath());
        picker->setFileMode(QFileDialog::ExistingFile);
        picker->setNameFilter(QStringLiteral("Video files (*.mp4 *.mkv)"));
        QVERIFY(!picker->testOption(QFileDialog::DontUseNativeDialog));
        dialog = std::move(picker);
    } else {
        dialog = std::make_unique<QDialog>(&window);
        dialog->setWindowTitle(QStringLiteral("UCT dialog stacking regression"));
    }
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
    QTRY_VERIFY(dialog->isVisible());
    if (nativeFileDialog) {
        // Refuse a Qt fallback: this row must exercise the Windows picker
        // used by Open video, not only the QWidget modality contract.
        const HWND owner = reinterpret_cast<HWND>(window.winId());
        QVERIFY(QTest::qWaitFor([&] {
            const HWND foreground = GetForegroundWindow();
            DWORD process = 0;
            GetWindowThreadProcessId(foreground, &process);
            wchar_t windowClass[64]{};
            GetClassNameW(foreground, windowClass, 64);
            return process == GetCurrentProcessId() &&
                GetAncestor(foreground, GA_ROOTOWNER) == owner &&
                QString::fromWCharArray(windowClass) == QStringLiteral("#32770");
        }, 1000));
    }
    QTRY_VERIFY(!overlay->isVisible());
    QVERIFY(!overlay->PaintedGeometryCurrent());
    overlay->SetWords(words); // OCR can finish inside the modal event loop.
    window.move(window.pos() + QPoint(2, 2));
    QApplication::processEvents();
    QVERIFY(!overlay->isVisible());
    QCOMPARE(overlay->WordCount(), 1);
    // The native file picker has no visible QWidget HWND; the normal peer
    // capture helper must not manufacture one just to take a screenshot.
    if (!nativeFileDialog)
        QVERIFY(SaveScreenshot(window, QStringLiteral("desktop-en-uct-modal-dialog-no-overlay")));
    dialog->reject();
    QTRY_VERIFY(!dialog->isVisible());
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QTRY_VERIFY(overlay->isVisible());
    QTRY_VERIFY(overlay->PaintedGeometryCurrent());
    QCOMPARE(overlay->WordCount(), 1);
    QCOMPARE(GlobalRect(overlay), GlobalRect(target));
    QVERIFY(SaveScreenshot(window, nativeFileDialog
        ? QStringLiteral("desktop-en-uct-native-file-dialog-closed-overlay-restored")
        : QStringLiteral("desktop-en-uct-modal-dialog-closed-overlay-restored")));
}

void MainWindowTests::ultraClearTextControlsHaveNativeGeometry()
{
    MainWindow window;
    QVERIFY(PrepareVisualWindow(window));
    window.show();
    const QString fit = FitVisualWindow(window, QSize(1380, 1000));
    QVERIFY2(fit.isEmpty(), qPrintable(fit));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    auto* panel = window.ultraClearTextPanel();
    auto* scroll = Named<QScrollArea>(window, "imageSettingsScroll");
    QVERIFY(panel && scroll);
    for (QWidget* parent = panel->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* section = qobject_cast<CollapsibleSection*>(parent)) section->setExpanded(true);
    }
    window.textClarityCheckbox()->setChecked(false);
    auto* enabled = panel->enableCheckbox();
    auto* original = panel->showOriginalCheckbox();
    QVERIFY(!enabled->isChecked());
    QVERIFY(!enabled->isEnabled());
    QVERIFY(!original->isEnabled());
    window.textClarityCheckbox()->setChecked(true);
    QVERIFY(enabled->isEnabled());
    scroll->ensureWidgetVisible(panel);
    QTRY_VERIFY(GlobalRect(scroll->viewport()).contains(GlobalRect(enabled)));
    QVERIFY2(ActivateFixtureWindow(enabled), test::ActivationFailureMessage());
    enabled->setFocus();
    QTRY_VERIFY(enabled->hasFocus());
    QTest::keyClick(enabled, Qt::Key_Space);
    QVERIFY(enabled->isChecked());
    QVERIFY(original->isEnabled());
    QTest::keyClick(enabled, Qt::Key_Tab);
    QTRY_VERIFY(original->hasFocus());
    QTest::keyClick(original, Qt::Key_Space);
    QVERIFY(original->isChecked());
    window.textClarityCheckbox()->setChecked(false);
    QVERIFY(enabled->isChecked()); // master gating preserves the saved preference
    QVERIFY(!enabled->isEnabled() && !original->isEnabled());
    window.textClarityCheckbox()->setChecked(true);
    QVERIFY(enabled->isChecked() && enabled->isEnabled() && original->isEnabled());
    for (QWidget* widget : {static_cast<QWidget*>(enabled), static_cast<QWidget*>(original),
                            static_cast<QWidget*>(panel->readingPane())}) {
        auto* accessible = QAccessible::queryAccessibleInterface(widget);
        QVERIFY(accessible && accessible->isValid());
        // Native styles expose the indicator/label click area for a checkbox,
        // rather than its layout's unused horizontal space. It must still be
        // nonempty and registered inside the actual screen-space widget.
        QVERIFY(!accessible->rect().isEmpty());
        QVERIFY2(GlobalRect(widget).contains(accessible->rect()),
                 qPrintable(QStringLiteral("Accessible bounds displaced for %1").arg(widget->objectName())));
        QVERIFY(!accessible->state().invisible);
    }
    auto* toggle = QAccessible::queryAccessibleInterface(enabled);
    QCOMPARE(toggle->role(), QAccessible::CheckBox);
    QVERIFY(toggle->state().checked);
    QVERIFY(panel->readingPane()->isReadOnly());
    QVERIFY(SaveScreenshot(window, QStringLiteral("desktop-en-ultra-clear-controls")));
    QString problem;
    QVERIFY2(NativeHitBelongsTo(enabled, &problem), qPrintable(problem));
}

void MainWindowTests::disabledSlidersHaveDistinctAppearance()
{
    MainWindow window;
    QPalette fixturePalette = window.palette();
    // Test the active accent independently of Windows' inactive-window theme.
    fixturePalette.setColor(QPalette::Highlight, QColor(QStringLiteral("#0078e0")));
    window.setPalette(fixturePalette);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1280, 720);
    window.show();
    window.setSimpleMode(false);
    auto expanded = window.sectionStates();
    for (auto it = expanded.begin(); it != expanded.end(); ++it) it.value() = true;
    window.setSectionStates(expanded);
    QCoreApplication::processEvents();
    const auto filledColor = [](QSlider* slider) {
        // Sample the centre of the visible filled track, away from its handle
        // and rounded ends; account for the screenshot's device pixel ratio.
        const QPoint point(slider->width() / 4, slider->height() / 2);
        const QImage image = slider->grab().toImage();
        return image.pixelColor(point * image.devicePixelRatio());
    };
    auto* master = window.textClarityCheckbox();
    for (auto* slider : {window.zoomCenterXSlider(), window.backgroundFlattenStrengthSlider()}) {
        master->setChecked(true);
        slider->setEnabled(true);
        slider->setValue((slider->minimum() + slider->maximum()) / 2);
        QCoreApplication::processEvents();
        const QColor enabled = filledColor(slider);
        if (slider == window.backgroundFlattenStrengthSlider()) master->setChecked(false);
        else slider->setEnabled(false);
        QCoreApplication::processEvents();
        QVERIFY(!slider->isEnabled());
        const QColor disabled = filledColor(slider);
        QVERIFY2(enabled != disabled, qPrintable(QStringLiteral(
            "Disabled slider fill still looks enabled: enabled=%1 disabled=%2 size=%3x%4")
            .arg(enabled.name(), disabled.name()).arg(slider->width()).arg(slider->height())));
        QVERIFY2(disabled.hsvSaturation() < 20, "Disabled fill retains an active accent color");
    }
}

void MainWindowTests::inspectorContrastAppearance_data()
{
    QTest::addColumn<bool>("light");
    QTest::newRow("light") << true;
    QTest::newRow("dark") << false;
}

void MainWindowTests::inspectorContrastAppearance()
{
    QFETCH(bool, light);
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    const QPalette originalPalette = application->palette();
    const auto restorePalette = qScopeGuard(
        [&application, &originalPalette]() { application->setPalette(originalPalette); });
    application->setPalette(SchemePalette(light));

    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    AddQuickModes(window);
    window.resize(1280, 720);
    window.show();
    window.setSimpleMode(false);
    auto expanded = window.sectionStates();
    for (auto it = expanded.begin(); it != expanded.end(); ++it) it.value() = true;
    window.setSectionStates(expanded);
    QCoreApplication::processEvents();

    const auto readableOn = [](const QColor& text, const QColor& surface) {
        return text.isValid() && theme_contrast::ContrastRatio(text, surface) >= 4.5;
    };
    const auto verifyPaletteRoles = [&readableOn](const QPalette& palette) {
        QVERIFY2(readableOn(palette.color(QPalette::Disabled, QPalette::WindowText),
                            palette.color(QPalette::Active, QPalette::Window)),
                 "Disabled window text is unreadable");
        QVERIFY2(readableOn(palette.color(QPalette::Disabled, QPalette::ButtonText),
                            palette.color(QPalette::Active, QPalette::Button)),
                 "Disabled button text is unreadable");
        QVERIFY2(readableOn(palette.color(QPalette::Disabled, QPalette::Text),
                            palette.color(QPalette::Active, QPalette::Base)),
                 "Disabled field text is unreadable");
        QVERIFY2(readableOn(palette.color(QPalette::Active, QPalette::PlaceholderText),
                            palette.color(QPalette::Active, QPalette::Base)),
                 "Placeholder text is unreadable");
    };
    verifyPaletteRoles(window.palette());

    auto* summary = Named<QLabel>(window, "currentModeSummaryLabel");
    QVERIFY(summary && !summary->text().isEmpty());
    QToolButton* header = nullptr;
    for (auto* button : window.findChildren<QToolButton*>()) {
        if (button->objectName() == QStringLiteral("collapsibleHeader") &&
            button->isVisible() && button->isChecked() &&
            window.rect().intersects(QRect(button->mapTo(&window, QPoint()), button->size()))) {
            header = button;
            break;
        }
    }
    QVERIFY2(header, "No visible expanded section header");
    QLabel* subtitle = nullptr;
    for (auto* label : window.findChildren<QLabel*>()) {
        if (label->objectName() == QStringLiteral("scopeSubtitle") && label->isVisible() &&
            !label->text().isEmpty()) {
            subtitle = label;
            break;
        }
    }
    QVERIFY2(subtitle, "No visible scope subtitle");

    const auto verifyGrabbedText = [&window, light](QWidget* headerWidget, QWidget* summaryWidget,
                                             QWidget* subtitleWidget) {
        QCoreApplication::processEvents();
        const QImage shot = window.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QString evidence = qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            shot.save(QDir(evidence).filePath(light ? QStringLiteral("inspector-contrast-light.png")
                                                   : QStringLiteral("inspector-contrast-dark.png")));
        }
        // The header crop skips its arrow so only the title text can pass.
        QVERIFY2(ReadablePixelCount(shot, ScaledWidgetRect(headerWidget, &window, shot, 4, 30), 4.5) >= 12,
                 "Expanded section header text is unreadable in this scheme");
        QVERIFY2(ReadablePixelCount(shot, ScaledWidgetRect(summaryWidget, &window, shot, 2), 4.5) >= 12,
                 "Current quick-mode name is unreadable in this scheme");
        QVERIFY2(ReadablePixelCount(shot, ScaledWidgetRect(subtitleWidget, &window, shot, 2), 4.5) >= 12,
                 "Scope subtitle is unreadable in this scheme");
    };
    verifyGrabbedText(header, summary, subtitle);

    auto* warning = window.cameraFormatNoticeLabel();
    QVERIFY(warning);
    QVERIFY2(readableOn(StyleSheetColor(warning->styleSheet()),
                        window.palette().color(QPalette::Active, QPalette::Window)),
             "Format warning color is unreadable in this scheme");

    auto* status = Named<QLabel>(window, "featureStatusLabel");
    QVERIFY(status);
    const QColor windowSurface = window.palette().color(QPalette::Active, QPalette::Window);
    window.setSuperResStatus(QStringLiteral("Off"), false);
    QVERIFY2(readableOn(StyleSheetColor(status->styleSheet()), windowSurface),
             "SuperRes Off status is unreadable in this scheme");
    window.setSuperResStatus(QStringLiteral("Active"), true);
    QVERIFY2(readableOn(StyleSheetColor(status->styleSheet()), windowSurface),
             "SuperRes active status is unreadable in this scheme");
    window.setSuperResStatus(QStringLiteral("Unavailable"), false);
    QVERIFY2(readableOn(StyleSheetColor(status->styleSheet()), windowSurface),
             "SuperRes warning status is unreadable in this scheme");

    const auto verifyIconInk = [&window](const QIcon& icon, const QColor& surface,
                                         const char* what) {
        int inkPixels = 0;
        const QColor ink = IconInkColor(icon, &inkPixels);
        QVERIFY2(inkPixels > 0 && theme_contrast::ContrastRatio(ink, surface) >= 3.0, what);
    };
    auto* search = Named<QLineEdit>(window, "imageSettingsSearch");
    QVERIFY(search && !search->actions().isEmpty());
    verifyIconInk(search->actions().constFirst()->icon(),
                  window.palette().color(QPalette::Active, QPalette::Base),
                  "Search icon ink is unreadable in this scheme");
    verifyIconInk(window.capturePhotoButton()->icon(),
                  window.palette().color(QPalette::Active, QPalette::Button),
                  "Photo icon ink is unreadable in this scheme");

    // An OS scheme switch after startup must re-derive every paired color.
    application->setPalette(SchemePalette(!light));
    QCoreApplication::processEvents();
    verifyPaletteRoles(window.palette());
    QVERIFY2(theme_contrast::IsLightTheme(window.palette()) != light,
             "Window did not follow the switched scheme");
    verifyGrabbedText(header, summary, subtitle);
    QVERIFY2(readableOn(StyleSheetColor(warning->styleSheet()),
                        window.palette().color(QPalette::Active, QPalette::Window)),
             "Format warning color is unreadable after the scheme switch");
    QVERIFY2(readableOn(StyleSheetColor(status->styleSheet()),
                        window.palette().color(QPalette::Active, QPalette::Window)),
             "SuperRes status color is unreadable after the scheme switch");
    verifyIconInk(search->actions().constFirst()->icon(),
                  window.palette().color(QPalette::Active, QPalette::Base),
                  "Search icon ink is unreadable after the scheme switch");
}

void MainWindowTests::searchIconsStayInsideFields()
{
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1280, 720);
    window.show();
    window.setSimpleMode(false);
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    QVERIFY(tabs);
    for (const char* name : {"imageSettingsSearch", "sharedSettingsSearch"}) {
        tabs->setCurrentWidget(Named<QWidget>(window,
            QByteArray(name) == "imageSettingsSearch" ? "imageTabPage" : "settingsTabPage"));
        auto* edit = Named<QLineEdit>(window, name);
        QVERIFY(edit);
        edit->setText(QStringLiteral("Zoom")); // also expose the clear button
        QCoreApplication::processEvents();
        const auto icons = edit->findChildren<QToolButton*>();
        QVERIFY(!icons.isEmpty());
        for (auto* icon : icons) {
            const QRect bounds(icon->mapTo(edit, QPoint()), icon->size());
            QVERIFY2(edit->rect().contains(bounds), qPrintable(QStringLiteral(
                "%1: icon %2,%3 %4x%5 outside field %6x%7")
                .arg(name).arg(bounds.x()).arg(bounds.y()).arg(bounds.width())
                .arg(bounds.height()).arg(edit->width()).arg(edit->height())));
            QVERIFY(qAbs(bounds.center().y() - edit->rect().center().y()) <= 2);
        }
    }
}

void MainWindowTests::gridBrowsingRequiresDeliberateActivation()
{
    MainWindow window;
    AddQuickModes(window);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* grid = Named<QWidget>(window, "modeGridPopup");
    auto* current = Named<QPushButton>(window, "currentModeButton");
    QVERIFY(gridButton && grid && current);
    QSignalSpy activated(&window, &MainWindow::quickModeActivated);
    const QString appliedLabel = current->text();

    QVERIFY2(ActivateFixtureWindow(gridButton), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(gridButton->window()));
    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    auto* list = window.presetList();
    QTRY_VERIFY(list->hasFocus());
    QTest::keyClick(list, Qt::Key_Right);
    QCOMPARE(list->currentRow(), 1);
    QCOMPARE(current->text(), appliedLabel);
    QCOMPARE(activated.size(), 0);
    QTest::keyClick(list, Qt::Key_Escape);
    QTRY_VERIFY(!grid->isVisible());
    QCOMPARE(activated.size(), 0);
    QCOMPARE(current->text(), appliedLabel);

    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    list->setCurrentRow(1);
    QTest::keyClick(list, Qt::Key_Return);
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toInt(), 1);
    QVERIFY(!grid->isVisible());

    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    list->setCurrentRow(2);
    QTest::keyClick(list, Qt::Key_Space);
    QTRY_COMPARE(activated.size(), 2);
    QCOMPARE(activated.at(1).at(0).toInt(), 2);
    QVERIFY(!grid->isVisible());

    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    list->scrollToItem(list->item(0));
    QCoreApplication::processEvents();
    const QRect firstItem = list->visualItemRect(list->item(0));
    QVERIFY(!firstItem.isEmpty());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstItem.center());
    QTRY_COMPARE(activated.size(), 3);
    QCOMPARE(activated.at(2).at(0).toInt(), 0);
}

void MainWindowTests::carouselUsesAppliedModeDuringGridBrowse()
{
    MainWindow window;
    AddQuickModes(window);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* grid = Named<QWidget>(window, "modeGridPopup");
    auto* next = Named<QPushButton>(window, "nextModeButton");
    auto* current = Named<QPushButton>(window, "currentModeButton");
    QVERIFY(gridButton && grid && next && current);
    QSignalSpy activated(&window, &MainWindow::quickModeActivated);
    QCOMPARE(window.presetList()->currentRow(), 0);
    const QString appliedLabel = current->text();

    gridButton->click();
    QTRY_VERIFY(grid->isVisible());
    window.presetList()->setCurrentRow(2);
    QCOMPARE(current->text(), appliedLabel);
    QCOMPARE(activated.size(), 0);
    next->click();
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toInt(), 1);
    QCOMPARE(window.presetList()->currentRow(), 1);
    QCOMPARE(current->text(), QStringLiteral("High contrast"));
    QVERIFY(!grid->isVisible());

    // Custom has no selected row. Previous should wrap to the last quick mode.
    window.presetList()->clearSelection();
    window.presetList()->setCurrentItem(nullptr);
    QCOMPARE(window.presetList()->currentRow(), -1);
    Named<QPushButton>(window, "previousModeButton")->click();
    QTRY_COMPARE(activated.size(), 2);
    QCOMPARE(activated.at(1).at(0).toInt(), window.presetList()->count() - 1);
}

void MainWindowTests::searchCanHandoffBetweenTabs()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.advancedModeButton()->click();
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* image = Named<QWidget>(window, "imageTabPage");
    auto* settings = Named<QWidget>(window, "settingsTabPage");
    auto* imageSearch = Named<QLineEdit>(window, "imageSettingsSearch");
    auto* sharedSearch = Named<QLineEdit>(window, "sharedSettingsSearch");
    QVERIFY(tabs && image && settings && imageSearch && sharedSearch);
    QCOMPARE(tabs->currentWidget(), image);
    imageSearch->setText(QStringLiteral("Camera acceleration"));
    QTest::keyClick(imageSearch, Qt::Key_Return);
    QTRY_COMPARE(tabs->currentWidget(), settings);
    QCOMPARE(sharedSearch->text(), QStringLiteral("Camera acceleration"));
    QVERIFY(window.testCameraAccelerationButton()->isVisible());

    sharedSearch->clear();
    tabs->setCurrentWidget(image);
    imageSearch->clear();
    imageSearch->setText(QStringLiteral("Zoom"));
    QTest::keyClick(imageSearch, Qt::Key_Return);
    QCOMPARE(tabs->currentWidget(), image);
    QVERIFY(window.zoomCheckbox()->isVisible());
}

void MainWindowTests::activeSearchRecomputesAfterLanguageSwitch()
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.advancedModeButton()->click();
    auto* imageSearch = Named<QLineEdit>(window, "imageSettingsSearch");
    auto* searchStatus = Named<QLabel>(window, "imageSettingsSearchStatus");
    QVERIFY(imageSearch && searchStatus);
    CollapsibleSection* colors = nullptr;
    for (auto* section : window.findChildren<CollapsibleSection*>()) {
        if (section->persistKey() == QStringLiteral("readability")) {
            colors = section;
            break;
        }
    }
    QVERIFY(colors);
    const QString heading = QStringLiteral("Colors and contrast");
    imageSearch->setText(heading);
    QCoreApplication::processEvents();
    QVERIFY(colors->isVisible());
    QVERIFY(searchStatus->isVisible());
    QVERIFY(searchStatus->text() != TranslateUi(QStringLiteral("No matching settings")));

    QString previousQuery = heading;
    for (AppLanguage language : {AppLanguage::Turkish, AppLanguage::German}) {
        QVERIFY(translations.SetLanguage(language, false));
        QCOMPARE(imageSearch->text(), previousQuery);
        QTRY_COMPARE(searchStatus->text(), TranslateUi(QStringLiteral("No matching settings")));
        QTRY_VERIFY(!colors->isVisible());
        previousQuery = TranslateUi(heading);
        imageSearch->setText(previousQuery);
        QTRY_VERIFY(colors->isVisible());
        QTRY_VERIFY(searchStatus->text() != TranslateUi(QStringLiteral("No matching settings")));
    }
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::busyActionSurvivesGermanResize()
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    QVERIFY(translations.SetLanguage(AppLanguage::German, false));
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.setExplainBusy(true);
    QCoreApplication::processEvents();
    QCOMPARE(window.explainNowButton()->text(), TranslateUi(QStringLiteral("Stop")));
    window.resize(900, 650);
    QCoreApplication::processEvents();
    QVERIFY(window.explainNowButton()->accessibleName().contains(
        TranslateUi(QStringLiteral("Stop"))));
    QCOMPARE(window.explainNowButton()->text(), TranslateUi(QStringLiteral("Stop")));
    window.setExplainBusy(false);
    QCoreApplication::processEvents();
    QCOMPARE(window.explainNowButton()->text(), TranslateUi(QStringLiteral("Explain")));
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::keyboardRegionsAndFindShortcut()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* image = Named<QWidget>(window, "imageTabPage");
    auto* assistant = Named<QWidget>(window, "assistantTabPage");
    auto* settings = Named<QWidget>(window, "settingsTabPage");
    auto* imageSearch = Named<QLineEdit>(window, "imageSettingsSearch");
    auto* sharedSearch = Named<QLineEdit>(window, "sharedSettingsSearch");
    QVERIFY(gridButton && tabs && image && assistant && settings &&
            imageSearch && sharedSearch);

    window.renderWidget()->setFocus();
    QTRY_VERIFY(window.renderWidget()->hasFocus());
    QTest::keyClick(window.renderWidget(), Qt::Key_F6);
    QTRY_VERIFY(window.simpleModeButton()->hasFocus());
    QTest::keyClick(window.simpleModeButton(), Qt::Key_F6);
    QTRY_VERIFY(gridButton->hasFocus());
    QTest::keyClick(gridButton, Qt::Key_F6, Qt::ShiftModifier);
    QTRY_VERIFY(window.simpleModeButton()->hasFocus());

    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(!window.isSimpleMode());
    QCOMPARE(tabs->currentWidget(), image);
    QTRY_VERIFY(imageSearch->hasFocus());

    tabs->setCurrentWidget(settings);
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QTest::keyClick(tabs, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(tabs->currentWidget(), settings);
    QTRY_VERIFY(sharedSearch->hasFocus());

    tabs->setCurrentWidget(assistant);
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QTest::keyClick(tabs, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(tabs->currentWidget(), image);
    QTRY_VERIFY(imageSearch->hasFocus());
}

void MainWindowTests::assistantInspectorTabTraversal()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.advancedModeButton()->click();
    auto* advanced = Named<QTabWidget>(window, "advancedPage");
    auto* assistantPage = Named<QWidget>(window, "assistantTabPage");
    QVERIFY(advanced && assistantPage);
    advanced->setCurrentWidget(assistantPage);
    QVERIFY(!assistantPage->findChild<QTabWidget*>());
    auto* view=window.assistantChatView();
    auto* newConversation=view->newButton();
    auto* historyButton=view->findChild<QToolButton*>(QStringLiteral("chatHistory"));
    auto* settingsButton=view->findChild<QToolButton*>(QStringLiteral("chatSettings"));
    QVERIFY(newConversation && historyButton && settingsButton);
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    newConversation->setFocus();QTRY_VERIFY(newConversation->hasFocus());
    QTest::keyClick(newConversation,Qt::Key_Tab);QTRY_VERIFY(historyButton->hasFocus());
    QTest::keyClick(historyButton,Qt::Key_Tab);QTRY_VERIFY(settingsButton->hasFocus());
    view->composer()->setFocus();QTRY_VERIFY(view->composer()->hasFocus());
    QTest::keyClick(view->composer(),Qt::Key_Tab);QTRY_VERIFY(view->attachmentCheckbox()->hasFocus());
    view->ShowHistory();
    auto* history=view->historyList();auto* rename=view->renameButton();
    QVERIFY(history->isVisible() && rename->isVisible());
    QVERIFY(history->count() == 0);
    // History population and action availability are app-owned. Mirror the
    // empty and selected-history states without constructing provider services.
    for (auto* action : {rename, view->exportButton(), view->deleteButton()}) action->setEnabled(false);
    QVERIFY(!rename->isEnabled());
    auto* row = new QListWidgetItem(QStringLiteral("Saved Explain conversation"), history);
    row->setData(Qt::UserRole, QStringLiteral("fixture-logical-chat"));
    history->setCurrentItem(row);
    for (auto* action : {rename, view->exportButton(), view->deleteButton()}) action->setEnabled(true);
    QCOMPARE(history->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("fixture-logical-chat"));
    history->setFocus();QTRY_VERIFY(history->hasFocus());
    QTest::keyClick(history,Qt::Key_Tab);QTRY_VERIFY(rename->hasFocus());
    QTest::keyClick(rename,Qt::Key_Escape);QTRY_VERIFY(!history->isVisible());
    QTRY_VERIFY(view->attachmentCheckbox()->hasFocus());

}

void MainWindowTests::floatingAssistantTabTraversal()
{
    MainWindow window;window.resize(1280,900);window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    AssistiveOverlay overlay(window.renderWidget());
    auto* view=overlay.chatView();
    overlay.SetChatSnapshot({{"conversationId","keyboard-fixture"},{"draft","keep this question"}});
    overlay.ShowChat(true);overlay.SetControlRoutingNotice(true,true);
    QTRY_VERIFY(overlay.isVisible());
    QVERIFY2(ActivateFixtureWindow(&overlay),test::ActivationFailureMessage());
    auto* start=overlay.findChild<QPushButton*>(QStringLiteral("agentStartConversation"));
    auto* without=overlay.findChild<QPushButton*>(QStringLiteral("agentSendWithoutControl"));
    QVERIFY(start && without && start->isVisible() && without->isVisible());
    view->composer()->setFocus();QTRY_VERIFY(view->composer()->hasFocus());
    QTest::keyClick(view->composer(),Qt::Key_Backtab);QTRY_VERIFY(without->hasFocus());
    QTest::keyClick(without,Qt::Key_Backtab);QTRY_VERIFY(start->hasFocus());
    overlay.agentControlPanel()->RefreshRoutingNotice(false);
    auto* probe=overlay.findChild<QPushButton*>(QStringLiteral("agentRoutingCapabilityTest"));
    QVERIFY(probe);QTRY_VERIFY(probe->hasFocus());
    overlay.agentControlPanel()->RefreshRoutingNotice(true);QTRY_VERIFY(start->hasFocus());
    QTest::keyClick(start,Qt::Key_Tab);QTRY_VERIFY(without->hasFocus());
    QTest::keyClick(without,Qt::Key_Tab);QTRY_VERIFY(view->composer()->hasFocus());
    view->ShowHistory();
    view->historyList()->addItem(QStringLiteral("Fixture history"));
    view->renameButton()->setEnabled(true);
    view->historyList()->setFocus();QTRY_VERIFY(view->historyList()->hasFocus());
    QTest::keyClick(view->historyList(),Qt::Key_Tab);QTRY_VERIFY(view->renameButton()->hasFocus());
    view->CloseDrawer();view->ShowSettings();
    auto* ai=view->findChild<QPushButton*>(QStringLiteral("chatAISettings"));
    QVERIFY(ai && ai->isVisible());
    ai->setFocus();QTRY_VERIFY(ai->hasFocus());
    QTest::keyClick(ai,Qt::Key_Tab);QTRY_VERIFY(view->connectButton()->hasFocus());
    SaveScreenshot(window,QStringLiteral("floating-assistant-keyboard"));
}

void MainWindowTests::chromeFollowsNativeOwnerMove_data()
{
    QTest::addColumn<bool>("simple");
    QTest::addColumn<bool>("rtl");
    QTest::newRow("simple-ltr") << true << false;
    QTest::newRow("advanced-ltr") << false << false;
    QTest::newRow("simple-rtl") << true << true;
    QTest::newRow("advanced-rtl") << false << true;
}

void MainWindowTests::chromeFollowsNativeOwnerMove()
{
    QFETCH(bool, simple);
    QFETCH(bool, rtl);
    MainWindow window;
    window.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    window.setSimpleMode(simple);
    window.resize(1100, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(ActivateFixtureWindow(&window));
    auto* render = window.renderWidget();
    QVERIFY(render);
    auto* glyphs = window.ultraClearTextOverlay();
    QVERIFY(glyphs);
    glyphs->SetViewTransform(ComputeViewTransform(1600, 900, render->width(), render->height(),
                                                1.0f, 0.5f, 0.5f, ViewportFitMode::kFit));
    glyphs->SetWords({{QStringLiteral("Reading"), QRectF(.28, .35, .23, .07),
                       {QColor(Qt::black), QColor(Qt::white)}, 1}});
    QTRY_VERIFY(glyphs->isVisible());
    const std::array<const char*, 9> names{"topLeftPanel", "bottomLeftPanel",
        "keystoneTrackingPanel", "bottomRightPanel", "modeGridPopup", "modeToast",
        "cameraPlaceholder", "uiVisibilityPanel", "ultraClearTextOverlay"};
    std::array<QWidget*, 9> peers{};
    for (size_t index = 0; index < peers.size(); ++index) {
        peers[index] = Named<QWidget>(window, names[index]);
        QVERIFY(peers[index]);
    }
    // Drain startup layout/activation before observing only the native moves.
    QTest::qWait(100);
    class LayoutObserver final : public QObject {
    public:
        int requests{};
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::LayoutRequest) ++requests;
            return false;
        }
    } observer;
    for (auto* peer : peers) peer->installEventFilter(&observer);
    const HWND owner = reinterpret_cast<HWND>(window.winId());
    for (const QPoint nativeDelta : {QPoint(57, 31), QPoint(-83, -49), QPoint(26, 18)}) {
        std::array<QRect, 9> before{};
        std::array<RECT, 9> nativeBefore{};
        std::array<HWND, 9> nativePeers{};
        for (size_t index = 0; index < peers.size(); ++index) {
            if (peers[index]->isVisible()) {
                nativePeers[index] = reinterpret_cast<HWND>(peers[index]->winId());
                QVERIFY(GetWindowRect(nativePeers[index], &nativeBefore[index]));
            }
        }
        for (size_t index = 0; index < peers.size(); ++index) before[index] = peers[index]->geometry();
        const QPoint origin = render->mapToGlobal(QPoint());
        RECT frame{};
        QVERIFY(GetWindowRect(owner, &frame));
        QVERIFY(SetWindowPos(owner, nullptr, frame.left + nativeDelta.x(),
            frame.top + nativeDelta.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        // No deferred events before these assertions. Native pixels may differ
        // from Qt logical pixels at non-unit scale; use the actual render delta.
        const QPoint logicalDelta = render->mapToGlobal(QPoint()) - origin;
        QVERIFY(!logicalDelta.isNull());
        for (size_t index = 0; index < peers.size(); ++index) {
            if (peers[index]->geometry() != before[index].translated(logicalDelta)) {
                qInfo() << "Native chrome move mismatch" << names[index]
                        << "DPR" << render->devicePixelRatioF()
                        << "viewport before/after" << origin << render->mapToGlobal(QPoint())
                        << "native delta" << nativeDelta
                        << "peer before/after" << before[index] << peers[index]->geometry();
            }
            QCOMPARE(peers[index]->geometry(), before[index].translated(logicalDelta));
            if (nativePeers[index]) {
                RECT actual{};
                QVERIFY(GetWindowRect(nativePeers[index], &actual));
                // Qt positions tool windows in whole logical pixels. At
                // fractional scale a native owner move can fall between them.
                const int tolerance = qFuzzyCompare(render->devicePixelRatioF(), qreal(1))
                    ? 0 : int(std::ceil(render->devicePixelRatioF()));
                QVERIFY(std::abs(actual.left - nativeBefore[index].left - nativeDelta.x()) <= tolerance);
                QVERIFY(std::abs(actual.top - nativeBefore[index].top - nativeDelta.y()) <= tolerance);
                QCOMPARE(actual.right - actual.left, nativeBefore[index].right - nativeBefore[index].left);
                QCOMPARE(actual.bottom - actual.top, nativeBefore[index].bottom - nativeBefore[index].top);
            }
        }
        QCoreApplication::processEvents();
        QCOMPARE(observer.requests, 0);
    }
    // Real viewport resizing still refits the responsive rectangular toolbar.
    const int oldWidth = render->width();
    window.resize(760, 600);
    QTRY_VERIFY(render->width() != oldWidth);
    QTRY_VERIFY(observer.requests > 0);
    const QRect viewport = GlobalRect(render);
    for (const char* name : {"bottomLeftPanel", "bottomRightPanel"}) {
        const QRect panel = GlobalRect(Named<QWidget>(window, name));
        QVERIFY(viewport.contains(panel));
    }
}

void MainWindowTests::minimumViewportChromeFits_data()
{
    QTest::addColumn<int>("language");
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("longModeName");
    QTest::newRow("english") << int(AppLanguage::English) << QStringLiteral("en")
                              << QStringLiteral("Read a printed textbook page");
    QTest::newRow("turkish") << int(AppLanguage::Turkish) << QStringLiteral("tr")
                              << QStringLiteral("Basılı ders kitabı sayfasını oku");
    QTest::newRow("german") << int(AppLanguage::German) << QStringLiteral("de")
                             << QStringLiteral("Gedruckte Lehrbuchseite lesen");
}

void MainWindowTests::minimumViewportChromeFits()
{
    QFETCH(int, language);
    QFETCH(QString, code);
    QFETCH(QString, longModeName);
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    QVERIFY(translations.SetLanguage(static_cast<AppLanguage>(language), false));
    MainWindow window;
    AddQuickModes(window);
    auto* list = window.presetList();
    SyncScreenshotState(window, code);
    list->setCurrentRow(1);
    auto* item = list->item(0);
    item->setData(Qt::AccessibleTextRole, longModeName);
    item->setData(Qt::StatusTipRole, longModeName);
    item->setText(QStringLiteral("1\n%1").arg(longModeName));
    list->setCurrentRow(0);
    window.resize(720, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    window.setAdvancedPanelWidth(360);
    QTRY_VERIFY_WITH_TIMEOUT(window.advancedPanelWidth() <= 380, 1000);
    auto* render = window.renderWidget();
    QTRY_VERIFY(render->width() >= 320 && render->width() <= 400);
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* bottomLeft = Named<QWidget>(window, "bottomLeftPanel");
    auto* bottomRight = Named<QWidget>(window, "bottomRightPanel");
    QVERIFY(top && bottomLeft && bottomRight);
    QTRY_VERIFY(top->isVisible() && bottomLeft->isVisible() && bottomRight->isVisible());
    SaveScreenshot(window, code + QStringLiteral("-minimum-viewport"));

    const QRect viewport(render->mapToGlobal(QPoint(0, 0)), render->size());
    const auto panelRect = [](QWidget* panel) {
        return QRect(panel->mapToGlobal(QPoint(0, 0)), panel->size());
    };
    const QRect topRect = panelRect(top);
    const QRect leftRect = panelRect(bottomLeft);
    const QRect rightRect = panelRect(bottomRight);
    for (const QRect& panel : {topRect, leftRect, rightRect}) {
        const QString details = QStringLiteral(
            "Chrome [%1,%2 %3x%4] exceeds viewport [%5,%6 %7x%8]")
            .arg(panel.x()).arg(panel.y()).arg(panel.width()).arg(panel.height())
            .arg(viewport.x()).arg(viewport.y())
            .arg(viewport.width()).arg(viewport.height());
        QVERIFY2(viewport.contains(panel), qPrintable(details));
    }
    QVERIFY2(!topRect.intersects(leftRect), "Top and quick-mode chrome overlap");
    QVERIFY2(!topRect.intersects(rightRect), "Top and action chrome overlap");
    QVERIFY2(!leftRect.intersects(rightRect), "Quick-mode and action chrome overlap");

    // The inspector forbids horizontal scrolling. Expand its Image groups so
    // a control cannot escape the viewport merely because it is initially
    // below a collapsed heading.
    auto* imageScroll = Named<QScrollArea>(window, "imageSettingsScroll");
    QVERIFY(imageScroll && imageScroll->widget() && imageScroll->viewport());
    auto expanded = window.sectionStates();
    for (auto it = expanded.begin(); it != expanded.end(); ++it) {
        it.value() = true;
    }
    window.setSectionStates(expanded);
    QCoreApplication::processEvents();
    const bool widthSettled = QTest::qWaitFor([&] {
        return imageScroll->widget()->width() <= imageScroll->viewport()->width() + 1;
    }, 1000);
    QList<QPair<int, QString>> widthConstraints;
    for (auto* child : imageScroll->widget()->findChildren<QWidget*>()) {
        if (!child->isVisible() || child->window() != &window) continue;
        const int minimum = std::max(child->minimumWidth(), child->minimumSizeHint().width());
        QString name = child->objectName();
        if (name.isEmpty()) name = child->accessibleName();
        if (name.isEmpty()) {
            if (auto* label = qobject_cast<QLabel*>(child)) name = label->text();
        }
        if (name.isEmpty()) name = QString::fromLatin1(child->metaObject()->className());
        for (auto* parent = child->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* section = qobject_cast<CollapsibleSection*>(parent)) {
                name += QStringLiteral(" in ") + section->headerWidget()->text();
                break;
            }
        }
        widthConstraints.append({minimum, name});
    }
    std::sort(widthConstraints.begin(), widthConstraints.end(),
              [](const auto& left, const auto& right) { return left.first > right.first; });
    QStringList widest;
    for (int index = 0; index < std::min(8, static_cast<int>(widthConstraints.size())); ++index) {
        widest.append(QStringLiteral("%1:%2")
                          .arg(widthConstraints[index].second)
                          .arg(widthConstraints[index].first));
    }
    const QString widthDetails = QStringLiteral(
        "Image scroll content=%1 viewport=%2; widest minimums %3")
        .arg(imageScroll->widget()->width())
        .arg(imageScroll->viewport()->width())
        .arg(widest.join(QStringLiteral(", ")));
    QVERIFY2(widthSettled,
             qPrintable(widthDetails));
    const int viewportWidth = imageScroll->viewport()->width();
    for (auto* widget : imageScroll->widget()->findChildren<QWidget*>()) {
        if (!widget->isVisible() || widget->window() != &window ||
            (!qobject_cast<QAbstractButton*>(widget) &&
             !qobject_cast<QComboBox*>(widget) &&
             !qobject_cast<QSlider*>(widget) &&
             widget->objectName() != QStringLiteral("sliderValueLabel"))) {
            continue;
        }
        const int left = widget->mapTo(imageScroll->viewport(), QPoint(0, 0)).x();
        const int right = left + widget->width();
        const QString details = QStringLiteral(
            "Image control %1 (%2) spans %3..%4 beyond scroll viewport 0..%5")
            .arg(widget->objectName(),
                 QString::fromLatin1(widget->metaObject()->className()))
            .arg(left).arg(right).arg(viewportWidth);
        QVERIFY2(left >= -1 && right <= viewportWidth + 1,
                 qPrintable(details));
    }
    auto* posterize = Named<QPushButton>(window, "quickColor_posterize-6");
    QVERIFY(posterize);
    imageScroll->ensureWidgetVisible(posterize);
    SaveScreenshot(window, code + QStringLiteral("-colors-minimum"));
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::bottomChromeFormsRectangularGroups_data()
{
    QTest::addColumn<int>("language");
    QTest::addColumn<QString>("code");
    QTest::newRow("english") << int(AppLanguage::English) << QStringLiteral("en");
    QTest::newRow("turkish") << int(AppLanguage::Turkish) << QStringLiteral("tr");
    QTest::newRow("german") << int(AppLanguage::German) << QStringLiteral("de");
}

void MainWindowTests::bottomChromeFormsRectangularGroups()
{
    QFETCH(int, language);
    QFETCH(QString, code);
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager translations(*application);
    QVERIFY(translations.SetLanguage(static_cast<AppLanguage>(language), false));
    MainWindow window;
    SyncScreenshotState(window, code);
    // German action labels need a wider camera to retain both corner groups.
    // Wrapping at 1280 is valid; the contract is content-fit, not a fixed width.
    const int wideWidth = language == int(AppLanguage::German) ? 1600 : 1280;
    window.resize(wideWidth, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* render = window.renderWidget();
    auto* bottomLeft = Named<QWidget>(window, "bottomLeftPanel");
    auto* bottomRight = Named<QWidget>(window, "bottomRightPanel");
    QVERIFY(render && bottomLeft && bottomRight);
    QTRY_VERIFY(bottomLeft->isVisible() && bottomRight->isVisible());
    const auto viewport = [render]() { return GlobalRect(render); };
    int rows = 0;

    // Simple at a normal size stays camera-first: two compact corner groups.
    QTRY_COMPARE(bottomRight->property("chromeDock").toString(), QStringLiteral("corner"));
    QCOMPARE(bottomLeft->property("chromeDock").toString(), QStringLiteral("corner"));
    QTRY_COMPARE(GlobalRect(bottomRight).right(), viewport().right());
    QCOMPARE(GlobalRect(bottomRight).bottom(), viewport().bottom());
    QCOMPARE(GlobalRect(bottomLeft).left(), viewport().left());
    QCOMPARE(GlobalRect(bottomLeft).bottom(), viewport().bottom());
    QVERIFY(GlobalRect(bottomLeft).width() + GlobalRect(bottomRight).width() < viewport().width());
    QString problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(rows, 1);
    SaveScreenshot(window, code + QStringLiteral("-bottom-chrome-simple-corners"));

    // The left cluster folds first; further narrowing wraps the actions
    // within their own trailing corner, without either full-width band.
    const auto verifyStrip = [&](const QString& name) -> QString {
        const QRect view = viewport();
        const QRect left = GlobalRect(bottomLeft);
        const QRect right = GlobalRect(bottomRight);
        if (left.left() != view.left() || right.right() != view.right() ||
            !view.contains(left) || !view.contains(right) || left.width() >= view.width()) {
            return QStringLiteral("%1: compact groups [%2..%3] and [%4..%5] exceed camera [%6..%7]")
                .arg(name).arg(left.left()).arg(left.right()).arg(right.left())
                .arg(right.right()).arg(view.left()).arg(view.right());
        }
        if (right.bottom() != view.bottom() || left.intersects(right)) {
            return QStringLiteral("%1: action row bottom %2 overlaps mode top %3 or is not at %4")
                .arg(name).arg(right.bottom()).arg(left.top()).arg(view.bottom());
        }
        return {};
    };
    window.resize(1100, 760);
    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    window.setAdvancedPanelWidth(440);
    QTRY_VERIFY(render->width() <= 700);
    QTRY_COMPARE(bottomLeft->property("modeRows").toInt(), 2);
    QTRY_VERIFY2(verifyStrip(QStringLiteral("advanced")).isEmpty(),
                 qPrintable(verifyStrip(QStringLiteral("advanced"))));
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    SaveScreenshot(window, code + QStringLiteral("-bottom-chrome-advanced-strip"));

    // Four actions fit farther down than the former five-button row. Use a
    // genuinely narrow camera so this case still exercises a filled 2x2 grid.
    window.resize(840, 760);
    QTRY_VERIFY(render->width() <= 400);
    QTRY_VERIFY2(verifyStrip(QStringLiteral("squeezed")).isEmpty(),
                 qPrintable(verifyStrip(QStringLiteral("squeezed"))));
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(rows, 2);
    SaveScreenshot(window, code + QStringLiteral("-bottom-chrome-two-action-rows"));

    // The narrowest camera preserves the two-row action group.
    window.resize(720, 720);
    window.setAdvancedPanelWidth(360);
    QTRY_VERIFY(render->width() <= 400);
    QTRY_VERIFY2(verifyStrip(QStringLiteral("minimum")).isEmpty(),
                 qPrintable(verifyStrip(QStringLiteral("minimum"))));
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(rows, 2);
    SaveScreenshot(window, code + QStringLiteral("-bottom-chrome-minimum-strip"));

    // Returning to Simple at full width restores the compact corners.
    window.simpleModeButton()->click();
    QTRY_VERIFY(window.isSimpleMode());
    window.resize(wideWidth, 800);
    QTRY_COMPARE(bottomRight->property("chromeDock").toString(), QStringLiteral("corner"));
    QTRY_VERIFY(GlobalRect(bottomRight).width() < viewport().width());
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(rows, 1);
    // Live retranslation must reflow the same pinned strip, without a resize.
    window.resize(720, 720);
    window.advancedModeButton()->click();
    window.setAdvancedPanelWidth(360);
    QCoreApplication::processEvents();
    const int beforeSwitchWidth = render->width();
    for (const AppLanguage next : {AppLanguage::English, AppLanguage::Turkish,
                                   AppLanguage::German}) {
        QVERIFY(translations.SetLanguage(next, false));
        QCoreApplication::processEvents();
        QCOMPARE(render->width(), beforeSwitchWidth);
        problem = ActionRowsProblem(window, &rows);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        for (auto* button : {window.capturePhotoButton(), window.recordButton(),
                             window.explainNowButton(),
                             window.annotationButton()}) {
            QVERIFY(GlobalRect(bottomRight).contains(GlobalRect(button)));
        }
    }
    // Active tracking is hidden in Advanced and must not reserve blank space.
    window.setKeystoneTrackingControls(true, true, false, false, true, false, 1, 2);
    QCoreApplication::processEvents();
    QVERIFY2(verifyStrip(QStringLiteral("hidden tracking")).isEmpty(),
             qPrintable(verifyStrip(QStringLiteral("hidden tracking"))));
    auto* tracking = Named<QWidget>(window, "keystoneTrackingPanel");
    QVERIFY(tracking && !tracking->isVisible());

    // Native peers do not contribute their minimum sizes to the main window.
    // A short, narrow Simple view must still keep its toolbar rows disjoint.
    window.simpleModeButton()->click();
    window.setKeystoneTrackingControls(false, false, false, false, false, false, 0, 0);
    window.resize(360, 270);
    QCoreApplication::processEvents();
    QVERIFY(!GlobalRect(bottomRight).intersects(GlobalRect(bottomLeft)));
    QVERIFY(translations.SetLanguage(AppLanguage::English, false));
}

void MainWindowTests::bottomChromeRetainsNativeSizeDuringRefresh_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("advanced");
    QTest::addColumn<int>("modeRows");
    QTest::addColumn<int>("actionRows");
    QTest::addColumn<bool>("rtl");
    for (bool rtl : {false, true}) {
        const QByteArray direction = rtl ? "rtl" : "ltr";
        QTest::newRow(("wide-" + direction).constData()) << 1400 << false << 1 << 1 << rtl;
        QTest::newRow(("folded-mode-" + direction).constData()) << 1100 << true << 2 << 1 << rtl;
        QTest::newRow(("folded-both-" + direction).constData()) << 840 << true << 2 << 2 << rtl;
    }
}

void MainWindowTests::bottomChromeRetainsNativeSizeDuringRefresh()
{
    QFETCH(int, width);
    QFETCH(bool, advanced);
    QFETCH(int, modeRows);
    QFETCH(int, actionRows);
    QFETCH(bool, rtl);
    MainWindow window;
    window.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    window.setSimpleMode(!advanced);
    window.resize(width, 740);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    if (advanced) window.setAdvancedPanelWidth(440);
    auto* left = Named<QWidget>(window, "bottomLeftPanel");
    auto* right = Named<QWidget>(window, "bottomRightPanel");
    QVERIFY(left && right);
    QTRY_VERIFY(left->isVisible() && right->isVisible());
    QTRY_COMPARE(left->property("modeRows").toInt(), modeRows);
    QTRY_COMPARE(right->property("actionRows").toInt(), actionRows);
    QTest::qWait(100);
    class ResizeObserver final : public QObject {
    public:
        QStringList sizes;
        bool eventFilter(QObject* object, QEvent* event) override {
            if (event->type() == QEvent::Resize) {
                const auto* resize = static_cast<QResizeEvent*>(event);
                sizes.append(QStringLiteral("%1 %2x%3 -> %4x%5")
                    .arg(object->objectName()).arg(resize->oldSize().width())
                    .arg(resize->oldSize().height()).arg(resize->size().width())
                    .arg(resize->size().height()));
            }
            return false;
        }
    } observer;
    left->installEventFilter(&observer);
    right->installEventFilter(&observer);
    const QSize leftSize = left->size(), rightSize = right->size();
    // Activation refits the peers even when the viewport has not changed.
    // A compact group must never resize to a wide trial layout and back.
    if (advanced) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            QEvent activation(QEvent::ApplicationActivate);
            QCoreApplication::sendEvent(&window, &activation);
            QCoreApplication::processEvents();
        }
        QVERIFY2(observer.sizes.isEmpty(), qPrintable(observer.sizes.join("; ")));
    }
    // Stay away from a wrapping threshold, including during actual resizes.
    for (int delta : {1, 2, -1, 0}) {
        window.resize(width + delta, window.height());
        QCoreApplication::processEvents();
        QCOMPARE(left->property("modeRows").toInt(), modeRows);
        QCOMPARE(right->property("actionRows").toInt(), actionRows);
        QCOMPARE(left->size(), leftSize);
        QCOMPARE(right->size(), rightSize);
        QVERIFY2(observer.sizes.isEmpty(), qPrintable(observer.sizes.join("; ")));
    }
    const HWND owner = reinterpret_cast<HWND>(window.winId());
    RECT frame{};
    QVERIFY(GetWindowRect(owner, &frame));
    QVERIFY(SetWindowPos(owner, nullptr, frame.left + 13, frame.top + 11, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
    QCoreApplication::processEvents();
    QVERIFY2(observer.sizes.isEmpty(), qPrintable(observer.sizes.join("; ")));
    const QRect viewport = GlobalRect(window.renderWidget());
    QVERIFY(viewport.contains(GlobalRect(left)) && viewport.contains(GlobalRect(right)));
    QVERIFY(SaveScreenshot(window, QStringLiteral("stable-bottom-chrome-%1-%2")
        .arg(modeRows == 1 ? "wide" : actionRows == 1 ? "folded-mode" : "folded-both")
        .arg(rtl ? "rtl" : "ltr")));
}

void MainWindowTests::hideUiPreservesPriorModeAndStreaming_data()
{
    QTest::addColumn<bool>("advancedMode");
    QTest::newRow("simple") << false;
    QTest::newRow("advanced-settings") << true;
}

void MainWindowTests::hideUiPreservesPriorModeAndStreaming()
{
    QFETCH(bool, advancedMode);
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = Named<QTabWidget>(window, "advancedPage");
    auto* settings = Named<QWidget>(window, "settingsTabPage");
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* bottomLeft = Named<QWidget>(window, "bottomLeftPanel");
    auto* bottomRight = Named<QWidget>(window, "bottomRightPanel");
    auto* visibilityPanel = Named<QWidget>(window, "uiVisibilityPanel");
    auto* toggle = window.uiVisibilityButton();
    QVERIFY(tabs && settings && top && bottomLeft && bottomRight &&
            visibilityPanel && toggle);
    if (advancedMode) {
        window.advancedModeButton()->click();
        QTRY_VERIFY(!window.isSimpleMode());
        tabs->setCurrentWidget(settings);
        window.setAdvancedPanelWidth(440);
        QTRY_VERIFY(window.advancedPanelWidth() >= 420);
    }
    AssistiveOverlay assistant(window.renderWidget());
    VisibleChatSession session;
    connect(&session, &VisibleChatSession::SnapshotChanged, window.assistantChatView(), &AssistantChatView::SetChatSnapshot);
    connect(&session, &VisibleChatSession::SnapshotChanged, &assistant, &AssistiveOverlay::SetChatSnapshot);
    const auto ticket = session.Admit(QStringLiteral("Explain this view"), ChatBackendKind::Explain);
    QVERIFY(ticket);
    QVERIFY(session.AppendDelta(*ticket, QStringLiteral("First part")));
    assistant.ShowChat(true);
    QTRY_VERIFY(assistant.isVisible());
    QTRY_VERIFY(toggle->isVisible());
    const QString screenshotPrefix = advancedMode ? QStringLiteral("advanced")
                                                  : QStringLiteral("simple");
    SaveScreenshot(window, screenshotPrefix + QStringLiteral("-assistant-visible"));

    toggle->click();
    QVERIFY(window.isUiHidden());
    QTRY_VERIFY(!top->isVisible() && !bottomLeft->isVisible() &&
                !bottomRight->isVisible() && !tabs->isVisible() &&
                !assistant.isVisible());
    QVERIFY(visibilityPanel->isVisible());
    QVERIFY(toggle->isVisible());
    const QString completed = QStringLiteral("First part, completed");
    QVERIFY(session.Complete(*ticket, completed));
    QVERIFY(!assistant.isVisible());
    const QPointF local(20, 20);
    const QPointF global = window.renderWidget()->mapToGlobal(local.toPoint());
    QMouseEvent move(QEvent::MouseMove, local, local, global,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &move);
    QVERIFY(window.isUiHidden());
    QVERIFY(!top->isVisible() && !bottomLeft->isVisible() && !assistant.isVisible());
    SaveScreenshot(window, screenshotPrefix + QStringLiteral("-ui-hidden"));

    toggle->click();
    QTRY_VERIFY(!window.isUiHidden());
    QCOMPARE(window.isSimpleMode(), !advancedMode);
    QTRY_VERIFY(top->isVisible() && bottomLeft->isVisible() &&
                bottomRight->isVisible() && assistant.isVisible());
    auto* body = assistant.chatView()->transcript();
    QVERIFY(body);
    QVERIFY(body->toPlainText().contains(completed));
    QCOMPARE(body->toPlainText(), window.assistantChatView()->transcript()->toPlainText());
    SaveScreenshot(window, screenshotPrefix + QStringLiteral("-ui-restored"));
    if (advancedMode) {
        QCOMPARE(tabs->currentWidget(), settings);
        QTRY_VERIFY(tabs->isVisible());
        QVERIFY(window.advancedPanelWidth() >= 420);
    }
}

void MainWindowTests::mouseFocusedChromeCanIdleFade()
{
    POINT ownerPointer{};
    QVERIFY(GetCursorPos(&ownerPointer));
    const auto restorePointer = qScopeGuard([ownerPointer] {
        SetCursorPos(ownerPointer.x, ownerPointer.y);
    });
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* idle = Named<QTimer>(window, "simpleChromeIdleTimer");
    QVERIFY(top && idle);
    QTRY_VERIFY(top->isVisible());
    QVERIFY2(ActivateFixtureWindow(top), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(top));
    // QWidget mouseClick injects Qt events without moving the native cursor.
    // Keep those events and FadeSimpleChrome's actual cursor baseline aligned.
    const QPoint clickLocal = window.simpleModeButton()->rect().center();
    const QPoint clickGlobal = window.simpleModeButton()->mapToGlobal(clickLocal);
    QCursor::setPos(clickGlobal);
    QCoreApplication::processEvents();
    const QPoint observedClickGlobal = QCursor::pos();
    // Windows rounds native pixels when Qt maps a mixed-DPI desktop. Check
    // the requested location within that rounding bound, then inject the
    // event at the actual cursor location rather than a different baseline.
    QVERIFY(qAbs(observedClickGlobal.x() - clickGlobal.x()) <= 1);
    QVERIFY(qAbs(observedClickGlobal.y() - clickGlobal.y()) <= 1);
    const QPoint observedClickLocal = window.simpleModeButton()->mapFromGlobal(observedClickGlobal);
    QVERIFY(window.simpleModeButton()->rect().contains(observedClickLocal));
    QTest::mouseClick(window.simpleModeButton(), Qt::LeftButton, Qt::NoModifier,
                      observedClickLocal);
    QTRY_VERIFY(window.simpleModeButton()->hasFocus());
    QVERIFY(idle->isActive());
    const QPointF stationaryGlobal(QCursor::pos());
    POINT stationaryNative{};
    QVERIFY(GetCursorPos(&stationaryNative));
    const auto pointerIsStationary = [stationaryNative] {
        POINT current{};
        return GetCursorPos(&current) &&
               current.x == stationaryNative.x && current.y == stationaryNative.y;
    };
    // A real single-shot timeout is no longer active when its signal runs.
    idle->stop();
    QVERIFY(QMetaObject::invokeMethod(idle, "timeout", Qt::DirectConnection));
    QTRY_VERIFY(!top->isVisible());
    QTest::qWait(300);
    QVERIFY2(pointerIsStationary(), "OKUFLOW_DESKTOP_INTERRUPTED: native pointer moved during the idle fixture");
    QVERIFY(!top->isVisible());

    // Hover changes caused by hiding native panels are not pointer input.
    const QPointF stationaryLocal = window.renderWidget()->mapFromGlobal(
        stationaryGlobal.toPoint());
    QEnterEvent enter(stationaryLocal, stationaryLocal, stationaryGlobal);
    QCoreApplication::sendEvent(window.renderWidget(), &enter);
    QMouseEvent stationary(QEvent::MouseMove, stationaryLocal, stationaryLocal,
                           stationaryGlobal, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &stationary);
    QVERIFY(!top->isVisible());
    // At 150% DPI an unchanged native cursor can arrive one third of a
    // logical pixel away from QCursor::pos()'s rounded baseline.
    const QPointF fractionalOffset(1.0 / 3.0, 1.0 / 3.0);
    QMouseEvent fractionalStationary(
        QEvent::MouseMove, stationaryLocal + fractionalOffset,
        stationaryLocal + fractionalOffset, stationaryGlobal + fractionalOffset,
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &fractionalStationary);
    QTest::qWait(100);
    QVERIFY2(pointerIsStationary(), "OKUFLOW_DESKTOP_INTERRUPTED: native pointer moved during stationary input");
    QVERIFY(!top->isVisible());
    QCOMPARE(QGuiApplication::applicationState(), Qt::ApplicationActive);

    const QPointF local = window.renderWidget()->mapToGlobal(QPoint(20, 20)) == QCursor::pos()
                              ? QPointF(40, 40) : QPointF(20, 20);
    const QPointF global = window.renderWidget()->mapToGlobal(local.toPoint());
    QCursor::setPos(global.toPoint());
    QCoreApplication::processEvents();
    const QPointF observedGlobal(QCursor::pos());
    QVERIFY(qAbs(observedGlobal.x() - global.x()) <= 1);
    QVERIFY(qAbs(observedGlobal.y() - global.y()) <= 1);
    QVERIFY(observedGlobal != stationaryGlobal);
    const QPointF observedLocal = window.renderWidget()->mapFromGlobal(observedGlobal.toPoint());
    QMouseEvent move(QEvent::MouseMove, observedLocal, observedLocal, observedGlobal,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &move);
    QTRY_VERIFY(top->isVisible());
}

void MainWindowTests::unrelatedNativeInputCannotRevealChrome()
{
    POINT ownerPointer{};
    QVERIFY(GetCursorPos(&ownerPointer));
    const auto restorePointer = qScopeGuard([ownerPointer] {
        SetCursorPos(ownerPointer.x, ownerPointer.y);
    });
    class NativeInputWindow final : public MainWindow {
    public:
        using MainWindow::nativeEventFilter;
    };
    NativeInputWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* idle = Named<QTimer>(window, "simpleChromeIdleTimer");
    QVERIFY(top && idle);
    QTRY_VERIFY(top->isVisible());
    // Drain the native move/enter events before manually expiring the idle
    // timer. Otherwise startup positioning can legitimately reveal chrome
    // after the fixture has already requested its fade.
    QCursor::setPos(window.renderWidget()->mapToGlobal(window.renderWidget()->rect().center()));
    QCoreApplication::processEvents();
    QTest::qWait(100);
    window.renderWidget()->setFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(window.renderWidget()->hasFocus());
    idle->stop();
    QVERIFY(QMetaObject::invokeMethod(idle, "timeout", Qt::DirectConnection));
    QTRY_VERIFY(!top->isVisible());

    QWidget unrelated;
    MSG message{};
    message.hwnd = reinterpret_cast<HWND>(unrelated.winId());
    message.message = WM_MOUSEMOVE;
    QVERIFY(GetCursorPos(&message.pt));
    message.pt.x += 40;
    window.nativeEventFilter({}, &message, nullptr);
    message.message = WM_KEYDOWN;
    message.wParam = 'A';
    window.nativeEventFilter({}, &message, nullptr);
    QVERIFY(!top->isVisible());

    message.hwnd = reinterpret_cast<HWND>(window.winId());
    message.message = WM_MOUSEMOVE;
    QVERIFY(GetCursorPos(&message.pt));
    // Logical rounding must not discard one physical pixel of native input.
    ++message.pt.x;
    window.nativeEventFilter({}, &message, nullptr);
    QTRY_VERIFY(top->isVisible());
}

void MainWindowTests::keyboardFocusedChromeSurvivesNativeHandoff()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* bottomLeft = Named<QWidget>(window, "bottomLeftPanel");
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* idle = Named<QTimer>(window, "simpleChromeIdleTimer");
    QVERIFY(top && bottomLeft && gridButton && idle && window.uiVisibilityButton());

    window.renderWidget()->setFocus();
    QTRY_VERIFY(window.renderWidget()->hasFocus());
    QTest::keyClick(window.renderWidget(), Qt::Key_F6);
    QTRY_VERIFY(window.simpleModeButton()->hasFocus());
    QTest::keyClick(window.simpleModeButton(), Qt::Key_F6);
    QTRY_VERIFY(gridButton->hasFocus());
    QVERIFY(idle->isActive());
    QVERIFY(QMetaObject::invokeMethod(idle, "timeout", Qt::DirectConnection));
    QTest::qWait(100);
    QVERIFY(top->isVisible() && bottomLeft->isVisible());

    QTest::keyClick(gridButton, Qt::Key_H, Qt::ControlModifier);
    QTRY_VERIFY(window.isUiHidden());
    QTRY_VERIFY(window.uiVisibilityButton()->isVisible());
    QTest::keyClick(window.uiVisibilityButton(), Qt::Key_F6);
    QTRY_VERIFY(!window.isUiHidden());
    QTRY_VERIFY(top->isVisible() && bottomLeft->isVisible());
    QVERIFY(window.isSimpleMode());
}

void MainWindowTests::simpleBackwardFocusCrossesPanels()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    QVERIFY(gridButton);
    QVERIFY2(ActivateFixtureWindow(gridButton), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(gridButton->window()));
    gridButton->setFocus();
    QTRY_VERIFY(gridButton->hasFocus());
    QTest::keyClick(gridButton, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(window.simpleTextClarityCheckbox()->hasFocus());
}

void MainWindowTests::modalDialogKeepsTabAndNumberKeys()
{
    MainWindow window;
    AddQuickModes(window);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(ActivateFixtureWindow(&window), test::ActivationFailureMessage());
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QDialog dialog(&window);
    dialog.setModal(true);
    auto* layout = new QHBoxLayout(&dialog);
    auto* first = new QPushButton(QStringLiteral("First"), &dialog);
    auto* second = new QPushButton(QStringLiteral("Second"), &dialog);
    layout->addWidget(first);
    layout->addWidget(second);
    QWidget::setTabOrder(first, second);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));
    first->setFocus();
    QTest::keyClick(first, Qt::Key_Tab);
    QTRY_VERIFY(second->hasFocus());
    QTest::keyClick(second, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(first->hasFocus());
    QTest::keyClick(first, Qt::Key_2);
    QCOMPARE(window.presetList()->currentRow(), 0);
}

} // namespace okuflow

OKUFLOW_GUI_TEST_MAIN(okuflow::MainWindowTests)
#include "main_window_tests.moc"
