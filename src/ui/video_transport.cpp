#ifdef _WIN32

#include "okuflow/ui/video_transport.hpp"

#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/theme_contrast.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

namespace okuflow {
namespace {

// One compact transport line: icon buttons at a 44-DIP minimum touch target
// flank the scrubber. Play and the icon-plus-"10" rewind get a little more
// width because they carry the primary action and a two-digit badge.
constexpr int kButtonExtent = 44;
constexpr int kPlayWidth = 52;
constexpr int kRewindWidth = 60;
constexpr int kIconExtent = 24;
constexpr int kMinTimelineWidth = 96;
constexpr int kRowSpacing = 4;

int SliderTime(qint64 milliseconds)
{
    return static_cast<int>(std::clamp<qint64>(milliseconds, 0,
                                             std::numeric_limits<int>::max()));
}

// Live is a viewing mode, not a claim that its pending camera pixels are
// already seekable. Draw its thumb/fill at the right edge without changing
// the inspected numeric value, verified range or true timestamp text. An
// expired hold similarly pins left, with a distinct warning-colored face.
// Qt uses this same style option for native handle hit testing and painting.
class TimelineSlider final : public QSlider {
public:
    enum class Edge { Position, Live, ExpiredHold };
    using QSlider::QSlider;
    void SetEdge(Edge edge)
    {
        if (edge_ == edge) return;
        edge_ = edge;
        update();
    }
protected:
    void initStyleOption(QStyleOptionSlider* option) const override
    {
        QSlider::initStyleOption(option);
        if (edge_ != Edge::Position && !isSliderDown()) {
            const int position = edge_ == Edge::Live ? option->maximum : option->minimum;
            option->sliderPosition = position;
            option->sliderValue = position;
        }
    }
private:
    Edge edge_{Edge::Position};
};

class SourceLabel final : public QLabel {
public:
    using QLabel::QLabel;
    void SetSource(const QString& source)
    {
        source_ = source;
        setAccessibleName(source);
        setToolTip(source);
        Refit();
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        Refit();
    }
private:
    void Refit() { setText(fontMetrics().elidedText(source_, Qt::ElideMiddle, width())); }
    QString source_;
};

void NameButton(QPushButton* button, const QString& source)
{
    button->setToolTip(source);
    button->setAccessibleName(source);
    RetranslateWidget(button);
    // The icon/glyph is deliberately separate from the spoken action.
    button->setProperty("okuflow.transportAction", source);
}

// The bundled Lucide set has a speaker icon but no muted variant, so the
// muted face is the same speaker with a diagonal slash painted over it.
QIcon SlashedIcon(const QIcon& base, const QColor& ink)
{
    QIcon result;
    for (int extent : {24, 32, 48}) {
        QPixmap pixmap = base.pixmap(QSize(extent, extent));
        if (pixmap.isNull()) continue;
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(ink, std::max(2, pixmap.width() / 12));
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const int insetX = pixmap.width() / 8;
        const int insetY = pixmap.height() / 8;
        painter.drawLine(insetX, pixmap.height() - insetY,
                         pixmap.width() - insetX, insetY);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

// Numeric UI Automation values remain milliseconds, while the Value text is
// an hour-aware time. Standard QWidget accessibility supplies focus/actions;
// the public Qt value interface retains the native slider contract.
class TimelineAccessibility final : public QAccessibleWidget,
                                    public QAccessibleValueInterface {
public:
    explicit TimelineAccessibility(QSlider* slider)
        : QAccessibleWidget(slider, QAccessible::Slider), slider_(slider) {}
    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::ValueInterface) return slider_->property("okuflowOutsideWindowPosition").isValid()
            ? nullptr:static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QVariant currentValue() const override { return slider_->value(); }
    QVariant maximumValue() const override { return slider_->maximum(); }
    QVariant minimumValue() const override { return slider_->minimum(); }
    QVariant minimumStepSize() const override { return slider_->singleStep(); }
    void setCurrentValue(const QVariant& value) override
    {
        bool valid = false;
        const qint64 target = value.toLongLong(&valid);
        if (valid && slider_->isEnabled() && !slider_->isSliderDown()) {
            slider_->setValue(static_cast<int>(std::clamp<qint64>(target, slider_->minimum(), slider_->maximum())));
        }
    }
    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Value) return slider_->accessibleDescription();
        return QAccessibleWidget::text(type);
    }
    QStringList actionNames() const override
    {
        auto names = QAccessibleWidget::actionNames();
        if (slider_->isEnabled()) names << increaseAction() << decreaseAction();
        return names;
    }
    void doAction(const QString& action) override
    {
        if (slider_->isEnabled() && action == increaseAction()) {
            slider_->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        } else if (slider_->isEnabled() && action == decreaseAction()) {
            slider_->triggerAction(QAbstractSlider::SliderSingleStepSub);
        } else {
            QAccessibleWidget::doAction(action);
        }
    }
private:
    QSlider* slider_{};
};

} // namespace

