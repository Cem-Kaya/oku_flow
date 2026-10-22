#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <vector>
#include <optional>

QT_BEGIN_NAMESPACE
class QLabel;
class QHBoxLayout;
class QPushButton;
class QSlider;
class QEvent;
class QResizeEvent;
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

// Presentation-only controls. Playback/document ownership stays in the app;
// incoming snapshots never issue playback commands or steal keyboard focus.
class VideoTransport final : public QWidget {
    Q_OBJECT
public:
    enum class Source { None, Recording, Video, Recap };
    enum class State {
        Live, BehindLive, Paused, Video, End, Opening, Seeking,
        Waiting, Finishing, Unavailable, Buffering
    };
    struct Snapshot {
        Source source{Source::None};
        State state{State::Paused};
        qint64 positionMs{};
        qint64 durationMs{}; // Verified seekable end, never a pending live tail.
        qint64 firstMs{}; // Rolling history's first available absolute time.
        bool canPauseLive{}; // Exact display lease can precede encoded media.
        bool playing{};
        bool seekable{};
        bool muted{true};
        bool hasAudio{};
        bool canSaveFrame{};
        bool canReturnToCamera{true};
        QString sourceName;
        quint64 sourceGeneration{};
    };

    explicit VideoTransport(QWidget* parent = nullptr);
    void SetSnapshot(const Snapshot& snapshot);
    const Snapshot& snapshot() const { return snapshot_; }
    void SetUiHidden(bool hidden);
    // Keyboard focus held inside the bar moves here when the bar hides,
    // instead of to whichever control Qt would pick next.
    void SetFocusReturnTarget(QWidget* target);
    // True when activating the control issues its request now. A control
    // that holds focus stays enabled but inert while busy or unavailable.
    bool IsActionReady(const QWidget* control) const;
    std::vector<QWidget*> FocusTargets() const;
    QSlider* timeline() const { return timeline_; }
    QPushButton* playPauseButton() const { return playPauseButton_; }
    QPushButton* jumpButton() const { return jumpButton_; }

signals:
    void SeekRequested(qint64 positionMs);
    void PlayPauseRequested();
    void JumpToEndRequested();
    void ReturnToCameraRequested();
    void SaveFrameRequested();
    void MutedChanged(bool muted);

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Gate { Ready, Busy, Unavailable };
    void UpdateActions();
    void ApplyGate(QWidget* control, Gate gate);
    bool SetActionShown(QPushButton* button, bool shown);
    bool ActionShown(const QWidget* control) const;
    void MoveFocusFrom(QWidget* leaving, QWidget* preferred);
    void ReturnFocus();
    void RefreshText();
    void ApplyButtonFaces();
    void ApplyFace(QPushButton* button);
    void ApplyStyle();
    void RequestAnnouncement();
    void AnnounceSettledState();
    void RefreshTimeline(bool force);
    void UpdateLayout();
    void CommitSeek(qint64 positionMs);
    qint64 SeekBaseline() const;
    std::vector<QPushButton*> SecondaryButtons() const;
    QString StateText() const;
    static QString TimeText(qint64 milliseconds);

    Snapshot snapshot_;
    QLabel* sourceLabel_{};
    QLabel* stateLabel_{};
    QLabel* timeLabel_{};
    QSlider* timeline_{};
    QPushButton* rewindButton_{};
    QPushButton* playPauseButton_{};
    QPushButton* jumpButton_{};
    QPushButton* returnButton_{};
    QPushButton* saveFrameButton_{};
    QPushButton* muteButton_{};
    QWidget* secondaryRow_{};
    QHBoxLayout* mainLayout_{};
    QHBoxLayout* metadataLayout_{};
    QHBoxLayout* secondaryLayout_{};
    QTimer* announcementTimer_{};
    QPointer<QWidget> focusReturnTarget_;
    QElapsedTimer timelineUpdateClock_;
    QString lastAnnouncement_;
    std::optional<qint64> pendingSeekMs_;
    std::vector<QPushButton*> layoutSecondaries_;
    bool dragging_{};
    bool updating_{};
    bool uiHidden_{};
    bool secondariesBelow_{};
    bool muteShown_{true};
    bool returnShown_{true};
    bool announcementPending_{};
    bool applyingStyle_{};
    int wheelRemainder_{};
    quint64 dragGeneration_{};
};

} // namespace okuflow

#endif
