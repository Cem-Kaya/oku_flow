#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace okuflow {

// Offline compatibility gate for the dedicated agent child. This does not
// prove dynamic-tool runtime acceptance, nor alter the ordinary Codex child.
struct CodexAgentLaunchProfile {
    bool available{};
    QString errorCode;
    QStringList arguments;
};

class CodexAgentProfile final {
public:
    static constexpr auto Version = "0.160.1";
    static CodexAgentLaunchProfile Validate(const QString& versionOutput,
                                            const QString& featureOutput,
                                            const QJsonArray& mcpInventory);
    static QString FeatureInventoryDigest(const QString& featureOutput);
    static QJsonObject Initialization();
    static QStringList DisabledFeatures();
};

} // namespace okuflow
