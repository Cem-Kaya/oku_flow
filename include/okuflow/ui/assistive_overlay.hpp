#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QDockWidget>
#include <QElapsedTimer>
#include <QPointer>
#include <QRect>
#include "okuflow/ui/assistant_chat_view.hpp"
#include "okuflow/ui/agent_control_panel.hpp"

#include <array>
#include <vector>

QT_BEGIN_NAMESPACE
class QEvent;
class QAction;
class QMainWindow;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QMoveEvent;
class QHideEvent;
class QPushButton;
class QShowEvent;
class QTextBrowser;
class QToolButton;
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

class AssistiveOverlay : public QDockWidget {
    Q_OBJECT
public:
    explicit AssistiveOverlay(QWidget* parent = nullptr);
    ~AssistiveOverlay() override;

    void SetContent(const QString& title, const QString& body, bool visible);
    void SetReadContent(const QString& title,const QString& body,bool visible);
    void SetChatSnapshot(const QJsonObject& snapshot);
    void ShowChat(bool visible);
    QWidget* ReadResultPane() const { return readResultPane_; }
    QTextBrowser* ReadBody() const { return bodyView_; }
    AssistantChatView* chatView() const { return chatView_; }
    AgentControlPanel* agentControlPanel() const { return agentControlPanel_; }
    // Temporarily hide all presentation while retaining content and the
    // latest requested visibility. This does not dismiss or cancel a turn.
    void SetUiSuppressed(bool suppressed);
    void SetSafeArea(const QRect& relativeSafeArea);
    void SetBusy(bool busy);
    // Inline routing refusal keeps the question intact and never takes focus.
    void SetControlRoutingNotice(bool visible, bool canStartControl = true);
    void ClearQuestionDraft();
    QString QuestionDraft() const;
    void RestoreRelativeGeometry(const QRect& geometry);
    QRect RelativeGeometry() const;
    void SetDockPosition(const QString& position);
    QString DockPosition() const;
    // Reading-first outer focus bridge; chat footer follows question,
    // Read Aloud, attachment, Send. The separate result pane keeps its own list.
    std::vector<QWidget*> FocusTargets() const;

signals:
    void Dismissed();
    void ReadAloudRequested(const QString& text);
    void QuestionSubmitted(const QString& question);
    void NewChatRequested();
    void StartControlConversationRequested();
    void SendWithoutControlRequested();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void UpdatePlacement();
    void BeginDrag(const QPoint& globalPosition);
    void BeginDockedDrag(const QPoint& globalPosition);
    void ContinueDockedDrag(const QPoint& globalPosition);
    void CancelDockedDrag();
    void ReleaseDockLatch();
    void ContinueDrag(const QPoint& globalPosition);
    void UpdateDockPreview();
    void FinishDrag(bool commit);
    void BeginResize(const QPoint& localPosition, const QPoint& globalPosition);
    void ContinueResize(const QPoint& globalPosition);
    void UpdateResizeCursor(const QPoint& localPosition);
    Qt::Edges ResizeEdgesAt(const QPoint& localPosition) const;
    QRect ConstrainedGeometry(const QRect& requested) const;
    void SubmitQuestion();
    void UpdateDockPositionControl();

    QPointer<QWidget> renderTarget_;
    QPointer<QMainWindow> dockHost_;
    QString title_;
    QString body_;
    QWidget* headerWidget_{};
    QLabel* titleLabel_{};
    QPushButton* newChatButton_{};
    QTextBrowser* bodyView_{};
    QLineEdit* questionEdit_{};
    QPushButton* askButton_{};
    QWidget* controlRoutingRow_{};
    QPushButton* startControlButton_{};
    QPushButton* sendWithoutControlButton_{};
    bool controlRoutingRequired_{};
    AssistantChatView* chatView_{};
    AgentControlPanel* agentControlPanel_{};
    QPointer<QWidget> readResultPane_;
    QLabel* readTitle_{};
    bool readDesiredVisible_{};
    QPushButton* readAloudButton_{};
    QToolButton* dockPositionButton_{};
    std::array<QAction*, 3> dockPositionActions_{};
    QToolButton* closeButton_{};
    QLabel* dockPreview_{};
    QString dockCandidate_;
    QString pendingDockCandidate_;
    QElapsedTimer dockCandidateSince_;
    QTimer* dockDebounceTimer_{};
    QString blockedDockSide_;
    QTimer* undockTimer_{};
    bool dockHeaderPressed_{};
    QPoint dockPressGlobal_;
    QPoint dockPointerGlobal_;
    QPoint dockGrabOffset_;
    int dockStartWidth_{};
    QPoint pointerStartGlobal_;
    QPoint parentOrigin_;
    QRect pointerStartGeometry_;
    Qt::Edges resizeEdges_{};
    bool dragging_{};
    bool nativeDragging_{};
    bool dragMoved_{};
    quint64 dragSerial_{};
    bool resizing_{};
    bool busy_{};
    bool desiredVisible_{};
    bool uiSuppressed_{};
    int suppressedDockWidth_{};
    bool placementInitialized_{};
    bool usingDefaultPlacement_{true};
    bool changingDockPosition_{};
    QRect restoredRelativeGeometry_;
    QRect safeArea_;
};

} // namespace okuflow

#endif // _WIN32
