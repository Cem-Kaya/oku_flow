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
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

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
    setFocusPolicy(Qt::StrongFocus); // Safe focus parking if no viewport is supplied.
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
        button->installEventFilter(this);
        return button;
    };
    rewindButton_ = makeButton("videoRewindButton", QStringLiteral("\u21b6 10"));
    playPauseButton_ = makeButton("videoPlayPauseButton", QStringLiteral("\u25b6"));
    jumpButton_ = makeButton("videoJumpButton", QStringLiteral(">>"));
    returnButton_ = makeButton("videoReturnCameraButton", QStringLiteral("\u21a9"));
    saveFrameButton_ = makeButton("videoSaveFrameButton", QStringLiteral("\u25a3"));
    muteButton_ = makeButton("videoMuteButton", QStringLiteral("\u266a"));
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

    announcementTimer_ = new QTimer(this);
    announcementTimer_->setSingleShot(true);
    announcementTimer_->setInterval(200);
    connect(announcementTimer_, &QTimer::timeout, this, &VideoTransport::AnnounceSettledState);
    ApplyStyle();

    connect(rewindButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(rewindButton_)) CommitSeek(std::max<qint64>(0, SeekBaseline() - 10000));
    });
    connect(playPauseButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(playPauseButton_)) emit PlayPauseRequested();
    });
    connect(jumpButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(jumpButton_)) emit JumpToEndRequested();
    });
    connect(returnButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(returnButton_)) emit ReturnToCameraRequested();
    });
    connect(saveFrameButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(saveFrameButton_)) emit SaveFrameRequested();
    });
    connect(muteButton_, &QPushButton::clicked, this, [this]() {
        if (IsActionReady(muteButton_)) emit MutedChanged(!snapshot_.muted);
    });
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
        snapshot_.muted != snapshot.muted || snapshot_.hasAudio != snapshot.hasAudio;
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
    snapshot_.durationMs = std::max<qint64>(0, snapshot_.durationMs);
    snapshot_.positionMs = std::max<qint64>(0, snapshot_.positionMs);
    const bool hasSource = snapshot_.source != Source::None;
    if (!hasSource) ReturnFocus();
    setVisible(hasSource && !uiHidden_);
    UpdateActions();
    if (changedText) RefreshText();
    RefreshTimeline(changedDocument || !snapshot_.playing);
    if (changedState) RequestAnnouncement();
}

void VideoTransport::SetUiHidden(bool hidden)
{
    if (hidden) ReturnFocus();
    uiHidden_ = hidden;
    setVisible(!hidden && snapshot_.source != Source::None);
    RequestAnnouncement();
}

void VideoTransport::SetFocusReturnTarget(QWidget* target)
{
    focusReturnTarget_ = target;
}

bool VideoTransport::ActionShown(const QWidget* control) const
{
    if (control == muteButton_) return muteShown_;
    if (control == returnButton_) return returnShown_;
    return true;
}

bool VideoTransport::IsActionReady(const QWidget* control) const
{
    return control && snapshot_.source != Source::None && !uiHidden_ &&
        ActionShown(control) && control->isEnabled() &&
        control->property("okuflowTransportGate").toInt() == static_cast<int>(Gate::Ready);
}

void VideoTransport::ApplyGate(QWidget* control, Gate gate)
{
    const bool changed = control->property("okuflowTransportGate").toInt() != static_cast<int>(gate);
    control->setProperty("okuflowTransportGate", static_cast<int>(gate));
    // Qt transfers focus immediately when setEnabled(false) is called. Keep
    // the semantic focus owner inert until the user leaves it or it is ready.
    control->setEnabled(gate == Gate::Ready || control->hasFocus());
    if (control != timeline_) {
        SetLiveAccessibleDescription(control, gate == Gate::Ready ? QString() : StateText());
    }
    if (changed) {
        control->style()->unpolish(control);
        control->style()->polish(control);
        control->update();
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
    const bool hasSource = snapshot_.source != Source::None;
    const bool pending = snapshot_.state == State::Opening || snapshot_.state == State::Seeking;
    const bool available = hasSource && snapshot_.state != State::Unavailable;
    const auto gate = [available](bool ready, bool busy = false) {
        return available && ready ? Gate::Ready : busy && available ? Gate::Busy : Gate::Unavailable;
    };
    ApplyGate(rewindButton_, gate(snapshot_.seekable && SeekBaseline() > 0));
    ApplyGate(playPauseButton_, gate(!pending && snapshot_.state != State::End &&
        (snapshot_.canSaveFrame || snapshot_.seekable), pending));
    ApplyGate(jumpButton_, gate(!pending &&
        (snapshot_.source == Source::Recording || snapshot_.seekable), pending));
    ApplyGate(timeline_, gate(snapshot_.seekable && snapshot_.durationMs > 0));
    ApplyGate(returnButton_, hasSource && snapshot_.canReturnToCamera ? Gate::Ready : Gate::Unavailable);
    ApplyGate(saveFrameButton_, gate(snapshot_.canSaveFrame && !pending, pending));
    ApplyGate(muteButton_, gate(snapshot_.source == Source::Video && snapshot_.hasAudio));
    const bool changedReturn = SetActionShown(returnButton_, snapshot_.source == Source::Video);
    const bool changedMute = SetActionShown(muteButton_, snapshot_.source == Source::Video && snapshot_.hasAudio);
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
    if (focus != this && !(focus && isAncestorOf(focus))) return;
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
    NameButton(rewindButton_, QStringLiteral("Rewind 10 seconds"));
    NameButton(playPauseButton_, snapshot_.playing ? QStringLiteral("Pause video")
                                                 : QStringLiteral("Resume video"));
    NameButton(jumpButton_, snapshot_.source == Source::Recording
                               ? QStringLiteral("Jump to live") : QStringLiteral("Jump to end"));
    NameButton(returnButton_, QStringLiteral("Return to camera"));
    NameButton(saveFrameButton_, QStringLiteral("Save frame"));
    NameButton(muteButton_, snapshot_.muted ? QStringLiteral("Unmute playback")
                                          : QStringLiteral("Mute playback"));
    setAccessibleName(TranslateUi(QStringLiteral("Video controls")));
    timeline_->setAccessibleName(TranslateUi(QStringLiteral("Video timeline")));
    timeline_->setToolTip(TranslateUi(QStringLiteral(
        "Left and Right: one second. Page Up and Page Down: ten seconds. Home: start. End: recorded end.")));
    timeLabel_->setMinimumWidth(timeLabel_->fontMetrics().horizontalAdvance(
        QStringLiteral("000:00:00 / 000:00:00")));
    UpdateLayout();
    ApplyStyle();
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
    const QString message = QStringLiteral("%1: %2, %3")
        .arg(sourceLabel_->accessibleName(), TranslateUi(StateText()), TimeText(snapshot_.positionMs));
    if (message == lastAnnouncement_) return;
    lastAnnouncement_ = message;
    QAccessibleAnnouncementEvent announcement(this, message);
    announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&announcement);
}

