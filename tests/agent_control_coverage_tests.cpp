// Plan 52 expanded-control coverage review (2026-10-14).
//
// Evidence classes stay separate in every slot:
//   [registry]  immutable Qt-free registry plus the tracked inventory data;
//   [validator] the app's QJson schema subset versus the Qt-free bridge subset;
//   [planner]   production pure image/assistant planners, without OkuFlowApp;
//   [native]    production per-frame normalizer (ProcessingSettings), no GPU;
//   [service]   production AppControlService with a labelled fake target.
// Nothing here opens a window, camera, provider, account, pipe or settings file,
// and no slot establishes native-handler, hardware or live-provider acceptance.
//
// Parent takeover: reviewed defects are hard regressions. A known defect must
// fail the gate until it is repaired; expected failures never count as coverage.
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/assistant_parameter_policy.hpp"
#include "okuflow/app/agent/recording_parameter_policy.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/app_language.hpp"
#include "okuflow/app/color_schemes.hpp"
#include "okuflow/app/constants.hpp"
#include "okuflow/app/settings_store.hpp"
#include "okuflow/cuda/cuda_interop.hpp"
#include "okuflow/mcp/json.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>
#include <regex>
#include <set>

using namespace okuflow::agent;
namespace mcp = okuflow::mcp;

namespace {
constexpr std::array kEditions{CatalogEdition::DevCore, CatalogEdition::DevCapture, CatalogEdition::DevCamera,
                               CatalogEdition::V1, CatalogEdition::DevFull};
QString Id(std::string_view text) { return QString::fromUtf8(text.data(), qsizetype(text.size())); }
QJsonObject Object(std::string_view text) {
    return QJsonDocument::fromJson(QByteArray(text.data(), qsizetype(text.size()))).object();
}
QJsonObject SchemaOf(std::string_view id) { return Object(FindParameter(id)->schema); }
QJsonObject Inventory() {
    QFile file(QFINDTESTDATA("data/app_control_inventory.json"));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
QString Emoji() { constexpr char32_t smile = 0x1F600; return QString::fromUcs4(&smile, 1); }
QString Hex32() { return QStringLiteral("0123456789abcdef0123456789abcdef"); }

bool AppAccepts(const QJsonValue& value, const QJsonObject& schema) { return MatchesControlSchema(value, schema); }
bool BridgeAccepts(const QJsonValue& value, const QJsonObject& schema) {
    const auto wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).toStdString();
    const auto parsed = mcp::Json::Parse(wrapped).Elements().front();
    return mcp::MatchesSchema(parsed, mcp::Json::Parse(QJsonDocument(schema).toJson(QJsonDocument::Compact).toStdString()));
}

// A valid representative for every pattern the registry or catalog emits. A new
// pattern must add its sample here, so no wire pattern escapes the corpus.
std::optional<QString> PatternSample(const QString& pattern) {
    const std::map<QString, QString> samples{
        {"^#[0-9a-fA-F]{6}$", "#A1b2C3"},
        {"^cam-[a-f0-9]{32}$", "cam-" + Hex32()},
        {"^(automatic|fmt-[a-f0-9]{32})$", "fmt-" + Hex32()},
        {"^(none|mic-[a-f0-9]{32})$", "mic-" + Hex32()},
        {"^(system|voice-[a-f0-9]{32})$", "system"},
        {"^dir-[a-f0-9]{32}$", "dir-" + Hex32()},
        {"^[a-z0-9-]{1,64}$", "preset-document"},
        {"^[A-Za-z0-9_.-]+$", "image.contrast"}};
    const auto found = samples.find(pattern);
    return found == samples.end() ? std::nullopt : std::optional<QString>(found->second);
}

// Defaults come from the production settings struct, mirroring the shape the
// target supplies; no live configuration or widget is read.
std::map<Field, QJsonValue> ImageDefaults() {
    const okuflow::settings::AdvancedConfig c;
    return {{Field::ZoomEnabled, c.zoomEnabled}, {Field::ZoomAmount, double(c.zoomAmount)},
            {Field::CenterX, double(c.zoomCenterX)}, {Field::CenterY, double(c.zoomCenterY)},
            {Field::FocusMarker, c.focusMarker}, {Field::Framing, QStringLiteral("fill")},
            {Field::PresentationRate, QStringLiteral("auto-up-to-120")}, {Field::VirtualJoystick, false},
            {Field::ZoomWheelAcceleration, true}, {Field::BlackWhite, c.blackWhiteEnabled},
            {Field::Threshold, double(c.blackWhiteThreshold)}, {Field::Contrast, double(c.contrast)},
            {Field::Brightness, double(c.brightness)}, {Field::AutoContrast, c.autoContrastEnabled},
            {Field::AutoContrastStrength, double(c.autoContrastStrength)}, {Field::Blur, c.blurEnabled},
            {Field::BlurSigma, double(c.blurSigma)}, {Field::BlurRadius, c.blurRadius},
            {Field::Temporal, c.temporalSmoothEnabled}, {Field::TemporalAlpha, double(c.temporalSmoothAlpha)},
            {Field::Spatial, c.spatialSharpenEnabled}, {Field::Upscaler, c.spatialUpscaler},
            {Field::Sharpness, double(c.spatialSharpness)}, {Field::Stabilization, c.stabilizationEnabled},
            {Field::Keystone, c.keystoneEnabled}, {Field::DebugView, c.debugView},
            {Field::TextClarity, c.autoTextClarityEnabled}, {Field::Flatten, c.backgroundFlattenEnabled},
            {Field::FlattenStrength, double(c.backgroundFlattenStrength)},
            {Field::Adaptive, c.adaptiveBinarizationEnabled}, {Field::Sauvola, double(c.sauvolaStrength)},
            {Field::Softness, double(c.binarizationSoftness)}, {Field::Polarity, c.textPolarityMode},
            {Field::Stroke, c.strokeWeight}, {Field::SmartSharpen, c.smartSharpenEnabled},
            {Field::SmartStrength, double(c.smartSharpenStrength)}, {Field::Clahe, c.claheEnabled},
            {Field::ClaheLimit, double(c.claheClipLimit)}, {Field::TwoColor, c.twoColorTextEnabled},
            {Field::Hysteresis, c.textHysteresisEnabled},
            {Field::HysteresisStrength, double(c.textHysteresisStrength)},
            {Field::SelectiveSharpen, c.selectiveSharpenEnabled}, {Field::FocusDetection, c.focusDetectionEnabled},
            {Field::FocusThreshold, double(c.focusThreshold)}, {Field::Glare, c.glareSuppressionEnabled},
            {Field::GlareStrength, double(c.glareSuppressionStrength)}, {Field::SuperResEnabled, c.mlSuperResEnabled},
            {Field::SuperResStrength, double(c.mlSuperResStrength)}, {Field::SuperResPrefer2x, c.mlSuperResPrefer2x},
            {Field::SuperResUltra1440p, c.mlSuperResUltra1440p}, {Field::SuperResPerformanceOverride, false},
            {Field::BumpHold, false}, {Field::KeystonePaused, false}, {Field::Scheme, QStringLiteral("normal")},
            {Field::CustomColorMode, QStringLiteral("gradient")}, {Field::CustomColorStopCount, 2},
            {Field::CustomColorStops, QJsonArray{"#000000", "#ffffff"}}, {Field::CustomColorStepped, false},
            {Field::CustomColorRecipe, QJsonObject{}}};
}
ControlPlan PlanImage(const std::map<Field, QJsonValue>& current, ToolId tool, const QJsonObject& args) {
    return AppControlTarget::PlanImageParameterDependencies(current, tool, args);
}
int WriteIndex(const ControlPlan& plan, Field field) {
    for (std::size_t i = 0; i < plan.writes.size(); ++i)
        if (plan.writes[i].field == field) return int(i);
    return -1;
}
std::optional<QJsonValue> Written(const ControlPlan& plan, Field field) {
    const auto index = WriteIndex(plan, field);
    return index < 0 ? std::nullopt : std::optional<QJsonValue>(plan.writes[std::size_t(index)].value);
}
// Native widget scales from MainWindow slider ranges and OkuFlowApp handlers.
double NativeScale(const QString& id) {
    if (id == "image.blackWhite.threshold") return 255;
    if (id == "image.blur.sigma" || id == "image.textClarity.clahe.clipLimit") return 10;
    if (id == "image.textClarity.focusDetection.threshold") return 1000;
    return 100;
}
QJsonValue NativeNumber(const QString& id, bool integer, double value) {
    if (integer) return id == "image.blur.radius" ? okuflow::app_constants::SnapBlurRadius(int(std::lround(value)))
                                                  : int(std::lround(value));
    const auto steps = std::lround(value * NativeScale(id));
    if (id == "image.blur.sigma") return double(okuflow::app_constants::SliderValueToSigma(int(steps)));
    return double(float(steps) / float(NativeScale(id)));
}
bool IsImageOrView(const ParameterDescriptor& p) {
    return p.group == ParameterGroup::Image || p.group == ParameterGroup::View;
}
bool IsCustomComponent(const QString& id) { return id.startsWith("image.colors.custom."); }

// Compact metadata traversal through the production paging function.
std::vector<QJsonObject> MetadataItems(std::string_view group, std::size_t limit, int* pages = nullptr,
                                       qsizetype* largest = nullptr) {
    std::vector<QJsonObject> items;
    std::string after;
    for (int page = 0; page < 1000; ++page) {
        const auto bytes = ParameterMetadataPage(group, after, limit, CatalogEdition::DevFull);
        if (largest) *largest = std::max<qsizetype>(*largest, qsizetype(bytes.size()));
        const auto object = Object(bytes);
        if (!object["ok"].toBool()) break;
        if (pages) ++*pages;
        for (const auto item : object["items"].toArray()) items.push_back(item.toObject());
        if (!object["hasMore"].toBool()) break;
        after = object["afterField"].toString().toStdString();
    }
    return items;
}

const QSet<QString> kSupportedKeywords{"$schema", "type", "properties", "required", "additionalProperties",
    "minProperties", "maxProperties", "items", "minItems", "maxItems", "uniqueItems", "minimum", "maximum",
    "minLength", "maxLength", "pattern", "enum", "const", "oneOf", "not", "default"};
bool BoundedPattern(const QString& pattern) {
    return !pattern.contains('*') && !pattern.contains('+') &&
           QRegularExpression(QStringLiteral("\\{\\d+(,\\d+)?\\}\\)?\\$$")).match(pattern).hasMatch();
}
struct SchemaAudit {
    QStringList unsupported, unbounded, patterns, defaults;
};
void Walk(const QJsonValue& value, const QString& path, bool input, SchemaAudit& audit) {
    if (value.isBool()) return;
    const auto schema = value.toObject();
    for (auto it = schema.begin(); it != schema.end(); ++it)
        if (!kSupportedKeywords.contains(it.key())) audit.unsupported << path + ":" + it.key();
    if (schema.contains("pattern")) audit.patterns << schema["pattern"].toString();
    if (schema.contains("default")) {
        auto copy = schema; copy.remove("default");
        if (!AppAccepts(schema["default"], copy) || !BridgeAccepts(schema["default"], copy)) audit.defaults << path;
    }
    const auto type = schema["type"].toString();
    if (input && !schema.contains("enum") && !schema.contains("const")) {
        if (type == "string" && !schema.contains("maxLength") && !BoundedPattern(schema["pattern"].toString()))
            audit.unbounded << path;
        if ((type == "number" || type == "integer") && !(schema.contains("minimum") && schema.contains("maximum")))
            audit.unbounded << path;
        if (type == "array" && !schema.contains("maxItems")) audit.unbounded << path;
        if (type == "object" && schema["additionalProperties"] != QJsonValue(false)) audit.unbounded << path;
    }
    const auto properties = schema["properties"].toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) Walk(it.value(), path + "." + it.key(), input, audit);
    if (schema.contains("items")) Walk(schema["items"], path + "[]", input, audit);
    if (schema["additionalProperties"].isObject()) Walk(schema["additionalProperties"], path + ".*", input, audit);
    const auto branches = schema["oneOf"].toArray();
    for (qsizetype i = 0; i < branches.size(); ++i) Walk(branches[i], path + "|" + QString::number(i), input, audit);
    if (schema.contains("not")) Walk(schema["not"], path + "!", false, audit);
}
SchemaAudit AuditEverySchema() {
    SchemaAudit audit;
    std::set<std::string_view> seen;
    for (const auto edition : kEditions)
        for (const auto& tool : ControlRegistry(edition)) {
            if (!seen.insert(tool.canonicalId).second) continue;
            Walk(Object(tool.inputSchema), Id(tool.canonicalId), true, audit);
            Walk(Object(tool.outputSchema), Id(tool.canonicalId) + ">", false, audit);
        }
    for (const auto& p : ParameterRegistry()) Walk(Object(p.schema), Id(p.id), true, audit);
    audit.unbounded.removeDuplicates();
    return audit;
}

