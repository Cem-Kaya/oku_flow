#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace okuflow {

// Offline compatibility gate for constrained ordinary vision/Assistant and
// dedicated agent children. This does not prove live provider/tool acceptance.
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
                                            const QJsonArray& mcpInventory,
                                            bool allowWebSearch = false);
    static QString FeatureInventoryDigest(const QString& featureOutput);
    static QJsonObject Initialization();
    static QStringList DisabledFeatures();
};

} // namespace okuflow
