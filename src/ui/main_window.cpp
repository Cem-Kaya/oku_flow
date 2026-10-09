#ifdef _WIN32

#include "openzoom/ui/main_window.hpp"
#include "openzoom/ui/assistive_overlay.hpp"
#include "openzoom/ui/annotation_overlay.hpp"
#include "openzoom/ui/joystick_overlay.hpp"

#include "openzoom/app/app.hpp"
#include "openzoom/app/constants.hpp"
#include "openzoom/app/language_manager.hpp"
#include "openzoom/d3d12/presenter.hpp"
#include "openzoom/ui/color_scheme_picker.hpp"
#include "openzoom/ui/collapsible_section.hpp"
#include "openzoom/ui/live_status_text.hpp"
#include "openzoom/ui/responsive_slider_row.hpp"
#include "openzoom/ui/wheel_safe_combo_box.hpp"
#include "openzoom/ui/ui_translation.hpp"

#include <QAbstractItemModel>
#include <QAbstractButton>
#include <QAction>
#include <QAccessible>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QAbstractItemView>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QParallelAnimationGroup>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>

namespace openzoom {

using namespace app_constants;

namespace {

constexpr int kSimpleChromeIdleMs = 5000;
constexpr int kSimpleChromeFadeMs = 260;
constexpr int kModeToastMs = 1200;
constexpr int kSimpleShortcutCount = 9;
constexpr int kAdvancedPanelMinimumWidth = 360;
constexpr int kAdvancedPanelDefaultWidth = 520;

QString PlainPresetLabel(const QString& original)
{
    if (original == QStringLiteral("Reading")) return QStringLiteral("Read a Page");
    if (original == QStringLiteral("High Contrast")) return QStringLiteral("High Contrast");
    if (original == QStringLiteral("Steady Text")) return QStringLiteral("Keep It Steady");
    if (original == QStringLiteral("Sharp Text")) return QStringLiteral("Sharpen Text");
    if (original == QStringLiteral("Large Zoom")) return QStringLiteral("Zoom In More");
    if (original == QStringLiteral("Low Light")) return QStringLiteral("See in Low Light");
    if (original == QStringLiteral("Scene Explain")) return QStringLiteral("Describe the Scene");
    return original;
}

bool HasEditableTextFocus()
{
    QWidget* focus = QApplication::focusWidget();
    while (focus) {
        if (qobject_cast<QLineEdit*>(focus) ||
            qobject_cast<QPlainTextEdit*>(focus)) {
            return true;
        }
        if (auto* combo = qobject_cast<QComboBox*>(focus); combo && combo->isEditable()) {
            return true;
        }
        focus = focus->parentWidget();
    }
    return false;
}

}  // namespace

MainWindow::MainWindow()
{
    setWindowTitle("OpenZoom");
    resize(1280, 720);
    QPalette readablePalette = palette();
    const QColor disabledText(QStringLiteral("#8f8f8f"));
    readablePalette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    readablePalette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    readablePalette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    setPalette(readablePalette);

    // Whole-app readability: bigger base font, a strong visible focus outline
    // on every interactive control, and chunky slider handles. Palette roles
    // are used throughout so the native light/dark theme is preserved.
    setStyleSheet(QStringLiteral(R"(
        QWidget { font-size: 12pt; }
        QPushButton, QToolButton {
            min-height: 32px;
            padding: 4px 14px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(button);
        }
        QPushButton:hover, QToolButton:hover { background: palette(midlight); }
        QPushButton:checked, QToolButton:checked {
            background: palette(highlight);
            color: palette(highlighted-text);
        }
        QPushButton:focus, QToolButton:focus { border: 3px solid palette(highlight); }
        QPushButton:disabled, QToolButton:disabled { color: #8f8f8f; }
        QComboBox {
            min-height: 32px;
            padding: 2px 10px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(button);
        }
        QComboBox:focus { border: 3px solid palette(highlight); }
        QCheckBox { spacing: 8px; padding: 2px; border: 3px solid transparent; border-radius: 6px; }
        QCheckBox:focus { border-color: palette(highlight); }
        QCheckBox::indicator { width: 20px; height: 20px; }
        QSlider { min-height: 30px; border: 3px solid transparent; border-radius: 6px; }
        QSlider:focus { border-color: palette(highlight); }
        QSlider::groove:horizontal { height: 8px; border-radius: 4px; background: palette(mid); }
        QSlider::sub-page:horizontal { background: palette(highlight); border-radius: 4px; }
        QSlider::handle:horizontal {
            width: 22px;
            margin: -8px 0;
            border-radius: 11px;
            background: palette(button);
            border: 2px solid palette(dark);
        }
        QSlider::handle:horizontal:hover { background: palette(midlight); }
        QListWidget { border: 2px solid palette(mid); border-radius: 6px; }
        QListWidget:focus { border: 3px solid palette(highlight); }
        QListWidget::item { padding: 6px; }
        QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        QToolButton#collapsibleHeader {
            text-align: left;
            font-weight: 600;
            border-radius: 4px;
            background: palette(button);
        }
        QLabel#scopeSubtitle { color: #b3b3b3; }
        QLabel#formatWarningLabel { color: #ffd166; font-weight: 600; }
        QPushButton#simpleModeButton, QPushButton#advancedModeButton {
            font-size: 13pt;
            font-weight: 600;
            min-height: 36px;
            padding: 2px 20px;
        }
        QPushButton#simpleModeButton {
            border-top-right-radius: 0;
            border-bottom-right-radius: 0;
        }
        QPushButton#advancedModeButton {
            border-top-left-radius: 0;
            border-bottom-left-radius: 0;
        }
        QWidget#topLeftPanel, QWidget#bottomLeftPanel, QWidget#keystoneTrackingPanel, QWidget#bottomRightPanel,
        QWidget#modeGridPopup, QWidget#modeToast {
            background: #111111;
            border: 3px solid #f4f4f4;
        }
        QWidget#topLeftPanel { border-top: 0; border-left: 0; border-bottom-right-radius: 8px; }
        QWidget#bottomLeftPanel { border-bottom: 0; border-left: 0; border-top-right-radius: 8px; }
        QWidget#keystoneTrackingPanel { border-bottom: 0; border-top-left-radius: 8px; border-top-right-radius: 8px; }
        QWidget#bottomRightPanel { border-bottom: 0; border-right: 0; border-top-left-radius: 8px; }
        QWidget#modeGridPopup {
            border-left: 0;
            border-top-left-radius: 0;
            border-bottom-left-radius: 0;
            border-top-right-radius: 8px;
            border-bottom-right-radius: 8px;
        }
        QWidget#modeToast { border-color: palette(highlight); border-radius: 8px; }
        QLabel#brandLabel { font-size: 15pt; font-weight: 700; color: #ffffff; }
        QLabel#processingStatusLabel { font-size: 11pt; font-weight: 700; }
        QLabel#modeToastTitle { font-size: 34pt; font-weight: 800; color: #ffffff; }
        QLabel#modeToastSubtitle { font-size: 16pt; font-weight: 600; color: #ffffff; }
        QPushButton#currentModeButton { font-size: 15pt; font-weight: 700; min-width: 220px; }
        QPushButton#previousModeButton, QPushButton#nextModeButton,
        QToolButton#modeGridButton { min-width: 52px; min-height: 52px; padding: 2px; }
        QWidget#keystoneTrackingPanel QPushButton { min-width: 52px; min-height: 52px; padding: 2px; }
        QWidget#bottomRightPanel QPushButton { min-width: 88px; min-height: 58px; font-size: 11pt; }
        QToolButton#advancedTabArrow {
            min-width: 40px;
            max-width: 40px;
            min-height: 40px;
            max-height: 40px;
            padding: 5px;
        }
        QPushButton#advancedNavButton {
            min-height: 40px;
            max-height: 40px;
            padding: 5px 10px;
        }
        QListWidget#presetList {
            background: #111111;
            border: 0;
            border-radius: 0;
            font-size: 13pt;
            font-weight: 650;
            outline: 0;
        }
        QListWidget#presetList::item {
            background: #1c1c1c;
            color: #ffffff;
            border: 2px solid #f4f4f4;
            border-radius: 6px;
            padding: 10px;
        }
        QListWidget#presetList::item:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
            border: 3px solid #ffffff;
        }
        QListWidget#presetList::item:focus { border: 3px solid palette(highlight); }
        QTextBrowser, QPlainTextEdit {
            padding: 7px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(base);
        }
        QTextBrowser:focus, QPlainTextEdit:focus { border: 3px solid palette(highlight); }
        QTabBar::tab { min-height: 34px; min-width: 92px; padding: 4px 10px; }
        QLabel#sectionLabel {
            font-size: 13pt;
            font-weight: 600;
            padding: 8px 0 4px 0;
            color: #d79ae6;
        }
        QLabel#featureStatusLabel {
            font-size: 10pt;
            font-weight: 600;
            padding: 2px 0 4px 30px;
        }
        QSplitter#advancedContentSplitter::handle:horizontal {
            width: 10px;
            background: palette(mid);
            border-left: 2px solid palette(dark);
            border-right: 2px solid palette(light);
        }
        QSplitter#advancedContentSplitter::handle:horizontal:hover,
        QSplitter#advancedContentSplitter::handle:horizontal:pressed {
            background: palette(highlight);
        }
    )"));

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // These controls are parented into solid overlays after the render widget
    // is created. Advanced keeps the same inspector and reuses the mode switch.
    simpleModeButton_ = new QPushButton("Simple");
    simpleModeButton_->setObjectName("simpleModeButton");
    simpleModeButton_->setCheckable(true);
    simpleModeButton_->setChecked(true);
    advancedModeButton_ = new QPushButton("Advanced");
    advancedModeButton_->setObjectName("advancedModeButton");
    advancedModeButton_->setCheckable(true);
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(simpleModeButton_);
    modeGroup->addButton(advancedModeButton_);

    processingStatusLabel_ = new QLabel("Processing: CPU");
    processingStatusLabel_->setObjectName("processingStatusLabel");
    processingStatusLabel_->setWordWrap(true);
    processingStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                     Qt::TextSelectableByKeyboard);
    processingStatusLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    processingStatusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    performanceDiagnosticsLabel_ =
        new QLabel(QStringLiteral("Collecting timing samples\u2026"));
    performanceDiagnosticsLabel_->setObjectName(
        QStringLiteral("performanceDiagnosticsLabel"));
    performanceDiagnosticsLabel_->setWordWrap(true);
    performanceDiagnosticsLabel_->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    performanceDiagnosticsLabel_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Preferred);

    presetList_ = new QListWidget();
    presetList_->setObjectName("presetList");
    presetList_->setSelectionMode(QAbstractItemView::SingleSelection);
    presetList_->setFocusPolicy(Qt::StrongFocus);
    presetList_->setFlow(QListView::LeftToRight);
    presetList_->setViewMode(QListView::IconMode);
    presetList_->setWrapping(true);
    presetList_->setMovement(QListView::Static);
    presetList_->setResizeMode(QListView::Adjust);
    presetList_->setWordWrap(true);
    presetList_->setTextElideMode(Qt::ElideNone);
    presetList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    presetList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    presetList_->setGridSize(QSize(205, 102));
    presetList_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    presetList_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    presetList_->setSpacing(4);

    presetDescriptionLabel_ = new QLabel();
    presetDescriptionLabel_->setObjectName("presetDescriptionLabel");
    presetDescriptionLabel_->hide();

    capturePhotoButton_ = new QPushButton("Photo");
    recordButton_ = new QPushButton("Record");
    recordButton_->setCheckable(true);
    explainNowButton_ = new QPushButton("Explain");
    readTextButton_ = new QPushButton("Read");
    annotationButton_ = new QPushButton(QStringLiteral("Draw"));
    annotationButton_->setCheckable(true);
    capturePhotoButton_->setToolTip("Capture original and processed photos");
    recordButton_->setToolTip("Record original and processed videos");
    explainNowButton_->setToolTip("Explain the current view");
    readTextButton_->setToolTip("Read text in the current view");

    // Advanced mode uses a right-side inspector so detailed controls do not
    // push the live image below the fold.
    auto* advancedPage = new QWidget();
    advancedPage->setObjectName("advancedPage");
    auto* advancedLayout = new QVBoxLayout(advancedPage);
    advancedLayout->setContentsMargins(10, 4, 10, 10);
    advancedLayout->setSpacing(8);

    auto makeSectionLabel = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setObjectName("sectionLabel");
        return label;
    };

    settingsSearchEdit_ = new QLineEdit();
    settingsSearchEdit_->setPlaceholderText(QStringLiteral("Search settings..."));
    settingsSearchEdit_->setClearButtonEnabled(true);
    settingsSearchEdit_->setAccessibleName(QStringLiteral("Search Advanced settings"));
    settingsSearchEdit_->setAccessibleDescription(
        QStringLiteral("Type part of a setting name to reveal and focus matching controls."));
    advancedLayout->addWidget(settingsSearchEdit_);

    applicationSection_ =
        new CollapsibleSection(QStringLiteral("Application"));
    applicationSection_->setPersistKey(QStringLiteral("application"));
    auto* applicationLayout =
        qobject_cast<QVBoxLayout*>(
            applicationSection_->contentWidget()->layout());
    auto* languageLabel =
        new QLabel(QStringLiteral("Application language"));
    applicationLanguageCombo_ = new WheelSafeComboBox();
    applicationLanguageCombo_->setIconSize(QSize(30, 20));
    for (AppLanguage language : SupportedAppLanguages()) {
        applicationLanguageCombo_->addItem(
            QIcon(AppLanguageFlagResource(language)),
            AppLanguageNativeName(language),
            AppLanguageCode(language));
    }
    applicationLanguageCombo_->setAccessibleName(
        QStringLiteral("Application language"));
    applicationLanguageCombo_->setAccessibleDescription(
        QStringLiteral(
            "Changes OpenZoom's interface language immediately. "
            "An assistant request already in progress keeps its original "
            "response language."));
    applicationLanguageCombo_->setToolTip(
        QStringLiteral(
            "Changes the interface immediately. In-progress assistant "
            "requests use the language selected when they started."));
    SetComboItemsAreData(applicationLanguageCombo_);
    languageLabel->setBuddy(applicationLanguageCombo_);
    applicationLayout->addWidget(languageLabel);
    applicationLayout->addWidget(applicationLanguageCombo_);
    advancedLayout->addWidget(applicationSection_);
    connect(applicationLanguageCombo_,
            &QComboBox::currentIndexChanged,
            this,
            [this](int index) {
                if (index >= 0) {
                    emit applicationLanguageRequested(
                        applicationLanguageCombo_->itemData(index).toString());
                }
            });

    deviceSection_ = new CollapsibleSection(QStringLiteral("Device"));
    deviceSection_->setPersistKey(QStringLiteral("device"));
    auto* deviceLayout =
        qobject_cast<QVBoxLayout*>(deviceSection_->contentWidget()->layout());
    auto* deviceScopeLabel = new QLabel(QStringLiteral("Shared by every profile"));
    deviceScopeLabel->setObjectName(QStringLiteral("scopeSubtitle"));
    deviceLayout->addWidget(deviceScopeLabel);
    auto* cameraLabel = new QLabel("Camera");
    cameraCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(cameraCombo_);
    cameraCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cameraCombo_->setMinimumContentsLength(20);
    cameraLabel->setBuddy(cameraCombo_);
    deviceLayout->addWidget(cameraLabel);
    deviceLayout->addWidget(cameraCombo_);

    auto* rotationLabel = new QLabel("Orientation");
    rotationCombo_ = new WheelSafeComboBox();
    rotationCombo_->addItem("0°");
    rotationCombo_->addItem("90°");
    rotationCombo_->addItem("180°");
    rotationCombo_->addItem("270°");
    rotationCombo_->setCurrentIndex(0);
    rotationCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    rotationCombo_->setEditable(false);
    rotationCombo_->setToolTip("Rotate input/output clockwise");
    rotationLabel->setBuddy(rotationCombo_);
    deviceLayout->addWidget(rotationLabel);
    deviceLayout->addWidget(rotationCombo_);

    deviceMoreSection_ =
        new CollapsibleSection(QStringLiteral("More device options"));
    deviceMoreSection_->setPersistKey(QStringLiteral("deviceMore"));
    deviceMoreSection_->setExpanded(false);
    auto* deviceMoreLayout =
        qobject_cast<QVBoxLayout*>(deviceMoreSection_->contentWidget()->layout());

    auto* cameraAccelerationLabel = new QLabel(QStringLiteral("Camera acceleration"));
    cameraAccelerationCombo_ = new WheelSafeComboBox();
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Automatic (recommended)"), 0);
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Always use GPU acceleration"), 1);
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Compatibility mode"), 2);
    cameraAccelerationCombo_->setToolTip(
        QStringLiteral(
            "Automatic uses hardware acceleration when the camera and driver "
            "pass startup validation, then falls back safely if needed."));
    cameraAccelerationCombo_->setAccessibleName(
        QStringLiteral("Camera acceleration mode"));
    cameraAccelerationLabel->setBuddy(cameraAccelerationCombo_);
    deviceMoreLayout->addWidget(cameraAccelerationLabel);
    deviceMoreLayout->addWidget(cameraAccelerationCombo_);

    cameraAccelerationStatusLabel_ = new QLabel(
        QStringLiteral("Camera acceleration has not started yet."));
    cameraAccelerationStatusLabel_->setObjectName(QStringLiteral("scopeSubtitle"));
    cameraAccelerationStatusLabel_->setWordWrap(true);
    cameraAccelerationStatusLabel_->setAccessibleName(
        QStringLiteral("Camera acceleration status"));
    deviceMoreLayout->addWidget(cameraAccelerationStatusLabel_);

    testCameraAccelerationButton_ =
        new QPushButton(QStringLiteral("Test this camera"));
    testCameraAccelerationButton_->setToolTip(
        QStringLiteral(
            "Tests GPU and compatibility capture in a separate process. "
            "The test cannot freeze OpenZoom if a camera driver hangs."));
    testCameraAccelerationButton_->setAccessibleName(
        QStringLiteral("Test camera acceleration"));
    testCameraAccelerationButton_->setAccessibleDescription(
        QStringLiteral(
            "Runs an isolated camera test and updates this camera's "
            "automatic acceleration decision."));
    deviceMoreLayout->addWidget(testCameraAccelerationButton_);

    auto* viewportRateLabel = new QLabel("Viewport motion rate");
    viewportRateCombo_ = new WheelSafeComboBox();
    viewportRateCombo_->addItem("Auto (up to 120 FPS)", 0);
    viewportRateCombo_->addItem("60 FPS", 1);
    viewportRateCombo_->addItem("90 FPS", 2);
    viewportRateCombo_->addItem("120 FPS", 3);
    viewportRateCombo_->addItem("Match display", 4);
    viewportRateCombo_->setToolTip(
        "Controls smooth pan and zoom presentation; camera frame rate is unchanged");
    viewportRateLabel->setBuddy(viewportRateCombo_);
    deviceMoreLayout->addWidget(viewportRateLabel);
    deviceMoreLayout->addWidget(viewportRateCombo_);

    auto* viewportFitLabel = new QLabel("Viewport framing");
    viewportFitCombo_ = new WheelSafeComboBox();
    viewportFitCombo_->addItem("Fill (crop)", 0);
    viewportFitCombo_->addItem("Fit (show all)", 1);
    viewportFitCombo_->setToolTip(
        "Fill uses the full viewport without stretching; Fit preserves the entire image with bars");
    viewportFitLabel->setBuddy(viewportFitCombo_);
    deviceMoreLayout->addWidget(viewportFitLabel);
    deviceMoreLayout->addWidget(viewportFitCombo_);

    controlsToggleButton_ = new QToolButton();
    controlsToggleButton_->setText("Advanced Tuning");
    controlsToggleButton_->setCheckable(true);
    controlsToggleButton_->setChecked(false);
    controlsToggleButton_->setArrowType(Qt::RightArrow);
    controlsToggleButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    promotePresetButton_ = new QPushButton("Save As Quick Option");
    joystickCheckbox_ = new QCheckBox("Virtual Joystick");
    joystickCheckbox_->setToolTip(
        QStringLiteral("Show an on-screen control for moving the zoomed view"));
    deviceMoreLayout->addWidget(joystickCheckbox_);
    zoomWheelAccelerationCheckbox_ =
        new QCheckBox(QStringLiteral("Zoom wheel acceleration"));
    zoomWheelAccelerationCheckbox_->setChecked(true);
    zoomWheelAccelerationCheckbox_->setToolTip(
        QStringLiteral("Accelerate fast Ctrl+scroll zoom gestures; keyboard zoom remains exact"));
    deviceMoreLayout->addWidget(zoomWheelAccelerationCheckbox_);
    deviceLayout->addWidget(deviceMoreSection_);
    advancedLayout->addWidget(deviceSection_);

    recordingSection_ = new CollapsibleSection(QStringLiteral("Recording"));
    recordingSection_->setPersistKey(QStringLiteral("recording"));
    auto* recordingLayout =
        qobject_cast<QVBoxLayout*>(recordingSection_->contentWidget()->layout());
    auto* recordingScopeLabel =
        new QLabel(QStringLiteral("Shared by original and processed recordings"));
    recordingScopeLabel->setObjectName(QStringLiteral("scopeSubtitle"));
    recordingLayout->addWidget(recordingScopeLabel);

    auto* recordingCanvasLabel =
        new QLabel(QStringLiteral("Processed recording resolution"));
    recordingCanvasCombo_ = new WheelSafeComboBox();
    recordingCanvasCombo_->addItem(
        "Match camera (recommended)",
        static_cast<int>(RecordingCanvasMode::Source));
    recordingCanvasCombo_->addItem(
        "360p (640 x 360)",
        static_cast<int>(RecordingCanvasMode::Nhd360));
    recordingCanvasCombo_->addItem(
        "480p (854 x 480)",
        static_cast<int>(RecordingCanvasMode::Sd480));
    recordingCanvasCombo_->addItem(
        "HD (1280 x 720)",
        static_cast<int>(RecordingCanvasMode::Hd720));
    recordingCanvasCombo_->addItem(
        "Full HD (1920 x 1080)",
        static_cast<int>(RecordingCanvasMode::FullHd1080));
    recordingCanvasCombo_->addItem(
        "Quad HD (2560 x 1440)",
        static_cast<int>(RecordingCanvasMode::QuadHd1440));
    recordingCanvasCombo_->addItem(
        "Ultra HD (3840 x 2160)",
        static_cast<int>(RecordingCanvasMode::UltraHd2160));
    recordingCanvasCombo_->setToolTip(
        "Controls the processed MP4 resolution. The original MP4 always uses "
        "the selected camera mode resolution.");
    recordingCanvasLabel->setBuddy(recordingCanvasCombo_);
    recordingLayout->addWidget(recordingCanvasLabel);
    recordingLayout->addWidget(recordingCanvasCombo_);

    auto* cameraFormatLabel =
        new QLabel(QStringLiteral("Camera and original recording resolution && frame rate"));
    cameraFormatCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(cameraFormatCombo_);
    cameraFormatCombo_->addItem(QStringLiteral("Automatic (driver's choice)"),
                                QString());
    cameraFormatCombo_->setToolTip(
        QStringLiteral("Controls the live camera plus original photos and "
                       "original video. Processed video uses the resolution above."));
    cameraFormatLabel->setBuddy(cameraFormatCombo_);
    cameraFormatNoticeLabel_ = new QLabel();
    cameraFormatNoticeLabel_->setObjectName(QStringLiteral("formatWarningLabel"));
    cameraFormatNoticeLabel_->setWordWrap(true);
    cameraFormatNoticeLabel_->setVisible(false);
    recordingLayout->addWidget(cameraFormatLabel);
    recordingLayout->addWidget(cameraFormatCombo_);
    recordingLayout->addWidget(cameraFormatNoticeLabel_);

    auto* microphoneLabel = new QLabel(QStringLiteral("Microphone"));
    microphoneCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(microphoneCombo_);
    microphoneCombo_->addItem(
        QStringLiteral("No microphone (video only)"),
        QStringLiteral("__none__"));
    microphoneCombo_->setToolTip(
        QStringLiteral(
            "Select sound for both original and processed video recordings. "
            "The microphone is used only while recording."));
    microphoneLabel->setBuddy(microphoneCombo_);
    recordingLayout->addWidget(microphoneLabel);
    recordingLayout->addWidget(microphoneCombo_);

    transcribeMicrophoneCheckbox_ =
        new QCheckBox(QStringLiteral("Transcribe microphone while recording"));
    transcribeMicrophoneCheckbox_->setToolTip(QStringLiteral(
        "Sends microphone audio to Codex Voice for live transcription, only "
        "while recording. Requires Codex signed in with ChatGPT."));
    recordingLayout->addWidget(transcribeMicrophoneCheckbox_);
    transcriptToNotesCheckbox_ =
        new QCheckBox(QStringLiteral("Add finalized transcript to lecture notes"));
    transcriptToNotesCheckbox_->setToolTip(QStringLiteral(
        "Appends each finalized phrase to the HTML lecture notes. Effective "
        "only while lecture notes are enabled."));
    transcriptToNotesCheckbox_->setEnabled(false);
    recordingLayout->addWidget(transcriptToNotesCheckbox_);
    connect(transcribeMicrophoneCheckbox_, &QCheckBox::toggled,
            transcriptToNotesCheckbox_, &QWidget::setEnabled);
    transcriptionStatusLabel_ = new QLabel();
    transcriptionStatusLabel_->setObjectName(QStringLiteral("transcriptionStatusLabel"));
    transcriptionStatusLabel_->setWordWrap(true);
    transcriptionStatusLabel_->setVisible(false);
    recordingLayout->addWidget(transcriptionStatusLabel_);
    transcriptionQuotaLabel_ = new QLabel();
    transcriptionQuotaLabel_->setObjectName(QStringLiteral("transcriptionQuotaLabel"));
    transcriptionQuotaLabel_->setWordWrap(true);
    transcriptionQuotaLabel_->setVisible(false);
    recordingLayout->addWidget(transcriptionQuotaLabel_);
    advancedLayout->addWidget(recordingSection_);

    advancedLayout->addWidget(makeSectionLabel("Profile"));
    auto* profileScopeLabel =
        new QLabel(QStringLiteral("Saved into the selected quick option"));
    profileScopeLabel->setObjectName(QStringLiteral("scopeSubtitle"));
    advancedLayout->addWidget(profileScopeLabel);
    auto* advancedHeaderLayout = new QVBoxLayout();
    advancedHeaderLayout->setSpacing(6);
    controlsToggleButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    promotePresetButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    resetProfileButton_ = new QPushButton(QStringLiteral("Reset Tuning"));
    resetProfileButton_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    resetProfileButton_->setToolTip(
        QStringLiteral("Reset profile-owned image and assistive tuning to defaults"));
    resetProfileButton_->setAccessibleName(QStringLiteral("Reset current profile tuning"));
    resetProfileButton_->setAccessibleDescription(
        QStringLiteral("Reset profile-owned settings while keeping the camera, orientation, "
                       "viewport rate, framing, and virtual joystick unchanged"));
    resetProfileButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    advancedHeaderLayout->addWidget(controlsToggleButton_);
    advancedHeaderLayout->addWidget(promotePresetButton_);
    advancedHeaderLayout->addWidget(resetProfileButton_);
    advancedLayout->addLayout(advancedHeaderLayout);
    connect(resetProfileButton_, &QPushButton::clicked,
            this, &MainWindow::resetCurrentProfileRequested);

    controlsContainer_ = new QWidget();
    auto* controlsLayout = new QVBoxLayout(controlsContainer_);
    controlsLayout->setContentsMargins(0, 0, 0, 12);
    controlsLayout->setSpacing(8);

    magnificationSection_ = new CollapsibleSection(QStringLiteral("Magnification"));
    magnificationSection_->setPersistKey(QStringLiteral("magnification"));
    auto* magnificationLayout =
        qobject_cast<QVBoxLayout*>(magnificationSection_->contentWidget()->layout());
    controlsLayout->addWidget(magnificationSection_);

    readabilitySection_ = new CollapsibleSection(QStringLiteral("Readability"));
    readabilitySection_->setPersistKey(QStringLiteral("readability"));
    auto* readabilityLayout =
        qobject_cast<QVBoxLayout*>(readabilitySection_->contentWidget()->layout());
    controlsLayout->addWidget(readabilitySection_);

    auto* bwLayout = new QHBoxLayout();
    bwLayout->setSpacing(8);
    bwCheckbox_ = new QCheckBox("Black && White");
    bwSlider_ = new WheelSafeSlider(Qt::Horizontal);
    bwSlider_->setRange(0, 255);
    bwSlider_->setPageStep(8);
    bwSlider_->setValue(128);
    bwSlider_->setEnabled(false);
    bwLayout->addWidget(bwCheckbox_);
    bwLayout->addWidget(bwSlider_, 1);
    readabilityLayout->addLayout(bwLayout);

    auto* zoomLayout = new QHBoxLayout();
    zoomLayout->setSpacing(8);
    zoomCheckbox_ = new QCheckBox("Zoom");
    zoomSlider_ = new WheelSafeSlider(Qt::Horizontal);
    zoomSlider_->setRange(kZoomSliderScale, kZoomSliderMaxMultiplier * kZoomSliderScale);
    zoomSlider_->setPageStep(10);
    zoomSlider_->setValue(kZoomSliderScale);
    zoomSlider_->setEnabled(false);
    zoomLayout->addWidget(zoomCheckbox_);
    zoomLayout->addWidget(zoomSlider_, 1);
    magnificationLayout->addLayout(zoomLayout);

    auto* blurLayout = new QVBoxLayout();
    blurLayout->setSpacing(8);
    blurCheckbox_ = new QCheckBox("Gaussian Blur");
    blurSigmaSlider_ = new WheelSafeSlider(Qt::Horizontal);
    blurSigmaSlider_->setRange(kBlurSigmaSliderMin, kBlurSigmaSliderMax);
    blurSigmaSlider_->setPageStep(2);
    blurSigmaSlider_->setSingleStep(1);
    blurSigmaSlider_->setValue(10);
    blurSigmaSlider_->setEnabled(false);
    blurSigmaValueLabel_ = new QLabel("1.0");
    blurSigmaValueLabel_->setMinimumWidth(40);

    blurRadiusSlider_ = new WheelSafeSlider(Qt::Horizontal);
    blurRadiusSlider_->setRange(kSupportedBlurRadii.front(), kSupportedBlurRadii.back());
    blurRadiusSlider_->setPageStep(1);
    blurRadiusSlider_->setSingleStep(1);
    blurRadiusSlider_->setTickInterval(1);
    blurRadiusSlider_->setTickPosition(QSlider::TicksBelow);
    blurRadiusSlider_->setValue(3);
    blurRadiusSlider_->setEnabled(false);
    blurRadiusValueLabel_ = new QLabel("3");
    blurRadiusValueLabel_->setMinimumWidth(40);

    blurLayout->addWidget(blurCheckbox_);
    auto* blurSigmaLayout = new QHBoxLayout();
    blurSigmaLayout->addWidget(new QLabel("Sigma"));
    blurSigmaLayout->addWidget(blurSigmaSlider_, 1);
    blurSigmaLayout->addWidget(blurSigmaValueLabel_);
    blurLayout->addLayout(blurSigmaLayout);
    auto* blurRadiusLayout = new QHBoxLayout();
    blurRadiusLayout->addWidget(new QLabel("Radius"));
    blurRadiusLayout->addWidget(blurRadiusSlider_, 1);
    blurRadiusLayout->addWidget(blurRadiusValueLabel_);
    blurLayout->addLayout(blurRadiusLayout);
    readabilityLayout->addLayout(blurLayout);

    stabilitySection_ = new CollapsibleSection(QStringLiteral("Stability"));
    stabilitySection_->setPersistKey(QStringLiteral("stability"));
    auto* stabilityLayout =
        qobject_cast<QVBoxLayout*>(stabilitySection_->contentWidget()->layout());
    controlsLayout->addWidget(stabilitySection_);
    auto* temporalLayout = new QHBoxLayout();
    temporalLayout->setSpacing(8);
    temporalSmoothCheckbox_ = new QCheckBox("Temporal Smooth");
    temporalSmoothCheckbox_->setChecked(false);
    temporalSmoothSlider_ = new WheelSafeSlider(Qt::Horizontal);
    temporalSmoothSlider_->setRange(5, 100);
    temporalSmoothSlider_->setPageStep(5);
    temporalSmoothSlider_->setValue(25);
    temporalSmoothSlider_->setEnabled(false);
    temporalSmoothValueLabel_ = new QLabel("0.25");
    temporalSmoothValueLabel_->setMinimumWidth(40);
    temporalLayout->addWidget(temporalSmoothCheckbox_);
    temporalLayout->addSpacing(12);
    temporalLayout->addWidget(new QLabel("Blend:"));
    temporalLayout->addWidget(temporalSmoothSlider_, 1);
    temporalLayout->addWidget(temporalSmoothValueLabel_);
    stabilityLayout->addLayout(temporalLayout);

    auto* stabilizationLayout = new QHBoxLayout();
    stabilizationLayout->setSpacing(8);
    stabilizationCheckbox_ = new QCheckBox("Stabilize Image");
    stabilizationCheckbox_->setToolTip(
        "Lock the mounted camera view using full-strength CUDA stabilization");
    stabilizationLayout->addWidget(stabilizationCheckbox_);
    stabilizationLayout->addStretch(1);
    stabilityLayout->addLayout(stabilizationLayout);

    bumpHoldCheckbox_ = new QCheckBox("Extra Stable");
    bumpHoldCheckbox_->setChecked(false);
    bumpHoldCheckbox_->setEnabled(false);
    bumpHoldCheckbox_->setToolTip(
        "Freeze on the last sharp frame during a bump, then smoothly return to live video");
    stabilityLayout->addWidget(bumpHoldCheckbox_);

    screenFixSection_ = new CollapsibleSection(QStringLiteral("Screen fix"));
    screenFixSection_->setPersistKey(QStringLiteral("screenFix"));
    auto* screenFixLayout =
        qobject_cast<QVBoxLayout*>(screenFixSection_->contentWidget()->layout());
    controlsLayout->addWidget(screenFixSection_);
    auto* keystoneLayout = new QHBoxLayout();
    keystoneLayout->setSpacing(8);
    keystoneCheckbox_ = new QCheckBox("Straighten Screen (Keystone)");
    keystoneLayout->addWidget(keystoneCheckbox_);
    keystoneLayout->addStretch(1);
    screenFixLayout->addLayout(keystoneLayout);

    advancedKeystoneTrackingRow_ = new QWidget();
    auto* advancedTrackingLayout = new QHBoxLayout(advancedKeystoneTrackingRow_);
    advancedTrackingLayout->setContentsMargins(0, 0, 0, 0);
    advancedTrackingLayout->setSpacing(8);
    advancedTrackingLayout->addWidget(new QLabel("Correction tracking:"));
    advancedKeystoneBackButton_ = new QPushButton("Back");
    advancedKeystonePauseButton_ = new QPushButton("Stop");
    advancedKeystoneNextButton_ = new QPushButton("Next");
    advancedKeystoneBackButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/step-back.svg")));
    advancedKeystonePauseButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/pause.svg")));
    advancedKeystoneNextButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/step-forward.svg")));
    for (QPushButton* button : {advancedKeystoneBackButton_, advancedKeystonePauseButton_,
                                advancedKeystoneNextButton_}) {
        button->setIconSize(QSize(22, 22));
        advancedTrackingLayout->addWidget(button);
    }
    advancedTrackingLayout->addStretch(1);
    advancedKeystoneTrackingRow_->setEnabled(false);
    screenFixLayout->addWidget(advancedKeystoneTrackingRow_);

    auto* autoContrastLayout = new QHBoxLayout();
    autoContrastLayout->setSpacing(8);
    autoContrastCheckbox_ = new QCheckBox("Auto Contrast");
    autoContrastStrengthSlider_ = new WheelSafeSlider(Qt::Horizontal);
    autoContrastStrengthSlider_->setRange(0, 100);
    autoContrastStrengthSlider_->setPageStep(5);
    autoContrastStrengthSlider_->setValue(70);
    autoContrastStrengthSlider_->setEnabled(false);
    autoContrastLayout->addWidget(autoContrastCheckbox_);
    autoContrastLayout->addSpacing(12);
    autoContrastLayout->addWidget(new QLabel("Strength:"));
    autoContrastLayout->addWidget(autoContrastStrengthSlider_, 1);
    readabilityLayout->addLayout(autoContrastLayout);

    textClaritySection_ = new CollapsibleSection(QStringLiteral("Text clarity"));
    textClaritySection_->setPersistKey(QStringLiteral("textClarity"));
    auto* textClaritySectionLayout =
        qobject_cast<QVBoxLayout*>(textClaritySection_->contentWidget()->layout());
    controlsLayout->addWidget(textClaritySection_);
    auto* textClarityLayout = new QVBoxLayout();
    textClarityLayout->setSpacing(8);
    textClarityCheckbox_ = new QCheckBox("Auto Text Clarity");
    textClarityLayout->addWidget(textClarityCheckbox_);

    auto addTextSliderRow = [textClarityLayout](QCheckBox*& checkbox,
                                                const QString& label,
                                                QSlider*& slider,
                                                int minimum, int maximum, int value) {
        checkbox = new QCheckBox(label);
        slider = new WheelSafeSlider(Qt::Horizontal);
        slider->setRange(minimum, maximum);
        slider->setPageStep(std::max(1, (maximum - minimum) / 10));
        slider->setValue(value);
        textClarityLayout->addWidget(new ResponsiveSliderRow(checkbox, slider));
    };
    addTextSliderRow(backgroundFlattenCheckbox_, "Flatten Background",
                     backgroundFlattenStrengthSlider_, 0, 100, 80);
    addTextSliderRow(adaptiveBinarizationCheckbox_, "Adaptive Text",
                     sauvolaStrengthSlider_, 10, 50, 28);

    auto* softRow = new QHBoxLayout();
    softRow->addWidget(new QLabel("Edge softness"));
    binarizationSoftnessSlider_ = new WheelSafeSlider(Qt::Horizontal);
    binarizationSoftnessSlider_->setRange(0, 25);
    binarizationSoftnessSlider_->setValue(6);
    softRow->addWidget(binarizationSoftnessSlider_, 1);
    textClarityLayout->addLayout(softRow);

    auto* polarityRow = new QHBoxLayout();
    polarityRow->addWidget(new QLabel("Text polarity"));
    textPolarityCombo_ = new WheelSafeComboBox();
    textPolarityCombo_->addItems({"Auto", "Dark on light", "Light on dark"});
    polarityRow->addWidget(textPolarityCombo_, 1);
    textClarityLayout->addLayout(polarityRow);

    auto* strokeRow = new QHBoxLayout();
    strokeRow->addWidget(new QLabel("Stroke weight"));
    strokeWeightSlider_ = new WheelSafeSlider(Qt::Horizontal);
    strokeWeightSlider_->setRange(-3, 3);
    strokeWeightSlider_->setValue(0);
    strokeWeightSlider_->setTickInterval(1);
    strokeWeightSlider_->setTickPosition(QSlider::TicksBelow);
    strokeRow->addWidget(strokeWeightSlider_, 1);
    textClarityLayout->addLayout(strokeRow);

    addTextSliderRow(smartSharpenCheckbox_, "Smart Sharpen",
                     smartSharpenStrengthSlider_, 0, 100, 45);
    addTextSliderRow(claheCheckbox_, "Local Contrast (CLAHE)",
                     claheClipLimitSlider_, 10, 80, 20);
    twoColorTextCheckbox_ = new QCheckBox("Two-Color Reading");
    textClarityLayout->addWidget(twoColorTextCheckbox_);
    addTextSliderRow(textHysteresisCheckbox_, "Steady Text Edges",
                     textHysteresisStrengthSlider_, 0, 25, 8);
    selectiveSharpenCheckbox_ = new QCheckBox("Sharpen Text Only");
    textClarityLayout->addWidget(selectiveSharpenCheckbox_);
    addTextSliderRow(focusDetectionCheckbox_, "Warn When Out of Focus",
                     focusThresholdSlider_, 1, 100, 12);
    addTextSliderRow(glareSuppressionCheckbox_, "Suppress Glare",
                     glareSuppressionStrengthSlider_, 0, 100, 50);
    addTextSliderRow(mlTextSuperResolutionCheckbox_, "NVIDIA Super Resolution",
                     mlTextSuperResolutionStrengthSlider_, 0, 100, 65);
    mlTextSuperResolutionUltra1440pCheckbox_ =
        new QCheckBox(QStringLiteral("Ultra quality (full frame, up to 1440p)"));
    mlTextSuperResolutionUltra1440pCheckbox_->setToolTip(
        QStringLiteral("Build a separate high-resolution AI scene from the full "
                       "camera frame, then apply viewport zoom and cropping. "
                       "720p cameras upscale 2x to 1440p; 1080p cameras upscale "
                       "4/3x to 1440p; 1440p cameras remain native."));
    textClarityLayout->addWidget(mlTextSuperResolutionUltra1440pCheckbox_);
    mlTextSuperResolutionPrefer2xCheckbox_ =
        new QCheckBox(QStringLiteral("Faster 2x mode (narrower view)"));
    mlTextSuperResolutionPrefer2xCheckbox_->setToolTip(
        QStringLiteral("Optional speed mode. Raises magnification to at least "
                       "2x, narrowing the visible source crop from 960x540 to "
                       "640x360 for a 1280x720 target. Leave off for maximum "
                       "source detail and a wider view."));
    textClarityLayout->addWidget(mlTextSuperResolutionPrefer2xCheckbox_);
    mlTextSuperResolutionStatusLabel_ = new QLabel(QStringLiteral("Off"));
    mlTextSuperResolutionStatusLabel_->setObjectName(QStringLiteral("featureStatusLabel"));
    mlTextSuperResolutionStatusLabel_->setWordWrap(true);
    mlTextSuperResolutionStatusLabel_->setMinimumWidth(0);
    mlTextSuperResolutionStatusLabel_->setSizePolicy(QSizePolicy::Ignored,
                                                     QSizePolicy::Preferred);
    mlTextSuperResolutionStatusLabel_->setAccessibleName(
        QStringLiteral("NVIDIA Super Resolution status"));
    textClarityLayout->addWidget(mlTextSuperResolutionStatusLabel_);
    mlTextSuperResolutionOverrideCheckbox_ =
        new QCheckBox(QStringLiteral("Ignore 24 ms performance limit"));
    mlTextSuperResolutionOverrideCheckbox_->setVisible(false);
    mlTextSuperResolutionOverrideCheckbox_->setAccessibleName(
        QStringLiteral("Ignore NVIDIA Super Resolution performance limit"));
    mlTextSuperResolutionOverrideCheckbox_->setAccessibleDescription(
        QStringLiteral("Keep NVIDIA Super Resolution active when its measured "
                       "latency exceeds 24 milliseconds. This can reduce camera frame rate."));
    mlTextSuperResolutionOverrideCheckbox_->setToolTip(
        QStringLiteral("Keep SuperRes active even when its average GPU time exceeds 24 ms"));
    connect(mlTextSuperResolutionOverrideCheckbox_,
            &QCheckBox::toggled,
            this,
            &MainWindow::superResPerformanceOverrideChanged);
    textClarityLayout->addWidget(mlTextSuperResolutionOverrideCheckbox_);