// Labelled fake target: it records effects only. Plans come either from a test
// lambda or from the production pure planners; it never models native handlers.
class FakeTarget final : public IAppControlTarget {
public:
    std::map<Field, QJsonValue> fields;
    std::function<ControlPlan(ToolId, const QJsonObject&)> prepare;
    std::function<bool(const FieldWrite&)> apply;
    std::function<QJsonObject(ToolId, const QJsonObject&)> query;
    std::vector<ControlCompletion> held;
    mutable int prepares{};
    int applies{}, saves{}, executes{};
    QJsonObject State() const override { return {{"fixture", true}}; }
    std::map<Field, QJsonValue> Fields() const override { return fields; }
    ControlPlan Prepare(ToolId tool, const QJsonObject& args, const QString&) const override {
        ++prepares;
        return prepare ? prepare(tool, args) : ControlPlan{};
    }
    bool Apply(const FieldWrite& write) override {
        ++applies;
        if (apply) return apply(write);
        fields[write.field] = write.value;
        return true;
    }
    bool Save() override { ++saves; return true; }
    QJsonObject Query(ToolId tool, const QJsonObject& args) const override { return query ? query(tool, args) : QJsonObject{}; }
    void Execute(ToolId, const QJsonObject&, const QString&, ContextKind, ControlCompletion completion) override {
        ++executes; held.push_back(std::move(completion));
    }
    bool ModalActive() const override { return false; }
    bool UserInteracting() const override { return false; }
    bool RecordingActive() const override { return false; }
    QString RecordingSession() const override { return {}; }
    QString RecordingOwner() const override { return QStringLiteral("manual"); }
    void StopOwnedRecording(const QString&) override {}
};
ControlResult Call(AppControlService& service, const QString& context, const QString& key, const QString& tool,
                   const QJsonObject& args = {}) {
    auto result = std::make_shared<std::optional<ControlResult>>();
    service.Submit({context, key, tool, args}, [result](ControlResult value) { *result = std::move(value); });
    for (int i = 0; i < 50 && !*result; ++i) QCoreApplication::processEvents();
    return *result ? **result : ControlResult::Failure("timeout");
}
QString Error(const ControlResult& result) { return result.content["error"].toObject()["code"].toString(); }
QString Embedded(AppControlService& service, ControlGrants grants = {}) {
    service.SetEnabled(true, true);
    return service.Begin({ContextKind::EmbeddedTurn, QStringLiteral("trusted-session"), 1, grants});
}
} // namespace

class AgentControlCoverageTests final : public QObject {
    Q_OBJECT
private slots:
    // [registry] Field-by-field reconciliation of the tracked inventory and the
    // compiled registry; group counts are derived, never sampled.
    void inventoryAndRegistryAgreeForEveryAdjustableField() {
        const auto inventory = Inventory();
        QVERIFY(!inventory.isEmpty());
        const auto controls = inventory["controls"].toArray();
        QCOMPARE(controls.size(), qsizetype(ParameterRegistry().size()));
        std::map<QString, int> inventoryGroups, registryGroups;
        QStringList advisoryOnly;
        std::function<void(const QJsonObject&, const QJsonObject&, const QString&)> shared =
            [&](const QJsonObject& tracked, const QJsonObject& compiled, const QString& path) {
                for (auto it = tracked.begin(); it != tracked.end(); ++it) {
                    if (it.key() == "multipleOf" || it.key() == "format") { advisoryOnly << path + ":" + it.key(); continue; }
                    QVERIFY2(compiled.contains(it.key()), qPrintable(path + " lacks " + it.key()));
                    if (it.key() == "properties") {
                        const auto trackedProperties = it.value().toObject(), compiledProperties = compiled["properties"].toObject();
                        QCOMPARE(compiledProperties.keys(), trackedProperties.keys());
                        for (auto p = trackedProperties.begin(); p != trackedProperties.end(); ++p)
                            shared(p.value().toObject(), compiledProperties[p.key()].toObject(), path + "." + p.key());
                    } else if (it.key() == "items") {
                        shared(it.value().toObject(), compiled["items"].toObject(), path + "[]");
                    } else {
                        QVERIFY2(compiled[it.key()] == it.value(), qPrintable(path + " differs at " + it.key()));
                    }
                }
            };
        for (const auto entry : controls) {
            const auto control = entry.toObject();
            const auto id = control["fieldId"].toString();
            const auto* p = FindParameter(id.toStdString());
            QVERIFY2(p, qPrintable(id));
            ++inventoryGroups[control["group"].toString()];
            QCOMPARE(Id(ParameterGroupName(p->group)), control["group"].toString());
            QCOMPARE(Id(p->persistence), control["nativePersistence"].toString());
            QCOMPARE(Id(p->units), control["units"].toString());
            QCOMPARE(Id(p->nativeSource), control["native"].toObject()["source"].toString());
            QCOMPARE(Id(p->nativeHandler), control["native"].toObject()["handler"].toString());
            shared(control["schema"].toObject(), Object(p->schema), id);
        }
        for (const auto& p : ParameterRegistry()) ++registryGroups[Id(ParameterGroupName(p.group))];
        const std::map<QString, int> expected{{"image", 49}, {"view", 9}, {"camera", 5}, {"recording", 2},
            {"assistant_preferences", 10}, {"notes_output", 2}, {"ui", 9}, {"annotation", 8}};
        QVERIFY(registryGroups == inventoryGroups);
        QVERIFY(registryGroups == expected);
        // multipleOf/format exist only in the tracked inventory. Neither wire
        // validator enforces them; the compiled schemas intentionally omit them.
        qInfo("inventory-only advisory keywords: %lld", static_cast<long long>(advisoryOnly.size()));
    }

    // [registry] Protected values and native-only workflows are discoverable
    // metadata only; no edition may publish them as a tool or setter property.
    void protectedAndWorkflowIdentitiesNeverBecomeTools() {
        const auto inventory = Inventory();
        std::set<QString> registryProtected, inventoryProtected, registryActions, inventoryActions;
        const auto protectedRegistry = ProtectedControlRegistry();
        for (const auto& p : protectedRegistry) QVERIFY(registryProtected.insert(Id(p.id)).second);
        for (const auto entry : inventory["protectedControls"].toArray()) {
            const auto o = entry.toObject();
            const auto id = o["fieldId"].toString();
            inventoryProtected.insert(id);
            const auto found = std::find_if(protectedRegistry.begin(), protectedRegistry.end(),
                                            [&](const auto& p) { return Id(p.id) == id; });
            QVERIFY2(found != protectedRegistry.end(), qPrintable(id));
            QCOMPARE(found->secret, o["mcpAccess"].toString() == "never_read_or_write");
        }
        std::set<QString> extra;
        std::set_difference(registryProtected.begin(), registryProtected.end(), inventoryProtected.begin(),
                            inventoryProtected.end(), std::inserter(extra, extra.begin()));
        const std::set<QString> leaseFields{"assistant.externalLease.frames", "assistant.externalLease.photos",
                                            "assistant.externalLease.recording", "assistant.externalLease.source"};
        QVERIFY(extra == leaseFields);
        for (const auto edition : kEditions)
            for (const auto& tool : ControlRegistry(edition)) {
                QVERIFY(!registryProtected.contains(Id(tool.canonicalId)));
                const auto properties = Object(tool.inputSchema)["properties"].toObject();
                for (const auto& id : registryProtected) {
                    QVERIFY2(!properties.contains(id), qPrintable(Id(tool.canonicalId) + " exposes " + id));
                    QVERIFY(!AppAccepts(QJsonObject{{id, true}}, Object(tool.inputSchema)));
                    QVERIFY(!BridgeAccepts(QJsonObject{{id, true}}, Object(tool.inputSchema)));
                }
            }
        for (const auto& action : ControlActionRegistry()) QVERIFY(registryActions.insert(Id(action.id)).second);
        for (const auto entry : inventory["actions"].toArray()) inventoryActions.insert(entry.toObject()["actionId"].toString());
        QVERIFY(registryActions == inventoryActions);
        QCOMPARE(registryActions.size(), std::size_t(39));
        for (const auto& action : ControlActionRegistry()) {
            bool published = false;
            for (const auto edition : kEditions) published |= FindControlTool(action.id, edition) != nullptr;
            if (action.status == "native-ui-only") QVERIFY2(!published, qPrintable(Id(action.id)));
            else {
                QCOMPARE(Id(action.status), QStringLiteral("existing-tool-runtime-gated"));
                QVERIFY2(FindControlTool(action.id, CatalogEdition::DevFull), qPrintable(Id(action.id)));
            }
        }
    }

    // [registry] Finite choices that have a production source of truth must
    // match it exactly, including every built-in reading color and language.
    void registryEnumsFollowProductionChoiceSources() {
        const auto choices = [](std::string_view id) {
            QStringList out;
            for (const auto value : SchemaOf(id)["enum"].toArray())
                out << (value.isString() ? value.toString() : QString::number(value.toInt()));
            return out;
        };
        QStringList schemes{"custom"};
        for (const auto& scheme : okuflow::color_schemes::BuiltInColorSchemes()) schemes << scheme.id;
        QStringList registrySchemes = choices("image.colors.schemeId");
        schemes.sort(); registrySchemes.sort();
        QCOMPARE(registrySchemes, schemes);
        QStringList languages{"system"};
        for (const auto language : okuflow::SupportedAppLanguages()) languages << okuflow::AppLanguageCode(language);
        QStringList registryLanguages = choices("ui.language");
        languages.sort(); registryLanguages.sort();
        QCOMPARE(registryLanguages, languages);
        QStringList radii;
        for (const auto radius : okuflow::app_constants::kSupportedBlurRadii) radii << QString::number(radius);
        QCOMPARE(choices("image.blur.radius"), radii);
        QCOMPARE(choices("view.presentationRate").size(), qsizetype(5));
        QCOMPARE(choices("recording.canvas").size(), qsizetype(7));
    }

    // [registry][validator] Both validators silently ignore keywords outside
    // their subset, so every emitted schema must stay inside it; patterns are
    // fully anchored and compile in both regular-expression engines.
    void schemasUseOnlyKeywordsBothValidatorsEnforce() {
        const auto audit = AuditEverySchema();
        QVERIFY2(audit.unsupported.isEmpty(), qPrintable(audit.unsupported.join(", ")));
        QVERIFY2(audit.defaults.isEmpty(), qPrintable(audit.defaults.join(", ")));
        QVERIFY(!audit.patterns.isEmpty());
        for (const auto& pattern : audit.patterns) {
            QVERIFY2(pattern.startsWith('^') && pattern.endsWith('$'), qPrintable(pattern));
            QVERIFY2(QRegularExpression(pattern).isValid(), qPrintable(pattern));
            bool compiles = true;
            try { std::regex expression(pattern.toStdString()); } catch (const std::regex_error&) { compiles = false; }
            QVERIFY2(compiles, qPrintable(pattern));
            QVERIFY2(PatternSample(pattern), qPrintable("corpus lacks a sample for " + pattern));
        }
        for (int group = 0; group < 8; ++group) {
            SchemaAudit groupAudit;
            Walk(Object(ParameterGetSchema(ParameterGroup(group))), "get", true, groupAudit);
            Walk(Object(ParameterSetSchema(ParameterGroup(group))), "set", true, groupAudit);
            QVERIFY2(groupAudit.unsupported.isEmpty(), qPrintable(groupAudit.unsupported.join(", ")));
        }
    }

