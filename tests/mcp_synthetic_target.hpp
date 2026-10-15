#pragma once
#include "mcp_jpeg_fixture.hpp"
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/assistant_parameter_policy.hpp"
#include "okuflow/app/agent/operation_tracker.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

// First-party synthetic state machine. No camera, file, audio or native window.
class FixtureTarget final : public okuflow::agent::IAppControlTarget {
    using Field = okuflow::agent::Field;
    using ToolId = okuflow::agent::ToolId;
    using StateId = okuflow::agent::OperationState;
    using Kind = okuflow::agent::OperationKind;
    using Result = okuflow::agent::ControlResult;
    std::map<Field, QJsonValue> fields_{{Field::ZoomEnabled, false},
                                        {Field::ZoomAmount, 1.0},
                                        {Field::CenterX, 0.5},
                                        {Field::CenterY, 0.5},
                                        {Field::Rotation, 0},
                                        {Field::Scheme, "normal"},
                                        {Field::Contrast, 1.0},
                                        {Field::Brightness, 0.0},
                                        {Field::AutoContrast, false},
                                        {Field::TextClarity, false},
                                        {Field::Preset, "fixture-scene"},
                                        {Field::PresetList, 1}};
    okuflow::agent::OperationTracker operations_;
    std::vector<QString> operationIds_;
    std::map<QString, QString> recordingOperations_, sessionsByOwner_;
    QString session_, owner_;
    const QString cameraA_ = "cam-" + QUuid::createUuid().toString(QUuid::Id128);
    const QString cameraB_ = "cam-" + QUuid::createUuid().toString(QUuid::Id128);
    QString camera_ = cameraA_;
    QString assistantProfile_ = "distinct";
    QString RequestedInstructions() const {
        return assistantProfile_ == "short" ? QStringLiteral("Synthetic instructions")
                                            : QString::fromUtf8("\xf0\x9f\xa7\xaa").repeated(4096);
    }
    QString Create(Kind kind, const QString &owner, const QString &session = {}) {
        auto id = operations_.Create(kind, owner, session);
        operationIds_.push_back(id);
        return id;
    }

