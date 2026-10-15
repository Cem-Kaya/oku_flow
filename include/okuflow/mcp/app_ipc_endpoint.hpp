#pragma once
#include "okuflow/app/agent/app_control_service.hpp"
#include <QObject>
#include <memory>

namespace okuflow::mcp {
// All public methods are GUI-only trusted app calls. IPC has no method for
// enabling control, approving a peer, creating a lease or widening grants.
class AppIpcEndpoint final : public QObject {
  public:
    explicit AppIpcEndpoint(agent::AppControlService &service, QObject *parent = nullptr);
    ~AppIpcEndpoint() override;
    bool SetEnabled(bool enabled);
    QString InstanceSelector() const;
    QString PendingClientName() const;
    QString PendingPeerId() const;
    QString ActiveLease() const;
    bool ApprovePending(const QString &expectedPeerId, const agent::ControlGrants &grants);
    void Revoke();
    void SetPendingChanged(std::function<void()> callback);
#if defined(OKUFLOW_MCP_TEST_HOOKS)
    // Fixture-only: no such entry point exists in the production target.
    void SetFrameFenceTestHooks(int delayMs, std::function<void()> posted);
#endif

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace okuflow::mcp
