#include "okuflow/app/app.hpp"
#include "okuflow/app/language_manager.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/collapsible_section.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

#include <algorithm>
#include <cstdlib>
#include <Windows.h>

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
                                      window.explainNowButton(), window.readTextButton(),
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

void SaveScreenshot(MainWindow& window, const QString& name)
{
    const QString root = qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS");
    if (root.isEmpty()) {
        return;
    }
    // Native tool windows can be exposed before their first compositor frame.
    QTest::qWait(80);
    QCoreApplication::processEvents();
    QDir directory(root);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        qWarning().noquote() << "Cannot create screenshot directory:" << root;
        return;
    }
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !GetWindowThreadProcessId(foreground, &foregroundProcess) ||
        foregroundProcess != GetCurrentProcessId()) {
        qWarning().noquote() << "Skipping UI screenshot while another process owns the foreground:"
                             << name << "foreground PID" << foregroundProcess;
        return;
    }
    const QString path = directory.filePath(name + QStringLiteral(".png"));
    // Corner controls and the quick grid are native tool windows. Capture the
    // composed desktop region so the optional evidence includes those peers.
    QScreen* screen = window.screen();
    const QRect area = window.frameGeometry();
    if (!screen || !screen->grabWindow(0, area.x(), area.y(),
                                       area.width(), area.height()).save(path)) {
        qWarning().noquote() << "Cannot save UI screenshot:" << path;
    }
}

} // namespace