    // [registry] Every model-supplied string, number, array and object has a
    // finite wire bound. Review finding F-10 lists the known exceptions.
    void everyAgentInputIsFinitelyBounded() {
        const auto unbounded = AuditEverySchema().unbounded;
        const QStringList known{"app.state.get.sinceEvent", "view.zoom.set.anchor|1.px", "view.zoom.set.anchor|1.py",
                                "view.center.set.point|1.px", "view.center.set.point|1.py"};
        QStringList unexpected;
        for (const auto& path : unbounded) if (!known.contains(path)) unexpected << path;
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(", ")));
        QVERIFY2(unbounded.isEmpty(), qPrintable(unbounded.join(", ")));
    }

    // [registry] Every parameter, protected entry and action is reachable exactly
    // once by paging, globally and per group, at limits 16, 7 and 1.
    void metadataTraversalReconcilesEveryEntryWithoutCaps() {
        std::set<QString> expected;
        for (const auto& p : ParameterRegistry()) QVERIFY(expected.insert(Id(p.id)).second);
        for (const auto& p : ProtectedControlRegistry()) QVERIFY(expected.insert(Id(p.id)).second);
        for (const auto& a : ControlActionRegistry()) QVERIFY(expected.insert(Id(a.id)).second);
        QCOMPARE(expected.size(), std::size_t(94 + 17 + 39));
        for (const std::size_t limit : {std::size_t(16), std::size_t(7), std::size_t(1)}) {
            int pages = 0; qsizetype largest = 0;
            const auto items = MetadataItems({}, limit, &pages, &largest);
            std::set<QString> ids;
            for (const auto& item : items) QVERIFY(ids.insert(item["id"].toString()).second);
            QVERIFY(ids == expected);
            QCOMPARE(pages, int((expected.size() + limit - 1) / limit));
            QVERIFY(largest <= 32 * 1024);
        }
        std::set<QString> grouped;
        for (int group = 0; group < 8; ++group) {
            const auto name = ParameterGroupName(ParameterGroup(group));
            for (const auto& item : MetadataItems(name, 16)) {
                QCOMPARE(item["group"].toString(), Id(name));
                QVERIFY2(grouped.insert(item["id"].toString()).second, qPrintable(item["id"].toString()));
                QVERIFY(!item.contains("value") && !item.contains("current") && !item.contains("saved"));
            }
        }
        QVERIFY(grouped == expected);
        using Page = std::pair<std::string_view, std::size_t>;
        for (const auto& bad : {Page{"image", 0}, Page{"image", 17}, Page{"secret", 16}})
            QVERIFY(!Object(ParameterMetadataPage(bad.first, {}, bad.second))["ok"].toBool());
        QVERIFY(!Object(ParameterMetadataPage("image", "assistant.apiKey", 16))["ok"].toBool());
    }

    // [registry][planner] Discovery metadata must describe the approval that
    // the production planner actually requires (finding F-09).
    void discoveryApprovalFlagsMatchActualPlanners() {
        std::map<QString, bool> requiresUi;
        for (const auto& item : MetadataItems({}, 16))
            if (item["kind"] == "parameter") requiresUi[item["id"].toString()] = item["requiresUi"].toBool();
        QStringList mismatched;
        const auto check = [&](const QString& id, const ControlPlan& plan) {
            QVERIFY2(plan.error.isEmpty(), qPrintable(id + " " + plan.error));
            if (plan.requiresApproval != requiresUi.at(id)) mismatched << id;
        };
        auto superRes = ImageDefaults(); superRes[Field::SuperResEnabled] = true;
        check("image.superResolution.enabled", PlanImage(ImageDefaults(), ToolId::ImageSettingsSet, {{"image.superResolution.enabled", true}}));
        check("image.superResolution.strength", PlanImage(superRes, ToolId::ImageSettingsSet, {{"image.superResolution.strength", 0.9}}));
        check("image.superResolution.prefer2x", PlanImage(superRes, ToolId::ImageSettingsSet, {{"image.superResolution.prefer2x", true}}));
        check("image.superResolution.ultra1440p", PlanImage(superRes, ToolId::ImageSettingsSet, {{"image.superResolution.ultra1440p", true}}));
        check("image.superResolution.performanceOverride", PlanImage(superRes, ToolId::ImageSettingsSet, {{"image.superResolution.performanceOverride", true}}));
        check("image.contrast", PlanImage(ImageDefaults(), ToolId::ImageSettingsSet, {{"image.contrast", 1.5}}));
        okuflow::settings::AssistiveSettings cfg;
        const QJsonArray models{QJsonObject{{"id", cfg.codexModel}, {"supportedReasoningEfforts", QJsonArray{"low", "medium"}}},
                                QJsonObject{{"id", "fixture-model"}, {"supportedReasoningEfforts", QJsonArray{"low"}}}};
        const QString voice = "voice-" + Hex32();
        const std::vector<std::pair<QString, QJsonValue>> assistant{
            {"assistant_preferences.provider", "openai-compatible"}, {"assistant_preferences.codexModel", "fixture-model"},
            {"assistant_preferences.codexReasoning", "medium"}, {"assistant_preferences.httpModel", "fixture-vision"},
            {"assistant_preferences.instructions", "Short answers"}, {"assistant_preferences.explainPrompt", "Describe"},
            {"assistant_preferences.overlayEnabled", false}, {"assistant_preferences.sceneExplain", true},
            {"assistant_preferences.ttsVoice", voice}, {"assistant_preferences.ttsRate", 0.5}};
        for (const auto& [id, value] : assistant)
            check(id, PrepareAssistantPreferencePatch(cfg, models, {voice}, {{id, value}}, false));
        check("notes_output.lectureNotes", PrepareAssistantPreferencePatch(cfg, {}, {}, {{"notes_output.lectureNotes", false}}, true));
        const QStringList known{"image.superResolution.enabled", "image.superResolution.strength",
            "image.superResolution.prefer2x", "image.superResolution.ultra1440p",
            "image.superResolution.performanceOverride", "assistant_preferences.codexModel",
            "assistant_preferences.codexReasoning", "assistant_preferences.httpModel",
            "assistant_preferences.instructions", "assistant_preferences.explainPrompt"};
        QStringList unexpected;
        for (const auto& id : mismatched) if (!known.contains(id)) unexpected << id;
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(", ")));
        QVERIFY2(mismatched.isEmpty(), qPrintable(mismatched.join(", ")));
    }

    // [validator] The app QJson subset and the Qt-free bridge subset must accept
    // and reject the same corpus for every parameter schema. The trailing line
    // break class is isolated in the next slot (finding F-04).
    void bridgeAndAppValidatorsAgreeOnRegistryCorpus() {
        QStringList divergences; int compared = 0, lineBreakOnly = 0;
        const auto compare = [&](const QString& where, const QJsonValue& value, const QJsonObject& schema) {
            ++compared;
            const bool app = AppAccepts(value, schema), bridge = BridgeAccepts(value, schema);
            if (app == bridge) return;
            const auto text = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
            if (text.contains("\\n")) { ++lineBreakOnly; return; }
            divergences << where + "=" + QString::fromUtf8(text);
        };
        for (const auto& p : ParameterRegistry()) {
            const auto schema = Object(p.schema);
            const auto type = schema["type"].toString();
            QJsonArray corpus{QJsonValue(QJsonValue::Null), true, false, 0, -1, 0.5, 1e300, "", "x", QJsonArray{}, QJsonObject{}};
            for (const auto choice : schema["enum"].toArray()) {
                corpus << choice;
                if (choice.isString()) corpus << choice.toString() + " " << choice.toString().toUpper() << choice.toString() + "\n";
                else corpus << choice.toDouble() + 1 << choice.toDouble() + 0.5;
            }
            if (type == "number" || type == "integer") {
                const double low = schema["minimum"].toDouble(-100), high = schema["maximum"].toDouble(100);
                for (const double value : {low, high, (low + high) / 2, low - 0.01, high + 0.01, low + 0.004,
                                           std::floor((low + high) / 2) + 0.5})
                    corpus << value;
            }
            if (type == "string" && schema.contains("pattern")) {
                const auto sample = PatternSample(schema["pattern"].toString());
                QVERIFY2(sample, qPrintable(Id(p.id)));
                corpus << *sample << *sample + "\n" << " " + *sample << sample->toUpper() << sample->left(sample->size() - 1)
                       << *sample + "0" << sample->repeated(300);
            } else if (type == "string" && !schema.contains("enum")) {
                const int maximum = schema["maxLength"].toInt();
                corpus << QString(maximum, 'a') << QString(maximum + 1, 'a') << Emoji().repeated(maximum)
                       << Emoji().repeated(maximum + 1);
            }
            if (type == "array") {
                const QString stop = "#00ff00";
                corpus << QJsonArray{stop, "#FFFFFF"} << QJsonArray{stop} << QJsonArray{stop, stop}
                       << QJsonArray{stop, "#gggggg"} << QJsonArray{stop, "#00ff00\n"} << QJsonArray{stop, 1};
                QJsonArray nine; for (int i = 0; i < 9; ++i) nine << stop;
                corpus << nine;
            }
            if (Id(p.id) == "ui.assistiveGeometry") {
                const QJsonObject good{{"x", -10}, {"y", 20}, {"width", 300}, {"height", 200}};
                auto extra = good; extra["z"] = 1;
                auto missing = good; missing.remove("height");
                auto zero = good; zero["width"] = 0;
                auto fraction = good; fraction["x"] = 0.5;
                corpus << good << extra << missing << zero << fraction;
            }
            if (Id(p.id) == "ui.sectionExpanded")
                corpus << QJsonObject{{"magnification", true}} << QJsonObject{{"unknown", true}}
                       << QJsonObject{{"device", "true"}};
            for (const auto value : corpus) compare(Id(p.id), value, schema);
        }
        for (int group = 0; group < 8; ++group) {
            const auto get = Object(ParameterGetSchema(ParameterGroup(group)));
            const auto first = get["properties"].toObject()["fields"].toObject()["items"].toObject()["enum"].toArray().first();
            QJsonArray seventeen; for (int i = 0; i < 17; ++i) seventeen << first;
            for (const auto& args : {QJsonObject{}, QJsonObject{{"fields", QJsonArray{first}}},
                                     QJsonObject{{"fields", QJsonArray{first, first}}}, QJsonObject{{"fields", QJsonArray{}}},
                                     QJsonObject{{"fields", QJsonArray{"assistant.apiKey"}}}, QJsonObject{{"fields", seventeen}}})
                compare(QStringLiteral("get"), args, get);
        }
        QVERIFY(compared > 1000);
        qInfo("validator corpus: %d comparisons, %d line-break divergences isolated", compared, lineBreakOnly);
        QVERIFY2(divergences.isEmpty(), qPrintable(divergences.join(" | ")));
    }

    // [validator] JSON Schema patterns are ECMA-262 without multiline: '$' ends
    // the input. PCRE's default '$' also matches before a final line break, so
    // the app path (used without the bridge by embedded providers) accepts
    // "system\n" where the bridge rejects it (finding F-04).
    void anchoredPatternsRejectTrailingLineBreaks() {
        QStringList appAccepts;
        int checked = 0;
        const auto probe = [&](const QString& where, const QJsonObject& schema) {
            const auto sample = PatternSample(schema["pattern"].toString());
            QVERIFY2(sample, qPrintable(where));
            QVERIFY2(AppAccepts(*sample, schema) && BridgeAccepts(*sample, schema), qPrintable(where));
            QVERIFY2(!BridgeAccepts(*sample + "\n", schema), qPrintable(where));
            ++checked;
            if (AppAccepts(*sample + "\n", schema)) appAccepts << where;
        };
        for (const auto& p : ParameterRegistry()) {
            const auto schema = Object(p.schema);
            if (schema.contains("pattern")) probe(Id(p.id), schema);
            if (schema["items"].toObject().contains("pattern")) probe(Id(p.id) + "[]", schema["items"].toObject());
        }
        const auto presetId = Object(FindControlTool("mode.activate", CatalogEdition::DevFull)->inputSchema)["properties"].toObject()["presetId"].toObject();
        probe(QStringLiteral("mode.activate.presetId"), presetId);
        QVERIFY(checked >= 8);
        // The current downstream consumers reject these values, which is why no
        // effect follows today; the contract must not depend on that.
        okuflow::settings::AssistiveSettings cfg;
        QCOMPARE(PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.ttsVoice", "system\n"}}, false).error,
                 QStringLiteral("unavailable"));
        QVERIFY2(appAccepts.isEmpty(), qPrintable(appAccepts.join(", ")));
    }

    // [planner] The image planner owns exactly the 49 image and 9 view fields.
    void imagePlannerOwnsEveryImageAndViewField() {
        std::set<Field> fields; int owned = 0;
        for (const auto& p : ParameterRegistry()) {
            const auto field = AppControlTarget::ImageParameterField(p.id);
            if (!IsImageOrView(p)) { QVERIFY2(!field, qPrintable(Id(p.id))); continue; }
            QVERIFY2(field, qPrintable(Id(p.id)));
            QVERIFY2(fields.insert(*field).second, qPrintable(Id(p.id)));
            QVERIFY(!FieldName(*field).isEmpty());
            ++owned;
        }
        QCOMPARE(owned, 58);
        QVERIFY(!AppControlTarget::ImageParameterField("image.colors.custom"));
        QVERIFY(!AppControlTarget::ImageParameterField("assistant.apiKey"));
    }

    // [planner][service] Grouped requests and target rows use registry ids, but
    // the service reports applied/effective/fieldRevisions/undo receipts with
    // FieldName() tokens. Clients cannot correlate them reliably (F-08).
    void groupedReceiptsUseTheRequestVocabulary() {
        int checked = 0;
        for (const auto& p : ParameterRegistry()) {
            const auto field = AppControlTarget::ImageParameterField(p.id);
            if (!field) continue;
            ++checked;
            FakeTarget target;
            target.fields = ImageDefaults();
            target.prepare = [&](ToolId tool,const QJsonObject& args) { return PlanImage(target.fields,tool,args); };
            target.query = [&](ToolId,const QJsonObject&) {
                return QJsonObject{{"parameters",QJsonObject{{Id(p.id),QJsonObject{{"requested",target.fields.at(*field)}}}}}};
            };
            AppControlService service(target,CatalogEdition::DevFull);
            const auto context=Embedded(service);
            const auto result=Call(service,context,"read",p.group==ParameterGroup::Image?"image_settings_get":"view_settings_get",
                {{"fields",QJsonArray{Id(p.id)}}});
            QVERIFY(result.content["ok"].toBool());
            const auto revisions=result.content["fieldRevisions"].toObject();
            QCOMPARE(revisions.size(),1);
            QVERIFY2(revisions.contains(Id(p.id)),qPrintable(Id(p.id)+" missing from receipt"));
        }
        QCOMPARE(checked, 58);
        // Legacy receipts retain their prior vocabulary; grouped setters
        // publish registry IDs through the actual service receipt path.
        QCOMPARE(FieldName(Field::Contrast),QStringLiteral("contrast"));
        FakeTarget target;
        target.fields=ImageDefaults();
        target.prepare=[&](ToolId tool,const QJsonObject& args) {return PlanImage(target.fields,tool,args);};
        AppControlService service(target,CatalogEdition::DevFull);
        const auto context=Embedded(service);
        const auto written=Call(service,context,"set","image_settings_set",{{"image.contrast",2.0}});
        QVERIFY(written.content["ok"].toBool());
        QCOMPARE(written.content["applied"].toObject(),(QJsonObject{{"image.contrast",2.0}}));
        QCOMPARE(written.content["effective"].toObject(),(QJsonObject{{"image.contrast",2.0}}));
        QCOMPARE(FieldName(Field::AssistantInstructions), QStringLiteral("assistant_preferences.instructions"));
        QCOMPARE(FieldName(Field::LectureNotes), QStringLiteral("notes_output.lectureNotes"));
    }

    // [planner] Every non-recipe image/view field: each finite choice and both
    // numeric bounds plan exactly the native widget value; out-of-contract
    // values reject the whole patch.
    void imagePlannerQuantizesEveryBoundaryLikeNativeWidgets() {
        int covered = 0, samples = 0;
        for (const auto& p : ParameterRegistry()) {
            const auto id = Id(p.id);
            if (!IsImageOrView(p) || IsCustomComponent(id)) continue;
            const auto field = *AppControlTarget::ImageParameterField(p.id);
            const auto schema = Object(p.schema);
            const auto tool = p.group == ParameterGroup::Image ? ToolId::ImageSettingsSet : ToolId::ViewSettingsSet;
            auto current = ImageDefaults();
            if (id == "image.stabilization.bumpHold") current[Field::Stabilization] = true;
            if (id == "image.keystone.paused") current[Field::Keystone] = true;
            if (id == "image.superResolution.performanceOverride") current[Field::SuperResEnabled] = true;
            std::vector<std::pair<QJsonValue, QJsonValue>> valid; // requested, expected planned value
            QJsonArray invalid;
            const auto type = schema["type"].toString();
            if (type == "boolean") {
                valid = {{true, true}, {false, false}};
                invalid = {"true", 1};
            } else if (schema.contains("enum")) {
                const auto choices = schema["enum"].toArray();
                for (qsizetype i = 0; i < choices.size(); ++i) {
                    QJsonValue expected = choices[i];
                    if (id == "image.spatial.backend" || id == "image.textClarity.polarity") expected = int(i);
                    valid.push_back({choices[i], expected});
                }
                invalid = {type == "integer" ? QJsonValue(12) : QJsonValue("not-a-choice"), QJsonValue(QJsonValue::Null)};
            } else {
                const bool integer = type == "integer";
                const double low = schema["minimum"].toDouble(), high = schema["maximum"].toDouble();
                for (const double value : {low, high, (low + high) / 2, integer ? low : low + 0.004})
                    valid.push_back({value, NativeNumber(id, integer, value)});
                invalid = {low - (integer ? 1 : 0.01), high + (integer ? 1 : 0.01), "1"};
                if (integer) invalid << low + 0.5;
            }
            for (const auto& [requested, expected] : valid) {
                const auto plan = PlanImage(current, tool, {{id, requested}});
                const auto where = id + "=" + QString::fromUtf8(QJsonDocument(QJsonArray{requested}).toJson(QJsonDocument::Compact));
                QVERIFY2(plan.error.isEmpty(), qPrintable(where + " " + plan.error));
                const auto written = Written(plan, field);
                QVERIFY2(written, qPrintable(where));
                if (written->isDouble() && expected.isDouble())
                    QVERIFY2(qFuzzyCompare(1 + written->toDouble(), 1 + expected.toDouble()), qPrintable(where));
                else
                    QVERIFY2(*written == expected, qPrintable(where));
                ++samples;
            }
            for (const auto bad : invalid) {
                const auto plan = PlanImage(current, tool, {{id, bad}});
                QVERIFY2(plan.error == "invalid_argument" && plan.writes.empty(), qPrintable(id));
            }
            ++covered;
        }
        QCOMPARE(covered, 54); // 58 minus the four custom-recipe components
        QVERIFY(samples > 150);
        qInfo("image/view fields: %d, accepted boundary/choice samples: %d", covered, samples);
    }

    // [planner] One invalid key, cross-group key, protected name or dependency
    // failure rejects the whole patch before any write is published.
    void imagePlannerRejectsWholeInvalidBatches() {
        const auto defaults = ImageDefaults();
        const std::vector<QJsonObject> rejected{
            {{"image.contrast", 1.5}, {"image.blur.radius", 12}},
            {{"image.contrast", 1.5}, {"view.zoom.amount", 2.0}},
            {{"image.contrast", 1.5}, {"assistant.apiKey", "never"}},
            {{"image.contrast", 1.5}, {"image.unknown", true}},
            {{"image.contrast", 1.5}, {"image.stabilization.bumpHold", true}},
            {{"image.contrast", 1.5}, {"image.keystone.paused", true}},
            {{"image.contrast", 1.5}, {"image.superResolution.performanceOverride", true}},
            {{"image.contrast", 1.5}, {"image.colors.custom.mode", "gradient"}},
            {{"image.superResolution.prefer2x", true}, {"image.superResolution.ultra1440p", true}},
            {}};
        for (const auto& args : rejected) {
            const auto plan = PlanImage(defaults, ToolId::ImageSettingsSet, args);
            QVERIFY2(!plan.error.isEmpty() && plan.writes.empty() && plan.observedFields.empty(),
                     qPrintable(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))));
        }
        QVERIFY(!PlanImage(defaults, ToolId::ViewSettingsSet, {{"image.contrast", 1.5}}).error.isEmpty());
        QVERIFY(!PlanImage(defaults, ToolId::AssistantPreferencesSet, {{"image.contrast", 1.5}}).error.isEmpty());
        QJsonArray seventeen; for (int i = 0; i < 17; ++i) seventeen << "image.contrast";
        for (const auto& args : {QJsonObject{{"fields", QJsonArray{}}}, QJsonObject{{"fields", QJsonArray{"image.contrast", "image.contrast"}}},
                                 QJsonObject{{"fields", QJsonArray{"view.zoom.amount"}}}, QJsonObject{{"fields", seventeen}},
                                 QJsonObject{{"fields", QJsonArray{"image.contrast"}}, {"extra", 1}}}) {
            const auto plan = PlanImage(defaults, ToolId::ImageSettingsGet, args);
            QVERIFY(!plan.error.isEmpty() && plan.observedFields.empty());
        }
        // A missing dependency in the current target state is unavailable, not partial.
        auto noRecipe = defaults; noRecipe.erase(Field::CustomColorRecipe);
        const auto missing = PlanImage(noRecipe, ToolId::ImageSettingsSet, {{"image.colors.custom.mode", "gradient"},
            {"image.colors.custom.stopCount", 2}, {"image.colors.custom.stops", QJsonArray{"#000000", "#ffffff"}},
            {"image.colors.custom.stepped", false}});
        QCOMPARE(missing.error, QStringLiteral("unavailable"));
        QVERIFY(missing.writes.empty());
    }

    // [planner] Custom colors are one validated recipe: all four components,
    // matching count, duotone exactly two stops, posterize stepped, lowercase.
    void customRecipeUndoRetainsItsCoupledClusterAfterAnApplyFailure() {
        for (const bool userEdit : {false, true}) {
            FakeTarget target;
            target.fields = ImageDefaults();
            const auto before = target.fields;
            target.prepare = [&](ToolId tool, const QJsonObject& args) { return PlanImage(target.fields, tool, args); };
            bool refuseInverse = false;
            target.apply = [&](const FieldWrite& write) {
                if (refuseInverse && write.field == Field::CustomColorStopCount && write.value == 2) return false;
                target.fields[write.field] = write.value;
                return true;
            };
            AppControlService service(target, CatalogEdition::DevFull);
            const auto context = Embedded(service);
            const QJsonObject recipe{{"image.colors.custom.mode", "posterize"}, {"image.colors.custom.stopCount", 3},
                {"image.colors.custom.stops", QJsonArray{"#ff0000", "#00ff00", "#0000ff"}}, {"image.colors.custom.stepped", true}};
            QVERIFY(Call(service, context, "recipe", "image_settings_set", recipe).content["ok"].toBool());
            refuseInverse = true;
            const auto failed = service.StopAndUndo().content;
            QVERIFY(failed["retained"].toArray().contains(FieldName(Field::CustomColorRecipe)));
            QVERIFY(failed["retained"].toArray().contains(FieldName(Field::CustomColorStopCount)));
            refuseInverse = false;
            const QJsonArray manualStops{"#112233", "#445566"};
            if (userEdit) {
                target.fields[Field::CustomColorStops] = manualStops;
                service.NativeFieldsChanged({Field::CustomColorStops});
            }
            const auto retried = service.StopAndUndo().content;
            QVERIFY(retried["retained"].toArray().isEmpty());
            if (userEdit) {
                QVERIFY(retried["restored"].toArray().isEmpty());
                QCOMPARE(target.fields.at(Field::CustomColorStops), QJsonValue(manualStops));
            } else {
                QVERIFY(target.fields == before);
            }
        }
    }
    void customRecipeIsOneAtomicValidatedCluster() {
        const auto defaults = ImageDefaults();
        const QJsonObject recipe{{"image.colors.custom.mode", "gradient"}, {"image.colors.custom.stopCount", 3},
            {"image.colors.custom.stops", QJsonArray{"#FF0000", "#00ff00", "#0000FF"}}, {"image.colors.custom.stepped", false}};
        const auto plan = PlanImage(defaults, ToolId::ImageSettingsSet, recipe);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.writes.front().field, Field::CustomColorRecipe);
        const auto applied = plan.writes.front().value.toObject();
        QCOMPARE(applied["mode"].toString(), QStringLiteral("gradient"));
        QCOMPARE(applied["stopCount"].toInt(), 3);
        QCOMPARE(applied["stops"].toArray(), (QJsonArray{"#ff0000", "#00ff00", "#0000ff"}));
        QCOMPARE(applied["schemeId"].toString(), QStringLiteral("custom"));
        QCOMPARE(Written(plan, Field::Scheme)->toString(), QStringLiteral("custom"));
        QCOMPARE(Written(plan, Field::CustomColorStops)->toArray(), applied["stops"].toArray());
        for (auto it = recipe.begin(); it != recipe.end(); ++it) {
            auto partial = recipe; partial.remove(it.key());
            const auto rejected = PlanImage(defaults, ToolId::ImageSettingsSet, partial);
            QVERIFY2(rejected.error == "invalid_argument" && rejected.writes.empty(), qPrintable(it.key()));
        }
        auto mismatch = recipe; mismatch["image.colors.custom.stopCount"] = 2;
        QCOMPARE(PlanImage(defaults, ToolId::ImageSettingsSet, mismatch).error, QStringLiteral("invalid_argument"));
        auto duotone = recipe; duotone["image.colors.custom.mode"] = "duotone";
        QCOMPARE(PlanImage(defaults, ToolId::ImageSettingsSet, duotone).error, QStringLiteral("invalid_argument"));
        duotone["image.colors.custom.stopCount"] = 2; duotone["image.colors.custom.stops"] = QJsonArray{"#000000", "#ffff00"};
        QVERIFY(PlanImage(defaults, ToolId::ImageSettingsSet, duotone).error.isEmpty());
        auto posterize = recipe; posterize["image.colors.custom.mode"] = "posterize";
        const auto stepped = PlanImage(defaults, ToolId::ImageSettingsSet, posterize);
        QVERIFY(stepped.error.isEmpty());
        QCOMPARE(*Written(stepped, Field::CustomColorStepped), QJsonValue(true));
        QCOMPARE(QJsonValue(stepped.writes.front().value.toObject().value("stepped")), QJsonValue(true));
        // Selecting custom without a stored two-stop recipe is invalid.
        QVERIFY(PlanImage(defaults, ToolId::ImageSettingsSet, {{"image.colors.schemeId", "custom"}}).error.isEmpty());
        auto broken = defaults; broken[Field::CustomColorStops] = QJsonArray{"#000000"};
        QCOMPARE(PlanImage(broken, ToolId::ImageSettingsSet, {{"image.colors.schemeId", "custom"}}).error,
                 QStringLiteral("invalid_argument"));
        // Natively reachable: edit the custom recipe while another scheme stays selected.
        auto keep = recipe; keep["image.colors.schemeId"] = "yellow-black";
        const auto kept = PlanImage(defaults, ToolId::ImageSettingsSet, keep);
        QVERIFY(kept.error.isEmpty());
        QCOMPARE(kept.writes.front().value.toObject()["schemeId"].toString(), QStringLiteral("yellow-black"));
        QCOMPARE(Written(kept, Field::Scheme)->toString(), QStringLiteral("yellow-black"));
    }

    // [planner] SuperRes couplings mirror OnTextClarityControlsChanged, are
    // ordered before zoom, and enabling/tuning it asks for exact approval.
    void superResolutionDependenciesAndApproval() {
        const auto defaults = ImageDefaults();
        const auto enable = PlanImage(defaults, ToolId::ImageSettingsSet, {{"image.superResolution.enabled", true}});
        QVERIFY(enable.error.isEmpty());
        QVERIFY(enable.requiresApproval);
        QCOMPARE(enable.approvalDomain, ApprovalDomain::Configuration);
        QCOMPARE(*Written(enable, Field::ZoomEnabled), QJsonValue(true));
        QCOMPARE(Written(enable, Field::ZoomAmount)->toDouble(), double(1.33f));
        QVERIFY(WriteIndex(enable, Field::SuperResEnabled) < WriteIndex(enable, Field::ZoomEnabled));
        QVERIFY(WriteIndex(enable, Field::ZoomEnabled) < WriteIndex(enable, Field::ZoomAmount));
        QVERIFY(enable.resetsHistory);
        const auto faster = PlanImage(defaults, ToolId::ImageSettingsSet,
                                      {{"image.superResolution.enabled", true}, {"image.superResolution.prefer2x", true}});
        QCOMPARE(Written(faster, Field::ZoomAmount)->toDouble(), 2.0);
        QCOMPARE(*Written(faster, Field::SuperResUltra1440p), QJsonValue(false));
        QVERIFY(WriteIndex(faster, Field::SuperResPrefer2x) < WriteIndex(faster, Field::SuperResEnabled));
        const auto ultra = PlanImage(defaults, ToolId::ImageSettingsSet,
                                     {{"image.superResolution.enabled", true}, {"image.superResolution.ultra1440p", true}});
        QVERIFY(ultra.error.isEmpty());
        QVERIFY(!Written(ultra, Field::ZoomAmount));
        QCOMPARE(*Written(ultra, Field::SuperResPrefer2x), QJsonValue(false));
        const auto zeroStrength = PlanImage(defaults, ToolId::ImageSettingsSet,
                                            {{"image.superResolution.enabled", true}, {"image.superResolution.strength", 0}});
        QCOMPARE(Written(zeroStrength, Field::SuperResStrength)->toDouble(), double(0.65f));
        auto on = defaults; on[Field::SuperResEnabled] = true; on[Field::SuperResPerformanceOverride] = true;
        on[Field::ZoomEnabled] = true; on[Field::ZoomAmount] = double(1.33f);
        const auto off = PlanImage(on, ToolId::ImageSettingsSet, {{"image.superResolution.enabled", false}});
        QCOMPARE(*Written(off, Field::SuperResPerformanceOverride), QJsonValue(false));
        QVERIFY(!Written(off, Field::ZoomAmount));
        // Changing ordinary text clarity while SR is already on adds no approval.
        const auto text = PlanImage(on, ToolId::ImageSettingsSet, {{"image.textClarity.glare.strength", 0.4}});
        QVERIFY(text.error.isEmpty());
        QVERIFY(!text.requiresApproval);
    }

    // [planner] Session-only stabilization/keystone states keep native coupling.
    void stabilizationAndKeystoneSessionDependencies() {
        auto held = ImageDefaults();
        held[Field::Stabilization] = true; held[Field::BumpHold] = true; held[Field::Keystone] = true; held[Field::KeystonePaused] = true;
        const auto disable = PlanImage(held, ToolId::ImageSettingsSet, {{"image.stabilization.enabled", false}});
        QVERIFY(disable.error.isEmpty());
        QCOMPARE(*Written(disable, Field::BumpHold), QJsonValue(false));
        QCOMPARE(*Written(disable, Field::KeystonePaused), QJsonValue(false));
        QVERIFY(WriteIndex(disable, Field::Stabilization) < WriteIndex(disable, Field::BumpHold));
        const auto both = PlanImage(ImageDefaults(), ToolId::ImageSettingsSet,
                                    {{"image.stabilization.enabled", true}, {"image.stabilization.bumpHold", true}});
        QVERIFY(both.error.isEmpty());
        QVERIFY(WriteIndex(both, Field::Stabilization) < WriteIndex(both, Field::BumpHold));
        const auto pausedOn = PlanImage(ImageDefaults(), ToolId::ImageSettingsSet,
                                        {{"image.keystone.enabled", true}, {"image.keystone.paused", true}});
        QVERIFY(pausedOn.error.isEmpty());
        QCOMPARE(*Written(pausedOn, Field::KeystonePaused), QJsonValue(true));
        QVERIFY(WriteIndex(pausedOn, Field::Keystone) < WriteIndex(pausedOn, Field::KeystonePaused));
    }

    // [planner] Grouped reads observe only the requested fields, so they can
    // rebaseline only what the agent actually received.
    void groupedReadsObserveOnlyRequestedFields() {
        const auto defaults = ImageDefaults();
        const auto two = PlanImage(defaults, ToolId::ImageSettingsGet, {{"fields", QJsonArray{"image.brightness", "image.contrast"}}});
        QVERIFY(two.error.isEmpty() && two.writes.empty());
        QVERIFY(std::set<Field>(two.observedFields.begin(), two.observedFields.end()) == (std::set<Field>{Field::Contrast, Field::Brightness}));
        QCOMPARE(PlanImage(defaults, ToolId::ImageSettingsGet, {}).observedFields.size(), std::size_t(49));
        QCOMPARE(PlanImage(defaults, ToolId::ViewSettingsGet, {}).observedFields.size(), std::size_t(9));
    }

    // [planner][native] ImageParameterEffectiveIntent (target "effective" rows)
    // must agree with the production per-frame normalizer for all 2^10 master/
    // refinement combinations, plus the two CUDA dispatch exclusions read in
    // cuda_interop.cpp (smart sharpen yields to adaptive binarization; two-color
    // is applied only inside the binarization branch). Auxiliary mask effects
    // are active only when adaptive text or selective smart sharpening consumes it.
    void effectiveIntentMatchesProductionTextClarityStages() {
        const std::array refinements{Field::Flatten, Field::Adaptive, Field::SmartSharpen, Field::Clahe, Field::TwoColor,
                                     Field::Hysteresis, Field::SelectiveSharpen, Field::FocusDetection, Field::Glare};
        QStringList disagreements;
        for (int mask = 0; mask < (1 << 10); ++mask) {
            auto fields = ImageDefaults();
            const bool master = mask & 1;
            fields[Field::TextClarity] = master;
            for (std::size_t i = 0; i < refinements.size(); ++i) fields[refinements[i]] = bool(mask & (2 << i));
            okuflow::ProcessingSettings settings;
            settings.enableAutoTextClarity = master;
            settings.enableBackgroundFlatten = fields[Field::Flatten].toBool();
            settings.enableAdaptiveBinarization = fields[Field::Adaptive].toBool();
            settings.enableSmartSharpen = fields[Field::SmartSharpen].toBool();
            settings.enableClahe = fields[Field::Clahe].toBool();
            settings.enableTwoColorText = fields[Field::TwoColor].toBool();
            settings.enableTextHysteresis = fields[Field::Hysteresis].toBool();
            settings.enableSelectiveSharpen = fields[Field::SelectiveSharpen].toBool();
            settings.enableFocusDetection = fields[Field::FocusDetection].toBool();
            settings.enableGlareSuppression = fields[Field::Glare].toBool();
            settings.ApplyTextClarityMaster();
            const auto stages = settings.TextClarityStages();
            const bool selective=stages.smartSharpen && !stages.adaptive && stages.selectiveSharpen;
            const bool consumedMask=stages.adaptive || selective;
            const std::vector<std::pair<Field, bool>> expected{
                {Field::Flatten, stages.background}, {Field::Adaptive, stages.adaptive}, {Field::Clahe, stages.clahe},
                {Field::Hysteresis, stages.hysteresis && consumedMask}, {Field::SelectiveSharpen, selective},
                {Field::FocusDetection, stages.focus}, {Field::Glare, stages.glare},
                {Field::SmartSharpen, stages.smartSharpen && !settings.enableAdaptiveBinarization},
                {Field::TwoColor, stages.adaptive && settings.enableTwoColorText},
                {Field::Sauvola,consumedMask},{Field::Softness,consumedMask},
                {Field::Polarity,consumedMask},{Field::Stroke,consumedMask}};
            for (const auto& [field, native] : expected)
                if (AppControlTarget::ImageParameterEffectiveIntent(field, fields) != native)
                    disagreements << QStringLiteral("%1@%2").arg(FieldName(field)).arg(mask);
        }
        QVERIFY2(disagreements.isEmpty(), qPrintable(disagreements.mid(0, 8).join(", ")));
        // Dormant refinements keep their saved choice but are not effective.
        auto dormant = ImageDefaults(); dormant[Field::Glare] = true; dormant[Field::GlareStrength] = 0.4;
        QVERIFY(!AppControlTarget::ImageParameterEffectiveIntent(Field::GlareStrength, dormant));
        dormant[Field::TextClarity] = true;
        QVERIFY(AppControlTarget::ImageParameterEffectiveIntent(Field::GlareStrength, dormant));
    }

    // [planner] Native handlers run only when a widget value changes. Coupled
    // resets derived from a mere mention change session state (finding F-06).
    void coupledResetsFollowActualChanges() {
        QStringList defects;
        auto keystone = ImageDefaults();
        keystone[Field::Stabilization] = true; keystone[Field::Keystone] = true; keystone[Field::KeystonePaused] = true;
        const auto unchanged = PlanImage(keystone, ToolId::ImageSettingsSet, {{"image.stabilization.enabled", true}});
        QVERIFY(unchanged.error.isEmpty());
        if (const auto paused = Written(unchanged, Field::KeystonePaused); paused && *paused != keystone[Field::KeystonePaused])
            defects << "no-op stabilization resumes paused keystone";
        auto lowered = ImageDefaults();
        lowered[Field::SuperResEnabled] = true; lowered[Field::ZoomEnabled] = false;
        const auto mention = PlanImage(lowered, ToolId::ImageSettingsSet, {{"image.textClarity.enabled", false}});
        QVERIFY(mention.error.isEmpty());
        if (const auto zoom = Written(mention, Field::ZoomEnabled); zoom && *zoom != lowered[Field::ZoomEnabled])
            defects << "no-op text clarity re-enables zoom under SR";
        QVERIFY2(defects.isEmpty(), qPrintable(defects.join("; ")));
    }

    // [planner] OnZoomToggled/SetZoom do not enforce the SR minimum natively.
    // The planner rewrites explicit requests without reporting it (F-07).
    void explicitZoomRequestIsNeverSilentlyInverted() {
        auto superRes = ImageDefaults();
        superRes[Field::SuperResEnabled] = true; superRes[Field::ZoomEnabled] = true; superRes[Field::ZoomAmount] = 1.5;
        QStringList defects;
        const auto disable = PlanImage(superRes, ToolId::ViewSettingsSet, {{"view.zoom.enabled", false}});
        if (disable.error.isEmpty() && Written(disable, Field::ZoomEnabled) == QJsonValue(true))
            defects << "view.zoom.enabled=false planned as true";
        const auto one = PlanImage(superRes, ToolId::ViewSettingsSet, {{"view.zoom.amount", 1.0}});
        if (one.error.isEmpty() && Written(one, Field::ZoomAmount) && Written(one, Field::ZoomAmount)->toDouble() > 1.0)
            defects << "view.zoom.amount=1 planned above 1";
        QVERIFY2(defects.isEmpty(), qPrintable(defects.join("; ")));
    }

    // [planner] All ten assistant preferences and the notes switch: exact field,
    void uiAndAnnotationPlannersCoverEveryFieldAndNativeDependency() {
        const std::map<Field,QJsonValue> current{
            {Field::UiMode,"simple"},{Field::UiInspectorWidth,520},{Field::UiLanguage,"en"},
            {Field::UiHidden,false},{Field::UiSectionExpanded,QJsonObject{{"device",true},{"magnification",true}}},
            {Field::UiAssistiveDock,"floating"},{Field::UiAssistiveGeometry,QJsonValue(QJsonValue::Null)},
            {Field::UiSetupPromptDeclined,false},{Field::UiActiveTab,"image"},
            {Field::AnnotationColor,"#fff000"},{Field::AnnotationWidth,8},{Field::AnnotationTextSize,36},
            {Field::AnnotationDashed,false},{Field::AnnotationShape,"rectangle"},
            {Field::AnnotationCaptureOnExit,false},{Field::AnnotationEnabled,false},{Field::AnnotationTool,"pointer"}};
        const QJsonObject samples{{"ui.mode","advanced"},{"ui.inspectorWidth",1200},{"ui.language","system"},
            {"ui.hidden",true},{"ui.sectionExpanded",QJsonObject{{"device",false}}},{"ui.assistiveDock","left"},
            {"ui.assistiveGeometry",QJsonObject{{"x",16},{"y",24},{"width",700},{"height",600}}},
            {"ui.setupPromptDeclined",true},{"ui.activeTab","settings"},{"annotation.color","#A1b2C3"},
            {"annotation.width",24},{"annotation.textSize",72},{"annotation.dashed",true},
            {"annotation.shape","ellipse"},{"annotation.captureOnExit",true},{"annotation.enabled",true},
            {"annotation.tool","eraser"}};
        int covered=0;
        std::set<Field> identities;
        for(const auto& descriptor:ParameterRegistry()) {
            const auto field=AppControlTarget::UiParameterField(descriptor.id);
            if(descriptor.group!=ParameterGroup::Ui && descriptor.group!=ParameterGroup::Annotation) {
                QVERIFY(!field);continue;
            }
            QVERIFY2(field,qPrintable(Id(descriptor.id)));QVERIFY(identities.insert(*field).second);
            const auto key=Id(descriptor.id);QVERIFY(samples.contains(key));
            const auto tool=descriptor.group==ParameterGroup::Ui?ToolId::UiPreferencesSet:ToolId::AnnotationPreferencesSet;
            const auto plan=AppControlTarget::PlanUiParameterDependencies(current,tool,{{key,samples[key]}});
            QVERIFY2(plan.error.isEmpty(),qPrintable(key+":"+plan.error));
            const auto written=Written(plan,*field);QVERIFY(written);
            if(*field==Field::AnnotationColor)QCOMPARE(written->toString(),QStringLiteral("#a1b2c3"));
            else if(*field==Field::UiSectionExpanded)
                QCOMPARE(written->toObject(),(QJsonObject{{"device",false},{"magnification",true}}));
            else QCOMPARE(*written,samples[key]);
            QCOMPARE(plan.requiresApproval,*field==Field::AnnotationEnabled);
            QCOMPARE(plan.requiresPhotos,*field==Field::AnnotationCaptureOnExit);
            const auto getter=descriptor.group==ParameterGroup::Ui?ToolId::UiPreferencesGet:ToolId::AnnotationPreferencesGet;
            const auto read=AppControlTarget::PlanUiParameterDependencies(current,getter,{{"fields",QJsonArray{key}}});
            QVERIFY(read.error.isEmpty());
            QCOMPARE(read.observedFields.size(),current.at(*field).isNull()?std::size_t(0):std::size_t(1));
            for(const auto bad:QJsonArray{QJsonValue(QJsonValue::Null),QJsonArray{},QJsonObject{{"injected",true}}}) {
                const auto denied=AppControlTarget::PlanUiParameterDependencies(current,tool,{{key,bad}});
                QCOMPARE(denied.error,QStringLiteral("invalid_argument"));QVERIFY(denied.writes.empty());
            }
            ++covered;
        }
        QCOMPARE(covered,17);
        const auto activation=AppControlTarget::PlanUiParameterDependencies(current,ToolId::AnnotationPreferencesSet,
            {{"annotation.enabled",true},{"annotation.tool","pointer"}});
        QVERIFY(activation.error.isEmpty());QVERIFY(activation.requiresApproval);
        QVERIFY(!Written(activation,Field::AnnotationTool)); // Native controlled activation retains this tool.
        const auto mixed=AppControlTarget::PlanUiParameterDependencies(current,ToolId::UiPreferencesSet,
            {{"ui.mode","advanced"},{"annotation.tool","pen"}});
        QCOMPARE(mixed.error,QStringLiteral("invalid_argument"));QVERIFY(mixed.writes.empty());
        const auto bounds=AppControlTarget::PlanUiParameterDependencies(current,ToolId::UiPreferencesSet,
            {{"ui.hidden",true},{"ui.inspectorWidth",1201}});
        QCOMPARE(bounds.error,QStringLiteral("invalid_argument"));QVERIFY(bounds.writes.empty());
        const auto noop=AppControlTarget::PlanUiParameterDependencies(current,ToolId::AnnotationPreferencesSet,
            {{"annotation.enabled",false},{"annotation.captureOnExit",false}});
        QVERIFY(noop.error.isEmpty());QVERIFY(noop.writes.empty());QVERIFY(!noop.requiresApproval && !noop.requiresPhotos);
    }

    void recordingPreferencePolicyRequiresExactStopAndFencesDeferredWrites() {
        const QString mic="mic-"+Hex32();
        const std::map<Field,QJsonValue> current{{Field::RecordingMicrophone,"none"},{Field::RecordingCanvas,"source"}};
        const QSet<QString> installed{mic};
        for(const auto* canvas:{"source","360p","480p","720p","1080p","1440p","2160p"}) {
            const auto plan=PrepareRecordingPreferencePatch(current,installed,{{"recording.canvas",canvas}});
            QVERIFY(plan.error.isEmpty());QVERIFY(!plan.requiresApproval && !plan.requiresRecording && !plan.asynchronous);
        }
        const auto noRecording=PrepareRecordingPreferencePatch(current,installed,{{"recording.microphone",mic}});
        QVERIFY(noRecording.error.isEmpty());QVERIFY(noRecording.requiresApproval);
        QVERIFY(!noRecording.requiresRecording && !noRecording.asynchronous);
        const RecordingPreferenceSession session{"exact-session","exact-start",true};
        const auto live=PrepareRecordingPreferencePatch(current,installed,
            {{"recording.microphone",mic},{"recording.canvas","720p"}},session);
        QVERIFY(live.error.isEmpty());QCOMPARE(live.writes.size(),std::size_t(2));
        QVERIFY(live.requiresRecording && live.asynchronous && live.requiresApproval && live.terminalRecording);
        QCOMPARE(live.targetIdentity,session.id);QCOMPARE(live.startedAt,session.startedAt);
        const auto unknown=PrepareRecordingPreferencePatch(current,{},{{"recording.microphone",mic},{"recording.canvas","720p"}},session);
        QCOMPARE(unknown.error,QStringLiteral("not_found"));QVERIFY(unknown.writes.empty());
        const auto stopping=PrepareRecordingPreferencePatch(current,installed,{{"recording.microphone",mic}},
            {session.id,session.startedAt,false});
        QCOMPARE(stopping.error,QStringLiteral("busy"));QVERIFY(stopping.writes.empty());
        const auto invalid=PrepareRecordingPreferencePatch(current,installed,{{"recording.microphone",mic},{"recording.canvas","8k"}},session);
        QCOMPARE(invalid.error,QStringLiteral("invalid_argument"));QVERIFY(invalid.writes.empty());
        const RecordingPreferenceFence fence{2,3,4,session.id};
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,4,session.id,true,false,false),QStringLiteral("waiting"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,4,{},false,false,false),QStringLiteral("ready"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,false,true,2,3,4,{},false,false,false),QStringLiteral("canceled"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,false,2,3,4,{},false,false,false),QStringLiteral("canceled"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,3,3,4,{},false,false,false),QStringLiteral("stale_state"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,4,4,{},false,false,false),QStringLiteral("stale_state"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,5,{},false,false,false),QStringLiteral("stale_state"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,4,"replacement",false,false,false),QStringLiteral("stale_state"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,4,session.id,false,true,false),QStringLiteral("unavailable"));
        QCOMPARE(RecordingPreferenceFenceStatus(fence,true,true,2,3,4,session.id,true,false,true),QStringLiteral("timeout"));
    }

    // [planner] All ten assistant preferences and the notes switch: exact field,
    // group split, protected names refused and approval only for configuration.
    void assistantPolicyCoversEveryAssistantAndNotesField() {
        okuflow::settings::AssistiveSettings cfg;
        const QJsonArray models{QJsonObject{{"id", cfg.codexModel}, {"supportedReasoningEfforts", QJsonArray{"low", "medium"}}},
                                QJsonObject{{"id", "fixture-model"}, {"supportedReasoningEfforts", QJsonArray{"medium", "high"}}}};
        const QString voice = "voice-" + Hex32();
        const std::vector<std::tuple<QString, QJsonValue, Field, bool>> cases{
            {"assistant_preferences.sceneExplain", true, Field::AssistantSceneExplain, false},
            {"assistant_preferences.overlayEnabled", false, Field::AssistantOverlay, false},
            {"assistant_preferences.provider", "openai-compatible", Field::AssistantProvider, true},
            {"assistant_preferences.codexModel", "fixture-model", Field::AssistantCodexModel, true},
            {"assistant_preferences.codexReasoning", "medium", Field::AssistantReasoning, true},
            {"assistant_preferences.httpModel", "fixture-vision", Field::AssistantHttpModel, true},
            {"assistant_preferences.instructions", "Short answers\nPlain words", Field::AssistantInstructions, true},
            {"assistant_preferences.explainPrompt", "Describe the board", Field::AssistantExplainPrompt, true},
            {"assistant_preferences.ttsVoice", voice, Field::AssistantVoice, false},
            {"assistant_preferences.ttsRate", -1.0, Field::AssistantTtsRate, false}};
        for (const auto& [id, value, field, approval] : cases) {
            QVERIFY(AppAccepts(QJsonObject{{id, value}}, Object(ParameterSetSchema(ParameterGroup::Assistant))));
            const auto plan = PrepareAssistantPreferencePatch(cfg, models, {voice}, {{id, value}}, false);
            QVERIFY2(plan.error.isEmpty(), qPrintable(id + " " + plan.error));
            QCOMPARE(plan.writes.front().field, field);
            QCOMPARE(plan.requiresApproval, approval);
            QCOMPARE(plan.approvalDomain, approval ? ApprovalDomain::Configuration : ApprovalDomain::None);
            QCOMPARE(PrepareAssistantPreferencePatch(cfg, models, {voice}, {{id, value}}, true).error, QStringLiteral("invalid_argument"));
        }
        // Model change with an unsupported saved effort couples the catalog's first effort after it.
        const auto coupled = PrepareAssistantPreferencePatch(cfg, models, {}, {{"assistant_preferences.codexModel", "fixture-model"}}, false);
        QCOMPARE(coupled.writes.size(), std::size_t(2));
        QCOMPARE(coupled.writes.back().field, Field::AssistantReasoning);
        QCOMPARE(coupled.writes.back().value.toString(), QStringLiteral("medium"));
        const auto notes = PrepareAssistantPreferencePatch(cfg, {}, {}, {{"notes_output.lectureNotes", false}}, true);
        QVERIFY(notes.error.isEmpty() && !notes.requiresApproval);
        // The trusted directory handle is schema-valid but not admitted by the preference policy.
        const auto root = PrepareAssistantPreferencePatch(cfg, {}, {}, {{"notes_output.root", "dir-" + Hex32()}}, true);
        QVERIFY(!root.error.isEmpty() && root.writes.empty());
        for (const auto& protectedId : {"assistant.apiKey", "assistant.credentialId", "assistant.workspace",
                                        "assistant.codexExecutable", "assistant.apiUrl", "assistant.controlEnabled"}) {
            const auto plan = PrepareAssistantPreferencePatch(cfg, models, {}, {{"assistant_preferences.overlayEnabled", true}, {protectedId, true}}, false);
            QVERIFY(!plan.error.isEmpty() && plan.writes.empty());
        }
    }

    // [planner] The wire contract bounds prompts to 4096 Unicode code points.
    // F-05 (UTF-16 counting) was repaired during this review; this is now a hard
    // regression check across the schema, both validators and the policy.
    void assistantPromptBoundCountsCodePoints() {
        okuflow::settings::AssistiveSettings cfg;
        const auto schema = Object(ParameterSetSchema(ParameterGroup::Assistant));
        const auto astral = Emoji().repeated(4096);
        const QJsonObject args{{"assistant_preferences.instructions", astral}};
        QVERIFY(AppAccepts(args, schema));
        QVERIFY(BridgeAccepts(args, schema));
        QVERIFY(PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.instructions", QString(4096, 'x')}}, false).error.isEmpty());
        QVERIFY(!PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.instructions", QString(4097, 'x')}}, false).error.isEmpty());
        const auto plan = PrepareAssistantPreferencePatch(cfg, {}, {}, args, false);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QVERIFY(!PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.instructions", Emoji().repeated(4097)}}, false).error.isEmpty());
    }

    // [planner] Assistant reads are projected and budgeted without truncating a
    // value: equal copies become typed sameAs references, then the largest rows
    // are withheld explicitly. A withheld row has no requested value, so the
    // target's getter cannot rebaseline it.
    void assistantReadsProjectAndBudgetWithoutTruncation() {
        const auto row = [](const QJsonValue& requested, const QJsonValue& saved) {
            return QJsonObject{{"requested", requested}, {"effective", requested}, {"saved", saved},
                               {"available", true}, {"options", QJsonArray{"a", "b"}}};
        };
        const QString wide(4096, QChar(0x5B57)); // 4096 code points, 12 KiB UTF-8 per copy
        const QString other(4096, QChar(0x6587));
        const auto budgetBytes = [](const QJsonObject& read) {
            return QJsonDocument(QJsonObject{{"parameters", read["parameters"]}}).toJson(QJsonDocument::Compact).size();
        };
        const auto audit = [&](const QJsonObject& original, const QJsonObject& read) {
            const auto rows = read["parameters"].toObject();
            QCOMPARE(rows.keys(), original.keys());
            for (auto it = rows.begin(); it != rows.end(); ++it) {
                const auto now = it.value().toObject(), before = original[it.key()].toObject();
                for (const auto* key : {"requested", "effective", "saved"}) {
                    if (!now.contains(key)) continue;
                    const auto value = now[key];
                    if (value.isString()) QCOMPARE(value, before[key]);
                    else if (value.isObject()) {
                        QCOMPARE(value.toObject(), (QJsonObject{{"sameAs", "requested"}}));
                        QCOMPARE(before[key], before["requested"]);
                    } else {
                        QVERIFY(value.isNull());
                        QVERIFY(now["filterRequired"].toBool());
                        QCOMPARE(now["unsupportedReason"].toString(), QStringLiteral("response_budget"));
                        QVERIFY(!now.contains("options"));
                    }
                }
            }
        };
        const QJsonObject shared{{"assistant_preferences.instructions", row(wide, wide)},
                                 {"assistant_preferences.explainPrompt", row(wide, "older")},
                                 {"assistant_preferences.httpModel", row("fixture", "fixture")}};
        const auto requested = BoundAssistantPreferenceRead(shared, "requested")["parameters"].toObject();
        for (const auto value : requested) QVERIFY(!value.toObject().contains("saved") && !value.toObject().contains("effective"));
        const auto saved = BoundAssistantPreferenceRead(shared, "saved")["parameters"].toObject();
        for (const auto value : saved) QVERIFY(!value.toObject().contains("requested") && !value.toObject().contains("effective"));
        const auto referenced = BoundAssistantPreferenceRead(shared, "all");
        QVERIFY(budgetBytes(referenced) <= 29 * 1024);
        audit(shared, referenced);
        for (const auto value : referenced["parameters"].toObject()) QVERIFY(!value.toObject()["filterRequired"].toBool());
        // Unsaved edits on two large fields: one row is withheld, the rest stay whole.
        const QJsonObject edited{{"assistant_preferences.instructions", row(wide, other)},
                                 {"assistant_preferences.explainPrompt", row(other, wide)},
                                 {"assistant_preferences.httpModel", row("fixture", "fixture")}};
        const auto withheld = BoundAssistantPreferenceRead(edited, "all");
        QVERIFY(budgetBytes(withheld) <= 29 * 1024);
        audit(edited, withheld);
        int filtered = 0;
        for (const auto value : withheld["parameters"].toObject()) filtered += value.toObject()["filterRequired"].toBool();
        QCOMPARE(filtered, 1);
        QVERIFY(!withheld["parameters"].toObject()["assistant_preferences.httpModel"].toObject()["filterRequired"].toBool());
        // A native value larger than the read budget (AI Settings has no length
        // cap) stays unreadable even through a single-field projection (F-14).
        const QJsonObject huge{{"assistant_preferences.instructions", row(QString(14000, QChar(0x5B57)), QJsonValue(QJsonValue::Null))}};
        const auto single = BoundAssistantPreferenceRead(huge, "requested")["parameters"].toObject()["assistant_preferences.instructions"].toObject();
        QVERIFY(single["requested"].isNull());
        QVERIFY(single["valueOutOfContract"].toBool());
        QVERIFY(single["requiresUi"].toBool());
        QVERIFY(!single["filterRequired"].toBool());
        QCOMPARE(single["unsupportedReason"].toString(),QStringLiteral("value_out_of_range"));
    }

    // [planner] Single-line identifiers appear in approval text; invisible bidi
    // and zero-width format characters must not reach them (F-12).
    void assistantSingleLineValuesRejectInvisibleFormatting() {
        okuflow::settings::AssistiveSettings cfg;
        QStringList accepted;
        for (const auto& value : {QStringLiteral("vision‮ledom"), QStringLiteral("vision​model"),
                                  QStringLiteral("vision⁦model⁩")}) {
            const auto plan = PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.httpModel", value}}, false);
            if (plan.error.isEmpty()) accepted << value.toUtf8().toHex();
        }
        QVERIFY(!PrepareAssistantPreferencePatch(cfg, {}, {}, {{"assistant_preferences.httpModel", "vision\x01model"}}, false).error.isEmpty());
        QVERIFY2(accepted.isEmpty(), qPrintable(accepted.join(", ")));
    }

    // [service][fake target] An Apply failure after earlier writes must not
    // leave a partially applied batch (F-03).
    void failedApplyLeavesNoPartialEffects() {
        FakeTarget target;
        target.fields = {{Field::Contrast, 1.0}, {Field::Brightness, 0.0}};
        target.prepare = [](ToolId tool, const QJsonObject& args) {
            ControlPlan plan;
            if (tool == ToolId::ImageSettingsSet)
                plan.writes = {{Field::Contrast, args["image.contrast"]}, {Field::Brightness, args["image.brightness"]}};
            return plan;
        };
        target.apply = [&](const FieldWrite& write) {
            if (write.field == Field::Brightness && write.value != QJsonValue(0.0)) return false;
            target.fields[write.field] = write.value; return true;
        };
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service);
        const auto result = Call(service, context, "batch", "image_settings_set", {{"image.contrast", 2.0}, {"image.brightness", 0.5}});
        QVERIFY(!result.content["ok"].toBool());
        QVERIFY(!result.content["partial"].toBool());
        QVERIFY(result.content["rolledBack"].toBool());
        QCOMPARE(target.fields[Field::Contrast], QJsonValue(1.0));
        QCOMPARE(target.fields[Field::Brightness],QJsonValue(0.0));
        QCOMPARE(target.saves,0);
    }

    // [service][fake target] Approval must execute the plan the person saw. F-01
    // (identity-only comparison) was repaired during this review at 22:06; a
    // widened or re-valued re-prepared plan is now stale. Hard regression check.
    void approvalExecutesOnlyTheReviewedPlan() {
        FakeTarget target;
        target.fields = {{Field::Contrast, 1.0}, {Field::Brightness, 0.0}};
        bool widened = false;
        target.prepare = [&](ToolId tool, const QJsonObject& args) {
            ControlPlan plan;
            if (tool != ToolId::ImageSettingsSet) return plan;
            plan.requiresApproval = true; plan.approvalDomain = ApprovalDomain::Configuration;
            plan.targetIdentity = QStringLiteral("fixture-target");
            plan.writes = {{Field::Contrast, args["image.contrast"]}};
            if (widened) plan.writes.push_back({Field::Brightness, 0.9});
            return plan;
        };
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service);
        const auto asked = Call(service, context, "ask", "image_settings_set", {{"image.contrast", 2.0}});
        QCOMPARE(Error(asked), QStringLiteral("consent_required"));
        const auto approvalId = asked.content["approvalId"].toString();
        QCOMPARE(service.PendingApproval()["arguments"].toObject(), (QJsonObject{{"image.contrast", 2.0}}));
        QCOMPARE(target.applies, 0);
        widened = true;
        QCOMPARE(Error(service.ResolveApproval(approvalId, true)), QStringLiteral("stale_state"));
        QCOMPARE(Error(service.ResolveApproval(approvalId, true)), QStringLiteral("not_found"));
        QCOMPARE(target.fields[Field::Brightness], QJsonValue(0.0));
        QCOMPARE(target.applies, 0);
        // Same fields, different normalized value: also stale.
        widened = false;
        double quantized = 2.0;
        target.prepare = [&](ToolId tool, const QJsonObject&) {
            ControlPlan plan;
            if (tool != ToolId::ImageSettingsSet) return plan;
            plan.requiresApproval = true; plan.approvalDomain = ApprovalDomain::Configuration;
            plan.targetIdentity = QStringLiteral("fixture-target");
            plan.writes = {{Field::Contrast, quantized}};
            return plan;
        };
        const auto again = Call(service, context, "ask-2", "image_settings_set", {{"image.contrast", 2.0}});
        QCOMPARE(Error(again), QStringLiteral("consent_required"));
        quantized = 2.5;
        QCOMPARE(Error(service.ResolveApproval(again.content["approvalId"].toString(), true)), QStringLiteral("stale_state"));
        QCOMPARE(target.fields[Field::Contrast], QJsonValue(1.0));
    }

    // [service][planner] sceneExplain=true is frame egress. F-02 (no domain, so a
    // batch started effects before the Apply-time grant check) was repaired at
    // 22:06/22:08 via ControlPlan::requiresFrames. Hard regression check for the
    // external lease (deny before effects) and the embedded turn (frame-grant
    // approval that activates the grant without applying the batch).
    void frameEgressPreferenceNeedsFrameGrantBeforeEffects() {
        FakeTarget target;
        target.fields = {{Field::AssistantOverlay, true}, {Field::AssistantSceneExplain, false}};
        okuflow::settings::AssistiveSettings cfg;
        target.prepare = [&](ToolId tool, const QJsonObject& args) {
            return tool == ToolId::AssistantPreferencesSet ? PrepareAssistantPreferencePatch(cfg, {}, {}, args, false) : ControlPlan{};
        };
        AppControlService* current = nullptr;
        // Mirrors OkuFlowApp::SetAgentSceneEnabled: only an active context with a frame grant may start scene requests.
        target.apply = [&](const FieldWrite& write) {
            if (write.field == Field::AssistantSceneExplain &&
                !current->ContextStatus()["grants"].toObject()["frames"].toBool()) return false;
            target.fields[write.field] = write.value; return true;
        };
        const QJsonObject args{{"assistant_preferences.overlayEnabled", false}, {"assistant_preferences.sceneExplain", true}};
        {
            AppControlService service(target, CatalogEdition::DevFull); current = &service;
            service.SetEnabled(false, true);
            const auto lease = service.Begin({ContextKind::ExternalLease, QStringLiteral("lease-owner"), 1, {}});
            QCOMPARE(Error(Call(service, lease, "scene", "assistant_preferences_set", args)), QStringLiteral("denied_by_policy"));
            QCOMPARE(target.applies, 0);
            QVERIFY(service.PendingApproval().isEmpty());
        }
        AppControlService service(target, CatalogEdition::DevFull); current = &service;
        const auto turn = Embedded(service);
        const auto asked = Call(service, turn, "scene", "assistant_preferences_set", args);
        QCOMPARE(Error(asked), QStringLiteral("consent_required"));
        QCOMPARE(service.PendingApproval()["kind"].toString(), QStringLiteral("frame-grant"));
        const auto granted = service.ResolveApproval(asked.content["approvalId"].toString(), true);
        QVERIFY(granted.content["grantActivated"].toBool());
        QCOMPARE(target.applies, 0);
        QVERIFY(Call(service, turn, "scene-retry", "assistant_preferences_set", args).content["ok"].toBool());
        QCOMPARE(target.fields[Field::AssistantSceneExplain], QJsonValue(true));
        QCOMPARE(target.fields[Field::AssistantOverlay], QJsonValue(false));
    }

    // [service][fake target] A failed grouped read must not refresh the stale
    // baseline; only a successful read of that field can.
    void failedGroupedReadKeepsStaleBaseline() {
        FakeTarget target;
        target.fields = {{Field::Contrast, 1.0}};
        bool readFails = true;
        target.prepare = [](ToolId tool, const QJsonObject& args) {
            ControlPlan plan;
            if (tool == ToolId::ImageSettingsGet) plan.observedFields = {Field::Contrast};
            if (tool == ToolId::ImageSettingsSet) plan.writes = {{Field::Contrast, args["image.contrast"]}};
            return plan;
        };
        target.query = [&](ToolId, const QJsonObject&) {
            return readFails ? ControlResult::Failure("unavailable").content
                             : QJsonObject{{"parameters", QJsonObject{{"image.contrast", QJsonObject{{"requested", 1.4}}}}}};
        };
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service);
        target.fields[Field::Contrast] = 1.4; service.NativeFieldsChanged({Field::Contrast});
        QCOMPARE(Error(Call(service, context, "read-1", "image_settings_get", {{"fields", QJsonArray{"image.contrast"}}})), QStringLiteral("unavailable"));
        QCOMPARE(Error(Call(service, context, "set-1", "image_settings_set", {{"image.contrast", 2.0}})), QStringLiteral("stale_state"));
        readFails = false;
        QVERIFY(Call(service, context, "read-2", "image_settings_get", {{"fields", QJsonArray{"image.contrast"}}}).content["ok"].toBool());
        QVERIFY(Call(service, context, "set-2", "image_settings_set", {{"image.contrast", 2.0}}).content["ok"].toBool());
        QCOMPARE(target.fields[Field::Contrast], QJsonValue(2.0));
    }

    // [service][fake target] Event replay keeps 256 records, returns at most 32
    // per read, reports loss and advances its cursor monotonically to the end.
    void eventReplayIsBoundedAndCursorAdvances() {
        FakeTarget target; target.fields = {{Field::Contrast, 1.0}};
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service);
        for (int i = 0; i < 300; ++i) service.NativeFieldsChanged({Field::Contrast});
        qint64 cursor = 0, last = 0; int reads = 0, delivered = 0; bool lost = false;
        for (; reads < 20; ++reads) {
            const auto state = Call(service, context, QStringLiteral("events-%1").arg(reads), "app_state_get", {{"sinceEvent", cursor}});
            QVERIFY(state.content["ok"].toBool());
            const auto events = state.content["eventsAfter"].toArray();
            QVERIFY(events.size() <= 32);
            if (reads == 0) lost = state.content["eventsLost"].toBool();
            else QVERIFY(!state.content["eventsLost"].toBool());
            for (const auto event : events) {
                const auto sequence = event.toObject()["sequence"].toInteger();
                QVERIFY(sequence > last); last = sequence; ++delivered;
                QCOMPARE(event.toObject()["contextId"].toString(), context);
            }
            const auto next = state.content["nextEventCursor"].toInteger();
            if (events.isEmpty()) { QCOMPARE(next, cursor); break; }
            QVERIFY(next > cursor); cursor = next;
        }
        QVERIFY(lost);
        QCOMPARE(delivered, 256);
        QCOMPARE(last, qint64(300));
        QVERIFY(reads <= 9);
    }

    // [service][fake target] Queue (8), idempotency reservations (128), token
    // bucket (burst four, two per second) and embedded mutation budget (20).
    void reservationQueueAndMutationBudgetsHold() {
        FakeTarget target;
        target.fields = {{Field::ZoomEnabled, true}, {Field::ZoomAmount, 1.0}};
        target.prepare = [](ToolId tool, const QJsonObject& args) {
            ControlPlan plan;
            if (tool == ToolId::Zoom) plan.writes = {{Field::ZoomAmount, args["magnification"]}};
            return plan;
        };
        qint64 now = 1000;
        AppControlService service(target, CatalogEdition::DevFull, nullptr, [&] { return now; });
        auto context = Embedded(service);
        int completed = 0; QString ninth;
        for (int i = 0; i < 9; ++i)
            service.Submit({context, QStringLiteral("queued-%1").arg(i), "app_state_get", {}}, [&, i](ControlResult result) {
                if (i == 8) ninth = Error(result); else completed += result.content["ok"].toBool();
            });
        QCOMPARE(ninth, QStringLiteral("busy"));
        QTRY_COMPARE(completed, 8);
        for (int i = 8; i < 128; ++i)
            QVERIFY(Call(service, context, QStringLiteral("reserve-%1").arg(i), "app_state_get").content["ok"].toBool());
        QCOMPARE(Error(Call(service, context, "reserve-128", "app_state_get")), QStringLiteral("budget_exhausted"));
        QVERIFY(Call(service, context, "reserve-9", "app_state_get").content["ok"].toBool());
        QCOMPARE(Error(Call(service, context, "reserve-9", "app_state_get", {{"sinceEvent", 1}})), QStringLiteral("invalid_argument"));
        service.End(context);
        context = Embedded(service);
        int applied = 0, busy = 0, step = 0;
        const auto zoom = [&] {
            ++step;
            const auto result = Call(service, context, QStringLiteral("zoom-%1").arg(step), "view_zoom_set",
                                     {{"magnification", 1.0 + 0.1 * step}});
            applied += result.content["ok"].toBool(); busy += Error(result) == "busy";
            return result;
        };
        for (int i = 0; i < 5; ++i) zoom();
        QCOMPARE(applied, 4); QCOMPARE(busy, 1);
        while (applied < 20) { now += 500; QVERIFY(zoom().content["ok"].toBool()); }
        now += 5000;
        QCOMPARE(Error(zoom()), QStringLiteral("budget_exhausted"));
        QCOMPARE(target.applies, 20);
    }

    // [service][fake target] Oversized replies and arguments fail closed before
    // delivery or before the target sees them.
    void oversizedStructuredResultsFailClosed() {
        FakeTarget target;
        target.query = [](ToolId, const QJsonObject&) {
            return QJsonObject{{"parameters", QJsonObject{{"image.contrast", QString(40000, 'x')}}}};
        };
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service);
        QCOMPARE(Error(Call(service, context, "large", "image_settings_get")), QStringLiteral("internal"));
        // 4096 schema-valid astral code points exceed the separate 16 KiB argument cap.
        const auto prepares = target.prepares;
        QCOMPARE(Error(Call(service, context, "args", "assistant_preferences_set",
                            {{"assistant_preferences.instructions", Emoji().repeated(4096)}})), QStringLiteral("invalid_argument"));
        QCOMPARE(target.prepares, prepares);
        QVERIFY(Call(service, context, "after", "app_state_get").content["ok"].toBool());
    }

    // [service][fake target] A late asynchronous completion of an approved
    // action retains its original human receipt identity, never appearing
    // in the replacement context's tool-visible event stream.
    void lateApprovalReceiptStaysWithItsContext() {
        FakeTarget target;
        AppControlService service(target, CatalogEdition::DevFull);
        auto first = Embedded(service);
        const auto asked = Call(service, first, "photo", "capture_photo_take");
        QCOMPARE(Error(asked), QStringLiteral("consent_required"));
        QVERIFY(service.ResolveApproval(asked.content["approvalId"].toString(), true).content["ok"].toBool());
        QCOMPARE(target.executes, 1);
        service.End(first);
        const auto second = Embedded(service);
        QVERIFY(!second.isEmpty() && second != first);
        QVector<QJsonObject> receipts;
        service.SetEventSink([&](const QJsonObject& event) {
            if(event["type"]=="operation_receipt")receipts.push_back(event);
        });
        target.held.front()({{{"ok", true}, {"pending", true}, {"operationId", "late"}, {"state", "queued"}}, {}});
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QCOMPARE(receipts.size(),1);
        QCOMPARE(receipts[0]["contextId"].toString(),first);
        QVERIFY(receipts[0]["outcome"].toObject()["pending"].toBool());
        const auto events = Call(service, second, "read", "app_state_get", {{"sinceEvent", 0}}).content["eventsAfter"].toArray();
        for (const auto event : events) QVERIFY(event.toObject()["type"] != "operation_receipt");
    }

    // [service][fake target] A revoked embedded photo grant reverts to exact
    // approval, and one approval never becomes a standing grant.
    void revokedEmbeddedGrantFallsBackToExactApproval() {
        FakeTarget target;
        AppControlService service(target, CatalogEdition::DevFull);
        const auto context = Embedded(service, {false, true, false, false});
        auto reply = std::make_shared<std::optional<ControlResult>>();
        service.Submit({context, "granted", "capture_photo_take", {}}, [reply](ControlResult result) { *reply = std::move(result); });
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QCOMPARE(target.executes, 1);
        target.held.front()({{{"ok", true}, {"pending", true}, {"operationId", "first"}, {"state", "queued"}}, {}});
        QTRY_VERIFY(reply->has_value());
        QVERIFY((*reply)->content["ok"].toBool());
        service.SetGrants(context, {});
        const auto revoked = Call(service, context, "revoked", "capture_photo_take");
        QCOMPARE(Error(revoked), QStringLiteral("consent_required"));
        QCOMPARE(target.executes, 1);
        QVERIFY(service.ResolveApproval(revoked.content["approvalId"].toString(), true).content["ok"].toBool());
        QCOMPARE(target.executes, 2);
        target.held.back()({{{"ok", true}, {"pending", true}, {"operationId", "second"}, {"state", "queued"}}, {}});
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QCOMPARE(Error(Call(service, context, "again", "capture_photo_take")), QStringLiteral("consent_required"));
        QCOMPARE(target.executes, 2);
    }
};

QTEST_GUILESS_MAIN(AgentControlCoverageTests)
#include "agent_control_coverage_tests.moc"