VideoTransport::VideoTransport(QWidget* parent) : QWidget(parent)
{
    static const bool factoryInstalled = []() {
        QAccessible::installFactory([](const QString&, QObject* object) -> QAccessibleInterface* {
            auto* slider = qobject_cast<QSlider*>(object);
            if (slider && slider->objectName() == QStringLiteral("videoTimeline")) {
                return new TimelineAccessibility(slider);
            }
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(factoryInstalled);
    setObjectName(QStringLiteral("videoTransport"));
    setFocusPolicy(Qt::StrongFocus); // Safe focus parking if no viewport is supplied.
    // A transport follows its viewport width. Propagating the wide row's
    // minimum to the parent prevents the very resize that should wrap it.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(10, 4, 10, 6);
    layout->setSpacing(2);
    // Understated metadata line: elided filename plus state badge. Neither
    // grows vertically; the full name stays in the tooltip/accessible name.
    auto* sourceRow = new QHBoxLayout();
    metadataLayout_ = sourceRow;
    sourceRow->setSpacing(8);
    sourceLabel_ = new SourceLabel(this);
    sourceLabel_->setObjectName(QStringLiteral("videoSourceLabel"));
    sourceLabel_->setTextFormat(Qt::PlainText);
    sourceLabel_->setMinimumWidth(0);
    sourceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    stateLabel_ = new QLabel(this);
    stateLabel_->setObjectName(QStringLiteral("videoStateLabel"));
    stateLabel_->setTextFormat(Qt::PlainText);
    stateLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sourceRow->addWidget(sourceLabel_, 1);
    sourceRow->addWidget(stateLabel_);
    layout->addLayout(sourceRow);

    const auto makeButton = [this](const char* name, const QString& glyph, int width) {
        auto* button = new QPushButton(glyph, this);
        button->setObjectName(QString::fromLatin1(name));
        button->setFixedSize(width, kButtonExtent);
        button->setIconSize(QSize(kIconExtent, kIconExtent));
        button->setAutoDefault(false);
        button->installEventFilter(this);
        return button;
    };
    rewindButton_ = makeButton("videoRewindButton", QStringLiteral("\u21b6 10"), kRewindWidth);
    playPauseButton_ = makeButton("videoPlayPauseButton", QStringLiteral("\u25b6"), kPlayWidth);
    jumpButton_ = makeButton("videoJumpButton", QStringLiteral(">>"), kButtonExtent);
    returnButton_ = makeButton("videoReturnCameraButton", QStringLiteral("\u21a9"), kButtonExtent);
    saveFrameButton_ = makeButton("videoSaveFrameButton", QStringLiteral("\u25a3"), kButtonExtent);
    muteButton_ = makeButton("videoMuteButton", QStringLiteral("\u266a"), kButtonExtent);
    collapseButton_ = makeButton("videoCollapseButton", QString(), kButtonExtent);
    collapseButton_->setCheckable(true);
    // A small chevron has the same native keyboard/touch target as actions.
    collapseButton_->setIconSize(QSize(14, 14));
    sourceRow->insertWidget(0, collapseButton_);

    timeline_ = new TimelineSlider(Qt::Horizontal, this);
    timeline_->setObjectName(QStringLiteral("videoTimeline"));
    timeline_->setMinimumSize(kMinTimelineWidth, kButtonExtent);
    timeline_->setSingleStep(1000);
    timeline_->setPageStep(10000);
    timeline_->setTracking(false);
    timeline_->installEventFilter(this);
    // A time axis keeps its direction in RTL interfaces too.
    timeline_->setLayoutDirection(Qt::LeftToRight);
    timeline_->setInvertedControls(false);
    timeLabel_ = new QLabel(this);
    timeLabel_->setObjectName(QStringLiteral("videoTimeLabel"));
    timeLabel_->setTextFormat(Qt::PlainText);
    timeLabel_->setAlignment(Qt::AlignCenter);
    timeLabel_->setLayoutDirection(Qt::LeftToRight);
    page_ = new QSpinBox(this);
    page_->setObjectName(QStringLiteral("documentPage"));
    page_->setMinimumHeight(kButtonExtent);
    page_->setKeyboardTracking(false);
    page_->hide();
    connect(page_, &QSpinBox::valueChanged, this, [this](int page) {
        if (updating_) return;
        if (IsActionReady(page_)) emit PageRequested(page - 1);
        else { const QSignalBlocker block(page_); page_->setValue(snapshot_.page + 1); }
    });

    // The one transport line: primary actions, scrubber, readout, then the
    // secondary actions. UpdateLayout owns widget placement from here on;
    // at very narrow widths the secondaries move to their own compact row
    // below so play and the scrubber keep usable space on the main line.
    mainLayout_ = new QHBoxLayout();
    mainLayout_->setSpacing(kRowSpacing);
    layout->addLayout(mainLayout_);
    secondaryRow_ = new QWidget(this);
    secondaryRow_->setObjectName(QStringLiteral("videoSecondaryRow"));
    secondaryLayout_ = new QHBoxLayout(secondaryRow_);
    secondaryLayout_->setContentsMargins(0, 0, 0, 0);
    secondaryLayout_->setSpacing(kRowSpacing);
    layout->addWidget(secondaryRow_);
    volumePopup_=new QFrame(this,Qt::Popup | Qt::FramelessWindowHint);
    volumePopup_->setObjectName(QStringLiteral("videoVolumePopup"));
    volumePopup_->setAutoFillBackground(true);
    volumePopup_->setAttribute(Qt::WA_TranslucentBackground,false);
    volumePopup_->setFrameShape(QFrame::StyledPanel);
    volumePopup_->setLineWidth(2);
    volumePopup_->setStyleSheet(QStringLiteral(
        "QFrame#videoVolumePopup { border:2px solid palette(mid); background:palette(window); }"
        "QSlider#videoVolumeSlider:focus { border:2px solid palette(highlight); }"
        "QSlider#videoVolumeSlider::groove:vertical { width:6px; background:palette(mid); border-radius:3px; }"
        "QSlider#videoVolumeSlider::add-page:vertical { background:palette(highlight); border-radius:3px; }"
        "QSlider#videoVolumeSlider::handle:vertical { height:44px; margin:0 -19px; "
        "border:2px solid palette(highlight); border-radius:6px; background:palette(button); }"));
    volumePopup_->installEventFilter(this);
    auto* audioLayout=new QVBoxLayout(volumePopup_);
    audioLayout->setContentsMargins(12,12,12,12);audioLayout->setSpacing(8);
    volumeLabel_=new QLabel(volumePopup_);volumeLabel_->setWordWrap(true);
    volumeLabel_->setAlignment(Qt::AlignCenter);
    volumeLabel_->hide(); // The speaker and percentage identify this compact player popover.
    volume_=new QSlider(Qt::Vertical,volumePopup_);
    volume_->setObjectName(QStringLiteral("videoVolumeSlider"));
    volume_->setRange(0,100);volume_->setValue(100);volume_->setSingleStep(5);volume_->setPageStep(10);
    volume_->setMinimumSize(kButtonExtent,160);volume_->installEventFilter(this);
    volumeLabel_->setBuddy(volume_);
    volumeValue_=new QLabel(QStringLiteral("100%"),volumePopup_);
    volumeValue_->setObjectName(QStringLiteral("videoVolumeValue"));
    volumeValue_->setAlignment(Qt::AlignCenter);
    audioLayout->addWidget(volumeLabel_);audioLayout->addWidget(volume_,1,Qt::AlignHCenter);
    audioLayout->addWidget(volumeValue_);
    qApp->installEventFilter(this);

    announcementTimer_ = new QTimer(this);
    announcementTimer_->setSingleShot(true);
    announcementTimer_->setInterval(200);
    connect(announcementTimer_, &QTimer::timeout, this, &VideoTransport::AnnounceSettledState);
    ApplyStyle();

    connect(rewindButton_, &QPushButton::clicked, this, [this]() {
        if (!IsActionReady(rewindButton_)) return;
        if (snapshot_.source == Source::Document) emit PageRequested(snapshot_.page - 1);
        else CommitSeek(std::max<qint64>(0, SeekBaseline() - 10000));
    });
    connect(playPauseButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(playPauseButton_)) emit PlayPauseRequested();
    });
    connect(jumpButton_, &QPushButton::clicked, this, [this]() {
        if (!IsActionReady(jumpButton_)) return;
        if (snapshot_.source == Source::Document) emit PageRequested(snapshot_.page + 1);
        else emit JumpToEndRequested();
    });
    connect(returnButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(returnButton_)) emit ReturnToCameraRequested();
    });
    connect(saveFrameButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(saveFrameButton_)) emit SaveFrameRequested();
    });
    connect(muteButton_, &QPushButton::clicked, this, [this]() {
        if (!IsActionReady(muteButton_)) return;
        if(volumePopup_->isVisible()) emit MutedChanged(!snapshot_.muted);
        else ShowVolumePopup();
    });
    connect(volume_,&QSlider::valueChanged,this,[this](int percent) {
        if(updating_)return;
        if(!IsActionReady(volume_)) {
            const QSignalBlocker block(volume_);volume_->setValue(snapshot_.muted?0:snapshot_.volumePercent);
        } else emit VolumeChanged(percent);
        RefreshVolumeText();
    });
    connect(collapseButton_, &QPushButton::clicked, this, [this]() {
        SetCollapsed(!collapsed_);
    });
    connect(timeline_, &QSlider::sliderPressed, this, [this]() {
        dragging_ = true;
        dragGeneration_ = snapshot_.sourceGeneration;
        RefreshTimeline(false);
    });
    connect(timeline_, &QSlider::sliderMoved, this, [this](int position) {
        timeLabel_->setText(QStringLiteral("%1 / %2").arg(TimeText(position),
                                                       TimeText(timeline_->maximum())));
    });
    connect(timeline_, &QSlider::sliderReleased, this, [this]() {
        if (!dragging_) return;
        const qint64 target = timeline_->sliderPosition();
        const bool sameDocument = dragGeneration_ == snapshot_.sourceGeneration;
        // Qt emits its non-tracking value change after sliderReleased. Set
        // that value silently first so releasing a drag issues one command.
        {
            const QSignalBlocker blocker(timeline_);
            timeline_->setValue(static_cast<int>(target));
        }
        dragging_ = false;
        if (sameDocument) CommitSeek(target, target == timeline_->maximum());
        RefreshTimeline(true);
    });
    connect(timeline_, &QSlider::valueChanged, this, [this](int position) {
        if (!updating_ && !dragging_) {
            timeline_->setAccessibleDescription(TranslateUi(QStringLiteral("%1 of %2"))
                .arg(TimeText(position), TimeText(timeline_->maximum())));
            CommitSeek(position, position == timeline_->maximum());
        }
    });
    connect(timeline_, &QSlider::actionTriggered, this, [this](int action) {
        // Native accessibility increase/decrease uses the same current-media
        // baseline as keys, even while the thumb is held for inspection.
        qint64 delta = 0;
        if (action == QAbstractSlider::SliderSingleStepAdd) delta = 1000;
        if (action == QAbstractSlider::SliderSingleStepSub) delta = -1000;
        if (action == QAbstractSlider::SliderPageStepAdd) delta = 10000;
        if (action == QAbstractSlider::SliderPageStepSub) delta = -10000;
        if (delta && !dragging_ && IsActionReady(timeline_)) {
            timeline_->setSliderPosition(SliderTime(std::clamp<qint64>(
                SeekBaseline() + delta, 0, snapshot_.durationMs)));
        }
    });
    const auto targets = FocusTargets();
    for (std::size_t index = 1; index < targets.size(); ++index) {
        QWidget::setTabOrder(targets[index - 1], targets[index]);
    }
    timelineUpdateClock_.start();
    RefreshText();
    UpdateActions();
    UpdateLayout();
    hide();
}

