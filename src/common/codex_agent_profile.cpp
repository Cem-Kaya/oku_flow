#include "okuflow/common/codex_agent_profile.hpp"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace okuflow {
namespace {
constexpr auto kInventoryDigest =
    "8559a983f25b11c3eb4d8de7c208439826556ceec3b261d1b84c9ef4e2477fcb";
const QRegularExpression kFeatureRow(
    QStringLiteral("^([A-Za-z0-9_.-]+)\\s+(.+?)\\s+(true|false)\\s*$"));
const QRegularExpression kBareMcpName(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));

CodexAgentLaunchProfile Unavailable(const char* code)
{
    return {false, QString::fromLatin1(code), {}};
}
} // namespace

QString CodexAgentProfile::FeatureInventoryDigest(const QString& output)
{
    if (output.size() > 128 * 1024) return {};
    QStringList rows;
    QSet<QString> names;
    for (const QString& row : output.split(QLatin1Char('\n'))) {
        if (row.trimmed().isEmpty()) continue;
        const auto match = kFeatureRow.match(row);
        if (!match.hasMatch() || names.contains(match.captured(1))) return {};
        names.insert(match.captured(1));
        rows.append(match.captured(1) + QLatin1Char('|') + match.captured(2).trimmed());
    }
    if (rows.size() != 154) return {};
    std::sort(rows.begin(), rows.end());
    return QString::fromLatin1(QCryptographicHash::hash(
        rows.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex());
}

QStringList CodexAgentProfile::DisabledFeatures()
{
    // Include optional variants so user-enabled features cannot widen this
    // child. The complete name/stability inventory is pinned above.
    return {QStringLiteral("apps"), QStringLiteral("plugins"),
        QStringLiteral("image_generation"), QStringLiteral("view_image"), QStringLiteral("multi_agent"),
        QStringLiteral("multi_agent_v2"), QStringLiteral("tool_suggest"),
        QStringLiteral("shell_tool"),
        QStringLiteral("code_mode"), QStringLiteral("code_mode_only"),
        QStringLiteral("computer_use"), QStringLiteral("browser_use"),
        QStringLiteral("browser_use_external"), QStringLiteral("browser_use_full_cdp_access"),
        QStringLiteral("in_app_browser"), QStringLiteral("in_app_local_automation"),
        QStringLiteral("hooks"), QStringLiteral("plugin_hooks"),
        QStringLiteral("remote_plugin"), QStringLiteral("skill_search"),
        QStringLiteral("skill_mcp_dependency_install")};
}

CodexAgentLaunchProfile CodexAgentProfile::Validate(
    const QString& version, const QString& features, const QJsonArray& mcp, bool allowWebSearch)
{
    if (version.trimmed() != QStringLiteral("codex-cli 0.160.1"))
        return Unavailable("unsupported_codex_version");
    if (FeatureInventoryDigest(features) != QLatin1String(kInventoryDigest))
        return Unavailable("unsupported_codex_features");
    if (mcp.size() > 128) return Unavailable("invalid_mcp_inventory");
    QStringList args{QStringLiteral("app-server"), QStringLiteral("--listen"),
                     QStringLiteral("stdio://")};
    for (const QString& feature : DisabledFeatures()) {
        args << QStringLiteral("--disable") << feature;
    }
    // The selected model may require code_mode_only in its catalog metadata.
    // Its host routes declared tools; it does not grant shell, filesystem,
    // browser, MCP or agent tools. Those providers remain disabled above.
    // Explicitly enable the host even when the user's ordinary CLI disables it.
    args << QStringLiteral("--enable") << QStringLiteral("code_mode_host");
    args << QStringLiteral("-c") << (allowWebSearch ? QStringLiteral("web_search=\"live\"") : QStringLiteral("web_search=\"disabled\""))
         << QStringLiteral("-c") << QStringLiteral("approval_policy=\"never\"")
         << QStringLiteral("-c") << QStringLiteral("sandbox_mode=\"read-only\"");
    // Apply the table override first: a later {} would erase the explicit
    // child disables before Codex merges configured servers into the table.
    args << QStringLiteral("-c") << QStringLiteral("mcp_servers={}");
    QSet<QString> seen;
    for (const QJsonValue& value : mcp) {
        if (!value.isObject()) return Unavailable("invalid_mcp_inventory");
        const auto object = value.toObject();
        const auto nameValue = object.value(QStringLiteral("name"));
        if (!nameValue.isString()) return Unavailable("invalid_mcp_inventory");
        const QString name = nameValue.toString();
        if (!kBareMcpName.match(name).hasMatch() || seen.contains(name))
            return Unavailable("invalid_mcp_server_name");
        seen.insert(name);
        args << QStringLiteral("-c")
             << QStringLiteral("mcp_servers.%1={enabled=false,command=\"codex.exe\"}").arg(name);
    }
    // CLI table overrides merge with configured tables: {} alone does not
    // remove ambient servers. Explicitly disable every inventoried bare key.
    // Supply a complete inert transport: enabled=false alone can replace a
    // configured transport with an invalid partial record. This command is
    // never started. Never copy configured transports or edit config.toml.
    return {true, {}, args};
}

QJsonObject CodexAgentProfile::Initialization()
{
    return {{QStringLiteral("clientInfo"), QJsonObject{
        {QStringLiteral("name"), QStringLiteral("okuflow_agent_control")},
        {QStringLiteral("title"), QStringLiteral("OkuFlow Assistant control")},
        {QStringLiteral("version"), QStringLiteral("1")}}},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("experimentalApi"), true}}}};
}
} // namespace okuflow
