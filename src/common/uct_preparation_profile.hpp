#pragma once
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

namespace okuflow::uct {
// Wall intervals on the calling preparation thread. Separate prefix keeps GPU
// per-read event consumers unchanged. No timer or JSON work when disabled.
class PreparationProfile {
public:
    explicit PreparationProfile(const char* role)
        : enabled_(qEnvironmentVariable("OKUFLOW_UCT_TRT_PROFILE") == "1"), role_(role) {
        if (enabled_) timer_.start();
    }
    void Mark(const char* phase) noexcept {
        if (!enabled_) return;
        try {
        const auto now = timer_.nsecsElapsed();
        qInfo().noquote() << "UCT_TRT_PREPARE" << QJsonDocument(QJsonObject{
            {"schema", 1}, {"role", role_}, {"phase", phase},
            {"wallMs", double(now - previous_) / 1.e6},
            {"elapsedMs", double(now) / 1.e6}}).toJson(QJsonDocument::Compact);
        previous_ = timer_.nsecsElapsed();
        } catch (...) { /* Diagnostics must never change preparation ownership. */ }
    }
private:
    bool enabled_;
    const char* role_;
    QElapsedTimer timer_;
    qint64 previous_{};
};
} // namespace okuflow::uct