VideoTransport::~VideoTransport()
{
    if(qApp)qApp->removeEventFilter(this);
    volumePopup_->removeEventFilter(this);
    volumePopup_->hide();
}

void VideoTransport::ShowVolumePopup()
{
    if(!IsActionReady(muteButton_))return;
    volumePopup_->setPalette(palette());
    volumePopup_->setFont(font());
    const auto available=muteButton_->screen()->availableGeometry();
    const int widthLimit=std::max(44,available.width()-16);
    volumePopup_->setMaximumWidth(widthLimit);
    volumePopup_->setMinimumWidth(std::min(widthLimit,std::max(76,
        fontMetrics().horizontalAdvance(QLocale().toString(100)+QLatin1Char('%'))+28)));
    volumePopup_->adjustSize();
    const auto top=muteButton_->mapToGlobal(QPoint(muteButton_->width()/2,0));
    const int x=std::clamp(top.x()-volumePopup_->width()/2,available.left(),
        std::max(available.left(),available.right()-volumePopup_->width()+1));
    int y=top.y()-volumePopup_->height()-6;
    if(y<available.top())y=top.y()+muteButton_->height()+6;
    y=std::clamp(y,available.top(),std::max(available.top(),available.bottom()-volumePopup_->height()+1));
    volumePopup_->move(x,y);
    volumePopup_->show();volumePopup_->raise();
    RefreshText();
    volume_->setFocus(Qt::PopupFocusReason);
}

void VideoTransport::HideVolumePopup(bool restoreFocus,QWidget* preferred)
{
    if(!volumePopup_->isVisible())return;
    const auto* focus=QApplication::focusWidget();
    const bool ownsFocus=focus && (focus==volumePopup_ || volumePopup_->isAncestorOf(focus));
    QPointer<QWidget> target;
    if(restoreFocus && ownsFocus) {
        if(preferred && preferred->isVisible() && preferred->isEnabled())target=preferred;
        else if(IsActionReady(muteButton_))target=muteButton_;
        else if(IsActionReady(playPauseButton_))target=playPauseButton_;
        else if(focusReturnTarget_ && focusReturnTarget_->isVisible() && focusReturnTarget_->isEnabled())target=focusReturnTarget_;
        else if(parentWidget())target=parentWidget();
    }
    volumePopup_->hide();
    if(target)target->setFocus(Qt::PopupFocusReason);
}

QString VideoTransport::TimeText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds) / 1000;
    const QLocale locale;
    const auto digits = [&locale](qint64 value, int width) {
        QString result = locale.toString(value);
        for (int count = static_cast<int>(QString::number(value).size()); count < width; ++count) {
            result.prepend(locale.zeroDigit());
        }
        return result;
    };
    // Disable grouping for hours: a long recording is still a time value.
    QLocale ungrouped = locale;
    ungrouped.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    if (seconds >= 3600) {
        return QStringLiteral("%1:%2:%3").arg(ungrouped.toString(seconds / 3600),
            digits((seconds / 60) % 60, 2), digits(seconds % 60, 2));
    }
    return QStringLiteral("%1:%2").arg(digits(seconds / 60, 2), digits(seconds % 60, 2));
}

QString VideoTransport::StateText() const
{
    if (snapshot_.source == Source::Document) {
        if (snapshot_.state == State::Opening || snapshot_.state == State::Seeking) return QStringLiteral("Opening file…");
        if (snapshot_.state == State::Unavailable) return QStringLiteral("File unavailable");
        return snapshot_.pageCount > 1 ? TranslateUi(QStringLiteral("Page %1 of %2"))
            .arg(QLocale().toString(snapshot_.page + 1), QLocale().toString(snapshot_.pageCount)) : QString();
    }
    switch (snapshot_.state) {
    case State::Live: return QStringLiteral("Live");
    case State::BehindLive: return QStringLiteral("Behind live");
    case State::Paused: return QStringLiteral("Paused");
    case State::Video: return QStringLiteral("Video");
    case State::End: return QStringLiteral("End of video");
    case State::Opening: return QStringLiteral("Opening video\u2026");
    case State::Seeking: return QStringLiteral("Seeking\u2026");
    case State::Waiting: return QStringLiteral("Waiting for recording");
    case State::Finishing: return QStringLiteral("Finishing recording");
    case State::Unavailable: return QStringLiteral("Video unavailable");
    case State::Buffering: return QStringLiteral("Buffering live video…");
    }
    return {};
}

