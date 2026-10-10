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
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

void AddQuickModes(MainWindow& window)
{
    auto* list = window.presetList();
    list->addItem(QStringLiteral("Read a page"));
    list->addItem(QStringLiteral("High contrast"));
    list->addItem(QStringLiteral("View a board"));
    list->setCurrentRow(0);
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
    void gridBrowsingRequiresDeliberateActivation();
    void carouselUsesAppliedModeDuringGridBrowse();
    void searchCanHandoffBetweenTabs();
    void activeSearchRecomputesAfterLanguageSwitch();
    void busyActionSurvivesGermanResize();
    void keyboardRegionsAndFindShortcut();
    void assistantInspectorTabTraversal();
    void minimumViewportChromeFits_data();
    void minimumViewportChromeFits();
    void hideUiPreservesPriorModeAndStreaming_data();
    void hideUiPreservesPriorModeAndStreaming();
    void mouseFocusedChromeCanIdleFade();
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
    QVERIFY(QMetaObject::invokeMethod(idle, "timeout", Qt::DirectConnection));
    QTRY_VERIFY(!top->isVisible());
    QTest::qWait(100);
    QVERIFY(!top->isVisible());

    const QPointF local(20, 20);
    const QPointF global = window.renderWidget()->mapToGlobal(local.toPoint());
    QMouseEvent move(QEvent::MouseMove, local, local, global,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.renderWidget(), &move);
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