class MainWindowTests final : public QObject {
    Q_OBJECT
private slots:
    void modesGridSearchAndGeometry_data();
    void modesGridSearchAndGeometry();
    void dependentControls();
    void textClarityMasterPreservesRefinements();
    void disabledSlidersHaveDistinctAppearance();
    void searchIconsStayInsideFields();
    void gridBrowsingRequiresDeliberateActivation();
    void carouselUsesAppliedModeDuringGridBrowse();
    void searchCanHandoffBetweenTabs();
    void activeSearchRecomputesAfterLanguageSwitch();
    void busyActionSurvivesGermanResize();
    void keyboardRegionsAndFindShortcut();
    void assistantInspectorTabTraversal();
    void minimumViewportChromeFits_data();
    void minimumViewportChromeFits();
    void bottomChromeFormsRectangularGroups_data();
    void bottomChromeFormsRectangularGroups();
    void hideUiPreservesPriorModeAndStreaming_data();
    void hideUiPreservesPriorModeAndStreaming();
    void mouseFocusedChromeCanIdleFade();
    void unrelatedNativeInputCannotRevealChrome();
    void keyboardFocusedChromeSurvivesNativeHandoff();
    void simpleBackwardFocusCrossesPanels();
    void modalDialogKeepsTabAndNumberKeys();
};

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
    window.activateWindow();
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

    gridButton->window()->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(gridButton->window()));
    QTest::mouseClick(gridButton, Qt::LeftButton);
    QTRY_VERIFY(grid->isVisible());
    QTRY_VERIFY(window.presetList()->hasFocus());
    QCOMPARE(window.presetList()->count(), 3);
    SaveScreenshot(window, code + QStringLiteral("-grid"));
    QTest::keyClick(window.presetList(), Qt::Key_Escape);
    QTRY_VERIFY(!grid->isVisible());
    for (int attempt = 0; attempt < 75 && !current->hasFocus(); ++attempt) {
        QTest::qWait(20);
    }
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
    QVERIFY2(current->hasFocus(), qPrintable(focusDetails));

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
    auto* transcriptPage = Named<QWidget>(window, "transcriptTabPage");
    auto* settingsPage = Named<QWidget>(window, "settingsTabPage");
    QVERIFY(splitter && advanced && imagePage && assistantPage && transcriptPage && settingsPage);
    QTRY_VERIFY(advanced->isVisible());
    QCOMPARE(advanced->count(), 4);
    QCOMPARE(advanced->widget(0), imagePage);
    QCOMPARE(advanced->widget(1), assistantPage);
    QCOMPARE(advanced->widget(2), transcriptPage);
    QCOMPARE(advanced->widget(3), settingsPage);
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
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    auto* grid = Named<QWidget>(window, "modeGridPopup");
    auto* current = Named<QPushButton>(window, "currentModeButton");
    QVERIFY(gridButton && grid && current);
    QSignalSpy activated(&window, &MainWindow::quickModeActivated);
    const QString appliedLabel = current->text();

    gridButton->window()->activateWindow();
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
    window.activateWindow();
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

    window.activateWindow();
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(!window.isSimpleMode());
    QCOMPARE(tabs->currentWidget(), image);
    QTRY_VERIFY(imageSearch->hasFocus());

    tabs->setCurrentWidget(settings);
    window.activateWindow();
    QTest::keyClick(tabs, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(tabs->currentWidget(), settings);
    QTRY_VERIFY(sharedSearch->hasFocus());

    tabs->setCurrentWidget(assistant);
    window.activateWindow();
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
    auto* assistantTabs = assistantPage->findChild<QTabWidget*>();
    QVERIFY(assistantTabs);
    assistantTabs->setCurrentIndex(0);
    auto* attach = window.assistantAttachFrameCheckbox();
    auto* newConversation = window.assistantNewButton();
    QVERIFY(attach && newConversation && attach->isVisible() && newConversation->isVisible());
    window.activateWindow();
    attach->setFocus();
    QTRY_VERIFY(attach->hasFocus());
    QTest::keyClick(attach, Qt::Key_Tab);
    QTRY_VERIFY(newConversation->hasFocus());

    assistantTabs->setCurrentIndex(1);
    auto* history = window.assistantHistoryList();
    auto* rename = window.assistantRenameButton();
    QVERIFY(history && rename && history->isVisible() && rename->isVisible());
    history->setFocus();
    QTRY_VERIFY(history->hasFocus());
    QTest::keyClick(history, Qt::Key_Tab);
    QTRY_VERIFY(rename->hasFocus());
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
    QTest::qWait(25);
    for (int attempt = 0;
         attempt < 50 && imageScroll->widget()->width() > imageScroll->viewport()->width() + 1;
         ++attempt) {
        QTest::qWait(20);
    }
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
    QVERIFY2(imageScroll->widget()->width() <= imageScroll->viewport()->width() + 1,
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

    // A narrow Advanced camera gets one full-width toolbar: the action row
    // directly above the carousel row, both exactly as wide as the camera.
    const auto verifyStrip = [&](const QString& name) -> QString {
        const QRect view = viewport();
        const QRect left = GlobalRect(bottomLeft);
        const QRect right = GlobalRect(bottomRight);
        if (left.left() != view.left() || left.right() != view.right() ||
            right.left() != view.left() || right.right() != view.right()) {
            return QStringLiteral("%1: strip rows [%2..%3] and [%4..%5] do not span camera [%6..%7]")
                .arg(name).arg(left.left()).arg(left.right()).arg(right.left())
                .arg(right.right()).arg(view.left()).arg(view.right());
        }
        if (left.bottom() != view.bottom() || right.bottom() + 1 != left.top()) {
            return QStringLiteral("%1: action row bottom %2 is not stacked on carousel top %3 at %4")
                .arg(name).arg(right.bottom()).arg(left.top()).arg(view.bottom());
        }
        return {};
    };
    window.resize(1100, 760);
    window.advancedModeButton()->click();
    QTRY_VERIFY(!window.isSimpleMode());
    window.setAdvancedPanelWidth(440);
    QTRY_VERIFY(render->width() <= 700);
    QTRY_COMPARE(bottomRight->property("chromeDock").toString(), QStringLiteral("strip"));
    QCOMPARE(bottomLeft->property("chromeDock").toString(), QStringLiteral("strip"));
    QTRY_VERIFY2(verifyStrip(QStringLiteral("advanced")).isEmpty(),
                 qPrintable(verifyStrip(QStringLiteral("advanced"))));
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    SaveScreenshot(window, code + QStringLiteral("-bottom-chrome-advanced-strip"));

    // The narrowest camera wraps actions into balanced full-width rows.
    window.resize(720, 720);
    window.setAdvancedPanelWidth(360);
    QTRY_VERIFY(render->width() <= 400);
    QTRY_VERIFY2(verifyStrip(QStringLiteral("minimum")).isEmpty(),
                 qPrintable(verifyStrip(QStringLiteral("minimum"))));
    problem = ActionRowsProblem(window, &rows);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QVERIFY(rows > 1);
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
                             window.explainNowButton(), window.readTextButton(),
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
    assistant.SetContent(QStringLiteral("Assistant"), QStringLiteral("First part"), true);
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
    assistant.SetContent(QStringLiteral("Assistant"),
                         QStringLiteral("First part, completed"), true);
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
    auto* body = assistant.findChild<QTextBrowser*>(QStringLiteral("assistiveBody"));
    QVERIFY(body);
    QCOMPARE(body->toPlainText(), QStringLiteral("First part, completed"));
    SaveScreenshot(window, screenshotPrefix + QStringLiteral("-ui-restored"));
    if (advancedMode) {
        QCOMPARE(tabs->currentWidget(), settings);
        QTRY_VERIFY(tabs->isVisible());
        QVERIFY(window.advancedPanelWidth() >= 420);
    }
}

void MainWindowTests::mouseFocusedChromeCanIdleFade()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* idle = Named<QTimer>(window, "simpleChromeIdleTimer");
    QVERIFY(top && idle);
    QTRY_VERIFY(top->isVisible());
    top->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(top));
    QTest::mouseClick(window.simpleModeButton(), Qt::LeftButton);
    QTRY_VERIFY(window.simpleModeButton()->hasFocus());
    QVERIFY(idle->isActive());
    // A real single-shot timeout is no longer active when its signal runs.
    idle->stop();
    QVERIFY(QMetaObject::invokeMethod(idle, "timeout", Qt::DirectConnection));
    QTRY_VERIFY(!top->isVisible());
    QTest::qWait(300);
    QVERIFY(!top->isVisible());

    // Hover changes caused by hiding native panels are not pointer input.
    const QPointF stationaryGlobal(QCursor::pos());
    const QPointF stationaryLocal = window.renderWidget()->mapFromGlobal(
        stationaryGlobal.toPoint());
    QEnterEvent enter(stationaryLocal, stationaryLocal, stationaryGlobal);
    QCoreApplication::sendEvent(window.renderWidget(), &enter);
    QMouseEvent stationary(QEvent::MouseMove, stationaryLocal, stationaryLocal,
                           stationaryGlobal, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &stationary);
    QTest::qWait(100);
    QVERIFY(!top->isVisible());
    QCOMPARE(QGuiApplication::applicationState(), Qt::ApplicationActive);

    const QPointF local = window.renderWidget()->mapToGlobal(QPoint(20, 20)) == QCursor::pos()
                              ? QPointF(40, 40) : QPointF(20, 20);
    const QPointF global = window.renderWidget()->mapToGlobal(local.toPoint());
    QMouseEvent move(QEvent::MouseMove, local, local, global,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &move);
    QTRY_VERIFY(top->isVisible());
}

void MainWindowTests::unrelatedNativeInputCannotRevealChrome()
{
    class NativeInputWindow final : public MainWindow {
    public:
        using MainWindow::nativeEventFilter;
    };
    NativeInputWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* top = Named<QWidget>(window, "topLeftPanel");
    auto* idle = Named<QTimer>(window, "simpleChromeIdleTimer");
    QVERIFY(top && idle);
    QTRY_VERIFY(top->isVisible());
    window.renderWidget()->setFocus(Qt::MouseFocusReason);
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
    message.pt.x += 40;
    window.nativeEventFilter({}, &message, nullptr);
    QTRY_VERIFY(top->isVisible());
}

void MainWindowTests::keyboardFocusedChromeSurvivesNativeHandoff()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
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
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto* gridButton = Named<QToolButton>(window, "modeGridButton");
    QVERIFY(gridButton);
    gridButton->window()->activateWindow();
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
    window.activateWindow();
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

QTEST_MAIN(okuflow::MainWindowTests)
#include "main_window_tests.moc"