void VideoTransport::SetSnapshot(const Snapshot& snapshot)
{
    const bool hadSource = snapshot_.source != Source::None;
    const bool changedDocument = snapshot_.sourceGeneration != snapshot.sourceGeneration ||
                                 snapshot_.source != snapshot.source;
    if(changedDocument)HideVolumePopup();
    const bool muteEdge=!changedDocument && snapshot_.muted!=snapshot.muted && volumePopup_->isVisible();
    const bool changedState = changedDocument || snapshot_.state != snapshot.state ||
        (snapshot.source == Source::Recap && snapshot.state == State::Unavailable &&
         snapshot_.diagnosticDetail != snapshot.diagnosticDetail) ||
        snapshot_.sourceName != snapshot.sourceName ||
        (!snapshot.playing && snapshot.state != State::Seeking &&
         snapshot_.positionMs != snapshot.positionMs);
    const bool changedText = changedState || snapshot_.playing != snapshot.playing ||
        snapshot_.page != snapshot.page || snapshot_.pageCount != snapshot.pageCount ||
        snapshot_.muted != snapshot.muted || snapshot_.hasAudio != snapshot.hasAudio ||
        snapshot_.volumePercent != std::clamp(snapshot.volumePercent,0,100) ||
        snapshot_.diagnosticDetail != snapshot.diagnosticDetail;
    if (changedDocument && dragging_) {
        dragging_ = false;
        const QSignalBlocker blocker(timeline_);
        timeline_->setSliderDown(false);
    }
    if (changedDocument || (pendingSeekMs_ &&
        (snapshot.positionMs == *pendingSeekMs_ ||
         (snapshot_.state == State::Seeking && snapshot.state != State::Seeking) ||
         snapshot.state == State::Unavailable || snapshot.state == State::End))) {
        pendingSeekMs_.reset();
    }
    if (changedDocument) {
        lastAnnouncement_.clear();
        wheelRemainder_ = 0;
    }
    snapshot_ = snapshot;
    snapshot_.volumePercent = std::clamp(snapshot_.volumePercent,0,100);
    snapshot_.durationMs = std::max<qint64>(0, snapshot_.durationMs);
    snapshot_.firstMs = std::clamp<qint64>(snapshot_.firstMs,0,snapshot_.durationMs);
    snapshot_.positionMs = std::max<qint64>(0, snapshot_.positionMs);
    const bool outside=snapshot_.source==Source::Recap &&
        (snapshot_.positionMs<snapshot_.firstMs || snapshot_.positionMs>snapshot_.durationMs);
    const bool wasOutside = timeline_->property("okuflowOutsideWindowPosition").isValid();
    timeline_->setProperty("okuflowOutsideWindowPosition",outside?QVariant(snapshot_.positionMs):QVariant());
    if (changedDocument || outside != wasOutside) {
        QAccessibleEvent event(timeline_,QAccessible::ObjectAttributeChanged);QAccessible::updateAccessibility(&event);
    }
    const bool hasSource = snapshot_.source != Source::None;
    if (!hasSource) ReturnFocus();
    collapseButton_->setVisible(hasSource && !uiHidden_);
    setVisible(hasSource && !uiHidden_ && !collapsed_);
    UpdateActions();
    if (changedText) RefreshText();
    const int effectiveVolume=snapshot_.muted?0:snapshot_.volumePercent;
    if(!volume_->isSliderDown() && volume_->value()!=effectiveVolume) {
        const QSignalBlocker block(volume_);volume_->setValue(effectiveVolume);
        RefreshVolumeText(); // Identical snapshots also acknowledge a refused edit.
    }
    if(muteEdge && volumePopup_->isVisible()) {
        const auto action=TranslateUi(snapshot_.muted?QStringLiteral("Unmute playback"):QStringLiteral("Mute playback"));
        const auto percent=QLocale().toString(snapshot_.muted?0:snapshot_.volumePercent)+QLatin1Char('%');
        QAccessibleAnnouncementEvent event(this,TranslateUi(QStringLiteral("Playback volume"))+
            QStringLiteral(": ")+percent+QStringLiteral(". ")+action);
        event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);QAccessible::updateAccessibility(&event);
    }
    RefreshTimeline(changedDocument || !snapshot_.playing);
    if (changedState) RequestAnnouncement();
    if (hadSource != hasSource) emit ControlsChanged();
}

void VideoTransport::SetUiHidden(bool hidden)
{
    if(hidden)HideVolumePopup(true,focusReturnTarget_);
    if (hidden) ReturnFocus();
    uiHidden_ = hidden;
    collapseButton_->setVisible(!hidden && snapshot_.source != Source::None);
    setVisible(!hidden && snapshot_.source != Source::None && !collapsed_);
    RequestAnnouncement();
}

void VideoTransport::SetCollapsed(bool collapsed)
{
    if(collapsed)HideVolumePopup(true,collapseButton_->parentWidget()!=this?collapseButton_:focusReturnTarget_.data());
    if (collapsed_ == collapsed) return;
    if (dragging_) {
        dragging_ = false;
        const QSignalBlocker blocker(timeline_);
        timeline_->setSliderDown(false);
    }
    if (collapsed) {
        auto* focus = QApplication::focusWidget();
        if (focus && isAncestorOf(focus)) {
            if (collapseButton_->parentWidget() != this && collapseButton_->isVisible())
                collapseButton_->setFocus(Qt::OtherFocusReason);
            else ReturnFocus();
        }
    }
    collapsed_ = collapsed;
    collapseButton_->setChecked(collapsed);
    setVisible(!collapsed_ && !uiHidden_ && snapshot_.source != Source::None);
    RefreshText();
    RefreshTimeline(true);
    UpdateActions();
    updateGeometry();
    emit ControlsChanged();
}

void VideoTransport::SetFocusReturnTarget(QWidget* target)
{
    focusReturnTarget_ = target;
}

bool VideoTransport::ActionShown(const QWidget* control) const
{
    if (collapsed_ && control != collapseButton_) return false;
    if (control == volume_) return muteShown_ && volumePopup_->isVisible();
    if (control == muteButton_) return muteShown_;
    if (control == returnButton_) return returnShown_;
    return true;
}

bool VideoTransport::IsActionReady(const QWidget* control) const
{
    return ActionShown(control) && IsCommandReady(control);
}

bool VideoTransport::IsCommandReady(const QWidget* control) const
{
    return control && snapshot_.source != Source::None && !uiHidden_ &&
        ((control != muteButton_ && control != volume_) || muteShown_) && (control != returnButton_ || returnShown_) &&
        control->isEnabled() &&
        control->property("okuflowTransportGate").toInt() == static_cast<int>(Gate::Ready);
}

void VideoTransport::ApplyGate(QWidget* control, Gate gate)
{
    const bool changed = control->property("okuflowTransportGate").toInt() != static_cast<int>(gate);
    control->setProperty("okuflowTransportGate", static_cast<int>(gate));
    // Qt transfers focus immediately when setEnabled(false) is called. Keep
    // the semantic focus owner inert until the user leaves it or it is ready.
    control->setEnabled(gate == Gate::Ready || control->hasFocus());
    if (control != timeline_ && control != volume_ && control != collapseButton_) {
        SetLiveAccessibleDescription(control, gate == Gate::Ready ? QString() : StateText());
    }
    if (changed) {
        control->style()->unpolish(control);
        control->style()->polish(control);
        control->update();
        // Icon ink follows the gate: an inert-but-focused button stays
        // enabled, so its face must dim without a disabled-state switch.
        if (auto* button = qobject_cast<QPushButton*>(control)) ApplyFace(button);
    }
}

bool VideoTransport::SetActionShown(QPushButton* button, bool shown)
{
    bool& previous = button == muteButton_ ? muteShown_ : returnShown_;
    if (previous == shown) return false;
    previous = shown;
    if (!shown) MoveFocusFrom(button, playPauseButton_);
    button->setVisible(shown);
    return true;
}

void VideoTransport::UpdateActions()
{
    const bool document = snapshot_.source == Source::Document;
    const bool hasSource = snapshot_.source != Source::None;
    const bool pending = snapshot_.state == State::Opening || snapshot_.state == State::Seeking;
    const bool available = hasSource && (snapshot_.state != State::Unavailable || snapshot_.canPauseLive || snapshot_.canSaveFrame);
    const auto gate = [available](bool ready, bool busy = false) {
        return available && ready ? Gate::Ready : busy && available ? Gate::Busy : Gate::Unavailable;
    };
    ApplyGate(collapseButton_, hasSource ? Gate::Ready : Gate::Unavailable);
    ApplyGate(rewindButton_, gate(document ? !pending && snapshot_.page > 0 : snapshot_.seekable && SeekBaseline() > snapshot_.firstMs));
    ApplyGate(page_, gate(document && !pending && snapshot_.pageCount > 1, pending));
    ApplyGate(playPauseButton_, gate(!document && !pending && snapshot_.state != State::End &&
        (snapshot_.canPauseLive || snapshot_.canSaveFrame || snapshot_.seekable), pending));
    ApplyGate(jumpButton_, snapshot_.source==Source::Recap && snapshot_.canReturnToCamera ? Gate::Ready :
        gate(!pending && (document ? snapshot_.page + 1 < snapshot_.pageCount : snapshot_.source == Source::Recording || snapshot_.seekable), pending));
    ApplyGate(timeline_, gate(snapshot_.seekable && snapshot_.durationMs > 0));
    ApplyGate(returnButton_, hasSource && snapshot_.canReturnToCamera ? Gate::Ready : Gate::Unavailable);
    ApplyGate(saveFrameButton_, gate(snapshot_.canSaveFrame && !pending, pending));
    ApplyGate(muteButton_, gate(snapshot_.source == Source::Video && snapshot_.hasAudio));
    ApplyGate(volume_,gate(snapshot_.source==Source::Video && snapshot_.hasAudio));
    const bool changedReturn = SetActionShown(returnButton_, snapshot_.source == Source::Video || document);
    const bool changedMute = SetActionShown(muteButton_, snapshot_.source == Source::Video && snapshot_.hasAudio);
    if(changedMute && !muteShown_)MoveFocusFrom(volume_,playPauseButton_);
    if(!muteShown_ || !available)HideVolumePopup();
    if (changedReturn || changedMute) UpdateLayout();
}