void VideoTransport::RefreshTimeline(bool force)
{
    if (dragging_) return; // Freeze both range and pointer-to-time mapping.
    const qint64 selected = SeekBaseline();
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
    if (!IsActionReady(timeline_)) return;
    const qint64 target = std::clamp<qint64>(positionMs, 0, snapshot_.durationMs);
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
    std::vector<QWidget*> targets{rewindButton_, playPauseButton_, jumpButton_, saveFrameButton_};
    if (muteShown_) targets.push_back(muteButton_);
    targets.push_back(timeline_);
    if (returnShown_) targets.push_back(returnButton_);
    return targets;
}

bool VideoTransport::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        QTimer::singleShot(0, this, &VideoTransport::UpdateActions);
    }
    if (watched == timeline_) {
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
                    case Qt::Key_Home: target = 0; break;
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
    return QWidget::eventFilter(watched, event);
}

std::vector<QPushButton*> VideoTransport::ActionButtons() const
{
    std::vector<QPushButton*> buttons{rewindButton_, playPauseButton_, jumpButton_, saveFrameButton_};
    if (muteShown_) buttons.push_back(muteButton_);
    if (returnShown_) buttons.push_back(returnButton_);
    return buttons;
}

void VideoTransport::ApplyButtonFaces()
{
    const std::array<std::pair<QPushButton*, QString>, 6> faces{{
        {rewindButton_, QStringLiteral("\u21b6 10")},
        {playPauseButton_, snapshot_.playing ? QStringLiteral("\u23f8") : QStringLiteral("\u25b6")},
        {jumpButton_, QStringLiteral(">>")}, {saveFrameButton_, QStringLiteral("\u25a3")},
        {muteButton_, snapshot_.muted ? QStringLiteral("\u266a\u00d7") : QStringLiteral("\u266a")},
        {returnButton_, QStringLiteral("\u21a9")}
    }};
    for (const auto& [button, glyph] : faces) {
        const QString face = labelled_ ? button->accessibleName() : glyph;
        if (button->text() != face) button->setText(face);
    }
}

void VideoTransport::ApplyStyle()
{
    if (applyingStyle_) return;
    applyingStyle_ = true;
    const QString style = QStringLiteral(
        "QWidget#videoTransport { border-top: 1px solid palette(mid); }"
        "QWidget#videoTransport QPushButton:focus { border: 3px solid palette(highlight); }"
        "QWidget#videoTransport QPushButton[okuflowTransportGate=\"1\"],"
        "QWidget#videoTransport QPushButton[okuflowTransportGate=\"2\"] { color: palette(mid); }"
        "QWidget#videoTransport QSlider:focus { border: 2px solid palette(highlight); }");
    if (styleSheet() != style) setStyleSheet(style);
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
    const int available = std::max(0, width() - 20);
    const auto buttons = ActionButtons();
    const int count = static_cast<int>(buttons.size());
    int required = (count - 1) * controlsLayout_->spacing();
    for (auto* button : buttons) {
        required += std::max(52, button->fontMetrics().horizontalAdvance(button->accessibleName()) + 32);
    }
    labelled_ = required <= available;
    ApplyButtonFaces();
    const int columns = labelled_ || count * 52 + (count - 1) * controlsLayout_->spacing() <= available
        ? count : std::min(3, count);
    if (columns == layoutColumns_ && buttons == layoutButtons_) return;
    layoutColumns_ = columns;
    layoutButtons_ = buttons;
    for (auto* button : {rewindButton_, playPauseButton_, jumpButton_, saveFrameButton_, muteButton_, returnButton_}) {
        controlsLayout_->removeWidget(button);
    }
    for (int column = 0; column < 6; ++column) controlsLayout_->setColumnStretch(column, column < columns ? 1 : 0);
    for (int index = 0; index < count; ++index) {
        controlsLayout_->addWidget(buttons[index], index / columns, index % columns);
    }
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
}

} // namespace okuflow

#endif