#if OPENZOOM_ENABLE_TEXT_SR
    mlTextSuperResolutionCheckbox_->setToolTip(
        "Use NVIDIA Video Effects SuperRes at 1.33x zoom and above; falls back to NIS automatically");
    mlTextSuperResolutionStrengthSlider_->setToolTip(
        "Set the NVIDIA SuperRes enhancement strength for this preset");
#else
    mlTextSuperResolutionCheckbox_->setEnabled(false);
    mlTextSuperResolutionStrengthSlider_->setEnabled(false);
    mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(false);
    mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(false);
    mlTextSuperResolutionCheckbox_->setToolTip(
        "Unavailable in this build; requires OPENZOOM_ENABLE_TEXT_SR");
#endif
    textClaritySectionLayout->addLayout(textClarityLayout);

    auto* displayColorLayout = new QVBoxLayout();
    displayColorLayout->setSpacing(8);
    displayColorLayout->addWidget(new QLabel("Display colors"));
    displayColorPicker_ = new ColorSchemePicker();
    displayColorLayout->addWidget(displayColorPicker_);
    contrastSlider_ = new WheelSafeSlider(Qt::Horizontal);
    contrastSlider_->setRange(25, 400);
    contrastSlider_->setPageStep(25);
    contrastSlider_->setValue(100);
    displayColorLayout->addWidget(
        new ResponsiveSliderRow(new QLabel("Contrast"), contrastSlider_));
    brightnessSlider_ = new WheelSafeSlider(Qt::Horizontal);
    brightnessSlider_->setRange(-100, 100);
    brightnessSlider_->setPageStep(10);
    brightnessSlider_->setValue(0);
    displayColorLayout->addWidget(
        new ResponsiveSliderRow(new QLabel("Brightness"), brightnessSlider_));
    readabilityLayout->addLayout(displayColorLayout);

    assistantSection_ = new CollapsibleSection(QStringLiteral("Assistant"));
    assistantSection_->setPersistKey(QStringLiteral("assistant"));
    auto* assistantSectionLayout =
        qobject_cast<QVBoxLayout*>(assistantSection_->contentWidget()->layout());
    controlsLayout->addWidget(assistantSection_);
    auto* assistiveLayout = new QVBoxLayout();
    assistiveLayout->setSpacing(6);
    vlmAssistCheckbox_ = new QCheckBox("Scene Explain");
    assistiveOverlayCheckbox_ = new QCheckBox("Assistive Overlay");
    assistiveOverlayCheckbox_->setChecked(true);
    annotationCaptureOnExitCheckbox_ =
        new QCheckBox(QStringLiteral("Save drawing to notes when leaving Draw"));
    annotationCaptureOnExitCheckbox_->setChecked(true);
    annotationCaptureOnExitCheckbox_->setAccessibleName(
        QStringLiteral("Save drawing to notes when leaving Draw"));
    annotationCaptureOnExitCheckbox_->setAccessibleDescription(
        QStringLiteral("Save an annotated snapshot to lecture notes when Draw mode closes."));
    aiSettingsButton_ = new QPushButton(QStringLiteral("AI Settings"));
    aiSettingsButton_->setObjectName(QStringLiteral("advancedNavButton"));
    aiSettingsButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/open-settings.svg")));
    aiSettingsButton_->setIconSize(QSize(26, 26));
    aiSettingsButton_->setToolTip(QStringLiteral("Open AI Settings dialog"));
    aiSettingsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    openNotesButton_ = new QPushButton("Open Notes");
    openUserDataFolderButton_ =
        new QPushButton(QStringLiteral("Open my OpenZoom folder"));
    changeUserDataFolderButton_ =
        new QPushButton(QStringLiteral("Change OpenZoom folder..."));
    setupAssistantButton_ = new QPushButton(QStringLiteral("Setup && Downloads..."));
    setupAssistantButton_->setToolTip(
        QStringLiteral("Set up Codex CLI and NVIDIA Video Effects"));
    assistiveLayout->addWidget(vlmAssistCheckbox_);
    assistiveLayout->addWidget(assistiveOverlayCheckbox_);
    assistiveLayout->addWidget(annotationCaptureOnExitCheckbox_);
    assistiveLayout->addWidget(openNotesButton_);
    assistiveLayout->addWidget(openUserDataFolderButton_);
    assistiveLayout->addWidget(changeUserDataFolderButton_);
    assistiveLayout->addWidget(setupAssistantButton_);
    assistantSectionLayout->addLayout(assistiveLayout);

    sharpeningSection_ = new CollapsibleSection(QStringLiteral("Sharpening"));
    sharpeningSection_->setPersistKey(QStringLiteral("sharpening"));
    auto* sharpeningSectionLayout =
        qobject_cast<QVBoxLayout*>(sharpeningSection_->contentWidget()->layout());
    controlsLayout->addWidget(sharpeningSection_);
    auto* spatialLayout = new QVBoxLayout();
    spatialLayout->setSpacing(6);
    spatialSharpenCheckbox_ = new QCheckBox("Spatial Sharpen");
    spatialSharpenCheckbox_->setChecked(false);
    spatialBackendCombo_ = new WheelSafeComboBox();
    spatialBackendCombo_->addItem("AMD FSR 1.0 (EASU + RCAS)");
    spatialBackendCombo_->addItem("NVIDIA Image Scaling (default)");
    spatialBackendCombo_->setEnabled(false);
    spatialSharpnessSlider_ = new WheelSafeSlider(Qt::Horizontal);
    spatialSharpnessSlider_->setRange(0, 100);
    spatialSharpnessSlider_->setPageStep(5);
    spatialSharpnessSlider_->setValue(25);
    spatialSharpnessSlider_->setEnabled(false);
    spatialSharpnessValueLabel_ = new QLabel("0.25");
    spatialSharpnessValueLabel_->setMinimumWidth(40);

    spatialLayout->addWidget(spatialSharpenCheckbox_);
    spatialLayout->addWidget(new QLabel("Backend"));
    spatialLayout->addWidget(spatialBackendCombo_);
    auto* sharpnessLayout = new QHBoxLayout();
    sharpnessLayout->addWidget(new QLabel("Sharpness"));
    sharpnessLayout->addWidget(spatialSharpnessSlider_, 1);
    sharpnessLayout->addWidget(spatialSharpnessValueLabel_);
    spatialLayout->addLayout(sharpnessLayout);
    sharpeningSectionLayout->addLayout(spatialLayout);

    auto* focusLayout = new QVBoxLayout();
    focusLayout->setSpacing(8);
    auto* focusXLabel = new QLabel("Focus X:");
    zoomCenterXSlider_ = new WheelSafeSlider(Qt::Horizontal);
    zoomCenterXSlider_->setRange(0, kZoomFocusSliderScale);
    zoomCenterXSlider_->setPageStep(5);
    zoomCenterXSlider_->setValue(kZoomFocusSliderScale / 2);
    auto* focusXLayout = new QHBoxLayout();
    focusXLayout->addWidget(focusXLabel);
    focusXLayout->addWidget(zoomCenterXSlider_, 1);
    focusLayout->addLayout(focusXLayout);

    auto* focusYLabel = new QLabel("Focus Y:");
    zoomCenterYSlider_ = new WheelSafeSlider(Qt::Horizontal);
    zoomCenterYSlider_->setRange(0, kZoomFocusSliderScale);
    zoomCenterYSlider_->setPageStep(5);
    zoomCenterYSlider_->setValue(kZoomFocusSliderScale / 2);
    auto* focusYLayout = new QHBoxLayout();
    focusYLayout->addWidget(focusYLabel);
    focusYLayout->addWidget(zoomCenterYSlider_, 1);
    focusLayout->addLayout(focusYLayout);
    magnificationLayout->addLayout(focusLayout);

    diagnosticsSection_ = new CollapsibleSection(QStringLiteral("Diagnostics"));
    diagnosticsSection_->setPersistKey(QStringLiteral("diagnostics"));
    diagnosticsSection_->setExpanded(false);
    auto* diagnosticsLayout =
        qobject_cast<QVBoxLayout*>(diagnosticsSection_->contentWidget()->layout());
    controlsLayout->addWidget(diagnosticsSection_);
    auto* debugLayout = new QHBoxLayout();
    debugLayout->setSpacing(8);
    debugButton_ = new QPushButton("Debug View");
    debugButton_->setCheckable(true);
    debugButton_->setChecked(false);
    debugLayout->addWidget(debugButton_);
    focusMarkerCheckbox_ = new QCheckBox("Show Focus Point");
    focusMarkerCheckbox_->setChecked(false);
    focusMarkerCheckbox_->setToolTip("Overlay a red marker at the current zoom focus");
    debugLayout->addWidget(focusMarkerCheckbox_);
    debugLayout->addStretch(1);
    diagnosticsLayout->addLayout(debugLayout);
    auto* processingStatusLayout = new QVBoxLayout();
    processingStatusLayout->setSpacing(3);
    processingStatusLayout->addWidget(new QLabel(QStringLiteral("Pipeline status")));
    processingStatusLayout->addWidget(processingStatusLabel_);
    processingStatusLayout->addWidget(
        new QLabel(QStringLiteral("Performance percentiles")));
    processingStatusLayout->addWidget(performanceDiagnosticsLabel_);
    diagnosticsLayout->addLayout(processingStatusLayout);