void VideoTransport::MoveFocusFrom(QWidget* leaving, QWidget* preferred)
{
    if (!leaving->hasFocus()) return;
    if (IsActionReady(preferred)) {
        preferred->setFocus(Qt::OtherFocusReason);
        return;
    }
    for (auto* target : FocusTargets()) {
        if (target != leaving && target != returnButton_ && IsActionReady(target)) {
            target->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
    if (focusReturnTarget_ && focusReturnTarget_->isVisible() && focusReturnTarget_->isEnabled()) {
        focusReturnTarget_->setFocus(Qt::OtherFocusReason);
    } else {
        setFocus(Qt::OtherFocusReason);
    }
}

void VideoTransport::ReturnFocus()
{
    QWidget* focus = QApplication::focusWidget();
    if (focus != this && focus != collapseButton_ && !(focus && isAncestorOf(focus))) return;
    if (focusReturnTarget_ && focusReturnTarget_->isVisible() && focusReturnTarget_->isEnabled()) {
        focusReturnTarget_->setFocus(Qt::OtherFocusReason);
    } else if (parentWidget()) {
        parentWidget()->setFocus(Qt::OtherFocusReason);
    } else {
        focus->clearFocus();
    }
}

void VideoTransport::RefreshText()
{
    const QString source = snapshot_.sourceName.isEmpty()
        ? TranslateUi(snapshot_.source == Source::Recording
                          ? QStringLiteral("Current recording") : QStringLiteral("Video"))
        : snapshot_.sourceName;
    static_cast<SourceLabel*>(sourceLabel_)->SetSource(source);
    SetLiveText(stateLabel_, StateText(), LivePoliteness::kSilent,
                QStringLiteral("Video playback"));
    stateLabel_->setToolTip(snapshot_.diagnosticDetail);
    stateLabel_->setAccessibleDescription(snapshot_.diagnosticDetail);
    NameButton(rewindButton_, snapshot_.source == Source::Document ? QStringLiteral("Previous page") : QStringLiteral("Rewind 10 seconds"));
    NameButton(playPauseButton_, snapshot_.playing ? QStringLiteral("Pause video")
                                                 : QStringLiteral("Resume video"));
    NameButton(jumpButton_, snapshot_.source == Source::Document ? QStringLiteral("Next page") : snapshot_.source == Source::Recording || snapshot_.source == Source::Recap
                               ? QStringLiteral("Jump to live") : QStringLiteral("Jump to end"));
    NameButton(returnButton_, QStringLiteral("Return to camera"));
    NameButton(saveFrameButton_, QStringLiteral("Save frame"));
    NameButton(muteButton_, volumePopup_->isVisible()
        ? (snapshot_.muted?QStringLiteral("Unmute playback"):QStringLiteral("Mute playback"))
        : QStringLiteral("Playback volume"));
    volumePopup_->setWindowTitle(TranslateUi(QStringLiteral("Playback volume")));
    volumePopup_->setAccessibleName(TranslateUi(QStringLiteral("Playback volume")));
    volumeLabel_->setText(TranslateUi(QStringLiteral("Playback volume")));
    volume_->setAccessibleName(TranslateUi(QStringLiteral("Playback volume")));
    {
        const QSignalBlocker block(volume_);
        if(!volume_->isSliderDown())volume_->setValue(snapshot_.muted?0:std::clamp(snapshot_.volumePercent,0,100));
    }
    RefreshVolumeText();
    NameButton(collapseButton_, collapsed_ ? QStringLiteral("Show UI") : QStringLiteral("Hide UI"));
    collapseButton_->setAccessibleDescription(TranslateUi(QStringLiteral("Video controls")));
    setAccessibleName(TranslateUi(QStringLiteral("Video controls")));
    timeline_->setAccessibleName(TranslateUi(QStringLiteral("Video timeline")));
    page_->setAccessibleName(TranslateUi(QStringLiteral("Page")));
    page_->setToolTip(TranslateUi(QStringLiteral("Page")));
    {
        const QSignalBlocker blocker(page_);
        page_->setRange(1, std::max(1, snapshot_.pageCount));
        page_->setValue(snapshot_.page + 1);
        page_->setSuffix(QStringLiteral(" / ") + QLocale().toString(std::max(1, snapshot_.pageCount)));
    }
    timeline_->setToolTip(TranslateUi(QStringLiteral(
        "Left and Right: one second. Page Up and Page Down: ten seconds. Home: start. End: recorded end.")));
    ApplyButtonFaces();
    // The rewind badge is text beside an icon; keep it unclipped when the
    // application font grows instead of pinning every button to one width.
    const int badgeRoom = rewindButton_->icon().isNull() ? 16 : kIconExtent + 18;
    const int rewindWidth = std::max(kRewindWidth, badgeRoom +
        rewindButton_->fontMetrics().horizontalAdvance(rewindButton_->text()));
    if (rewindButton_->minimumWidth() != rewindWidth) {
        rewindButton_->setFixedWidth(rewindWidth);
    }
    UpdateLayout();
    ApplyStyle();
}

void VideoTransport::RefreshVolumeText()
{
    const auto value=QLocale().toString(volume_->value())+QLatin1Char('%');
    volumeValue_->setText(value);
    volume_->setAccessibleDescription(value);
    volume_->setToolTip(TranslateUi(QStringLiteral("Playback volume"))+QStringLiteral(": ")+value);
}

void VideoTransport::RequestAnnouncement()
{
    announcementTimer_->stop();
    announcementPending_ = snapshot_.source != Source::None && !uiHidden_;
    // Opening/Seeking are visible immediately but never announced as a
    // sequence of stale intermediate selections. The settled snapshot owns PTS.
    if (announcementPending_ && snapshot_.state != State::Opening && snapshot_.state != State::Seeking) {
        announcementTimer_->start();
    }
}

void VideoTransport::AnnounceSettledState()
{
    if (!announcementPending_ || !isVisible() || snapshot_.source == Source::None ||
        snapshot_.state == State::Opening || snapshot_.state == State::Seeking) return;
    announcementPending_ = false;
    QString message = QStringLiteral("%1: %2, %3")
        .arg(sourceLabel_->accessibleName(), TranslateUi(StateText()), TimeText(snapshot_.positionMs));
    if (snapshot_.source == Source::Document) message = sourceLabel_->accessibleName() + QStringLiteral(": ") + TranslateUi(StateText());
    if (snapshot_.source == Source::Recap && snapshot_.state == State::Unavailable &&
        !snapshot_.diagnosticDetail.isEmpty()) message += QStringLiteral(". ") + snapshot_.diagnosticDetail;
    if (message == lastAnnouncement_) return;
    lastAnnouncement_ = message;
    QAccessibleAnnouncementEvent announcement(this, message);
    announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&announcement);
}

void VideoTransport::RefreshTimeline(bool force)
{
    const qint64 selected = SeekBaseline();
    const bool followingLive = (snapshot_.source == Source::Recap || snapshot_.source == Source::Recording) &&
        snapshot_.state == State::Live && !pendingSeekMs_ && snapshot_.seekable &&
        snapshot_.durationMs > snapshot_.firstMs;
    const bool expiredHold = snapshot_.source == Source::Recap && !followingLive && !dragging_ &&
        selected < snapshot_.firstMs;
    static_cast<TimelineSlider*>(timeline_)->SetEdge(followingLive ? TimelineSlider::Edge::Live :
        expiredHold ? TimelineSlider::Edge::ExpiredHold : TimelineSlider::Edge::Position);
    const bool hidePlayhead = snapshot_.source == Source::Recap && !followingLive && !expiredHold && !dragging_ &&
        (selected < snapshot_.firstMs || selected > snapshot_.durationMs);
    if (timeline_->property("okuflowOutsideWindow").toBool() != hidePlayhead ||
        timeline_->property("okuflowExpiredHold").toBool() != expiredHold) {
        timeline_->setProperty("okuflowOutsideWindow", hidePlayhead);
        timeline_->setProperty("okuflowExpiredHold", expiredHold);
        timeline_->style()->unpolish(timeline_);
        timeline_->style()->polish(timeline_);
        timeline_->update();
    }
    if (dragging_) return; // Freeze both range and pointer-to-time mapping.
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(TimeText(selected), TimeText(snapshot_.durationMs)));
    if(snapshot_.source==Source::Recap)
        timeLabel_->setText(QStringLiteral("%1–%2 · %3").arg(TimeText(snapshot_.firstMs),TimeText(snapshot_.durationMs),TimeText(selected)));
    // Reserve exactly the room this duration's readout needs, so the label
    // neither jitters during playback nor starves the scrubber when narrow.
    const auto longest=TimeText(std::max(snapshot_.durationMs,selected));
    const QString widest = snapshot_.source==Source::Recap?QStringLiteral("%1–%1 · %1").arg(longest)
        :QStringLiteral("%1 / %1").arg(longest);
    const int readoutWidth = timeLabel_->fontMetrics().horizontalAdvance(widest) + 8;
    if (timeLabel_->minimumWidth() != readoutWidth) {
        timeLabel_->setMinimumWidth(readoutWidth);
        UpdateLayout();
    }
    const QString description = snapshot_.source==Source::Recap
        ?QStringLiteral("%1–%2 · %3").arg(TimeText(snapshot_.firstMs),TimeText(snapshot_.durationMs),TimeText(selected))
        : TranslateUi(QStringLiteral("%1 of %2"))
        .arg(TimeText(SliderTime(selected)),
             TimeText(SliderTime(snapshot_.durationMs)));
    // Keep the native accessible slider quiet during playback. A focused
    // slider retains its value until the reader makes a keyboard selection.
    const bool rangeChanged=timeline_->minimum()!=SliderTime(snapshot_.firstMs) || timeline_->maximum()!=SliderTime(snapshot_.durationMs);
    // Rolling recap bounds must track eviction immediately. Saved video keeps
    // its existing focused inspection range until an explicit seek refresh.
    const bool canUpdate = force || (snapshot_.source==Source::Recap && rangeChanged) ||
        (!timeline_->hasFocus() && timelineUpdateClock_.elapsed() >= 1000);
    if (!canUpdate) return;
    updating_ = true;
    timeline_->setAccessibleDescription(description);
    {
        const QSignalBlocker blocker(timeline_);
        timeline_->setRange(SliderTime(snapshot_.firstMs), SliderTime(snapshot_.durationMs));
        if(force || !timeline_->hasFocus())timeline_->setValue(SliderTime(std::min(selected, snapshot_.durationMs)));
    }
    timelineUpdateClock_.restart();
    updating_ = false;
}

