#ifdef _WIN32

#include "okuflow/ui/video_transport.hpp"

#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>

namespace okuflow {
namespace {

int SliderTime(qint64 milliseconds)
{
    return static_cast<int>(std::clamp<qint64>(milliseconds, 0,
                                             std::numeric_limits<int>::max()));
}

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
        if (type == QAccessible::ValueInterface) return static_cast<QAccessibleValueInterface*>(this);
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
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(4);
    auto* sourceRow = new QHBoxLayout();
    sourceRow->setSpacing(10);
    sourceLabel_ = new SourceLabel(this);
    sourceLabel_->setObjectName(QStringLiteral("videoSourceLabel"));
    sourceLabel_->setTextFormat(Qt::PlainText);
    sourceLabel_->setMinimumWidth(0);
    sourceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stateLabel_ = new QLabel(this);
    stateLabel_->setObjectName(QStringLiteral("videoStateLabel"));
    stateLabel_->setTextFormat(Qt::PlainText);
    stateLabel_->setWordWrap(true);
    sourceRow->addWidget(sourceLabel_, 1);
    sourceRow->addWidget(stateLabel_);
    layout->addLayout(sourceRow);

    controlsRow_ = new QWidget(this);
    controlsLayout_ = new QGridLayout(controlsRow_);
    controlsLayout_->setContentsMargins(0, 0, 0, 0);
    controlsLayout_->setSpacing(6);
    const auto makeButton = [this](const char* name, const QString& glyph) {
        auto* button = new QPushButton(glyph, controlsRow_);
        button->setObjectName(QString::fromLatin1(name));
        button->setMinimumSize(52, 52);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        button->setAutoDefault(false);
        return button;
    };
    rewindButton_ = makeButton("videoRewindButton", QStringLiteral("\u21b6 10"));
    playPauseButton_ = makeButton("videoPlayPauseButton", QStringLiteral("\u25b6"));
    jumpButton_ = makeButton("videoJumpButton", QStringLiteral(">>"));
    returnButton_ = makeButton("videoReturnCameraButton", QStringLiteral("\u21a9"));
    saveFrameButton_ = makeButton("videoSaveFrameButton", QStringLiteral("\u25a3"));
    muteButton_ = makeButton("videoMuteButton", QStringLiteral("\u266a"));
    muteButton_->setCheckable(true);
    layout->addWidget(controlsRow_);

    auto* timelineRow = new QHBoxLayout();
    timelineRow->setSpacing(8);
    timeline_ = new QSlider(Qt::Horizontal, this);
    timeline_->setObjectName(QStringLiteral("videoTimeline"));
    timeline_->setMinimumHeight(44);
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
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeLabel_->setLayoutDirection(Qt::LeftToRight);
    timelineRow->addWidget(timeline_, 1);
    timelineRow->addWidget(timeLabel_);
    layout->addLayout(timelineRow);

    setStyleSheet(QStringLiteral(
        "QWidget#videoTransport { border-top: 1px solid palette(mid); }"
        "QWidget#videoTransport QPushButton:focus { border: 3px solid palette(highlight); }"
        "QWidget#videoTransport QSlider:focus { border: 2px solid palette(highlight); }"));

    connect(rewindButton_, &QPushButton::clicked, this, [this]() {
        CommitSeek(std::max<qint64>(0, snapshot_.positionMs - 10000));
    });
    connect(playPauseButton_, &QPushButton::clicked, this, &VideoTransport::PlayPauseRequested);
    connect(jumpButton_, &QPushButton::clicked, this, &VideoTransport::JumpToEndRequested);
    connect(returnButton_, &QPushButton::clicked, this, &VideoTransport::ReturnToCameraRequested);
    connect(saveFrameButton_, &QPushButton::clicked, this, &VideoTransport::SaveFrameRequested);
    connect(muteButton_, &QPushButton::clicked, this, &VideoTransport::MutedChanged);
    connect(timeline_, &QSlider::sliderPressed, this, [this]() {
        dragging_ = true;
        dragGeneration_ = snapshot_.sourceGeneration;
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
        if (sameDocument) CommitSeek(target);
        RefreshTimeline(true);
    });
    connect(timeline_, &QSlider::valueChanged, this, [this](int position) {
        if (!updating_ && !dragging_) {
            timeline_->setAccessibleDescription(TranslateUi(QStringLiteral("%1 of %2"))
                .arg(TimeText(position), TimeText(timeline_->maximum())));
            CommitSeek(position);
        }
    });
    const auto targets = FocusTargets();
    for (std::size_t index = 1; index < targets.size(); ++index) {
        QWidget::setTabOrder(targets[index - 1], targets[index]);
    }
    timelineUpdateClock_.start();
    RefreshText(false);
    UpdateLayout();
    hide();
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
    }
    return {};
}