#if OPENZOOM_ENABLE_TEXT_SR
    maxineAttribution_ = new QLabel(QStringLiteral("SuperRes powered by NVIDIA Maxine\u2122"));
    maxineAttribution_->setWordWrap(true);
    maxineAttribution_->setObjectName(QStringLiteral("vendorAttribution"));
    maxineAttribution_->setAccessibleName(QStringLiteral("NVIDIA Maxine attribution"));
    diagnosticsLayout->addWidget(maxineAttribution_);
#endif

    const std::array<CollapsibleSection*, 11> advancedSections{
        applicationSection_, deviceSection_, recordingSection_, magnificationSection_, readabilitySection_,
        stabilitySection_, screenFixSection_, textClaritySection_,
        assistantSection_, sharpeningSection_, diagnosticsSection_};
    for (CollapsibleSection* section : advancedSections) {
        connect(section, &CollapsibleSection::expandedChanged,
                this, &MainWindow::sectionStatesChanged);
    }
    connect(deviceMoreSection_, &CollapsibleSection::expandedChanged,
            this, &MainWindow::sectionStatesChanged);
    connect(settingsSearchEdit_, &QLineEdit::textChanged,
            this, &MainWindow::FilterAdvancedSettings);
    connect(settingsSearchEdit_, &QLineEdit::returnPressed, this, [this]() {
        if (firstSettingsSearchMatch_) {
            firstSettingsSearchMatch_->setFocus(Qt::ShortcutFocusReason);
            if (advancedScroll_) {
                advancedScroll_->ensureWidgetVisible(firstSettingsSearchMatch_, 8, 8);
            }
        }
    });
    auto* searchShortcut =
        new QShortcut(QKeySequence::Find, this);
    connect(searchShortcut, &QShortcut::activated, this, [this]() {
        if (settingsSearchEdit_) {
            settingsSearchEdit_->setFocus(Qt::ShortcutFocusReason);
            settingsSearchEdit_->selectAll();
        }
    });

    // The old all-or-nothing Advanced Tuning gate is retained as a hidden
    // compatibility accessor while individual sections own disclosure.
    controlsToggleButton_->setChecked(true);
    controlsToggleButton_->hide();
    controlsContainer_->show();
    advancedLayout->addWidget(controlsContainer_);
    // QScrollArea expands short content to the viewport. Keep all inspector
    // rows packed at the top when Advanced Tuning is collapsed.
    advancedLayout->addStretch(1);

    advancedScroll_ = new QScrollArea();
    advancedScroll_->setWidget(advancedPage);
    advancedScroll_->setWidgetResizable(true);
    advancedScroll_->setFrameShape(QFrame::NoFrame);
    advancedScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    advancedScroll_->setMinimumWidth(0);
    advancedScroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* imageTabPage = new QWidget();
    auto* imageTabLayout = new QVBoxLayout(imageTabPage);
    imageTabLayout->setContentsMargins(8, 8, 8, 0);
    imageTabLayout->setSpacing(8);
    imageTabLayout->addWidget(aiSettingsButton_);
    imageTabLayout->addWidget(advancedScroll_, 1);

    auto* assistantPage = new QWidget();
    auto* assistantLayout = new QVBoxLayout(assistantPage);
    assistantLayout->setContentsMargins(10, 8, 10, 10);
    assistantLayout->setSpacing(8);

    auto* assistantAiSettingsButton = new QPushButton(QStringLiteral("AI Settings"));
    assistantAiSettingsButton->setObjectName(QStringLiteral("advancedNavButton"));
    assistantAiSettingsButton->setIcon(QIcon(QStringLiteral(":/openzoom/icons/open-settings.svg")));
    assistantAiSettingsButton->setIconSize(QSize(26, 26));
    assistantAiSettingsButton->setToolTip(QStringLiteral("Open AI Settings dialog"));
    assistantAiSettingsButton->setAccessibleName(QStringLiteral("AI Settings"));
    assistantAiSettingsButton->setAccessibleDescription(
        QStringLiteral("Configure the vision assistant and speech output"));
    assistantAiSettingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(assistantAiSettingsButton, &QPushButton::clicked,
            aiSettingsButton_, &QPushButton::click);
    assistantLayout->addWidget(assistantAiSettingsButton);

    assistantConnectionLabel_ = new QLabel("Starting Codex...");
    assistantConnectionLabel_->setWordWrap(true);
    assistantUsageLabel_ = new QLabel();
    assistantUsageLabel_->setWordWrap(true);
    assistantConnectButton_ = new QPushButton("Connect ChatGPT");
    assistantConnectButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    assistantLayout->addWidget(assistantConnectionLabel_);
    assistantLayout->addWidget(assistantUsageLabel_);
    assistantLayout->addWidget(assistantConnectButton_);

    auto* assistantTabs = new QTabWidget();
    auto* chatPage = new QWidget();
    auto* chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(0, 6, 0, 0);
    chatLayout->setSpacing(8);
    assistantTranscript_ = new QTextBrowser();
    assistantTranscript_->setOpenExternalLinks(false);
    assistantTranscript_->setPlaceholderText("Ask about the current camera view.");
    assistantTranscript_->setMinimumHeight(220);
    assistantPromptEdit_ = new QPlainTextEdit();
    assistantPromptEdit_->setPlaceholderText("Ask about what is visible...");
    assistantPromptEdit_->setMaximumHeight(105);
    assistantAttachFrameCheckbox_ = new QCheckBox("Attach current view");
    assistantAttachFrameCheckbox_->setChecked(true);
    assistantNewButton_ = new QPushButton("New Conversation");
    assistantSendButton_ = new QPushButton("Send");
    assistantStopButton_ = new QPushButton("Stop");
    assistantStopButton_->setEnabled(false);
    auto* chatActions = new QHBoxLayout();
    chatActions->setSpacing(6);
    chatActions->addWidget(assistantNewButton_);
    chatActions->addStretch(1);
    chatActions->addWidget(assistantStopButton_);
    chatActions->addWidget(assistantSendButton_);
    chatLayout->addWidget(assistantTranscript_, 1);
    chatLayout->addWidget(assistantPromptEdit_);
    chatLayout->addWidget(assistantAttachFrameCheckbox_);
    chatLayout->addLayout(chatActions);

    auto* historyPage = new QWidget();
    auto* historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 6, 0, 0);
    historyLayout->setSpacing(8);
    assistantHistoryList_ = new QListWidget();
    assistantHistoryList_->setSelectionMode(QAbstractItemView::SingleSelection);
    assistantHistoryList_->setWordWrap(true);
    assistantRenameButton_ = new QPushButton("Rename");
    assistantExportButton_ = new QPushButton("Export");
    assistantDeleteButton_ = new QPushButton("Delete");
    auto* historyActions = new QHBoxLayout();
    historyActions->setSpacing(6);
    historyActions->addWidget(assistantRenameButton_);
    historyActions->addWidget(assistantExportButton_);
    historyActions->addWidget(assistantDeleteButton_);
    historyLayout->addWidget(assistantHistoryList_, 1);
    historyLayout->addLayout(historyActions);

    assistantTabs->addTab(chatPage, "Chat");
    assistantTabs->addTab(historyPage, "History");
    assistantLayout->addWidget(assistantTabs, 1);

    // Read-only live transcript view (plan 36): no send button and no
    // implied Assistant conversation. Finalized text stays selectable.
    auto* transcriptPage = new QWidget();
    auto* transcriptLayout = new QVBoxLayout(transcriptPage);
    transcriptLayout->setContentsMargins(10, 10, 10, 10);
    transcriptLayout->setSpacing(6);
    auto* transcriptPartialCaption = new QLabel(QStringLiteral("Current phrase"));
    transcriptPartialCaption->setObjectName(QStringLiteral("scopeSubtitle"));
    transcriptPartialLabel_ = new QLabel();
    transcriptPartialLabel_->setWordWrap(true);
    transcriptPartialLabel_->setMinimumHeight(40);
    transcriptPartialCaption->setBuddy(transcriptPartialLabel_);
    auto* transcriptFinalsCaption = new QLabel(QStringLiteral("Finalized transcript"));
    transcriptFinalsCaption->setObjectName(QStringLiteral("scopeSubtitle"));
    transcriptFinalsView_ = new QPlainTextEdit();
    transcriptFinalsView_->setReadOnly(true);
    transcriptFinalsView_->setTabChangesFocus(true);
    transcriptFinalsCaption->setBuddy(transcriptFinalsView_);
    transcriptLayout->addWidget(transcriptPartialCaption);
    transcriptLayout->addWidget(transcriptPartialLabel_);
    transcriptLayout->addWidget(transcriptFinalsCaption);
    transcriptLayout->addWidget(transcriptFinalsView_, 1);

    auto* advancedTabs = new QTabWidget();
    advancedTabs->addTab(imageTabPage, "Image");
    advancedTabs->addTab(assistantPage, "Assistant");
    advancedTabs->addTab(transcriptPage, "Transcript");
    previousAdvancedTabButton_ = new QToolButton();
    previousAdvancedTabButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    previousAdvancedTabButton_->setIconSize(QSize(26, 26));
    previousAdvancedTabButton_->setToolTip(QStringLiteral("Previous Advanced section"));
    previousAdvancedTabButton_->setAccessibleName(QStringLiteral("Previous Advanced section"));
    nextAdvancedTabButton_ = new QToolButton();
    nextAdvancedTabButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    nextAdvancedTabButton_->setIconSize(QSize(26, 26));
    nextAdvancedTabButton_->setToolTip(QStringLiteral("Next Advanced section"));
    nextAdvancedTabButton_->setAccessibleName(QStringLiteral("Next Advanced section"));
    auto* leftTabCorner = new QWidget();
    auto* leftTabCornerLayout = new QHBoxLayout(leftTabCorner);
    leftTabCornerLayout->setContentsMargins(0, 0, 4, 0);
    leftTabCornerLayout->addWidget(previousAdvancedTabButton_);
    auto* rightTabCorner = new QWidget();
    auto* rightTabCornerLayout = new QHBoxLayout(rightTabCorner);
    rightTabCornerLayout->setContentsMargins(4, 0, 0, 0);
    helpButton_ = new QToolButton();
    helpButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    helpButton_->setIcon(style()->standardIcon(QStyle::SP_MessageBoxQuestion));
    helpButton_->setIconSize(QSize(26, 26));
    helpButton_->setToolTip(QStringLiteral("Open controls and features help"));
    helpButton_->setAccessibleName(QStringLiteral("Open help"));
    helpButton_->setAccessibleDescription(
        QStringLiteral("Show a guide to OpenZoom controls and features"));
    rightTabCornerLayout->addWidget(helpButton_);
    rightTabCornerLayout->addWidget(nextAdvancedTabButton_);
    advancedTabs->setCornerWidget(leftTabCorner, Qt::TopLeftCorner);
    advancedTabs->setCornerWidget(rightTabCorner, Qt::TopRightCorner);
    connect(previousAdvancedTabButton_, &QToolButton::clicked, this, [advancedTabs]() {
        const int count = advancedTabs->count();
        if (count > 0) advancedTabs->setCurrentIndex((advancedTabs->currentIndex() + count - 1) % count);
    });
    connect(nextAdvancedTabButton_, &QToolButton::clicked, this, [advancedTabs]() {
        const int count = advancedTabs->count();
        if (count > 0) advancedTabs->setCurrentIndex((advancedTabs->currentIndex() + 1) % count);
    });
    connect(helpButton_, &QToolButton::clicked, this, &MainWindow::ShowHelpDialog);
    UpdateDirectionalUi();
    advancedTabs->setMinimumWidth(kAdvancedPanelMinimumWidth);
    advancedTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    advancedPanel_ = advancedTabs;

    renderWidget_ = new RenderWidget();
    renderWidget_->installEventFilter(this);
    renderWidget_->setMouseTracking(true);
    renderWidget_->setMinimumSize(320, 240);
    contentSplitter_ = new QSplitter(Qt::Horizontal);
    contentSplitter_->setObjectName(QStringLiteral("advancedContentSplitter"));
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(10);
    contentSplitter_->addWidget(renderWidget_);
    contentSplitter_->addWidget(advancedPanel_);
    contentSplitter_->setStretchFactor(0, 1);
    contentSplitter_->setStretchFactor(1, 0);
    contentSplitter_->setSizes({width() - kAdvancedPanelDefaultWidth,
                                kAdvancedPanelDefaultWidth});
    if (QSplitterHandle* handle = contentSplitter_->handle(1)) {
        handle->setToolTip(QStringLiteral("Drag to resize Advanced settings"));
        handle->setAccessibleName(QStringLiteral("Resize Advanced settings"));
    }
    connect(contentSplitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        if (advancedPanel_ && advancedPanel_->isVisible() && advancedPanel_->width() > 0) {
            advancedPanelPreferredWidth_ = advancedPanel_->width();
        }
    });
    rootLayout->addWidget(contentSplitter_, 1);
    annotationOverlay_ = new AnnotationOverlay(renderWidget_, this);

    // D3D presents directly into a native window, so frameless owned tool
    // windows provide predictable stacking and opacity above the swap chain.
    const Qt::WindowFlags chromeFlags = Qt::Tool |
                                        Qt::FramelessWindowHint |
                                        Qt::NoDropShadowWindowHint;
    topLeftPanel_ = new QWidget(this, chromeFlags);
    topLeftPanel_->setObjectName("topLeftPanel");
    auto* topLeftLayout = new QHBoxLayout(topLeftPanel_);
    topLeftLayout->setContentsMargins(10, 8, 8, 8);
    topLeftLayout->setSpacing(8);
    auto* brandLabel = new QLabel("OpenZoom");
    brandLabel->setObjectName("brandLabel");
    topLeftLayout->addWidget(brandLabel);
    topLeftLayout->addWidget(simpleModeButton_);
    topLeftLayout->addWidget(advancedModeButton_);

    bottomLeftPanel_ = new QWidget(this, chromeFlags);
    bottomLeftPanel_->setObjectName("bottomLeftPanel");
    auto* bottomLeftLayout = new QHBoxLayout(bottomLeftPanel_);
    bottomLeftLayout->setContentsMargins(8, 8, 10, 10);
    bottomLeftLayout->setSpacing(6);
    modeGridButton_ = new QToolButton();
    modeGridButton_->setObjectName("modeGridButton");
    modeGridButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    modeGridButton_->setIconSize(QSize(30, 30));
    modeGridButton_->setToolTip("Show all quick modes");
    previousModeButton_ = new QPushButton();
    previousModeButton_->setObjectName("previousModeButton");
    previousModeButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    previousModeButton_->setIconSize(QSize(30, 30));
    previousModeButton_->setToolTip("Previous quick mode");
    currentModeButton_ = new QPushButton("Read a Page   [1]");
    currentModeButton_->setObjectName("currentModeButton");
    currentModeButton_->setToolTip("Show all quick modes");
    nextModeButton_ = new QPushButton();
    nextModeButton_->setObjectName("nextModeButton");
    nextModeButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    nextModeButton_->setIconSize(QSize(30, 30));
    nextModeButton_->setToolTip("Next quick mode");
    bottomLeftLayout->addWidget(modeGridButton_);
    bottomLeftLayout->addWidget(previousModeButton_);
    bottomLeftLayout->addWidget(currentModeButton_);
    bottomLeftLayout->addWidget(nextModeButton_);
    simpleTextClarityCheckbox_ = new QCheckBox("Text Clarity");
    simpleTextClarityCheckbox_->setToolTip("Automatically clarify text in the camera view");
    topLeftLayout->addWidget(simpleTextClarityCheckbox_);

    keystoneTrackingPanel_ = new QWidget(this, chromeFlags);
    keystoneTrackingPanel_->setObjectName("keystoneTrackingPanel");
    auto* simpleTrackingLayout = new QHBoxLayout(keystoneTrackingPanel_);
    simpleTrackingLayout->setContentsMargins(8, 8, 8, 10);
    simpleTrackingLayout->setSpacing(6);
    simpleKeystoneBackButton_ = new QPushButton();
    simpleKeystonePauseButton_ = new QPushButton();
    simpleKeystoneNextButton_ = new QPushButton();
    simpleKeystoneBackButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/step-back.svg")));
    simpleKeystonePauseButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/pause.svg")));
    simpleKeystoneNextButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/step-forward.svg")));
    for (QPushButton* button : {simpleKeystoneBackButton_, simpleKeystonePauseButton_,
                                simpleKeystoneNextButton_}) {
        button->setIconSize(QSize(30, 30));
        simpleTrackingLayout->addWidget(button);
    }
    keystoneTrackingPanel_->hide();

    bottomRightPanel_ = new QWidget(this, chromeFlags);
    bottomRightPanel_->setObjectName("bottomRightPanel");
    auto* bottomRightLayout = new QHBoxLayout(bottomRightPanel_);
    bottomRightLayout->setContentsMargins(10, 8, 8, 10);
    bottomRightLayout->setSpacing(6);
    capturePhotoButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/camera.svg")));
    recordButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/record.svg")));
    explainNowButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/explain.svg")));
    readTextButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/read.svg")));
    annotationButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/draw.svg")));
    annotationButton_->setIconSize(QSize(28, 28));
    for (QPushButton* button : {capturePhotoButton_, recordButton_, explainNowButton_, readTextButton_}) {
        button->setIconSize(QSize(28, 28));
    }
    annotationButton_->setMinimumSize(88, 58);
    connect(openUserDataFolderButton_, &QPushButton::clicked, this,
            &MainWindow::openUserDataFolderRequested);
    connect(changeUserDataFolderButton_, &QPushButton::clicked, this,
            &MainWindow::changeUserDataFolderRequested);
    auto* openFolderShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), this);
    connect(openFolderShortcut, &QShortcut::activated, this,
            &MainWindow::openUserDataFolderRequested);
    bottomRightLayout->addWidget(capturePhotoButton_);
    bottomRightLayout->addWidget(recordButton_);
    bottomRightLayout->addWidget(explainNowButton_);
    bottomRightLayout->addWidget(readTextButton_);
    bottomRightLayout->addWidget(annotationButton_);

    // Simple-mode live transcript overlay (plan 36): never activates, never
    // takes input or focus, and never announces per-delta — screen-reader
    // users get state changes through the live status path instead.
    simpleTranscriptPanel_ = new QWidget(
        this, chromeFlags | Qt::WindowTransparentForInput |
                  Qt::WindowDoesNotAcceptFocus);
    simpleTranscriptPanel_->setObjectName(QStringLiteral("simpleTranscriptPanel"));
    auto* simpleTranscriptLayout = new QVBoxLayout(simpleTranscriptPanel_);
    simpleTranscriptLayout->setContentsMargins(12, 8, 12, 8);
    simpleTranscriptLabel_ = new QLabel();
    simpleTranscriptLabel_->setObjectName(QStringLiteral("simpleTranscriptLabel"));
    simpleTranscriptLabel_->setWordWrap(true);
    simpleTranscriptLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    simpleTranscriptLayout->addWidget(simpleTranscriptLabel_);
    simpleTranscriptPanel_->hide();

    connect(annotationButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (changingAnnotationMode_) {
            return;
        }
        if (checked) {
            setAnnotationMode(true);
            return;
        }
        if (annotationOverlay_) {
            annotationOverlay_->CommitPendingText();
        }
        if (annotationOverlay_ && annotationOverlay_->HasInk() &&
            annotationOverlay_->CaptureOnExit()) {
            emit annotationSnapshotRequested(2);
        }
        if (annotationOverlay_) {
            annotationOverlay_->ClearInk();
        }
        setAnnotationMode(false);
    });
    connect(annotationOverlay_, &AnnotationOverlay::SnapshotRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk()) {
                    emit annotationSnapshotRequested(0);
                }
            });
    connect(annotationOverlay_, &AnnotationOverlay::ClearRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk()) {
                    emit annotationSnapshotRequested(1);
                    annotationOverlay_->ClearInk();
                }
            });
    connect(annotationOverlay_, &AnnotationOverlay::ExitRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk() &&
                    annotationOverlay_->CaptureOnExit()) {
                    emit annotationSnapshotRequested(2);
                }
                annotationOverlay_->ClearInk();
                setAnnotationMode(false);
            });
    connect(annotationOverlay_, &AnnotationOverlay::CleanPhotoRequested,
            capturePhotoButton_, &QPushButton::click);
    connect(annotationOverlay_, &AnnotationOverlay::PreferencesChanged,
            this, &MainWindow::annotationPreferencesChanged);
    connect(annotationCaptureOnExitCheckbox_, &QCheckBox::toggled,
            this, [this](bool checked) {
                if (!annotationOverlay_) {
                    return;
                }
                annotationOverlay_->SetPreferences(
                    annotationOverlay_->InkColor(),
                    annotationOverlay_->InkWidthPixels(),
                    checked,
                    annotationOverlay_->Dashed(),
                    annotationOverlay_->ShapeKind(),
                    annotationOverlay_->TextSizePixels());
                emit annotationPreferencesChanged(
                    annotationOverlay_->InkColor().name(QColor::HexRgb),
                    annotationOverlay_->InkWidthPixels(),
                    checked,
                    annotationOverlay_->Dashed(),
                    annotationOverlay_->ShapeKind(),
                    annotationOverlay_->TextSizePixels());
            });

    modeGridPopup_ = new QWidget(this, chromeFlags);
    modeGridPopup_->setObjectName("modeGridPopup");
    auto* modeGridLayout = new QVBoxLayout(modeGridPopup_);
    modeGridLayout->setContentsMargins(8, 8, 8, 8);
    modeGridLayout->addWidget(presetList_);
    modeGridPopup_->hide();

    modeToast_ = new QWidget(this, Qt::ToolTip |
                                   Qt::FramelessWindowHint |
                                   Qt::NoDropShadowWindowHint);
    modeToast_->setObjectName("modeToast");
    auto* modeToastLayout = new QVBoxLayout(modeToast_);
    modeToastLayout->setContentsMargins(28, 18, 28, 18);
    modeToastLayout->setSpacing(4);
    modeToastTitle_ = new QLabel("READ A PAGE");
    modeToastTitle_->setObjectName("modeToastTitle");
    modeToastTitle_->setAlignment(Qt::AlignCenter);
    modeToastSubtitle_ = new QLabel("Reading profile");
    modeToastSubtitle_->setObjectName("modeToastSubtitle");
    modeToastSubtitle_->setAlignment(Qt::AlignCenter);
    modeToastLayout->addWidget(modeToastTitle_);
    modeToastLayout->addWidget(modeToastSubtitle_);
    modeToast_->setAccessibleName("Quick mode changed");
    modeToast_->setAttribute(Qt::WA_ShowWithoutActivating);
    modeToast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    modeToast_->hide();
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        panel->setAttribute(Qt::WA_ShowWithoutActivating);
    }

    simpleChromeIdleTimer_ = new QTimer(this);
    simpleChromeIdleTimer_->setSingleShot(true);
    simpleChromeIdleTimer_->setInterval(kSimpleChromeIdleMs);
    connect(simpleChromeIdleTimer_, &QTimer::timeout, this, &MainWindow::FadeSimpleChrome);
    modeToastTimer_ = new QTimer(this);
    modeToastTimer_->setSingleShot(true);
    modeToastTimer_->setInterval(kModeToastMs);
    connect(modeToastTimer_, &QTimer::timeout, this, [this]() {
        modeToast_->hide();
    });

    connect(modeGridButton_, &QToolButton::clicked, this, &MainWindow::ToggleModeGrid);
    connect(currentModeButton_, &QPushButton::clicked, this, &MainWindow::ToggleModeGrid);
    connect(previousModeButton_, &QPushButton::clicked, this, [this]() { ActivateRelativePreset(-1); });
    connect(nextModeButton_, &QPushButton::clicked, this, [this]() { ActivateRelativePreset(1); });
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystoneStepBackRequested);
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystonePauseResumeRequested);
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystoneStepForwardRequested);
    }
    connect(presetList_, &QListWidget::currentItemChanged,
            this, &MainWindow::UpdateCurrentPresetUi);
    connect(presetList_, &QListWidget::itemClicked, this, [this]() {
        modeGridPopup_->hide();
        RevealSimpleChrome();
    });
    connect(presetList_->model(), &QAbstractItemModel::rowsInserted,
            this, [this](const QModelIndex&, int first, int last) {
                for (int row = first; row <= last; ++row) {
                    QListWidgetItem* item = presetList_->item(row);
                    if (!item) continue;
                    const QString original = item->text();
                    const QString plain = PlainPresetLabel(original);
                    item->setData(Qt::AccessibleTextRole, plain);
                    item->setData(Qt::StatusTipRole, original);
                    item->setText(row < kSimpleShortcutCount
                                      ? QStringLiteral("%1\n%2").arg(row + 1).arg(plain)
                                      : plain);
                    item->setTextAlignment(Qt::AlignCenter);
                }
            });
    connect(recordButton_, &QPushButton::toggled, this, [this](bool checked) {
        recordButton_->setIcon(style()->standardIcon(checked ? QStyle::SP_MediaStop
                                                             : QStyle::SP_MediaPlay));
    });

    qApp->installEventFilter(this);
    qApp->installNativeEventFilter(this);
    UpdateDirectionalUi();
    QTimer::singleShot(0, this, [this]() {
        UpdateSimpleChromeGeometry();
        RevealSimpleChrome();
    });

    connect(simpleModeButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            setSimpleMode(true);
        }
    });
    connect(advancedModeButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            setSimpleMode(false);
        }
    });
    connect(bumpHoldCheckbox_, &QCheckBox::toggled, this,
            [this](bool checked) {
                QAccessibleAnnouncementEvent announcement(
                    this, checked
                              ? QStringLiteral("Extra Stable on")
                              : QStringLiteral("Extra Stable off"));
                announcement.setPoliteness(
                    QAccessible::AnnouncementPoliteness::Polite);
                QAccessible::updateAccessibility(&announcement);
            });
    // Screen-reader metadata for every interactive control. Static accessible
    // names belong only on controls whose label never changes; dynamic text is
    // initialized and updated through SetLiveText so UIA receives its value.
    auto setA11y = [this](QWidget* widget, const QString& name,
                          const QString& description) {
        QString scopedDescription = description;
        if (!description.contains(QStringLiteral("setting shared by all profiles"),
                                  Qt::CaseInsensitive) &&
            !description.contains(QStringLiteral("setting saved with"),
                                  Qt::CaseInsensitive)) {
            if (deviceMoreSection_ && deviceMoreSection_->isAncestorOf(widget)) {
                scopedDescription =
                    QStringLiteral("Viewport setting shared by all profiles. ") +
                    description;
            } else if (deviceSection_ && deviceSection_->isAncestorOf(widget)) {
                scopedDescription =
                    QStringLiteral("Device setting shared by all profiles. ") +
                    description;
            } else if (controlsContainer_ &&
                       controlsContainer_->isAncestorOf(widget)) {
                scopedDescription =
                    QStringLiteral("Profile setting saved with the current quick option. ") +
                    description;
            }
        }
        widget->setAccessibleName(name);
        widget->setAccessibleDescription(scopedDescription);
    };
    setA11y(controlsToggleButton_, "Advanced Tuning",
            "Show or hide the advanced tuning controls");
    setA11y(promotePresetButton_, "Save As Quick Option",
            "Save the current advanced settings as a reusable quick mode");
    setA11y(capturePhotoButton_, "Capture Photo",
            "Save synchronized original and processed camera photos");
    setA11y(recordButton_, "Record Video",
            "Start or stop synchronized original and processed MP4 recordings");
    setA11y(annotationButton_, "Draw on camera view",
            "Open annotation mode to mark the lecture view with scene-anchored ink.");
    setA11y(joystickCheckbox_, "Virtual Joystick",
            "Viewport setting shared by all profiles. Show an on-screen joystick overlay for panning the zoom focus.");
    setA11y(rotationCombo_, "Rotation",
            "Device setting shared by all profiles. Rotate the camera image clockwise in 90 degree steps.");
    setA11y(viewportRateCombo_, "Viewport motion rate",
            "Viewport setting shared by all profiles. Choose how smoothly pan and zoom move without changing the camera frame rate.");
    setA11y(viewportFitCombo_, "Viewport framing",
            "Viewport setting shared by all profiles. Choose Fill to crop without stretching or Fit to show the entire camera image.");
    setA11y(recordingCanvasCombo_, "Processed recording resolution",
            "Global recording setting. Choose Match camera or a fixed "
            "resolution from 360p through Ultra HD for the processed MP4. "
            "The original MP4 uses the selected camera mode resolution.");
    setA11y(cameraCombo_, "Camera",
            "Device setting shared by all profiles. Select the active camera device.");
    setA11y(microphoneCombo_, "Recording microphone",
            "Global recording setting. Select microphone audio for both "
            "original and processed MP4 recordings, or choose no microphone.");
    setA11y(transcribeMicrophoneCheckbox_, "Transcribe microphone while recording",
            "Global recording setting. Sends microphone audio to Codex Voice "
            "for live transcription only while recording. Requires Codex "
            "signed in with ChatGPT. Recording works without it.");
    setA11y(transcriptToNotesCheckbox_, "Add finalized transcript to lecture notes",
            "Appends each finalized phrase to the HTML lecture notes. "
            "Effective only while lecture notes are enabled.");
    setA11y(transcriptionStatusLabel_, "Transcription status",
            "Reports the live transcription state. Recording is never "
            "affected by transcription problems.");
    setA11y(transcriptionQuotaLabel_, "Codex usage",
            "Shows the general Codex quota window. Voice-specific remaining "
            "time is not exposed by this Codex app-server.");
    setA11y(transcriptPartialLabel_, "Current phrase",
            "The phrase being transcribed right now. Partial text is "
            "presentation-only until it is finalized.");
    setA11y(transcriptFinalsView_, "Finalized transcript",
            "Read-only list of finalized transcript phrases from the "
            "current recording. Text is selectable.");
    setA11y(cameraFormatCombo_, "Resolution and frame rate",
            "Device setting shared by all profiles. Select the camera capture "
            "mode used by original photos and original video.");
    setA11y(cameraFormatNoticeLabel_, "Camera format notice",
            "Reports when the camera driver selected a different mode.");
    setA11y(zoomWheelAccelerationCheckbox_, "Zoom wheel acceleration",
            "Viewport setting shared by all profiles. Accelerate fast Control scroll gestures.");
    setA11y(presetList_, "Quick Modes",
            "Choose a task-oriented preset such as reading or high contrast");
    setA11y(bwCheckbox_, "Black and White",
            "Profile setting saved with the current quick option. Convert the image to thresholded black and white.");
    setA11y(bwSlider_, "Black and White Threshold",
            "Brightness threshold for the black and white conversion");
    setA11y(zoomCheckbox_, "Zoom",
            "Profile setting saved with the current quick option. Enable the magnifier.");
    setA11y(zoomSlider_, "Zoom Amount",
            "Magnification level");
    setA11y(blurCheckbox_, "Gaussian Blur",
            "Smooth the image with a Gaussian blur");
    setA11y(blurSigmaSlider_, "Blur Sigma",
            "Strength of the Gaussian blur");
    setA11y(blurRadiusSlider_, "Blur Radius",
            "Radius of the Gaussian blur in pixels");
    setA11y(temporalSmoothCheckbox_, "Temporal Smooth",
            "Reduce flicker by averaging consecutive frames");
    setA11y(temporalSmoothSlider_, "Temporal Smooth Blend",
            "How strongly new frames blend into the running average");
    setA11y(vlmAssistCheckbox_, "Scene Explain",
            "Describe the magnified scene using an AI vision model");
    setA11y(assistiveOverlayCheckbox_, "Assistive Overlay",
            "Show assistant results in an on-screen panel");
    setA11y(spatialSharpenCheckbox_, "Spatial Sharpen",
            "Sharpen and upscale the image on the GPU");
    setA11y(spatialBackendCombo_, "Sharpen Backend",
            "Choose the GPU sharpening algorithm");
    setA11y(spatialSharpnessSlider_, "Sharpness",
            "Strength of the spatial sharpening");
    setA11y(zoomCenterXSlider_, "Focus X",
            "Horizontal position of the zoom focus");
    setA11y(zoomCenterYSlider_, "Focus Y",
            "Vertical position of the zoom focus");
    setA11y(debugButton_, "Debug View",
            "Show the intermediate processing stages in a grid");
    setA11y(focusMarkerCheckbox_, "Show Focus Point",
            "Overlay a marker at the current zoom focus");
    setA11y(processingStatusLabel_, "Processing Status",
            "Whether frames are processed on the CPU or the GPU");
    setA11y(simpleModeButton_, "Simple Mode",
            "Show the simple view with quick modes and large controls");
    setA11y(advancedModeButton_, "Advanced Mode",
            "Show the advanced view with every tuning control");
    setA11y(explainNowButton_, "Explain Now",
            "Describe the current camera view once using the AI vision model");
    setA11y(readTextButton_, "Read Text",
            "Read the current camera text with the vision assistant");
    setA11y(stabilizationCheckbox_, "Stabilize Image",
            "Lock the mounted camera view with full-strength CUDA stabilization");
    setA11y(bumpHoldCheckbox_, "Extra Stable",
            "During a mounted-camera bump, temporarily show the last sharp stabilized frame and crossfade back when tracking recovers; moving people or content can be hidden while the frame is held");
    setA11y(keystoneCheckbox_, "Straighten Screen (Keystone)",
            "Automatically straighten a projected screen viewed at an angle");
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        setA11y(button, "Previous Screen Correction",
                "Freeze automatic screen correction and return to the previous accepted correction");
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        setA11y(button, "Stop Automatic Screen Correction",
                "Freeze the current screen correction");
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        setA11y(button, "Next Screen Correction",
                "Use the next saved correction or find one new correction while stopped");
    }
    setA11y(autoContrastCheckbox_, "Auto Contrast",
            "Automatically stretch washed-out colors for better readability");
    setA11y(autoContrastStrengthSlider_, "Auto Contrast Strength",
            "How strongly the automatic contrast correction is applied");
    setA11y(simpleTextClarityCheckbox_, "Text Clarity",
            "Automatically select the text clarity processing stack");
    setA11y(textClarityCheckbox_, "Auto Text Clarity",
            "Automatically clarify text using local document analysis");
    setA11y(backgroundFlattenCheckbox_, "Flatten Background",
            "Remove shadows and uneven page lighting");
    setA11y(adaptiveBinarizationCheckbox_, "Adaptive Text",
            "Separate text from its local background using a Sauvola threshold");
    setA11y(textPolarityCombo_, "Text Polarity",
            "Choose automatic, dark on light, or light on dark text");
    setA11y(strokeWeightSlider_, "Stroke Weight",
            "Make text strokes thinner or bolder");
    setA11y(smartSharpenCheckbox_, "Smart Sharpen",
            "Denoise and sharpen text without bright edge halos");
    setA11y(claheCheckbox_, "Local Contrast",
            "Equalize contrast in separate image regions");
    setA11y(twoColorTextCheckbox_, "Two Color Reading",
            "Map detected ink and paper to the selected display colors");
    setA11y(textHysteresisCheckbox_, "Steady Text Edges",
            "Keep thresholded letter edges from flickering between frames");
    setA11y(selectiveSharpenCheckbox_, "Sharpen Text Only",
            "Apply sharpening near detected text strokes and preserve pictures");
    setA11y(focusDetectionCheckbox_, "Warn When Out of Focus",
            "Warn before reading when the camera image is too blurry");
    setA11y(glareSuppressionCheckbox_, "Suppress Glare",
            "Reduce small blown highlights on glossy pages and boards");
    setA11y(mlTextSuperResolutionCheckbox_, "ML Text Super Resolution",
            "Use NVIDIA Video Effects SuperRes when zoom is one point three three times or greater");
    setA11y(mlTextSuperResolutionStrengthSlider_, "Super Resolution Strength",
            "Set NVIDIA SuperRes enhancement strength for this preset");
    setA11y(mlTextSuperResolutionUltra1440pCheckbox_, "Ultra 1440p Super Resolution",
            "Use the full camera frame to build a separate high-resolution scene up to 1440p");
    setA11y(mlTextSuperResolutionPrefer2xCheckbox_, "Faster 2x Mode",
            "Optional speed mode with a narrower view. Leave off for maximum source detail");
    setA11y(displayColorPicker_, "Display Colors",
            "Choose a high contrast color scheme such as white on black");
    setA11y(contrastSlider_, "Contrast",
            "Contrast of the displayed image");
    setA11y(brightnessSlider_, "Brightness",
            "Brightness of the displayed image");
    setA11y(aiSettingsButton_, "AI Settings",
            "Configure the vision assistant and speech output");
    setA11y(openNotesButton_, "Open Notes",
            "Open the lecture notes file written by the assistive features");
    setA11y(openUserDataFolderButton_, "Open my OpenZoom folder",
            "Open the folder containing OpenZoom photos, recordings, notes, and analysis files");
    setA11y(changeUserDataFolderButton_, "Change OpenZoom folder",
            "Choose where OpenZoom saves new photos, recordings, notes, and analysis files");
    setA11y(setupAssistantButton_, "Setup and Downloads",
            "Set up Codex CLI and NVIDIA Video Effects");
    setA11y(assistantConnectionLabel_, "Codex Connection Status",
            "Current Codex app-server and ChatGPT account status");
    setA11y(assistantUsageLabel_, "Codex Usage",
            "Percentage remaining in the current Codex subscription usage window");
    setA11y(assistantConnectButton_, "Connect ChatGPT",
            "Sign in to Codex with a ChatGPT subscription");
    setA11y(assistantTranscript_, "Assistant Conversation",
            "Messages in the current OpenZoom assistant conversation");
    setA11y(assistantPromptEdit_, "Assistant Question",
            "Question for the OpenZoom vision assistant");
    setA11y(assistantAttachFrameCheckbox_, "Attach Current View",
            "Include the current processed camera frame with the question");
    setA11y(assistantSendButton_, "Send Question",
            "Send the question to the OpenZoom vision assistant");
    setA11y(assistantStopButton_, "Stop Assistant",
            "Stop the current assistant response");
    setA11y(assistantNewButton_, "New Conversation",
            "Start a new persistent OpenZoom assistant conversation");
    setA11y(assistantHistoryList_, "Assistant History",
            "OpenZoom assistant conversations saved by Codex");
    setA11y(assistantRenameButton_, "Rename Conversation",
            "Rename the selected assistant conversation");
    setA11y(assistantExportButton_, "Export Conversation",
            "Export the current assistant transcript to a text file");
    setA11y(assistantDeleteButton_, "Delete Conversation",
            "Permanently delete the selected assistant conversation");
    setA11y(modeGridButton_, "Show Quick Modes",
            "Open the grid of premade quick modes");
    setA11y(previousModeButton_, "Previous Quick Mode",
            "Apply the previous premade image setting");
    setA11y(currentModeButton_, "Current Quick Mode",
            "Show all premade quick modes");
    setA11y(nextModeButton_, "Next Quick Mode",
            "Apply the next premade image setting");
    SetLiveText(recordButton_, recordButton_->text(),
                LivePoliteness::kSilent, QStringLiteral("Record"));
    SetLiveText(cameraFormatNoticeLabel_, cameraFormatNoticeLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Camera format notice"));
    SetLiveText(processingStatusLabel_, processingStatusLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Pipeline status"));
    SetLiveText(assistantConnectionLabel_, assistantConnectionLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Codex connection status"));
    SetLiveText(assistantUsageLabel_, assistantUsageLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Codex usage"));
    SetLiveText(assistantConnectButton_, assistantConnectButton_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("ChatGPT connection"));

    // Tab order is local to each frameless overlay window. The application
    // event filter below bridges those groups while Simple mode is active.
    QWidget::setTabOrder(simpleModeButton_, advancedModeButton_);
    QWidget::setTabOrder(advancedModeButton_, simpleTextClarityCheckbox_);
    QWidget::setTabOrder(modeGridButton_, previousModeButton_);
    QWidget::setTabOrder(previousModeButton_, currentModeButton_);
    QWidget::setTabOrder(currentModeButton_, nextModeButton_);
    QWidget::setTabOrder(simpleKeystoneBackButton_, simpleKeystonePauseButton_);
    QWidget::setTabOrder(simpleKeystonePauseButton_, simpleKeystoneNextButton_);
    QWidget::setTabOrder(advancedKeystoneBackButton_, advancedKeystonePauseButton_);
    QWidget::setTabOrder(advancedKeystonePauseButton_, advancedKeystoneNextButton_);
    QWidget::setTabOrder(capturePhotoButton_, recordButton_);
    QWidget::setTabOrder(recordButton_, explainNowButton_);
    QWidget::setTabOrder(explainNowButton_, readTextButton_);
    QWidget::setTabOrder(readTextButton_, annotationButton_);
    QWidget::setTabOrder(cameraCombo_, rotationCombo_);
    QWidget::setTabOrder(rotationCombo_, recordingCanvasCombo_);
    QWidget::setTabOrder(recordingCanvasCombo_, cameraFormatCombo_);
    QWidget::setTabOrder(cameraFormatCombo_, microphoneCombo_);
    QWidget::setTabOrder(microphoneCombo_, transcribeMicrophoneCheckbox_);
    QWidget::setTabOrder(transcribeMicrophoneCheckbox_, transcriptToNotesCheckbox_);

    setCentralWidget(central);
    setSimpleMode(true);
}

MainWindow::~MainWindow()
{
    if (qApp) {
        qApp->removeNativeEventFilter(this);
        qApp->removeEventFilter(this);
    }
}

QMap<QString, bool> MainWindow::sectionStates() const
{
    QMap<QString, bool> states;
    const std::array<CollapsibleSection*, 12> sections{
        applicationSection_, deviceSection_, deviceMoreSection_, recordingSection_, magnificationSection_,
        readabilitySection_, stabilitySection_, screenFixSection_,
        textClaritySection_, assistantSection_, sharpeningSection_,
        diagnosticsSection_};
    for (const CollapsibleSection* section : sections) {
        if (section && !section->persistKey().isEmpty()) {
            states.insert(section->persistKey(), section->isExpanded());
        }
    }
    return states;
}

void MainWindow::setSectionStates(const QMap<QString, bool>& states)
{
    const std::array<CollapsibleSection*, 12> sections{
        applicationSection_, deviceSection_, deviceMoreSection_, recordingSection_, magnificationSection_,
        readabilitySection_, stabilitySection_, screenFixSection_,
        textClaritySection_, assistantSection_, sharpeningSection_,
        diagnosticsSection_};
    for (CollapsibleSection* section : sections) {
        if (section && states.contains(section->persistKey())) {
            const QSignalBlocker blocker(section);
            section->setExpanded(states.value(section->persistKey()));
        }
    }
}

void MainWindow::updateSectionChangedCounts(
    const settings::AdvancedConfig& current,
    const settings::AdvancedConfig& defaults)
{
    auto different = [](auto lhs, auto rhs) {
        if constexpr (std::is_floating_point_v<decltype(lhs)>) {
            return std::abs(lhs - rhs) > 0.0001f;
        }
        return lhs != rhs;
    };

    const int magnificationChanges =
        different(current.zoomEnabled, defaults.zoomEnabled) +
        different(current.zoomAmount, defaults.zoomAmount) +
        different(current.zoomCenterX, defaults.zoomCenterX) +
        different(current.zoomCenterY, defaults.zoomCenterY) +
        different(current.focusMarker, defaults.focusMarker);
    magnificationSection_->setChangedCount(magnificationChanges);

    const int readabilityChanges =
        different(current.blackWhiteEnabled, defaults.blackWhiteEnabled) +
        different(current.blackWhiteThreshold, defaults.blackWhiteThreshold) +
        different(current.blurEnabled, defaults.blurEnabled) +
        different(current.blurSigma, defaults.blurSigma) +
        different(current.blurRadius, defaults.blurRadius) +
        different(current.autoContrastEnabled, defaults.autoContrastEnabled) +
        different(current.autoContrastStrength, defaults.autoContrastStrength) +
        different(current.displayColorMode, defaults.displayColorMode) +
        different(current.contrast, defaults.contrast) +
        different(current.brightness, defaults.brightness);
    readabilitySection_->setChangedCount(readabilityChanges);

    const int stabilityChanges =
        different(current.stabilizationEnabled, defaults.stabilizationEnabled) +
        different(current.temporalSmoothEnabled, defaults.temporalSmoothEnabled) +
        different(current.temporalSmoothAlpha, defaults.temporalSmoothAlpha);
    stabilitySection_->setChangedCount(stabilityChanges);

    screenFixSection_->setChangedCount(
        different(current.keystoneEnabled, defaults.keystoneEnabled));

    const int textChanges =
        different(current.autoTextClarityEnabled, defaults.autoTextClarityEnabled) +
        different(current.backgroundFlattenEnabled, defaults.backgroundFlattenEnabled) +
        different(current.backgroundFlattenStrength, defaults.backgroundFlattenStrength) +
        different(current.adaptiveBinarizationEnabled, defaults.adaptiveBinarizationEnabled) +
        different(current.sauvolaStrength, defaults.sauvolaStrength) +
        different(current.binarizationSoftness, defaults.binarizationSoftness) +
        different(current.textPolarityMode, defaults.textPolarityMode) +
        different(current.strokeWeight, defaults.strokeWeight) +
        different(current.smartSharpenEnabled, defaults.smartSharpenEnabled) +
        different(current.smartSharpenStrength, defaults.smartSharpenStrength) +
        different(current.claheEnabled, defaults.claheEnabled) +
        different(current.claheClipLimit, defaults.claheClipLimit) +
        different(current.twoColorTextEnabled, defaults.twoColorTextEnabled) +
        different(current.textHysteresisEnabled, defaults.textHysteresisEnabled) +
        different(current.textHysteresisStrength, defaults.textHysteresisStrength) +
        different(current.selectiveSharpenEnabled, defaults.selectiveSharpenEnabled) +
        different(current.focusDetectionEnabled, defaults.focusDetectionEnabled) +
        different(current.focusThreshold, defaults.focusThreshold) +
        different(current.glareSuppressionEnabled, defaults.glareSuppressionEnabled) +
        different(current.glareSuppressionStrength,
                  defaults.glareSuppressionStrength);
    textClaritySection_->setChangedCount(textChanges);

    const int assistantChanges =
        different(current.vlmAssistEnabled, defaults.vlmAssistEnabled) +
        different(current.assistiveOverlayEnabled,
                  defaults.assistiveOverlayEnabled);
    assistantSection_->setChangedCount(assistantChanges);

    const int sharpeningChanges =
        different(current.spatialSharpenEnabled, defaults.spatialSharpenEnabled) +
        different(current.spatialUpscaler, defaults.spatialUpscaler) +
        different(current.spatialSharpness, defaults.spatialSharpness) +
        different(current.mlSuperResEnabled, defaults.mlSuperResEnabled) +
        different(current.mlSuperResStrength, defaults.mlSuperResStrength) +
        different(current.mlSuperResPrefer2x, defaults.mlSuperResPrefer2x) +
        different(current.mlSuperResUltra1440p, defaults.mlSuperResUltra1440p);
    sharpeningSection_->setChangedCount(sharpeningChanges);

    const int diagnosticChanges =
        different(current.debugView, defaults.debugView);
    diagnosticsSection_->setChangedCount(diagnosticChanges);

    int deviceMoreChanges = 0;
    if (viewportRateCombo_ && viewportRateCombo_->currentIndex() != 0) {
        ++deviceMoreChanges;
    }
    if (viewportFitCombo_ && viewportFitCombo_->currentIndex() != 0) {
        ++deviceMoreChanges;
    }
    if (joystickCheckbox_ && joystickCheckbox_->isChecked()) {
        ++deviceMoreChanges;
    }
    if (zoomWheelAccelerationCheckbox_ &&
        !zoomWheelAccelerationCheckbox_->isChecked()) {
        ++deviceMoreChanges;
    }
    deviceMoreSection_->setChangedCount(deviceMoreChanges);

    for (CollapsibleSection* section :
         {deviceMoreSection_, recordingSection_, magnificationSection_, readabilitySection_,
          stabilitySection_, screenFixSection_, textClaritySection_,
          assistantSection_, sharpeningSection_, diagnosticsSection_}) {
        if (section && section->changedCount() > 0) {
            section->setExpanded(true);
        }
    }
}

void MainWindow::FilterAdvancedSettings(const QString& query)
{
    const QString needle = query.trimmed().toCaseFolded();
    const bool searching = !needle.isEmpty();
    firstSettingsSearchMatch_ = nullptr;
    int matchCount = 0;

    const std::array<CollapsibleSection*, 11> sections{
        applicationSection_, deviceSection_, recordingSection_, magnificationSection_, readabilitySection_,
        stabilitySection_, screenFixSection_, textClaritySection_,
        assistantSection_, sharpeningSection_, diagnosticsSection_};
    for (CollapsibleSection* section : sections) {
        if (!section) {
            continue;
        }
        bool sectionMatches = !searching;
        const auto descendants = section->contentWidget()->findChildren<QWidget*>();
        for (QWidget* widget : descendants) {
            QString text = widget->accessibleName() + QLatin1Char(' ') +
                           widget->toolTip();
            if (auto* label = qobject_cast<QLabel*>(widget)) {
                text += QLatin1Char(' ') + label->text();
            } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                text += QLatin1Char(' ') + button->text();
            } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
                text += QLatin1Char(' ') + combo->currentText();
            }
            if (searching && text.toCaseFolded().contains(needle)) {
                sectionMatches = true;
                ++matchCount;
                if (!firstSettingsSearchMatch_ && widget->focusPolicy() != Qt::NoFocus) {
                    firstSettingsSearchMatch_ = widget;
                }
            }
        }
        section->setVisible(sectionMatches);
        section->setSearchExpanded(searching && sectionMatches);
        if (section == deviceSection_ && deviceMoreSection_) {
            bool moreMatches = false;
            const auto moreWidgets =
                deviceMoreSection_->contentWidget()->findChildren<QWidget*>();
            for (QWidget* widget : moreWidgets) {
                QString text = widget->accessibleName() + QLatin1Char(' ') +
                               widget->toolTip();
                if (auto* label = qobject_cast<QLabel*>(widget)) {
                    text += QLatin1Char(' ') + label->text();
                } else if (auto* button =
                               qobject_cast<QAbstractButton*>(widget)) {
                    text += QLatin1Char(' ') + button->text();
                } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
                    text += QLatin1Char(' ') + combo->currentText();
                }
                if (searching && text.toCaseFolded().contains(needle)) {
                    moreMatches = true;
                    break;
                }
            }
            deviceMoreSection_->setSearchExpanded(searching && moreMatches);
        }
    }

    if (settingsSearchEdit_) {
        const QString resultText =
            searching
                ? QStringLiteral("%1 settings match").arg(matchCount)
                : QStringLiteral("Search Advanced settings");
        settingsSearchEdit_->setAccessibleDescription(resultText);
        QAccessibleEvent event(settingsSearchEdit_, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void MainWindow::setApp(OpenZoomApp* app)
{
    app_ = app;
}

RenderWidget* MainWindow::renderWidget() const
{
    return renderWidget_;
}

QComboBox* MainWindow::cameraCombo() const { return cameraCombo_; }
QComboBox* MainWindow::microphoneCombo() const { return microphoneCombo_; }
QListWidget* MainWindow::presetList() const { return presetList_; }
QLabel* MainWindow::presetDescriptionLabel() const { return presetDescriptionLabel_; }
QPushButton* MainWindow::promotePresetButton() const { return promotePresetButton_; }
QCheckBox* MainWindow::blackWhiteCheckbox() const { return bwCheckbox_; }
QSlider* MainWindow::blackWhiteSlider() const { return bwSlider_; }
QCheckBox* MainWindow::zoomCheckbox() const { return zoomCheckbox_; }
QSlider* MainWindow::zoomSlider() const { return zoomSlider_; }
QPushButton* MainWindow::debugButton() const { return debugButton_; }
QComboBox* MainWindow::rotationCombo() const { return rotationCombo_; }
QComboBox* MainWindow::viewportRateCombo() const { return viewportRateCombo_; }
QComboBox* MainWindow::viewportFitCombo() const { return viewportFitCombo_; }
QComboBox* MainWindow::cameraAccelerationCombo() const {
    return cameraAccelerationCombo_;
}
QLabel* MainWindow::cameraAccelerationStatusLabel() const {
    return cameraAccelerationStatusLabel_;
}
QPushButton* MainWindow::testCameraAccelerationButton() const {
    return testCameraAccelerationButton_;
}
QComboBox* MainWindow::recordingCanvasCombo() const {
    return recordingCanvasCombo_;
}
QComboBox* MainWindow::applicationLanguageCombo() const {
    return applicationLanguageCombo_;
}
QComboBox* MainWindow::cameraFormatCombo() const { return cameraFormatCombo_; }
QLabel* MainWindow::cameraFormatNoticeLabel() const { return cameraFormatNoticeLabel_; }
QCheckBox* MainWindow::zoomWheelAccelerationCheckbox() const {
    return zoomWheelAccelerationCheckbox_;
}
QCheckBox* MainWindow::focusMarkerCheckbox() const { return focusMarkerCheckbox_; }
QSlider* MainWindow::zoomCenterXSlider() const { return zoomCenterXSlider_; }
QSlider* MainWindow::zoomCenterYSlider() const { return zoomCenterYSlider_; }
QCheckBox* MainWindow::joystickCheckbox() const { return joystickCheckbox_; }
QToolButton* MainWindow::controlsToggleButton() const { return controlsToggleButton_; }
QWidget* MainWindow::controlsContainer() const { return controlsContainer_; }
QCheckBox* MainWindow::blurCheckbox() const { return blurCheckbox_; }
QSlider* MainWindow::blurSigmaSlider() const { return blurSigmaSlider_; }
QSlider* MainWindow::blurRadiusSlider() const { return blurRadiusSlider_; }
QLabel* MainWindow::blurSigmaValueLabel() const { return blurSigmaValueLabel_; }
QLabel* MainWindow::blurRadiusValueLabel() const { return blurRadiusValueLabel_; }
QPushButton* MainWindow::capturePhotoButton() const { return capturePhotoButton_; }
QPushButton* MainWindow::recordButton() const { return recordButton_; }
QPushButton* MainWindow::annotationButton() const { return annotationButton_; }
AnnotationOverlay* MainWindow::annotationOverlay() const { return annotationOverlay_; }
QCheckBox* MainWindow::temporalSmoothCheckbox() const { return temporalSmoothCheckbox_; }
QSlider* MainWindow::temporalSmoothSlider() const { return temporalSmoothSlider_; }
QLabel* MainWindow::temporalSmoothValueLabel() const { return temporalSmoothValueLabel_; }
QCheckBox* MainWindow::vlmAssistCheckbox() const { return vlmAssistCheckbox_; }
QCheckBox* MainWindow::assistiveOverlayCheckbox() const { return assistiveOverlayCheckbox_; }
QCheckBox* MainWindow::spatialSharpenCheckbox() const { return spatialSharpenCheckbox_; }
QComboBox* MainWindow::spatialBackendCombo() const { return spatialBackendCombo_; }
QSlider* MainWindow::spatialSharpnessSlider() const { return spatialSharpnessSlider_; }
QLabel* MainWindow::spatialSharpnessValueLabel() const { return spatialSharpnessValueLabel_; }
QLabel* MainWindow::processingStatusLabel() const { return processingStatusLabel_; }
QLabel* MainWindow::performanceDiagnosticsLabel() const
{
    return performanceDiagnosticsLabel_;
}
QAbstractButton* MainWindow::simpleModeButton() const { return simpleModeButton_; }
QAbstractButton* MainWindow::advancedModeButton() const { return advancedModeButton_; }
QPushButton* MainWindow::explainNowButton() const { return explainNowButton_; }
QPushButton* MainWindow::readTextButton() const { return readTextButton_; }
QCheckBox* MainWindow::transcribeMicrophoneCheckbox() const
{
    return transcribeMicrophoneCheckbox_;
}
QCheckBox* MainWindow::transcriptToNotesCheckbox() const
{
    return transcriptToNotesCheckbox_;
}
QLabel* MainWindow::transcriptionStatusLabel() const { return transcriptionStatusLabel_; }
QLabel* MainWindow::transcriptionQuotaLabel() const { return transcriptionQuotaLabel_; }
QLabel* MainWindow::transcriptPartialLabel() const { return transcriptPartialLabel_; }
QPlainTextEdit* MainWindow::transcriptFinalsView() const { return transcriptFinalsView_; }

void MainWindow::SetSimpleTranscriptActive(bool active)
{
    if (simpleTranscriptActive_ == active && simpleTranscriptPanel_ &&
        simpleTranscriptPanel_->isVisible() == (active && isSimpleMode())) {
        return;
    }
    simpleTranscriptActive_ = active;
    if (!active) {
        simpleTranscriptPartial_.clear();
        simpleTranscriptFinals_.clear();
        RefreshSimpleTranscriptText();
    }
    if (simpleTranscriptPanel_) {
        simpleTranscriptPanel_->setVisible(active && isSimpleMode());
        if (simpleTranscriptPanel_->isVisible()) {
            UpdateSimpleChromeGeometry();
        }
    }
}

void MainWindow::SetSimpleTranscriptPartial(const QString& text)
{
    if (simpleTranscriptPartial_ == text) {
        return;
    }
    simpleTranscriptPartial_ = text;
    RefreshSimpleTranscriptText();
}

void MainWindow::AppendSimpleTranscriptFinal(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    simpleTranscriptFinals_.append(text);
    while (simpleTranscriptFinals_.size() > 3) {
        simpleTranscriptFinals_.removeFirst();
    }
    RefreshSimpleTranscriptText();
}

void MainWindow::RefreshSimpleTranscriptText()
{
    if (!simpleTranscriptLabel_) {
        return;
    }
    QStringList lines = simpleTranscriptFinals_;
    if (!simpleTranscriptPartial_.trimmed().isEmpty()) {
        lines.append(simpleTranscriptPartial_);
    }
    // Silent update: plain text only, no accessible announcement per delta.
    simpleTranscriptLabel_->setText(lines.join(QStringLiteral("\n")));
    if (simpleTranscriptPanel_ && simpleTranscriptPanel_->isVisible()) {
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::setAnnotationPreferences(const QColor& color,
                                          int widthPixels,
                                          bool captureOnExit,
                                          bool dashed,
                                          const QString& shapeKind,
                                          int textSizePixels)
{
    if (annotationCaptureOnExitCheckbox_) {
        const QSignalBlocker blocker(annotationCaptureOnExitCheckbox_);
        annotationCaptureOnExitCheckbox_->setChecked(captureOnExit);
    }
    if (annotationOverlay_) {
        annotationOverlay_->SetPreferences(color,
                                           widthPixels,
                                           captureOnExit,
                                           dashed,
                                           shapeKind,
                                           textSizePixels);
    }
}

void MainWindow::setAnnotationViewTransform(const ViewTransform& transform)
{
    if (annotationOverlay_) {
        annotationOverlay_->SetViewTransform(transform);
    }
}

void MainWindow::setAnnotationMode(bool enabled)
{
    if (!annotationOverlay_ || !annotationButton_) {
        return;
    }
    if (!enabled) {
        annotationOverlay_->CommitPendingText();
    }
    changingAnnotationMode_ = true;
    annotationButton_->setChecked(enabled);
    changingAnnotationMode_ = false;
    annotationOverlay_->SetActive(enabled);
    if (enabled) {
        chromePinned_ = true;
        RevealSimpleChrome();
        // The annotation canvas is a transparent top-level tool window. Keep
        // the persistent action bar above it so visible Photo, Record/Stop,
        // Explain, Read, and Draw controls remain clickable in both modes.
        if (bottomRightPanel_) {
            bottomRightPanel_->show();
            bottomRightPanel_->raise();
        }
    } else {
        chromePinned_ = false;
        annotationButton_->setFocus(Qt::OtherFocusReason);
        if (isSimpleMode()) {
            simpleChromeIdleTimer_->start();
        }
    }
}
QCheckBox* MainWindow::stabilizationCheckbox() const { return stabilizationCheckbox_; }
QCheckBox* MainWindow::bumpHoldCheckbox() const {
    return bumpHoldCheckbox_;
}
QCheckBox* MainWindow::keystoneCheckbox() const { return keystoneCheckbox_; }
QCheckBox* MainWindow::autoContrastCheckbox() const { return autoContrastCheckbox_; }
QSlider* MainWindow::autoContrastStrengthSlider() const { return autoContrastStrengthSlider_; }
QCheckBox* MainWindow::simpleTextClarityCheckbox() const { return simpleTextClarityCheckbox_; }
QCheckBox* MainWindow::textClarityCheckbox() const { return textClarityCheckbox_; }
QCheckBox* MainWindow::backgroundFlattenCheckbox() const { return backgroundFlattenCheckbox_; }
QSlider* MainWindow::backgroundFlattenStrengthSlider() const { return backgroundFlattenStrengthSlider_; }
QCheckBox* MainWindow::adaptiveBinarizationCheckbox() const { return adaptiveBinarizationCheckbox_; }
QSlider* MainWindow::sauvolaStrengthSlider() const { return sauvolaStrengthSlider_; }
QSlider* MainWindow::binarizationSoftnessSlider() const { return binarizationSoftnessSlider_; }
QComboBox* MainWindow::textPolarityCombo() const { return textPolarityCombo_; }
QSlider* MainWindow::strokeWeightSlider() const { return strokeWeightSlider_; }
QCheckBox* MainWindow::smartSharpenCheckbox() const { return smartSharpenCheckbox_; }
QSlider* MainWindow::smartSharpenStrengthSlider() const { return smartSharpenStrengthSlider_; }
QCheckBox* MainWindow::claheCheckbox() const { return claheCheckbox_; }
QSlider* MainWindow::claheClipLimitSlider() const { return claheClipLimitSlider_; }
QCheckBox* MainWindow::twoColorTextCheckbox() const { return twoColorTextCheckbox_; }
QCheckBox* MainWindow::textHysteresisCheckbox() const { return textHysteresisCheckbox_; }
QSlider* MainWindow::textHysteresisStrengthSlider() const { return textHysteresisStrengthSlider_; }
QCheckBox* MainWindow::selectiveSharpenCheckbox() const { return selectiveSharpenCheckbox_; }
QCheckBox* MainWindow::focusDetectionCheckbox() const { return focusDetectionCheckbox_; }
QSlider* MainWindow::focusThresholdSlider() const { return focusThresholdSlider_; }
QCheckBox* MainWindow::glareSuppressionCheckbox() const { return glareSuppressionCheckbox_; }
QSlider* MainWindow::glareSuppressionStrengthSlider() const { return glareSuppressionStrengthSlider_; }
QCheckBox* MainWindow::mlTextSuperResolutionCheckbox() const { return mlTextSuperResolutionCheckbox_; }
QSlider* MainWindow::mlTextSuperResolutionStrengthSlider() const { return mlTextSuperResolutionStrengthSlider_; }
QCheckBox* MainWindow::mlTextSuperResolutionPrefer2xCheckbox() const {
    return mlTextSuperResolutionPrefer2xCheckbox_;
}
QCheckBox* MainWindow::mlTextSuperResolutionUltra1440pCheckbox() const {
    return mlTextSuperResolutionUltra1440pCheckbox_;
}
ColorSchemePicker* MainWindow::displayColorPicker() const { return displayColorPicker_; }
QSlider* MainWindow::contrastSlider() const { return contrastSlider_; }
QSlider* MainWindow::brightnessSlider() const { return brightnessSlider_; }
QPushButton* MainWindow::aiSettingsButton() const { return aiSettingsButton_; }
QPushButton* MainWindow::openNotesButton() const { return openNotesButton_; }
QPushButton* MainWindow::setupAssistantButton() const { return setupAssistantButton_; }

void MainWindow::setMaxineRuntimeInstalled(bool installed)
{
    maxineRuntimeInstalled_ = installed;
#if OPENZOOM_ENABLE_TEXT_SR
    if (mlTextSuperResolutionCheckbox_) {
        mlTextSuperResolutionCheckbox_->setEnabled(installed);
        mlTextSuperResolutionCheckbox_->setToolTip(
            installed
                ? QStringLiteral("Use NVIDIA Video Effects SuperRes at 1.33x zoom and above; "
                                 "falls back to NIS automatically")
                : QStringLiteral("NVIDIA Video Effects runtime is not installed; "
                                 "open Setup & Downloads"));
    }
    if (mlTextSuperResolutionStrengthSlider_) {
        mlTextSuperResolutionStrengthSlider_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (mlTextSuperResolutionPrefer2xCheckbox_) {
        mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (mlTextSuperResolutionUltra1440pCheckbox_) {
        mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (!installed && mlTextSuperResolutionStatusLabel_) {
        setSuperResStatus(QStringLiteral("Runtime not installed; use Setup & Downloads"),
                          false);
    }
#else
    Q_UNUSED(installed);
    if (mlTextSuperResolutionCheckbox_) {
        mlTextSuperResolutionCheckbox_->setEnabled(false);
        mlTextSuperResolutionCheckbox_->setToolTip(
            QStringLiteral("Unavailable in this build; NVIDIA Super Resolution "
                           "support was not compiled"));
    }
    if (mlTextSuperResolutionStrengthSlider_) {
        mlTextSuperResolutionStrengthSlider_->setEnabled(false);
    }
    if (mlTextSuperResolutionPrefer2xCheckbox_) {
        mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(false);
    }
    if (mlTextSuperResolutionUltra1440pCheckbox_) {
        mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(false);
    }
    if (mlTextSuperResolutionStatusLabel_) {
        setSuperResStatus(QStringLiteral("Unavailable in this build"), false);
    }
#endif

    if (!maxineAttribution_) {
        return;
    }
    const QString status = installed
                               ? QStringLiteral("NVIDIA Video Effects runtime installed")
                               : QStringLiteral("NVIDIA Video Effects runtime not installed; use Setup & Downloads");
    maxineAttribution_->setToolTip(status);
    maxineAttribution_->setAccessibleDescription(status);
}

bool MainWindow::isMaxineRuntimeInstalled() const
{
    return maxineRuntimeInstalled_;
}

void MainWindow::ShowHelpDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("OpenZoom Help"));
    dialog.setWindowIcon(windowIcon());
    dialog.setModal(true);
    dialog.setMinimumSize(520, 420);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* guide = new QTextBrowser(&dialog);
    guide->setAccessibleName(QStringLiteral("OpenZoom controls and features guide"));
    guide->setOpenExternalLinks(false);
    guide->setHtml(QStringLiteral(
        "<h2>Controls</h2>"
        "<p><b>Simple</b> maximizes the camera view. Use the corner carousel "
        "or number keys 1 through 9 to change quick modes.</p>"
        "<p><b>Advanced</b> opens detailed image and Assistant settings. Drag "
        "the divider at the panel edge to resize it.</p>"
        "<p>Drag the camera view to pan. Use Ctrl plus the mouse wheel to zoom. "
        "Enable <b>Virtual Joystick</b> at the top of the Image panel for an "
        "on-screen movement control.</p>"
        "<p>Photo and Record save both original and processed versions. Explain "
        "describes the scene; Read recognizes text. Press <b>Ctrl+Shift+O</b> "
        "to open the folder containing your OpenZoom files.</p>"
        "<h2>Features</h2>"
        "<p><b>Text Clarity</b> combines document cleanup, local contrast, "
        "sharpening, and stable text edges. <b>Display Colors</b> provides "
        "reading palettes and custom color schemes.</p>"
        "<p><b>NVIDIA Super Resolution</b> improves zoomed detail when the "
        "optional NVIDIA Video Effects runtime is installed.</p>"
        "<p><b>Text reading and Assistant</b> can read text, explain the view, answer "
        "follow-up questions, and add results to lecture notes.</p>"));
    layout->addWidget(guide, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::setSuperResStatus(const QString& status,
                                   bool active,
                                   bool performanceLimited)
{
    if (!mlTextSuperResolutionStatusLabel_) {
        return;
    }
    SetLiveText(mlTextSuperResolutionStatusLabel_, status,
                LivePoliteness::kSilent,
                QStringLiteral("NVIDIA Super Resolution status"));
    mlTextSuperResolutionStatusLabel_->setToolTip(status);
    mlTextSuperResolutionStatusLabel_->setAccessibleDescription(status);
    const QString color = status == QStringLiteral("Off")
                              ? QStringLiteral("#cfcfcf")
                              : active ? QStringLiteral("#33d17a")
                                       : QStringLiteral("#f6c85f");
    mlTextSuperResolutionStatusLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(color));
    if (mlTextSuperResolutionOverrideCheckbox_) {
        mlTextSuperResolutionOverrideCheckbox_->setVisible(
            maxineRuntimeInstalled_ &&
            (performanceLimited || mlTextSuperResolutionOverrideCheckbox_->isChecked()));
    }
}

void MainWindow::setSuperResPerformanceOverrideChecked(bool checked)
{
    if (!mlTextSuperResolutionOverrideCheckbox_) {
        return;
    }
    const QSignalBlocker blocker(mlTextSuperResolutionOverrideCheckbox_);
    mlTextSuperResolutionOverrideCheckbox_->setChecked(checked);
    if (!checked) {
        mlTextSuperResolutionOverrideCheckbox_->setVisible(false);
    }
}
QLabel* MainWindow::assistantConnectionLabel() const { return assistantConnectionLabel_; }
QLabel* MainWindow::assistantUsageLabel() const { return assistantUsageLabel_; }
QPushButton* MainWindow::assistantConnectButton() const { return assistantConnectButton_; }
QTextBrowser* MainWindow::assistantTranscript() const { return assistantTranscript_; }
QPlainTextEdit* MainWindow::assistantPromptEdit() const { return assistantPromptEdit_; }
QCheckBox* MainWindow::assistantAttachFrameCheckbox() const { return assistantAttachFrameCheckbox_; }
QPushButton* MainWindow::assistantSendButton() const { return assistantSendButton_; }
QPushButton* MainWindow::assistantStopButton() const { return assistantStopButton_; }
QPushButton* MainWindow::assistantNewButton() const { return assistantNewButton_; }
QListWidget* MainWindow::assistantHistoryList() const { return assistantHistoryList_; }
QPushButton* MainWindow::assistantRenameButton() const { return assistantRenameButton_; }
QPushButton* MainWindow::assistantExportButton() const { return assistantExportButton_; }
QPushButton* MainWindow::assistantDeleteButton() const { return assistantDeleteButton_; }

void MainWindow::setKeystoneTrackingControls(bool active,
                                              bool available,
                                              bool paused,
                                              bool canStepBack,
                                              bool canStepForward,
                                              bool stepPending,
                                              int position,
                                              int count)
{
    const bool wasActive = keystoneTrackingActive_;
    const QString pauseText = paused ? QStringLiteral("Continue") : QStringLiteral("Stop");
    const bool geometryChanged = wasActive != active ||
                                 advancedKeystonePauseButton_->text() != pauseText;
    keystoneTrackingActive_ = active;

    const QString positionText = count > 0
                                     ? QStringLiteral(" Correction %1 of %2.")
                                           .arg(std::clamp(position, 1, count))
                                           .arg(count)
                                     : QString();
    const QString unavailable = QStringLiteral("GPU screen correction is unavailable.");
    const QString backTip = available
                                ? QStringLiteral("Previous screen correction.%1").arg(positionText)
                                : unavailable;
    const QString pauseTip = available
                                 ? (paused ? QStringLiteral("Continue automatic screen correction.%1")
                                           : QStringLiteral("Stop and hold the current screen correction.%1"))
                                       .arg(positionText)
                                 : unavailable;
    const QString nextTip = !available
                                ? unavailable
                                : stepPending
                                      ? QStringLiteral("Finding one new screen correction...")
                                      : QStringLiteral("Next screen correction, or find one new correction.%1")
                                            .arg(positionText);

    SetLiveText(advancedKeystonePauseButton_, pauseText,
                LivePoliteness::kSilent,
                QStringLiteral("Automatic screen correction"));
    const QIcon pauseIcon(paused ? QStringLiteral(":/openzoom/icons/play.svg")
                                 : QStringLiteral(":/openzoom/icons/pause.svg"));
    advancedKeystonePauseButton_->setIcon(pauseIcon);
    simpleKeystonePauseButton_->setIcon(pauseIcon);

    const bool controlsEnabled = active && available;
    advancedKeystoneTrackingRow_->setEnabled(controlsEnabled);
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        button->setEnabled(controlsEnabled && canStepBack);
        button->setToolTip(backTip);
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        button->setEnabled(controlsEnabled);
        button->setToolTip(pauseTip);
        button->setAccessibleName(paused ? QStringLiteral("Continue Automatic Screen Correction")
                                         : QStringLiteral("Stop Automatic Screen Correction"));
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        button->setEnabled(controlsEnabled && paused && canStepForward && !stepPending);
        button->setToolTip(nextTip);
    }

    if (!active || !isSimpleMode()) {
        keystoneTrackingPanel_->hide();
    } else if (simpleChromeVisible_) {
        keystoneTrackingPanel_->show();
        keystoneTrackingPanel_->raise();
    }
    if (geometryChanged) {
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::ActivatePresetRow(int row)
{
    if (!presetList_ || row < 0 || row >= presetList_->count()) {
        return;
    }

    QListWidgetItem* item = presetList_->item(row);
    if (presetList_->currentRow() == row && item) {
        const QString label = item->data(Qt::AccessibleTextRole).toString();
        ShowModeAnnouncement(label, item->data(Qt::StatusTipRole).toString());
    } else {
        presetList_->setCurrentRow(row);
    }
    presetList_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    modeGridPopup_->hide();
    RevealSimpleChrome();
}

void MainWindow::ActivateRelativePreset(int offset)
{
    if (!presetList_ || presetList_->count() == 0) {
        return;
    }
    const int count = presetList_->count();
    int row = presetList_->currentRow();
    if (row < 0) {
        row = 0;
    } else {
        row = (row + offset + count) % count;
    }
    ActivatePresetRow(row);
}

void MainWindow::ToggleModeGrid()
{
    if (!modeGridPopup_) {
        return;
    }
    const bool show = !modeGridPopup_->isVisible();
    if (show) {
        if (isSimpleMode()) {
            RevealSimpleChrome();
        } else if (bottomLeftPanel_) {
            bottomLeftPanel_->setWindowOpacity(1.0);
            bottomLeftPanel_->show();
            bottomLeftPanel_->raise();
        }
        simpleChromeIdleTimer_->stop();
        UpdateSimpleChromeGeometry();
        modeGridPopup_->show();
        modeGridPopup_->raise();
        presetList_->setFocus(Qt::ShortcutFocusReason);
        if (QListWidgetItem* current = presetList_->currentItem()) {
            presetList_->scrollToItem(current, QAbstractItemView::PositionAtCenter);
        }
    } else {
        modeGridPopup_->hide();
        if (isSimpleMode()) {
            RevealSimpleChrome();
        }
        if (currentModeButton_) {
            currentModeButton_->setFocus(Qt::PopupFocusReason);
        }
    }
}

void MainWindow::UpdateCurrentPresetUi(QListWidgetItem* current, QListWidgetItem* previous)
{
    if (!currentModeButton_) {
        return;
    }

    if (!current) {
        const QString buttonText = QStringLiteral("Custom Setup");
        currentModeButton_->setProperty("fullChromeText", buttonText);
        SetLiveText(currentModeButton_, buttonText,
                    LivePoliteness::kSilent,
                    QStringLiteral("Current quick mode"));
        return;
    }

    QString label = current->data(Qt::AccessibleTextRole).toString();
    if (label.isEmpty()) {
        label = PlainPresetLabel(current->data(Qt::StatusTipRole).toString());
    }
    const int row = presetList_->row(current);
    const QString shortcut = row >= 0 && row < kSimpleShortcutCount
                                 ? QStringLiteral("   [%1]").arg(row + 1)
                                 : QString();
    const QString buttonText = label + shortcut;
    currentModeButton_->setProperty("fullChromeText", buttonText);
    SetLiveText(currentModeButton_, buttonText,
                LivePoliteness::kSilent,
                QStringLiteral("Current quick mode"));

    if (previous && previous != current && isSimpleMode()) {
        ShowModeAnnouncement(label, current->data(Qt::StatusTipRole).toString());
    }
}

void MainWindow::ShowModeAnnouncement(const QString& label, const QString& profileName)
{
    if (label.trimmed().isEmpty() || !modeToast_) {
        return;
    }

    modeToastTitle_->setText(label);
    modeToastSubtitle_->setText(
        profileName.trimmed().isEmpty()
            ? TranslateUi(QStringLiteral("Quick mode"))
            : TranslateUi(QStringLiteral("%1 profile")).arg(profileName));
    modeToast_->setAccessibleName(
        TranslateUi(QStringLiteral("Quick mode changed to %1")).arg(label));
    UpdateSimpleChromeGeometry();
    modeToast_->show();
    modeToast_->raise();
    modeToastTimer_->start();

    QAccessibleAnnouncementEvent accessibleAnnouncement(modeToast_, label);
    accessibleAnnouncement.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
    QAccessible::updateAccessibility(&accessibleAnnouncement);
}

void MainWindow::UpdateSimpleChromeGeometry()
{
    if (!renderWidget_ || !topLeftPanel_ || renderWidget_->width() <= 0 || renderWidget_->height() <= 0) {
        return;
    }

    const int viewWidth = renderWidget_->width();
    const int viewHeight = renderWidget_->height();
    const QPoint viewOrigin = renderWidget_->mapToGlobal(QPoint(0, 0));
    const QString currentModeText =
        currentModeButton_->property("fullChromeText").toString().isEmpty()
            ? currentModeButton_->text()
            : currentModeButton_->property("fullChromeText").toString();
    currentModeButton_->setText(currentModeText);
    currentModeButton_->setMinimumWidth(220);
    currentModeButton_->setMaximumWidth(QWIDGETSIZE_MAX);
    const std::array<std::pair<QPushButton*, QString>, 5> actionButtons{{
        {capturePhotoButton_, QStringLiteral("Photo")},
        {recordButton_, !recordButton_->isEnabled()
                            ? QStringLiteral("Finishing")
                            : recordButton_->isChecked()
                                  ? QStringLiteral("Stop")
                                  : QStringLiteral("Record")},
        {explainNowButton_, explainNowButton_->text() == QStringLiteral("Stop")
                                ? QStringLiteral("Stop")
                                : QStringLiteral("Explain")},
        {readTextButton_, QStringLiteral("Read")},
        {annotationButton_, QStringLiteral("Draw")},
    }};
    for (const auto& [button, label] : actionButtons) {
        button->setText(label);
        button->setMinimumWidth(88);
        button->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    for (QWidget* panel :
         {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_,
          bottomRightPanel_}) {
        panel->adjustSize();
    }
    const int trackingWidth =
        keystoneTrackingActive_ ? keystoneTrackingPanel_->width() : 0;
    const bool compactActions =
        bottomLeftPanel_->width() + trackingWidth +
            bottomRightPanel_->width() >
        viewWidth;
    if (compactActions) {
        for (const auto& [button, label] : actionButtons) {
            button->setProperty("fullChromeText", label);
            button->setText(QString());
            button->setFixedWidth(58);
        }
    }
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        panel->adjustSize();
    }
    if (bottomLeftPanel_->width() + trackingWidth +
            bottomRightPanel_->width() >
        viewWidth) {
        currentModeButton_->setMinimumWidth(120);
        currentModeButton_->setMaximumWidth(180);
        currentModeButton_->setText(
            currentModeButton_->fontMetrics().elidedText(
                currentModeText, Qt::ElideRight, 160));
        bottomLeftPanel_->adjustSize();
    }

    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const auto leadingX = [&](int itemWidth) {
        return rightToLeft
                   ? viewOrigin.x() + std::max(0, viewWidth - itemWidth)
                   : viewOrigin.x();
    };
    const auto trailingX = [&](int itemWidth) {
        return rightToLeft
                   ? viewOrigin.x()
                   : viewOrigin.x() + std::max(0, viewWidth - itemWidth);
    };

    topLeftPanel_->move(leadingX(topLeftPanel_->width()), viewOrigin.y());
    bottomLeftPanel_->move(leadingX(bottomLeftPanel_->width()),
                           viewOrigin.y() + std::max(0, viewHeight - bottomLeftPanel_->height()));
    const bool trackingInline = keystoneTrackingActive_ &&
                                bottomLeftPanel_->width() + keystoneTrackingPanel_->width() +
                                        bottomRightPanel_->width() <= viewWidth;
    const int leftChromeHeight = bottomLeftPanel_->height() +
                                 (keystoneTrackingActive_ && !trackingInline
                                      ? keystoneTrackingPanel_->height()
                                      : 0);
    const int leftChromeWidth = trackingInline
                                    ? bottomLeftPanel_->width() + keystoneTrackingPanel_->width()
                                    : std::max(bottomLeftPanel_->width(),
                                               keystoneTrackingActive_ ? keystoneTrackingPanel_->width() : 0);
    if (keystoneTrackingActive_) {
        const int trackingX =
            trackingInline
                ? (rightToLeft
                       ? viewWidth - bottomLeftPanel_->width() -
                             keystoneTrackingPanel_->width()
                       : bottomLeftPanel_->width())
                : (rightToLeft
                       ? viewWidth - keystoneTrackingPanel_->width()
                       : 0);
        const int trackingY = trackingInline
                                  ? viewHeight - keystoneTrackingPanel_->height()
                                  : viewHeight - bottomLeftPanel_->height() - keystoneTrackingPanel_->height();
        keystoneTrackingPanel_->move(viewOrigin.x() + trackingX,
                                     viewOrigin.y() + std::max(0, trackingY));
    }
    const bool bottomPanelsOverlap = leftChromeWidth + bottomRightPanel_->width() > viewWidth;
    const int bottomRightY = bottomPanelsOverlap
                                 ? viewHeight - leftChromeHeight - bottomRightPanel_->height()
                                 : viewHeight - bottomRightPanel_->height();
    bottomRightPanel_->move(trailingX(bottomRightPanel_->width()),
                            viewOrigin.y() + std::max(0, bottomRightY));

    if (simpleTranscriptPanel_ && simpleTranscriptPanel_->isVisible()) {
        const int overlayWidth = std::min(viewWidth - 24, 900);
        simpleTranscriptPanel_->setFixedWidth(std::max(240, overlayWidth));
        simpleTranscriptPanel_->adjustSize();
        const int chromeTop =
            std::min(viewHeight - leftChromeHeight,
                     std::min(bottomRightY, viewHeight - bottomRightPanel_->height()));
        const int overlayY =
            std::max(0, chromeTop - simpleTranscriptPanel_->height() - 8);
        simpleTranscriptPanel_->move(
            viewOrigin.x() +
                std::max(0, (viewWidth - simpleTranscriptPanel_->width()) / 2),
            viewOrigin.y() + overlayY);
    }

    if (modeGridPopup_) {
        const int popupWidth = std::min(viewWidth, std::max(420, std::min(860, viewWidth * 3 / 4)));
        const int columns = std::max(2, (popupWidth - 28) / 220);
        const int rows = std::max(1, (presetList_->count() + columns - 1) / columns);
        const int popupHeight = std::min(std::max(126, rows * 106 + 16),
                                        std::max(126, viewHeight - leftChromeHeight));
        const QSize gridSize(std::max(150, (popupWidth - 32) / columns), 102);
        presetList_->setGridSize(gridSize);
        for (int row = 0; row < presetList_->count(); ++row) {
            if (QListWidgetItem* item = presetList_->item(row)) {
                item->setSizeHint(gridSize - QSize(8, 8));
            }
        }
        modeGridPopup_->setGeometry(leadingX(popupWidth),
                                    viewOrigin.y() + std::max(0, viewHeight - leftChromeHeight - popupHeight),
                                    popupWidth,
                                    popupHeight);
    }

    if (modeToast_) {
        const int toastWidth = std::min(760, std::max(300, viewWidth - 32));
        const int toastHeight = std::min(170, std::max(112, viewHeight / 4));
        modeToast_->setGeometry(viewOrigin.x() + std::max(0, (viewWidth - toastWidth) / 2),
                                viewOrigin.y() + std::max(0, (viewHeight - toastHeight) / 2),
                                toastWidth,
                                toastHeight);
    }
}

void MainWindow::SetChromeOpacity(qreal opacity, int durationMs, bool hideWhenFinished)
{
    if (chromeAnimation_) {
        chromeAnimation_->stop();
        chromeAnimation_->deleteLater();
        chromeAnimation_ = nullptr;
    }

    chromeAnimation_ = new QParallelAnimationGroup(this);
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        if (!panel) {
            continue;
        }
        if (panel == keystoneTrackingPanel_ && !keystoneTrackingActive_) {
            panel->hide();
            continue;
        }
        if (opacity > 0.0) {
            panel->show();
            panel->raise();
        }
        auto* animation = new QPropertyAnimation(panel, "windowOpacity", chromeAnimation_);
        animation->setDuration(durationMs);
        animation->setStartValue(panel->windowOpacity());
        animation->setEndValue(opacity);
        chromeAnimation_->addAnimation(animation);
    }

    QParallelAnimationGroup* animationGroup = chromeAnimation_;
    connect(animationGroup, &QParallelAnimationGroup::finished, this,
            [this, animationGroup, hideWhenFinished]() {
                if (hideWhenFinished && !simpleChromeVisible_ && isSimpleMode()) {
                    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
                        if (panel) {
                            panel->hide();
                        }
                    }
                }
                if (chromeAnimation_ == animationGroup) {
                    chromeAnimation_ = nullptr;
                }
                animationGroup->deleteLater();
            });
    animationGroup->start();
}

bool MainWindow::SimpleChromeHasFocus() const
{
    QWidget* focus = QApplication::focusWidget();
    if (!focus) {
        return false;
    }
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_, modeGridPopup_}) {
        if (panel && (focus == panel || panel->isAncestorOf(focus))) {
            return true;
        }
    }
    return false;
}

void MainWindow::RevealSimpleChrome()
{
    if (!isSimpleMode()) {
        return;
    }
    const bool alreadyVisible =
        simpleChromeVisible_ &&
        topLeftPanel_ && topLeftPanel_->isVisible() &&
        bottomLeftPanel_ && bottomLeftPanel_->isVisible() &&
        bottomRightPanel_ && bottomRightPanel_->isVisible();
    if (alreadyVisible) {
        // Mouse movement reaches both Qt's event filter and the native event
        // filter. While the chrome is already visible, activity should only
        // extend its idle deadline; moving/raising four native tool windows on
        // every pointer sample starves the high-refresh viewport clock.
        if (!chromePinned_ && modeGridPopup_ && !modeGridPopup_->isVisible()) {
            const int remainingMs = simpleChromeIdleTimer_->remainingTime();
            if (remainingMs < 0 ||
                remainingMs <= kSimpleChromeIdleMs - 100) {
                simpleChromeIdleTimer_->start();
            }
        }
        return;
    }
    const bool needsAnimation = !simpleChromeVisible_ ||
                                (topLeftPanel_ && !topLeftPanel_->isVisible());
    simpleChromeVisible_ = true;
    UpdateSimpleChromeGeometry();
    if (needsAnimation) {
        SetChromeOpacity(1.0, 120, false);
    } else {
        for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
            if (panel) {
                if (panel == keystoneTrackingPanel_ && !keystoneTrackingActive_) {
                    panel->hide();
                    continue;
                }
                panel->show();
                panel->raise();
            }
        }
    }
    if (!chromePinned_ && !modeGridPopup_->isVisible()) {
        simpleChromeIdleTimer_->start();
    }
}

void MainWindow::FadeSimpleChrome()
{
    if (!isSimpleMode() || chromePinned_) {
        return;
    }
    if (modeGridPopup_->isVisible() || SimpleChromeHasFocus()) {
        simpleChromeIdleTimer_->start();
        return;
    }
    simpleChromeVisible_ = false;
    SetChromeOpacity(0.0, kSimpleChromeFadeMs, true);
}

void MainWindow::setSimpleMode(bool simple)
{
    if (advancedPanel_) {
        advancedPanel_->setVisible(!simple);
    }
    // Checking one button in the exclusive group unchecks the other; the
    // toggled handlers re-enter here but converge because setChecked() and
    // setCurrentIndex() are no-ops once the state matches.
    if (simple && simpleModeButton_) {
        simpleModeButton_->setChecked(true);
        bottomLeftPanel_->show();
        if (keystoneTrackingActive_) {
            keystoneTrackingPanel_->show();
        }
        bottomRightPanel_->show();
        if (simpleTranscriptPanel_ && simpleTranscriptActive_) {
            simpleTranscriptPanel_->show();
        }
        UpdateSimpleChromeGeometry();
        RevealSimpleChrome();
    } else if (!simple && advancedModeButton_) {
        advancedModeButton_->setChecked(true);
        QTimer::singleShot(0, this, [this]() { ApplyAdvancedPanelWidth(); });
        simpleChromeIdleTimer_->stop();
        modeToastTimer_->stop();
        modeGridPopup_->hide();
        modeToast_->hide();
        simpleChromeVisible_ = true;
        if (chromeAnimation_) {
            chromeAnimation_->stop();
        }
        topLeftPanel_->setWindowOpacity(1.0);
        topLeftPanel_->show();
        topLeftPanel_->raise();
        // Keep the quick-mode carousel available in Advanced as a compact
        // camera-corner control instead of duplicating the preset model.
        bottomLeftPanel_->setWindowOpacity(1.0);
        bottomLeftPanel_->show();
        bottomLeftPanel_->raise();
        keystoneTrackingPanel_->hide();
        bottomRightPanel_->setWindowOpacity(1.0);
        bottomRightPanel_->show();
        bottomRightPanel_->raise();
        if (simpleTranscriptPanel_) {
            // The Advanced Transcript tab shows the same content instead.
            simpleTranscriptPanel_->hide();
        }
        UpdateSimpleChromeGeometry();
    }
}

bool MainWindow::isSimpleMode() const
{
    return !advancedPanel_ || !advancedPanel_->isVisible();
}

int MainWindow::advancedPanelWidth() const
{
    if (advancedPanel_ && advancedPanel_->isVisible() && advancedPanel_->width() > 0) {
        return advancedPanel_->width();
    }
    return advancedPanelPreferredWidth_;
}

void MainWindow::setAdvancedPanelWidth(int width)
{
    advancedPanelPreferredWidth_ = std::clamp(width, kAdvancedPanelMinimumWidth, 1200);
    QTimer::singleShot(0, this, [this]() { ApplyAdvancedPanelWidth(); });
}

void MainWindow::ApplyAdvancedPanelWidth()
{
    if (!contentSplitter_ || !advancedPanel_ || !advancedPanel_->isVisible()) {
        return;
    }
    const int totalWidth = contentSplitter_->width();
    if (totalWidth <= 0) {
        return;
    }
    const int maximumPanelWidth = std::max(kAdvancedPanelMinimumWidth,
                                           totalWidth - renderWidget_->minimumWidth());
    const int panelWidth = std::clamp(advancedPanelPreferredWidth_,
                                      kAdvancedPanelMinimumWidth,
                                      maximumPanelWidth);
    contentSplitter_->setSizes({std::max(renderWidget_->minimumWidth(), totalWidth - panelWidth),
                                panelWidth});
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    const bool controlZoom =
        app_ && (event->modifiers() & Qt::ControlModifier) &&
        !HasEditableTextFocus();
    if (controlZoom &&
        (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
        app_->HandleKeyboardZoom(1.0f);
        event->accept();
        return;
    }
    if (controlZoom && event->key() == Qt::Key_Minus) {
        app_->HandleKeyboardZoom(-1.0f);
        event->accept();
        return;
    }
    if (app_ && app_->HandlePanKey(event->key(), true)) {
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (app_ && app_->HandlePanKey(event->key(), false)) {
        event->accept();
        return;
    }
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::LayoutDirectionChange) {
        UpdateDirectionalUi();
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::UpdateDirectionalUi()
{
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const QString previousIcon =
        rightToLeft ? QStringLiteral(":/openzoom/icons/next.svg")
                    : QStringLiteral(":/openzoom/icons/previous.svg");
    const QString nextIcon =
        rightToLeft ? QStringLiteral(":/openzoom/icons/previous.svg")
                    : QStringLiteral(":/openzoom/icons/next.svg");
    const QString backIcon =
        rightToLeft ? QStringLiteral(":/openzoom/icons/step-forward.svg")
                    : QStringLiteral(":/openzoom/icons/step-back.svg");
    const QString forwardIcon =
        rightToLeft ? QStringLiteral(":/openzoom/icons/step-back.svg")
                    : QStringLiteral(":/openzoom/icons/step-forward.svg");

    if (previousAdvancedTabButton_) {
        previousAdvancedTabButton_->setIcon(QIcon(previousIcon));
    }
    if (nextAdvancedTabButton_) {
        nextAdvancedTabButton_->setIcon(QIcon(nextIcon));
    }
    if (advancedKeystoneBackButton_) {
        advancedKeystoneBackButton_->setIcon(QIcon(backIcon));
    }
    if (advancedKeystoneNextButton_) {
        advancedKeystoneNextButton_->setIcon(QIcon(forwardIcon));
    }
    if (simpleKeystoneBackButton_) {
        simpleKeystoneBackButton_->setIcon(QIcon(backIcon));
    }
    if (simpleKeystoneNextButton_) {
        simpleKeystoneNextButton_->setIcon(QIcon(forwardIcon));
    }
    if (previousModeButton_) {
        previousModeButton_->setIcon(
            style()->standardIcon(QStyle::SP_ArrowBack));
    }
    if (nextModeButton_) {
        nextModeButton_->setIcon(
            style()->standardIcon(QStyle::SP_ArrowForward));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    const bool userActivity = type == QEvent::MouseMove ||
                              type == QEvent::MouseButtonPress ||
                              type == QEvent::Wheel ||
                              type == QEvent::KeyPress ||
                              type == QEvent::FocusIn ||
                              type == QEvent::Enter ||
                              type == QEvent::TouchBegin ||
                              type == QEvent::ApplicationActivate;
    if (isSimpleMode() && userActivity) {
        RevealSimpleChrome();
    }
    if (!isSimpleMode() && type == QEvent::ApplicationActivate) {
        simpleChromeVisible_ = true;
        topLeftPanel_->setWindowOpacity(1.0);
        bottomLeftPanel_->setWindowOpacity(1.0);
        bottomRightPanel_->setWindowOpacity(1.0);
        UpdateSimpleChromeGeometry();
        topLeftPanel_->show();
        topLeftPanel_->raise();
        bottomLeftPanel_->show();
        bottomLeftPanel_->raise();
        bottomRightPanel_->show();
        bottomRightPanel_->raise();
    }

    if (type == QEvent::ApplicationDeactivate) {
        // The chrome panels, grid popup, and toast are top-level tool windows,
        // which stay above OTHER applications' windows. Hide them when the user
        // switches away (e.g. to take notes); reactivation reveals them again
        // via the ApplicationActivate branch above.
        simpleChromeIdleTimer_->stop();
        if (modeToastTimer_) {
            modeToastTimer_->stop();
        }
        if (modeToast_) {
            modeToast_->hide();
        }
        if (modeGridPopup_) {
            modeGridPopup_->hide();
        }
        for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
            if (panel) {
                panel->hide();
            }
        }
        simpleChromeVisible_ = false;
    }

    if (type == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && modeGridPopup_->isVisible()) {
            modeGridPopup_->hide();
            if (isSimpleMode()) {
                RevealSimpleChrome();
            }
            if (currentModeButton_) {
                currentModeButton_->setFocus(Qt::PopupFocusReason);
            }
            event->accept();
            return true;
        }
    }

    if (isSimpleMode() && type == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_H && key->modifiers() == Qt::ControlModifier) {
            chromePinned_ = !chromePinned_;
            RevealSimpleChrome();
            if (chromePinned_) {
                simpleChromeIdleTimer_->stop();
            }
            QAccessibleAnnouncementEvent announcement(
                this,
                chromePinned_ ? QStringLiteral("Controls pinned")
                              : QStringLiteral("Controls will hide automatically"));
            announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
            QAccessible::updateAccessibility(&announcement);
            event->accept();
            return true;
        }
        if (key->key() == Qt::Key_Tab &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) &&
            !modeGridPopup_->isVisible()) {
            std::vector<QWidget*> focusOrder{
                simpleModeButton_, advancedModeButton_, simpleTextClarityCheckbox_, modeGridButton_,
                previousModeButton_, currentModeButton_, nextModeButton_,
                simpleKeystoneBackButton_, simpleKeystonePauseButton_, simpleKeystoneNextButton_,
                capturePhotoButton_, recordButton_, explainNowButton_, readTextButton_,
                annotationButton_};
            if (annotationOverlay_ && annotationOverlay_->IsActive()) {
                const auto annotationTargets = annotationOverlay_->FocusTargets();
                focusOrder.insert(focusOrder.end(),
                                  annotationTargets.begin(),
                                  annotationTargets.end());
            }
            if (auto* overlay = findChild<AssistiveOverlay*>();
                overlay && overlay->isVisible()) {
                const auto overlayTargets = overlay->FocusTargets();
                focusOrder.insert(focusOrder.end(), overlayTargets.begin(), overlayTargets.end());
            }
            QWidget* current = QApplication::focusWidget();
            int currentIndex = -1;
            for (int i = 0; i < static_cast<int>(focusOrder.size()); ++i) {
                if (focusOrder[i] == current) {
                    currentIndex = i;
                    break;
                }
            }
            const int direction = key->modifiers() & Qt::ShiftModifier ? -1 : 1;
            if (currentIndex < 0) {
                currentIndex = direction > 0 ? -1 : 0;
            }
            for (int step = 1; step <= static_cast<int>(focusOrder.size()); ++step) {
                const int candidate = (currentIndex + direction * step +
                                       static_cast<int>(focusOrder.size())) %
                                      static_cast<int>(focusOrder.size());
                QWidget* target = focusOrder[candidate];
                if (target && target->isVisible() && target->isEnabled()) {
                    target->setFocus(direction > 0 ? Qt::TabFocusReason
                                                   : Qt::BacktabFocusReason);
                    event->accept();
                    return true;
                }
            }
        }
        const bool plainKey = key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier;
        if (plainKey && !HasEditableTextFocus() &&
            key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9) {
            ActivatePresetRow(key->key() - Qt::Key_1);
            event->accept();
            return true;
        }
    }

    if ((watched == this || watched == centralWidget() || watched == contentSplitter_) &&
                           (type == QEvent::Move ||
                            type == QEvent::Resize ||
                            type == QEvent::WindowStateChange)) {
        QTimer::singleShot(0, this, &MainWindow::UpdateSimpleChromeGeometry);
    }

    if (watched == renderWidget_) {
        switch (type) {
        case QEvent::Resize:
            UpdateSimpleChromeGeometry();
            break;
        case QEvent::Wheel: {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                if (app_) {
                    app_->HandleZoomWheel(wheel);
                }
                event->accept();
                return true;
            } else if (app_ && app_->HandlePanScroll(wheel)) {
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (modeGridPopup_ && modeGridPopup_->isVisible()) {
                modeGridPopup_->hide();
                RevealSimpleChrome();
            }
            if (mouse->button() == Qt::MiddleButton) {
                if (app_) {
                    app_->BeginMousePan(mouse->position(), renderWidget_->size());
                }
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (app_ && app_->UpdateMousePan(mouse->position())) {
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                if (app_) {
                    app_->EndMousePan();
                }
                event->accept();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEventFilter(const QByteArray&, void* message, qintptr*)
{
    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (!nativeMessage || !isSimpleMode()) {
        return false;
    }

    switch (nativeMessage->message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_SETFOCUS:
        RevealSimpleChrome();
        break;
    default:
        break;
    }
    return false;
}

} // namespace openzoom

#endif // _WIN32