void VideoTransport::CommitSeek(qint64 positionMs, bool atRangeEnd)
{
    if (!IsActionReady(timeline_)) return;
    if ((snapshot_.source == Source::Recap || snapshot_.source == Source::Recording) &&
        (atRangeEnd || positionMs >= snapshot_.durationMs)) {
        // The displayed right edge means live, even if finalized history
        // advanced while the drag retained its original pointer mapping.
        pendingSeekMs_.reset();
        RefreshTimeline(true);
        UpdateActions();
        emit JumpToEndRequested();
        return;
    }
    const qint64 target = std::clamp<qint64>(positionMs, snapshot_.firstMs, snapshot_.durationMs);
    pendingSeekMs_ = target;
    RefreshTimeline(true);
    UpdateActions();
    emit SeekRequested(target);
}

qint64 VideoTransport::SeekBaseline() const
{
    return pendingSeekMs_.value_or(snapshot_.positionMs);
}

std::vector<QWidget*> VideoTransport::FocusTargets() const
{
    // Tab order follows the single-line visual order: primary actions,
    // scrubber, secondary actions. Return to camera stays last.
    if (collapsed_) return {};
    std::vector<QWidget*> targets{rewindButton_, playPauseButton_, jumpButton_, timeline_,
                                  saveFrameButton_};
    if (snapshot_.source == Source::Document) {
        targets = snapshot_.pageCount > 1 ? std::vector<QWidget*>{rewindButton_, page_, jumpButton_, saveFrameButton_}
            : std::vector<QWidget*>{saveFrameButton_};
    }
    if (collapseButton_->parentWidget() == this) targets.insert(targets.begin(), collapseButton_);
    if (muteShown_) targets.push_back(muteButton_);
    if (returnShown_) targets.push_back(returnButton_);
    return targets;
}

