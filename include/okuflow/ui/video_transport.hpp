#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <vector>

QT_BEGIN_NAMESPACE
class QLabel;
class QGridLayout;
class QPushButton;
class QSlider;
class QEvent;
class QResizeEvent;
QT_END_NAMESPACE

namespace okuflow {

// Presentation-only controls. Playback/document ownership stays in the app;
// incoming snapshots never issue playback commands or steal keyboard focus.
class VideoTransport final : public QWidget {
    Q_OBJECT
public:
    enum class Source { None, Recording, Video };
    enum class State {
        Live, BehindLive, Paused, Video, End, Opening, Seeking,
        Waiting, Finishing, Unavailable
    };
    struct Snapshot {
        Source source{Source::None};
        State state{State::Paused};
        qint64 positionMs{};
        qint64 durationMs{}; // Verified seekable end, never a pending live tail.
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
    void RefreshText(bool announce);
    void RefreshTimeline(bool force);
    void UpdateLayout();
    void CommitSeek(qint64 positionMs);
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
    QWidget* controlsRow_{};
    QGridLayout* controlsLayout_{};
    QElapsedTimer timelineUpdateClock_;
    bool dragging_{};
    bool updating_{};
    bool uiHidden_{};
    int layoutColumns_{};
    quint64 dragGeneration_{};
};

} // namespace okuflow

#endif
