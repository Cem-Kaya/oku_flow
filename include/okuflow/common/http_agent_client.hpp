#pragma once

#include "okuflow/app/agent/app_control_service.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <functional>

namespace okuflow {

enum class HttpAgentToolMode { Off, Tools, LegacyFunctions };
struct HttpAgentConfig {
    QUrl endpoint; // exact chat/completions URL; never supplied by the model
    QString model;
    QByteArray apiKey; // Authorization header only
    HttpAgentToolMode toolMode{HttpAgentToolMode::Off};
};
struct HttpAgentTurn {
    QString contextId; // already issued by AppControlService::Begin
    QString originKey; // app-issued provider/session/generation identity
    QString systemPrompt;
    QString userPrompt;
    QByteArray jpeg; // initial app-authorized viewport, optional
    QJsonArray textHistory; // app-owned completed role pairs only; no historical images
    std::optional<agent::AuthorizedObservation> observation;
};
struct HttpAgentOutcome { QString text, errorCode; bool interrupted{}; };
struct HttpAgentCapability { bool available{}, parallelToolCallsAccepted{}; };

// One bounded, non-streaming turn. Configuration is local-only; capability
// tests require an explicit caller action. This adapter never creates grants
// or contexts and never forwards HTTP error bodies or network diagnostics.
// Public calls run on the runtime/GUI thread shared with AppControlService.
class HttpAgentClient final : public QObject {
public:
    using Completion = std::function<void(HttpAgentOutcome)>;
    using CapabilityCompletion = std::function<void(HttpAgentCapability, QString)>;
    explicit HttpAgentClient(QObject* parent = nullptr);
    ~HttpAgentClient() override;
    void Configure(const HttpAgentConfig& config);
    void SetControlService(agent::AppControlService* service);
    bool Busy() const { return active_; }
    HttpAgentCapability Capability() const;
    bool Start(HttpAgentTurn turn, Completion completion);
    bool TestCapability(CapabilityCompletion completion, bool testParallelToolCalls = false);
    void Cancel();
private:
    struct Call { QString id, name; QJsonObject arguments; };
    QString ProfileKey() const;
    bool ConfigurationValid() const;
    QJsonArray Descriptors(bool legacy) const;
    void Post();
    void ReplyFinished(quint64 generation, QNetworkReply* reply);
    bool ParseCalls(const QJsonObject& message, std::vector<Call>& calls, bool probe) const;
    void RunNextCall(quint64 generation);
    void Finish(QString text = {}, QString error = {}, bool interrupted = false);
    void StripImages();
    bool IsCurrent(quint64 generation) const;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QPointer<agent::AppControlService> service_;
    HttpAgentConfig config_;
    HttpAgentTurn turn_;
    QHash<QString,HttpAgentCapability> capabilities_;
    QJsonArray messages_, observations_;
    std::vector<Call> calls_;
    std::vector<agent::AuthorizedObservation> imageAuthorities_;
    QSet<QString> seenIds_;
    QByteArray response_;
    QTimer deadline_, contextTimer_;
    QElapsedTimer elapsed_;
    Completion completion_;
    CapabilityCompletion capabilityCompletion_;
    quint64 generation_{}, configurationEpoch_{}, serviceEpoch_{};
    int rounds_{}, callCount_{}, nextCall_{}, images_{}, imageBytes_{};
    bool active_{}, probing_{}, probingParallel_{}, tooLarge_{}, configurationChanging_{};
};
} // namespace okuflow