bool VideoTransport::eventFilter(QObject* watched, QEvent* event)
{
    if(volumePopup_ && volumePopup_->isVisible()) {
        const auto* widget=qobject_cast<QWidget*>(watched);
        const bool ownedWindow=widget && (widget->window()==volumePopup_ || widget->window()==window());
        const bool modalBlocked=QApplication::activeModalWidget() && QApplication::activeModalWidget()!=volumePopup_;
        if(ownedWindow && !modalBlocked && event->type()==QEvent::MouseButtonPress) {
            auto* mouse=static_cast<QMouseEvent*>(event);
            const QRect speaker(muteButton_->mapToGlobal(QPoint()),muteButton_->size());
            if(mouse->button()==Qt::LeftButton && speaker.contains(mouse->globalPosition().toPoint())) {
                if(IsActionReady(muteButton_))emit MutedChanged(!snapshot_.muted);
                return true;
            }
            if(!volumePopup_->geometry().contains(mouse->globalPosition().toPoint()))HideVolumePopup(false);
        }
        if(!ownedWindow && event->type()==QEvent::MouseButtonPress)HideVolumePopup(false);
        if(ownedWindow && !modalBlocked && event->type()==QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape) {
            HideVolumePopup();return true;
        }
        if(event->type()==QEvent::ApplicationDeactivate)HideVolumePopup(false);
    }
    if(watched==volumePopup_ && event->type()==QEvent::Hide) {
        NameButton(muteButton_,QStringLiteral("Playback volume"));

    }
    if(watched==this && event->type()==QEvent::Hide)HideVolumePopup(false);
    if(volumePopup_->isVisible() && watched==window() &&
        (event->type()==QEvent::Move || event->type()==QEvent::Resize))HideVolumePopup();
    if (event->type() == QEvent::FocusOut && qobject_cast<QWidget*>(watched) &&
        isAncestorOf(static_cast<QWidget*>(watched))) {
        QTimer::singleShot(0, this, &VideoTransport::UpdateActions);
    }
    if (watched == timeline_) {
        if (event->type() == QEvent::MouseButtonPress && snapshot_.state == State::Live &&
            (snapshot_.source == Source::Recap || snapshot_.source == Source::Recording)) {
            // An explicit pointer interaction ends focused inspection. Bring
            // the real range/value up to date before Qt maps the live thumb.
            RefreshTimeline(true);
        }
        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (timeline_->hasFocus() && !dragging_ && IsActionReady(timeline_) &&
                wheel->modifiers() == Qt::NoModifier) {
                wheelRemainder_ += wheel->angleDelta().y();
                const int steps = wheelRemainder_ / 120;
                wheelRemainder_ %= 120;
                if (steps) CommitSeek(SeekBaseline() + static_cast<qint64>(steps) * 1000);
            }
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->modifiers() == Qt::NoModifier &&
                (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
                              key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown ||
                              key->key() == Qt::Key_Home || key->key() == Qt::Key_End)) {
                if (!dragging_ && IsActionReady(timeline_)) {
                    qint64 target = SeekBaseline();
                    switch (key->key()) {
                    case Qt::Key_Left: target -= 1000; break;
                    case Qt::Key_Right: target += 1000; break;
                    case Qt::Key_PageUp: target += 10000; break;
                    case Qt::Key_PageDown: target -= 10000; break;
                    case Qt::Key_Home: target = snapshot_.firstMs; break;
                    case Qt::Key_End: target = snapshot_.durationMs; break;
                    }
                    CommitSeek(target);
                }
                event->accept();
                return true;
            }
            if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
                if (!key->isAutoRepeat() && IsActionReady(playPauseButton_)) emit PlayPauseRequested();
                event->accept();
                return true;
            }
        }
    }
    if(watched==volume_ && event->type()==QEvent::KeyPress) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab) {
            const bool backwards=key->key()==Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier);
            HideVolumePopup(false);
            auto* next=backwards?muteButton_:returnButton_;
            if(IsActionReady(next))next->setFocus(Qt::TabFocusReason);
            else if(IsActionReady(playPauseButton_))playPauseButton_->setFocus(Qt::TabFocusReason);
            return true;
        }
        if(key->key()==Qt::Key_Space && key->modifiers()==Qt::NoModifier) {
            if(!key->isAutoRepeat() && IsActionReady(muteButton_))emit MutedChanged(!snapshot_.muted);
            return true;
        }
    }
    if(watched==volume_ && event->type()==QEvent::Wheel &&
        (!volumePopup_->isVisible() || !volume_->hasFocus())) {
        event->ignore();return true;
    }
    return QWidget::eventFilter(watched, event);
}

std::vector<QPushButton*> VideoTransport::SecondaryButtons() const
{
    std::vector<QPushButton*> buttons{saveFrameButton_};
    if (muteShown_) buttons.push_back(muteButton_);
    if (returnShown_) buttons.push_back(returnButton_);
    return buttons;
}

void VideoTransport::ApplyFace(QPushButton* button)
{
    QString resource;
    QString glyph;
    QString caption; // Short badge kept beside the icon; never a full label.
    if (button == rewindButton_) {
        resource = snapshot_.source == Source::Document ? QStringLiteral(":/okuflow/icons/previous.svg") : QStringLiteral(":/okuflow/icons/undo.svg");
        glyph = snapshot_.source == Source::Document ? QStringLiteral("‹") : QStringLiteral("\u21b6 10");
        if (snapshot_.source != Source::Document) caption = QLocale().toString(10);
    } else if (button == playPauseButton_) {
        resource = snapshot_.playing ? QStringLiteral(":/okuflow/icons/pause.svg")
                                     : QStringLiteral(":/okuflow/icons/play.svg");
        glyph = snapshot_.playing ? QStringLiteral("\u23f8") : QStringLiteral("\u25b6");
    } else if (button == jumpButton_) {
        resource = QStringLiteral(":/okuflow/icons/step-forward.svg");
        glyph = QStringLiteral(">>");
    } else if (button == saveFrameButton_) {
        resource = QStringLiteral(":/okuflow/icons/save.svg");
        glyph = QStringLiteral("\u25a3");
    } else if (button == muteButton_) {
        resource = QStringLiteral(":/okuflow/icons/read.svg");
        glyph = snapshot_.muted ? QStringLiteral("\u266a\u00d7") : QStringLiteral("\u266a");
    } else if (button == returnButton_) {
        resource = QStringLiteral(":/okuflow/icons/camera.svg");
        glyph = QStringLiteral("\u21a9");
    } else if (button == collapseButton_) {
        resource = QStringLiteral(":/okuflow/icons/next.svg");
        glyph = collapsed_ ? QStringLiteral("\u2303") : QStringLiteral("\u2304");
    } else {
        return;
    }
    const bool slashed = button == muteButton_ && snapshot_.muted;
    const QPalette pal = palette();
    const QColor readyInk = slashed
        ? theme_contrast::WarningTextColor(theme_contrast::IsLightTheme(pal))
        : pal.color(QPalette::Active, QPalette::ButtonText);
    const QColor dimInk = pal.color(QPalette::Disabled, QPalette::ButtonText);
    const bool ready = button->property("okuflowTransportGate").toInt() ==
                       static_cast<int>(Gate::Ready);
    const QColor ink = ready ? readyInk : dimInk;
    const QString faceKey = resource + QLatin1Char('|') + ink.name(QColor::HexArgb) +
                            QLatin1Char('|') +
                            (slashed ? QStringLiteral("muted") : glyph + caption);
    if (button->property("okuflowFaceKey").toString() == faceKey) return;
    button->setProperty("okuflowFaceKey", faceKey);

    QIcon icon = theme_contrast::ThemedIcon(resource, ink, dimInk);
    if (!icon.isNull() && button == collapseButton_) {
        QTransform rotation;
        rotation.rotate(collapsed_ ? -90 : 90);
        icon = QIcon(icon.pixmap(28, 28).transformed(rotation, Qt::SmoothTransformation));
    }
    if (!icon.isNull() && slashed) icon = SlashedIcon(icon, ink);
    if (!icon.isNull()) {
        button->setIcon(icon);
        if (button->text() != caption) button->setText(caption);
    } else {
        // Hosts without the compiled icon resources (unit fixtures) keep
        // the previous unicode glyphs instead of blank buttons.
        auto buttonPalette=button->palette();
        buttonPalette.setColor(QPalette::Active,QPalette::ButtonText,ink);
        buttonPalette.setColor(QPalette::Inactive,QPalette::ButtonText,ink);
        if(button->palette()!=buttonPalette)button->setPalette(buttonPalette);
        button->setIcon(QIcon());
        if (button->text() != glyph) button->setText(glyph);
    }
}

void VideoTransport::ApplyButtonFaces()
{
    for (auto* button : {rewindButton_, playPauseButton_, jumpButton_,
                         saveFrameButton_, muteButton_, returnButton_, collapseButton_}) {
        ApplyFace(button);
    }
}