void VideoTransport::SetSnapshot(const Snapshot& snapshot)
{
    const bool changedDocument = snapshot_.sourceGeneration != snapshot.sourceGeneration ||
                                 snapshot_.source != snapshot.source;
    const bool changedState = changedDocument || snapshot_.state != snapshot.state ||
        snapshot_.sourceName != snapshot.sourceName ||
        (!snapshot.playing && snapshot.state != State::Seeking &&
         snapshot_.positionMs != snapshot.positionMs);
    const bool changedText = changedState || snapshot_.playing != snapshot.playing ||
                             snapshot_.muted != snapshot.muted;
    if (changedDocument && dragging_) {
        dragging_ = false;
        const QSignalBlocker blocker(timeline_);
        timeline_->setSliderDown(false);
    }
    snapshot_ = snapshot;
    snapshot_.durationMs = std::max<qint64>(0, snapshot_.durationMs);
    snapshot_.positionMs = std::max<qint64>(0, snapshot_.positionMs);
    const bool hasSource = snapshot_.source != Source::None;
    setVisible(hasSource && !uiHidden_);
    rewindButton_->setEnabled(snapshot_.seekable && snapshot_.positionMs > 0);
    const bool pending = snapshot_.state == State::Opening || snapshot_.state == State::Seeking;
    playPauseButton_->setEnabled(hasSource && !pending && snapshot_.state != State::End &&
        snapshot_.state != State::Unavailable && (snapshot_.canSaveFrame || snapshot_.seekable));
    jumpButton_->setEnabled(hasSource && !pending &&
        (snapshot_.source == Source::Recording || snapshot_.seekable));
    timeline_->setEnabled(snapshot_.seekable && snapshot_.durationMs > 0);
    returnButton_->setEnabled(snapshot_.canReturnToCamera);
    saveFrameButton_->setEnabled(snapshot_.canSaveFrame);
    muteButton_->setEnabled(snapshot_.hasAudio);
    {
        const QSignalBlocker blocker(muteButton_);
        muteButton_->setChecked(snapshot_.muted);
    }
    if (changedText) RefreshText(changedState);
    RefreshTimeline(changedDocument || !snapshot_.playing);
}

void VideoTransport::SetUiHidden(bool hidden)
{
    uiHidden_ = hidden;
    setVisible(!hidden && snapshot_.source != Source::None);
}

void VideoTransport::RefreshText(bool announce)
{
    const QString source = snapshot_.sourceName.isEmpty()
        ? TranslateUi(snapshot_.source == Source::Recording
                          ? QStringLiteral("Current recording") : QStringLiteral("Video"))
        : snapshot_.sourceName;
    static_cast<SourceLabel*>(sourceLabel_)->SetSource(source);
    SetLiveText(stateLabel_, StateText(), LivePoliteness::kSilent,
                QStringLiteral("Video playback"));
    if (announce && snapshot_.source != Source::None) {
        QAccessibleAnnouncementEvent announcement(this, QStringLiteral("%1: %2, %3")
            .arg(source, TranslateUi(StateText()), TimeText(snapshot_.positionMs)));
        announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
        QAccessible::updateAccessibility(&announcement);
    }
    playPauseButton_->setText(snapshot_.playing ? QStringLiteral("\u23f8") : QStringLiteral("\u25b6"));
    NameButton(rewindButton_, QStringLiteral("Rewind 10 seconds"));
    NameButton(playPauseButton_, snapshot_.playing ? QStringLiteral("Pause video")
                                                 : QStringLiteral("Resume video"));
    SetLiveAccessibleDescription(playPauseButton_, snapshot_.state == State::End
        ? QStringLiteral("End of video") : QString());
    NameButton(jumpButton_, snapshot_.source == Source::Recording
                               ? QStringLiteral("Jump to live") : QStringLiteral("Jump to end"));
    NameButton(returnButton_, QStringLiteral("Return to camera"));
    NameButton(saveFrameButton_, QStringLiteral("Save frame"));
    NameButton(muteButton_, snapshot_.muted ? QStringLiteral("Unmute playback")
                                          : QStringLiteral("Mute playback"));
    muteButton_->setText(snapshot_.muted ? QStringLiteral("\u266a\u00d7") : QStringLiteral("\u266a"));
    setAccessibleName(TranslateUi(QStringLiteral("Video controls")));
    timeline_->setAccessibleName(TranslateUi(QStringLiteral("Video timeline")));
    timeline_->setToolTip(TranslateUi(QStringLiteral(
        "Left and Right: one second. Page Up and Page Down: ten seconds. Home: start. End: recorded end.")));
    timeLabel_->setMinimumWidth(timeLabel_->fontMetrics().horizontalAdvance(
        QStringLiteral("000:00:00 / 000:00:00")));
}

