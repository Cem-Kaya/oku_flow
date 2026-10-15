#pragma once
#if defined(_WIN32) || defined(Q_MOC_RUN)
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QToolButton;
QT_END_NAMESPACE
namespace okuflow {
// Two instances render one app-owned VisibleChatSession. All model content is
// escaped; viewing a message can never approve an action. Scroll/selection are
// local to this view and are not part of the shared session snapshot.
class AssistantChatView final : public QWidget {
    Q_OBJECT
public:
    explicit AssistantChatView(QWidget* parent = nullptr);
    void SetChatSnapshot(const QJsonObject& snapshot);
    void SetSettingsWidget(QWidget* widget);
    void SetApprovalWidget(QWidget* widget);
    void SetRoutingWidget(QWidget* widget);
    void SetAttachCurrentView(bool attach);
    bool attachCurrentView() const;
    void SetUndoAvailable(bool available);
    void SetConnectionState(bool connected);
    void ShowSettings();
    void ShowHistory();
    void CloseDrawer();
    bool DrawerContainsFocus() const;
    void FocusApproval(QWidget* deny);
    QWidget* headerWidget() const { return header_; }
    QLabel* connectionLabel() const { return connection_; }
    QLabel* usageLabel() const { return usage_; }
    QPushButton* connectButton() const { return connect_; }
    QPushButton* newButton() const { return new_; }
    QPushButton* sendButton() const { return send_; }
    QPushButton* stopButton() const { return stop_; }
    QPushButton* stopUndoButton() const { return stopUndo_; }
    QPushButton* readAloudButton() const { return readAloud_; }
    QTextBrowser* transcript() const { return transcript_; }
    QPlainTextEdit* composer() const { return composer_; }
    QCheckBox* attachmentCheckbox() const { return attachment_; }
    QListWidget* historyList() const { return history_; }
    QPushButton* renameButton() const { return rename_; }
    QPushButton* exportButton() const { return export_; }
    QPushButton* deleteButton() const { return delete_; }
signals:
    void ChatDraftEdited(const QString& draft);
    void ChatAttachmentChanged(bool attached);
    void ChatSubmitted(const QString& draft);
    void ChatNewRequested();
    void ChatHistoryRequested();
    void ChatSelected(const QString& id);
    void ChatReadAloudRequested(const QString& messageId,const QString& text);
    void AISettingsRequested();
    void StopRequested();
    void StopAndUndoRequested();
    void EscapeRequested();
protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched,QEvent* event) override;
private:
    void OpenDrawer(QWidget* drawer);
    void RenderMessages();
    void RefreshActions();
    void RefreshTabOrder();
    void RefreshIcons();
    QWidget *header_{}, *body_{}, *bodyContent_{}, *settingsDrawer_{}, *historyDrawer_{}, *actionStrip_{};
    QScrollArea* settingsScroll_{};
    QLabel *connection_{}, *connectionDot_{}, *usage_{};
    QPushButton *connect_{}, *new_{}, *send_{}, *stop_{}, *stopUndo_{}, *readAloud_{};
    QPushButton *rename_{}, *export_{}, *delete_{};
    QPlainTextEdit* composer_{};
    QCheckBox* attachment_{};
    QTextBrowser* transcript_{};
    QListWidget* history_{};
    QPointer<QWidget> restoreFocus_;
    QJsonObject snapshot_;
    QString renderedHtml_;
    QWidget *approval_{}, *routing_{};
    bool undoAvailable_{};
    bool renderQueued_{};
};
} // namespace okuflow
#endif