void VideoTransport::ApplyStyle()
{
    if (applyingStyle_) return;
    applyingStyle_ = true;
    const QString style = QStringLiteral(
        "QWidget#videoTransport { border-top: 1px solid palette(mid); }"
        "QWidget#videoTransport QPushButton { border: 1px solid palette(mid);"
        " border-radius: 8px; background: palette(button); padding: 0; }"
        "QWidget#videoTransport QPushButton:hover { border-color: palette(highlight); }"
        "QWidget#videoTransport QPushButton:focus { border: 3px solid palette(highlight); }"
        "QWidget#videoTransport QPushButton[okuflowTransportGate=\"1\"],"
        "QWidget#videoTransport QPushButton[okuflowTransportGate=\"2\"] { color: palette(mid); }"
        "QWidget#videoTransport QSlider:focus { border: 2px solid palette(highlight); }"
        "QWidget#videoTransport QSlider::groove:horizontal { height: 6px;"
        " border-radius: 3px; background: palette(mid); }"
        "QWidget#videoTransport QSlider::sub-page:horizontal {"
        " border-radius: 3px; background: palette(highlight); }"
        "QWidget#videoTransport QSlider::handle:horizontal { width: 18px; height: 28px;"
        " margin: -11px 0; border-radius: 6px; background: palette(button);"
        " border: 2px solid palette(highlight); }"
        "QWidget#videoTransport QSlider[okuflowOutsideWindow=\"true\"]::handle:horizontal {"
        " background: transparent; border: none; }"
        "QWidget#videoTransport QSlider[okuflowOutsideWindow=\"true\"]::sub-page:horizontal {"
        " background: palette(mid); }"
        "QWidget#videoTransport QSlider[okuflowExpiredHold=\"true\"]::handle:horizontal {"
        " background: %1; border: 2px dashed palette(window); }")
        .arg(theme_contrast::WarningTextColor(theme_contrast::IsLightTheme(palette())).name());
    if (styleSheet() != style) setStyleSheet(style);
    // Checked means the bar is hidden, not an active blue processing mode.
    // The button is reparented into the quick-mode corner.
    const QString collapseStyle = QStringLiteral(
        "QPushButton#videoCollapseButton { border: 1px solid palette(mid);"
        " border-radius: 8px; background: palette(button); color: palette(button-text); padding: 0; }"
        "QPushButton#videoCollapseButton:checked { background: palette(button);"
        " color: palette(button-text); border-color: palette(mid); }"
        "QPushButton#videoCollapseButton:hover { border-color: palette(highlight); }"
        "QPushButton#videoCollapseButton:focus { border: 3px solid palette(highlight); }");
    if (collapseButton_->styleSheet() != collapseStyle) collapseButton_->setStyleSheet(collapseStyle);
    const QString badge = snapshot_.state == State::Live ? QStringLiteral(
        "color: #ffffff; background: #14351e; border: 2px solid #ffffff; border-radius: 4px; padding: 3px 8px; font-weight: bold;")
        : snapshot_.state == State::BehindLive ? QStringLiteral(
        "color: #ffffff; background: #4d3400; border: 2px solid #ffffff; border-radius: 4px; padding: 3px 8px; font-weight: bold;")
        : QString();
    if (stateLabel_->styleSheet() != badge) stateLabel_->setStyleSheet(badge);
    applyingStyle_ = false;
}

void VideoTransport::UpdateLayout()
{
    const bool document = snapshot_.source == Source::Document;
    const auto secondaries = SecondaryButtons();
    // Everything stays on the main line while the scrubber can keep its
    // minimum width. Below that the secondaries drop to a second compact
    // row; buttons never shrink under their fixed touch-target size.
    int required = 20 + rewindButton_->minimumWidth() + kPlayWidth + kButtonExtent +
                   timeLabel_->minimumWidth() + kMinTimelineWidth;
    for (auto* button : secondaries) required += button->minimumWidth();
    required += kRowSpacing * (4 + static_cast<int>(secondaries.size()));
    const bool below = width() > 0 && width() < required && !secondaries.empty();
    if (below == secondariesBelow_ && secondaries == layoutSecondaries_ &&
        collapsed_ == layoutCollapsed_ && document == layoutDocument_ &&
        (snapshot_.pageCount > 1) == layoutDocumentPages_ && mainLayout_->count() > 0) {
        return;
    }
    secondariesBelow_ = below;
    layoutSecondaries_ = secondaries;
    layoutCollapsed_ = collapsed_;
    layoutDocument_ = document;
    layoutDocumentPages_ = snapshot_.pageCount > 1;
    const QPointer<QWidget> focused = isAncestorOf(QApplication::focusWidget())
        ? QApplication::focusWidget() : nullptr;
    const auto strip = [](QLayout* stripLayout) {
        while (QLayoutItem* item = stripLayout->takeAt(0)) delete item;
    };
    strip(mainLayout_);
    strip(secondaryLayout_);
    metadataLayout_->removeWidget(timeLabel_);
    metadataLayout_->removeWidget(jumpButton_);
    if (document) {
        mainLayout_->addWidget(rewindButton_);
        mainLayout_->addWidget(page_);
        mainLayout_->addWidget(jumpButton_);
        mainLayout_->addStretch(1);
        (below ? secondaryLayout_ : mainLayout_)->addWidget(saveFrameButton_);
        (below ? secondaryLayout_ : mainLayout_)->addWidget(returnButton_);
        for (auto* widget : std::vector<QWidget*>{playPauseButton_, timeline_, timeLabel_, muteButton_}) widget->hide();
        const bool pages = !collapsed_ && snapshot_.pageCount > 1;
        rewindButton_->setVisible(pages); page_->setVisible(pages); jumpButton_->setVisible(pages);
        saveFrameButton_->setVisible(!collapsed_); returnButton_->setVisible(!collapsed_);
        secondaryRow_->setVisible(!collapsed_ && below);
        const auto targets = FocusTargets();
        for (size_t i = 1; i < targets.size(); ++i) QWidget::setTabOrder(targets[i-1], targets[i]);
        return;
    }
    page_->hide();
    mainLayout_->addWidget(rewindButton_);
    mainLayout_->addWidget(playPauseButton_);
    mainLayout_->addWidget(jumpButton_);
    mainLayout_->addWidget(timeline_, 1);
    // At narrow widths the time shares the metadata line; the primary
    // buttons and a usable scrubber still fit in a 320-DIP viewport.
    (below ? metadataLayout_ : mainLayout_)->addWidget(timeLabel_);
    for (auto* button : secondaries) {
        (below ? secondaryLayout_ : mainLayout_)->addWidget(button);
        button->setVisible(true); // reparenting a QWidget hides it
    }
    if (below) secondaryLayout_->addStretch(1);
    secondaryRow_->setVisible(below);
    for (auto* widget : std::vector<QWidget*>{rewindButton_, playPauseButton_, timeline_,
                         timeLabel_, saveFrameButton_}) widget->setVisible(!collapsed_);
    muteButton_->setVisible(!collapsed_ && muteShown_);
    returnButton_->setVisible(!collapsed_ && returnShown_);
    secondaryRow_->setVisible(!collapsed_ && below);
    if(focused && focused->isEnabled() && focused->isVisibleTo(this) && !focused->hasFocus())
        focused->setFocus(Qt::OtherFocusReason);
    const auto targets = FocusTargets();
    for (std::size_t index = 1; index < targets.size(); ++index) {
        QWidget::setTabOrder(targets[index - 1], targets[index]);
    }
}

void VideoTransport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    UpdateLayout();
}

void VideoTransport::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange || event->type() == QEvent::FontChange) {
        RefreshText();
        RefreshTimeline(true);
        UpdateLayout();
    }
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        // Icon pixmaps are baked with the palette ink at render time.
        ApplyButtonFaces();
        ApplyStyle();
    }
}

} // namespace okuflow

#endif