void VideoTransport::RefreshTimeline(bool force)
{
    if (dragging_) return; // Freeze both range and pointer-to-time mapping.
    const qint64 selected = snapshot_.positionMs;
    timeLabel_->setText(QStringLiteral("%1 / %2").arg(TimeText(selected), TimeText(snapshot_.durationMs)));
    const QString description = TranslateUi(QStringLiteral("%1 of %2"))
        .arg(TimeText(SliderTime(std::min(selected, snapshot_.durationMs))),
             TimeText(SliderTime(snapshot_.durationMs)));
    // Keep the native accessible slider quiet during playback. A focused
    // slider retains its value until the reader makes a keyboard selection.
    const bool canUpdate = force || (!timeline_->hasFocus() && timelineUpdateClock_.elapsed() >= 1000);
    if (!canUpdate) return;
    updating_ = true;
    timeline_->setAccessibleDescription(description);
    {
        const QSignalBlocker blocker(timeline_);
        timeline_->setRange(0, SliderTime(snapshot_.durationMs));
        timeline_->setValue(SliderTime(std::min(selected, snapshot_.durationMs)));
    }
    timelineUpdateClock_.restart();
    updating_ = false;
}

void VideoTransport::CommitSeek(qint64 positionMs)
{
    if (!snapshot_.seekable) return;
    emit SeekRequested(std::clamp<qint64>(positionMs, 0, snapshot_.durationMs));
}

std::vector<QWidget*> VideoTransport::FocusTargets() const
{
    return {rewindButton_, playPauseButton_, jumpButton_, returnButton_,
            saveFrameButton_, muteButton_, timeline_};
}

bool VideoTransport::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == timeline_) {
        if (event->type() == QEvent::Wheel) {
            event->ignore();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!dragging_ && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
                              key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown ||
                              key->key() == Qt::Key_Home || key->key() == Qt::Key_End)) {
                // Focus freezes autonomous thumb movement, not keyboard access
                // to newly committed recording media.
                const QSignalBlocker blocker(timeline_);
                timeline_->setMaximum(SliderTime(snapshot_.durationMs));
            }
            if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
                if (!key->isAutoRepeat() && playPauseButton_->isEnabled()) emit PlayPauseRequested();
                event->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoTransport::UpdateLayout()
{
    const int available = std::max(0, width() - 20);
    int columns = 6;
    int required = 5 * controlsLayout_->spacing();
    for (auto* button : {rewindButton_, playPauseButton_, jumpButton_, returnButton_,
                          saveFrameButton_, muteButton_}) {
        required += std::max(52, button->minimumSizeHint().width());
    }
    if (required > available) columns = 3;
    if (columns == layoutColumns_) return;
    layoutColumns_ = columns;
    const std::array<QPushButton*, 6> buttons{rewindButton_, playPauseButton_, jumpButton_,
        returnButton_, saveFrameButton_, muteButton_};
    for (auto* button : buttons) controlsLayout_->removeWidget(button);
    for (int column = 0; column < 6; ++column) controlsLayout_->setColumnStretch(column, column < columns ? 1 : 0);
    for (int index = 0; index < 6; ++index) {
        controlsLayout_->addWidget(buttons[index], index / columns, index % columns);
    }
}

void VideoTransport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    UpdateLayout();
    RefreshText(false);
}

void VideoTransport::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange || event->type() == QEvent::FontChange) {
        RefreshText(false);
        RefreshTimeline(true);
        UpdateLayout();
    }
}

} // namespace okuflow

#endif
