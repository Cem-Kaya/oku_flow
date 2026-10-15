#include "mcp_synthetic_target.hpp"
#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <iostream>
#include <thread>
#include <windows.h>

using namespace okuflow;
void Output(QJsonObject json) {
    const auto b = QJsonDocument(json).toJson(QJsonDocument::Compact);
    std::fwrite(b.data(), 1, b.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    FixtureTarget target;
    auto edition = agent::BuiltControlCatalogEdition();
    if (app.arguments().contains("--catalog-mismatch"))
        edition = edition == agent::CatalogEdition::DevCore ? agent::CatalogEdition::DevCamera
                                                            : agent::CatalogEdition::DevCore;
    agent::AppControlService service(target, edition);
    target.sourceChanged = [&] { service.SourceChanged(); };
    target.accepted = Output;
    mcp::AppIpcEndpoint endpoint(service);
    service.SetEnabled(false, true);
    if (!endpoint.SetEnabled(true))
        return 3;
    Output({{"type", "ready"},
            {"selector", endpoint.InstanceSelector()},
            {"catalogEdition", QString::fromUtf8(agent::CatalogEditionName(edition).data(),
                                                 agent::CatalogEditionName(edition).size())}});
    QString displayedPeer;
    endpoint.SetPendingChanged([&] {
        const auto id = endpoint.PendingPeerId();
        if (!id.isEmpty() && id != displayedPeer)
            Output({{"type", "pending"}, {"name", endpoint.PendingClientName()}, {"peerId", id}});
        displayedPeer = id;
    });
    std::thread input([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            QMetaObject::invokeMethod(
                &app,
                [&, line] {
                    if (line.starts_with("approve-all:"))
                        Output({{"type", "approved"},
                                {"ok", endpoint.ApprovePending(QString::fromStdString(line.substr(12)),
                                                               {true, true, true, true})}});
                    else if (line == "settle") {
                        target.Settle();
                        Output({{"type", "settled"}});
                    } else if (line == "hold-photo") {
                        target.holdPhoto = true;
                        Output({{"type", "photo-held"}});
                    } else if (line == "release-photo") {
                        target.Release();
                        Output({{"type", "photo-released"}});
                    } else if (line == "fixture-status") {
                        auto status = target.State();
                        status["type"] = "fixture-status";
                        Output(status);
                    } else if (line == "manual-recording") {
                        target.ManualRecording();
                        Output({{"type", "manual-started"}, {"sessionId", target.RecordingSession()}});
                    } else if (line == "user-zoom") {
                        target.UserZoom();
                        service.NativeFieldsChanged({agent::Field::ZoomAmount});
                        Output({{"type", "user-changed"}});
                    } else if (line == "assistant-equal" || line == "assistant-short" ||
                               line == "assistant-distinct") {
                        target.SetAssistantProfile(QString::fromStdString(line.substr(10)));
                        Output({{"type", "assistant-profile-set"}});
                    } else if (line.starts_with("approve-frames:"))
                        Output({{"type", "approved"},
                                {"ok", endpoint.ApprovePending(QString::fromStdString(line.substr(15)),
                                                               {true, false, false, false})}});
                    else if (line.starts_with("approve:"))
                        Output({{"type", "approved"},
                                {"ok", endpoint.ApprovePending(QString::fromStdString(line.substr(8)), {})}});
                    else if (line == "revoke") {
                        endpoint.Revoke();
                        Output({{"type", "revoked"}});
                    } else if (line == "off") {
                        QElapsedTimer timer;
                        timer.start();
                        endpoint.SetEnabled(false);
                        Output({{"type", "off"}, {"elapsedMs", timer.elapsed()}});
                    } else if (line == "hold-fence") {
                        endpoint.SetFrameFenceTestHooks(900, [] { Output({{"type", "fence-posted"}}); });
                        Output({{"type", "fence-armed"}});
                    } else if (line == "exit") {
                        endpoint.SetEnabled(false);
                        app.quit();
                    }
                },
                Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    QTimer::singleShot(90000, &app, &QCoreApplication::quit);
    int result = app.exec();
    if (input.joinable()) {
        CancelSynchronousIo(input.native_handle());
        input.join();
    }
    return result;
}
