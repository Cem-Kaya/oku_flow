#pragma once
#if defined(_WIN32) || defined(Q_MOC_RUN)
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/common/codex_json_rpc_process.hpp"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <map>
#include <memory>

namespace okuflow {
// Dedicated restricted child. No ordinary Assistant history, thread/resume,
// shell, external MCP, account management or implicit inference is exposed.
// TestCapability is the sole tiny inference probe, initiated by native Send or
// the optional diagnostic button; configuration/startup never initiates it.
class CodexAgentClient final : public QObject {
    Q_OBJECT
public:
    explicit CodexAgentClient(QObject* parent = nullptr);
    ~CodexAgentClient() override;
    void Configure(QString executable, QString model, QString effort, QString instructions);
    void SetControlService(agent::AppControlService* service);
    void TestCapability();
    bool IsReady() const { return available_; }
    bool IsTurnActive() const { return active_; }
    bool SubmitTurn(QString contextId, QString prompt, QByteArray jpeg = {},
                    QJsonArray completedTextContext = {},
                    std::optional<agent::AuthorizedObservation> observation = {});
    void Cancel();
    void ResetConversation();
    void Shutdown();
signals:
    void AvailabilityChanged(bool available, const QString& code);
    void ConversationCreated(const QJsonObject& thread);
    void TurnStarted(const QString& threadId, const QString& turnId);
    void TextDelta(const QString& threadId, const QString& turnId, const QString& delta);
    void TurnFinished(const QString& threadId, const QString& turnId, const QString& text,
                      const QString& errorCode, bool interrupted);
private:
    struct Call { QJsonValue id; QJsonObject params; quint64 epoch{}, generation{}; qint64 deadline{}; };
    void Preflight(int stage);
    void Initialize();
    void StartThread();
    void StartTurn();
    bool HandleCall(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void DispatchCall(Call call);
    void Answer(const Call& call, agent::ControlResult result);
    void HandleNotification(const QString& method, const QJsonObject& params);
    void Finish(QString code = {}, bool interrupted = false);
    void FailSession(const QString& code);
    void Retire(const QString& thread, const QString& turn);
    bool Retired(const QString& thread, const QString& turn) const;
    void Tick();
    QString CallIdentity(const QJsonValue& id) const;
    QString ControlKey(const Call& call) const;
    QJsonArray Tools(bool probe) const;
    qint64 Now() const;
    std::unique_ptr<CodexJsonRpcProcess> rpc_;
    QProcess preflight_;
    QTimer stageDeadline_, watchdog_;
    QElapsedTimer clock_;
    QPointer<agent::AppControlService> service_;
    QString executable_, model_, effort_, instructions_, workspace_;
    QString version_, features_;
    QByteArray preflightBytes_;
    QString context_, prompt_, thread_, turn_, text_;
    QByteArray jpeg_;
    QJsonArray completedTextContext_;
    std::optional<agent::AuthorizedObservation> initialImageAuthorization_;
    QStringList retired_;
    std::vector<Call> buffered_;
    std::vector<std::pair<QString,QJsonObject>> earlyEvents_;
    std::map<QString,Call> calls_;
    quint64 epoch_{}, generation_{};
    quint64 configurationEpoch_{}, serviceEpoch_{};
    qint64 started_{}, activity_{};
    int anomalies_{}, imageBase64Bytes_{};
    bool initialized_{}, available_{}, probing_{}, active_{}, startPending_{}, stopping_{};
};
} // namespace okuflow
#endif
