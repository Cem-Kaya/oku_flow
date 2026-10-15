#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)
#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

// A view of app-owned consent records. Only deliberate user activations emit
// authorization signals; incoming model/provider text is always plain text.
class AgentControlPanel final : public QWidget {
    Q_OBJECT
public:
    explicit AgentControlPanel(QWidget* parent = nullptr);
    void SetEnabled(bool embedded, bool external);
    // frames is turn/current-source scoped; photos and recording are session scoped.
    void SetSessionGrants(bool frames, bool photos, bool recording);
    void SetPendingApproval(const QJsonObject& record);
    void ClearPendingApproval();
    void FocusPendingApproval();
    void SetExternalConnection(const QString& connectionId, const QString& safeIdentity);
    void SetExternalLease(const QJsonObject& status);
    void AppendAction(const QString& text, bool assertive = false);
    // Pass source-English catalog status keys, so live language changes retain
    // the source; arbitrary safe DTO/provider text still remains plain text.
    void SetStatus(const QString& text, bool assertive = false, bool coalesce = true);
    void SetCapabilityStatus(const QString& text);
    // Without tool support the row offers the explicit capability test instead
    // of a control conversation; RefreshRoutingNotice updates a shown row only.
    void SetRoutingNotice(bool visible, bool canStartControl = true);
    void RefreshRoutingNotice(bool canStartControl);
    bool routingNoticeShown() const { return !routingRow_->isHidden(); }
    QWidget* routingNoticeWidget() const { return routingRow_; }
    QWidget* approvalWidget() const { return approvalRow_; }
    QPushButton* approvalDenyButton() const { return deny_; }
    bool hasPendingApproval() const { return !approvalId_.isEmpty(); }
    void RequestStopAndUndo();

signals:
    void enabledChanged(bool embedded, bool external);
    void sessionGrantsChanged(bool frames, bool photos, bool recording);
    void approvalResolved(const QString& approvalId, bool allow);
    void approveLeaseRequested(const QString& connectionId, const QJsonObject& grants);
    void revokeLeaseRequested();
    void stopAndUndoRequested();
    void capabilityTestRequested();
    void startControlConversationRequested();
    void sendWithoutControlRequested();
    // Connect to MainWindow's persistent accessible QLabel, outside hideable UI.
    void statusAnnouncementRequested(const QString& text, bool assertive, bool coalesce);
    void actionReceiptAdded(const QString& text, bool assertive);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void UpdateAvailability();
    void UpdateCountdowns();
    void ResolvePending(bool allow);
    void Retranslate();
    QCheckBox *photos_{}, *recording_{};
    QCheckBox *leaseFrames_{}, *leaseSource_{}, *leasePhotos_{}, *leaseRecording_{};
    QLabel *status_{}, *routingMessage_{}, *approvalSummary_{}, *approvalExpiry_{}, *connection_{}, *leaseStatus_{}, *capability_{};
    QPlainTextEdit *approvalArguments_{}, *transcript_{};
    QWidget *approvalRow_{}, *externalSection_{}, *bundle_{}, *routingRow_{};
    QWidget* approvalDetails_{};
    QPushButton *allow_{}, *deny_{}, *reviewLease_{}, *issueLease_{}, *revokeLease_{};
    QPushButton *startControl_{}, *routingTest_{};
    QString approvalId_, connectionId_, connectionIdentity_, capabilityText_;
    QJsonObject approvalRecord_, leaseRecord_;
    QElapsedTimer approvalClock_, leaseClock_;
    qint64 approvalRemainingMs_{}, leaseAbsoluteMs_{}, leaseIdleMs_{};
    quint64 approvalSequence_{};
    QTimer* countdown_{};
    bool embeddedEnabled_{},externalEnabled_{},framesGranted_{};
};
} // namespace okuflow
#endif