  public:
    int writes{}, ownedStops{}, acceptedPhotos{};
    bool holdPhoto{}, userInteracting{};
    std::function<void()> sourceChanged;
    std::function<void(const QJsonObject &)> accepted;
    std::vector<std::function<void()>> held;
    QJsonObject State() const override {
        QJsonObject fields;
        for (const auto &[field, value] : fields_)
            fields[okuflow::agent::FieldName(field)] = value;
        return {{"fixture", true},
                {"fields", fields},
                {"writes", writes},
                {"cameraHandle", camera_},
                {"savedCameraHandle", cameraA_},
                {"recordingActive", !session_.isEmpty()},
                {"sessionId", session_},
                {"ownerKind", owner_ == "manual" ? "manual" : "external-lease"},
                {"ownedStops", ownedStops},
                {"acceptedPhotos", acceptedPhotos}};
    }
    std::map<Field, QJsonValue> Fields() const override {
        auto fields = fields_;
        fields[Field::AssistantInstructions] = RequestedInstructions();
        return fields;
    }
    okuflow::agent::ControlPlan Prepare(ToolId tool, const QJsonObject &args,
                                        const QString &owner) const override {
        okuflow::agent::ControlPlan p;
        // DevFull fixture is an explicit metadata/read subset, not a fake
        // success implementation of all newly advertised native setters.
        if (static_cast<int>(tool) >= static_cast<int>(ToolId::ControlsList) &&
            tool != ToolId::ControlsList && tool != ToolId::ViewSettingsGet &&
            tool != ToolId::AssistantPreferencesGet) {
            p.error = "unavailable";
            return p;
        }
        if (tool == ToolId::AssistantPreferencesGet && args.value("valueKind").toString("all") != "saved") {
            const auto row = Query(tool, args)
                                 .value("parameters")
                                 .toObject()
                                 .value("assistant_preferences.instructions")
                                 .toObject();
            if (row.value("requested").isString() && !row.value("filterRequired").toBool())
                p.observedFields.push_back(Field::AssistantInstructions);
        }
        auto write = [&](Field field, QJsonValue value) { p.writes.push_back({field, value}); };
        auto point = [&](QJsonObject pt) {
            if (pt["space"] != "scene") {
                p.error = "stale_observation";
                return;
            }
            write(Field::CenterX, pt["x"]);
            write(Field::CenterY, pt["y"]);
        };
        switch (tool) {
        case ToolId::Zoom:
            write(Field::ZoomEnabled, true);
            write(Field::ZoomAmount, args["magnification"]);
            if (args.contains("anchor"))
                point(args["anchor"].toObject());
            break;
        case ToolId::Center:
            point(args["point"].toObject());
            break;
        case ToolId::ResetView:
            write(Field::ZoomAmount, 1.0);
            write(Field::CenterX, 0.5);
            write(Field::CenterY, 0.5);
            break;
        case ToolId::Contrast:
            write(Field::Contrast, args["contrast"]);
            write(Field::Brightness, args["brightness"]);
            break;
        case ToolId::ReadingColors: {
            auto intent = args["intent"].toString();
            auto scheme = intent == "scheme"                        ? args["schemeId"].toString()
                          : intent == "grayscale"                   ? "grayscale"
                          : intent == "photographic_color"          ? "normal"
                          : intent == "high_contrast_dark_on_light" ? "black-white"
                                                                    : "white-black";
            if (!QStringList{"normal", "grayscale", "black-white", "white-black"}.contains(scheme))
                p.error = "not_found";
            else {
                write(Field::Scheme, scheme);
                write(Field::TextClarity, intent.startsWith("high_contrast"));
            }
            break;
        }
        case ToolId::ModeActivate:
            if (!QStringList{"fixture-scene", "fixture-document"}.contains(args["presetId"].toString()))
                p.error = "not_found";
            else {
                write(Field::Preset, args["presetId"]);
                write(Field::TextClarity, args["presetId"] == "fixture-document");
            }
            break;
        case ToolId::AutoAdjust:
            write(Field::AutoContrast, true);
            write(Field::TextClarity, args["goal"] != "see_scene");
            break;
        case ToolId::Orientation: {
            double x = fields_.at(Field::CenterX).toDouble(), y = fields_.at(Field::CenterY).toDouble();
            int delta = (args["degreesClockwise"].toInt() - fields_.at(Field::Rotation).toInt() + 360) % 360;
            for (int i = 0; i < delta / 90; ++i) {
                auto old = x;
                x = 1 - y;
                y = old;
            }
            write(Field::Rotation, args["degreesClockwise"]);
            write(Field::CenterX, x);
            write(Field::CenterY, y);
            break;
        }
        case ToolId::CameraSelect:
            p.targetIdentity = args["cameraHandle"].toString();
            if (p.targetIdentity != cameraA_ && p.targetIdentity != cameraB_)
                p.error = "not_found";
            break;
        case ToolId::RecordingStart:
            if (!session_.isEmpty() && owner_ != owner)
                p.error = "already_active";
            break;
        case ToolId::RecordingStop: {
            auto requested = args["sessionId"].toString();
            if (requested.isEmpty() && sessionsByOwner_.contains(owner))
                requested = sessionsByOwner_.at(owner);
            p.targetIdentity = requested;
            auto found = recordingOperations_.find(requested);
            auto op = found == recordingOperations_.end() ? std::nullopt : operations_.Get(found->second);
            p.ownedRecording = (requested == session_ && owner_ == owner) || (op && op->owner == owner);
            p.terminalRecording = op && (op->state == StateId::Completed || op->state == StateId::Failed);
            if (requested.isEmpty())
                p.error = "not_found";
            else if (requested != session_ && !p.terminalRecording)
                p.error = "stale_state";
            break;
        }
        default:
            break;
        }
        return p;
    }
    bool Apply(const okuflow::agent::FieldWrite &write) override {
        if (!fields_.contains(write.field))
            return false;
        fields_[write.field] = write.value;
        ++writes;
        return true;
    }
    bool Save() override { return true; }
    QJsonObject Query(ToolId tool, const QJsonObject &args) const override {
        if (tool == ToolId::AssistantPreferencesGet) {
            const auto filter = args.value("fields").toArray();
            if (!filter.isEmpty() &&
                (filter.size() != 1 || filter[0] != "assistant_preferences.instructions"))
                return Result::Failure("unavailable").content;
            const auto requested = RequestedInstructions();
            const auto saved = assistantProfile_ == "distinct"
                                   ? QString::fromUtf8("\xf0\x9f\xa7\xad").repeated(4096)
                                   : requested;
            auto result = okuflow::agent::BoundAssistantPreferenceRead(
                {{"assistant_preferences.instructions", QJsonObject{{"requested", requested},
                                                                    {"effective", requested},
                                                                    {"saved", saved},
                                                                    {"available", true},
                                                                    {"unsupportedReason", ""},
                                                                    {"persistence", "shared_saved"},
                                                                    {"writable", true}}}},
                args.value("valueKind").toString("all"));
            result["fixture"] = true;
            result["support"] = "synthetic-assistant-projection";
            return result;
        }
        if (tool == ToolId::ControlsList) {
            const auto page = okuflow::agent::ParameterMetadataPage(
                args["group"].toString().toStdString(), args["afterField"].toString().toStdString(),
                static_cast<std::size_t>(args["limit"].toInt(16)));
            return QJsonDocument::fromJson(QByteArray::fromStdString(page)).object();
        }
        if (tool == ToolId::ViewSettingsGet) {
            QJsonObject parameters;
            const auto requested = args["fields"].toArray();
            for (const auto &[id, field] : std::map<QString, Field>{{"view.zoom.enabled", Field::ZoomEnabled},
                                                                    {"view.zoom.amount", Field::ZoomAmount},
                                                                    {"view.center.x", Field::CenterX},
                                                                    {"view.center.y", Field::CenterY}}) {
                if (requested.isEmpty() || requested.contains(id))
                    parameters[id] = fields_.at(field);
            }
            return {{"fixture", true}, {"support", "synthetic-read-subset"}, {"parameters", parameters}};
        }
        if (tool == ToolId::Operation)
            return operations_.SafeStatus(args["operationId"].toString());
        if (tool == ToolId::Modes)
            return {{"modes", QJsonArray{QJsonObject{{"presetId", "fixture-scene"},
                                                     {"displayName", "Synthetic scene"}},
                                         QJsonObject{{"presetId", "fixture-document"},
                                                     {"displayName", "Synthetic document"}}}}};
        if (tool == ToolId::CameraSources)
            return {{"sources",
                     QJsonArray{QJsonObject{{"cameraHandle", cameraA_}, {"displayName", "Camera 1"}},
                                QJsonObject{{"cameraHandle", cameraB_}, {"displayName", "Camera 2"}}}}};
        return State();
    }
    void Execute(ToolId tool, const QJsonObject &args, const QString &owner, okuflow::agent::ContextKind,
                 okuflow::agent::ControlCompletion done) override {
        if (tool == ToolId::Observe) {
            done({{{"ok", true}}, McpFixtureJpeg()});
            return;
        }
        if (tool == ToolId::Metrics) {
            done({{{"ok", true}, {"lumaMean", 0.5}, {"fixture", true}}, {}});
            return;
        }
        if (tool == ToolId::Wait) {
            done({{{"ok", true}, {"met", true}, {"fixture", true}}, {}});
            return;
        }
        if (tool == ToolId::Photo) {
            auto id = Create(Kind::Photo, owner);
            ++acceptedPhotos;
            Result result{{{"ok", true}, {"pending", true}, {"operationId", id}, {"state", "queued"}}, {}};
            if (holdPhoto) {
                held.push_back([done, result] { done(result); });
                if (accepted)
                    accepted({{"type", "photo-accepted"}, {"operationId", id}});
            } else
                done(result);
            return;
        }
        if (tool == ToolId::CameraSelect || tool == ToolId::AutoAdjust) {
            auto id = Create(tool == ToolId::CameraSelect ? Kind::Camera : Kind::Adjustment, owner);
            if (tool == ToolId::CameraSelect) {
                camera_ = args["cameraHandle"].toString();
                if (sourceChanged)
                    sourceChanged();
            }
            done({{{"ok", true}, {"pending", true}, {"operationId", id}, {"state", "requested"}}, {}});
            return;
        }
        if (tool == ToolId::RecordingStart) {
            if (!session_.isEmpty()) {
                auto result = operations_.SafeStatus(recordingOperations_.at(session_));
                result["changed"] = false;
                done({result, {}});
                return;
            }
            session_ = QUuid::createUuid().toString(QUuid::Id128);
            owner_ = owner;
            sessionsByOwner_[owner] = session_;
            auto id = Create(Kind::Recording, owner, session_);
            recordingOperations_[session_] = id;
            done({{{"ok", true},
                   {"pending", true},
                   {"operationId", id},
                   {"sessionId", session_},
                   {"state", "starting"},
                   {"requestedAudio", args["audio"]},
                   {"actualAudio", "none"},
                   {"ownerKind", "external-lease"}},
                  {}});
            return;
        }
        if (tool == ToolId::RecordingStop) {
            auto plan = Prepare(tool, args, owner);
            if (!plan.error.isEmpty()) {
                done(Result::Failure(plan.error));
                return;
            }
            auto id = recordingOperations_.at(plan.targetIdentity);
            if (plan.terminalRecording) {
                auto result = operations_.SafeStatus(id);
                result["changed"] = false;
                done({result, {}});
                return;
            }
            operations_.Advance(id, StateId::Stopping);
            session_.clear();
            owner_.clear();
            ++ownedStops;
            done({{{"ok", true},
                   {"pending", true},
                   {"operationId", id},
                   {"sessionId", plan.targetIdentity},
                   {"state", "stopping"}},
                  {}});
            return;
        }
        done(Result::Failure("unavailable"));
    }
    void Settle() {
        for (const auto &id : operationIds_) {
            auto op = operations_.Get(id);
            if (!op)
                continue;
            if (op->kind == Kind::Photo && op->state == StateId::Queued) {
                operations_.Advance(id, StateId::Readback);
                operations_.Advance(id, StateId::Writing);
                operations_.Advance(id, StateId::Committed,
                                    {{"originalName", "IMG_fixture_original.jpg"},
                                     {"processedName", "IMG_fixture_processed.jpg"}});
            } else if (op->kind == Kind::Recording && op->state == StateId::Starting)
                operations_.Advance(id, StateId::Recording);
            else if (op->kind == Kind::Recording && op->state == StateId::Stopping)
                operations_.Advance(id, StateId::Completed);
            else if ((op->kind == Kind::Camera || op->kind == Kind::Adjustment) &&
                     op->state == StateId::Requested) {
                operations_.Advance(id, StateId::Starting);
                operations_.Advance(id, StateId::FirstFrame);
            }
        }
    }
    void Release() {
        holdPhoto = false;
        auto callbacks = std::move(held);
        held.clear();
        for (auto &callback : callbacks)
            callback();
    }
    void ManualRecording() {
        session_ = QUuid::createUuid().toString(QUuid::Id128);
        owner_ = "manual";
    }
    void UserZoom() { fields_[Field::ZoomAmount] = 3.0; }
    void SetAssistantProfile(const QString &kind) { assistantProfile_ = kind; }
    bool ModalActive() const override { return false; }
    bool UserInteracting() const override { return userInteracting; }
    bool RecordingActive() const override { return !session_.isEmpty(); }
    QString RecordingSession() const override { return session_; }
    QString RecordingOwner() const override { return owner_; }
    void StopOwnedRecording(const QString &owner) override {
        if (owner_ != owner || session_.isEmpty())
            return;
        auto found = recordingOperations_.find(session_);
        if (found != recordingOperations_.end())
            operations_.Advance(found->second, StateId::Stopping);
        session_.clear();
        owner_.clear();
        ++ownedStops;
    }
};
