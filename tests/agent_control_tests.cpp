#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/operation_tracker.hpp"
#include "okuflow/app/agent/observation_service.hpp"
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/app/agent/camera_source_intent.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/receipt_labels.hpp"
#include <QDateTime>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtTest>
#include <set>
#include <algorithm>
#include <future>
#include <stdexcept>

using namespace okuflow::agent;
class Target final : public IAppControlTarget {
public:
    std::map<Field,QJsonValue> fields{{Field::ZoomEnabled,false},{Field::ZoomAmount,1.0},{Field::CenterX,0.5},{Field::CenterY,0.5},
        {Field::Rotation,0},{Field::Contrast,1.0},{Field::Brightness,0.0},{Field::Upscaler,0},{Field::Polarity,0}};
    int saves{}, effects{}, captures{}, ownedStops{}, cleanupCalls{}, sourceCleanupCalls{};
    bool modal{}, interacting{}, recording{}, saveFails{}, exactApproval{}, configurationApproval{};
    bool failBrightness{}, failContrastRollback{}, coupledContrast{}, retryUnacknowledged{};
    bool failCommit{}, failCameraApply{};
    int sourceRestores{};
    std::function<void()> synchronize;
    std::function<bool(const ControlPlan&)> rejectBegin; // validation-only refusal, no effect
    QString session{"session-a"}, owner{"manual"}, ownedSource;
    ControlCompletion delayed;
    QJsonObject operationStatus{{"ok",true},{"operationId","operation-a"},{"state","queued"}};
    QJsonObject State() const override { return {{"zoom",fields.at(Field::ZoomAmount)}}; }
    std::map<Field,QJsonValue> Fields() const override { return fields; }
    void SynchronizeNativeState() override { if(synchronize)synchronize(); }
    bool BeginFieldPlan(const ControlPlan& plan) override { return !(rejectBegin && rejectBegin(plan)); }
    bool CommitFieldPlan(const ControlPlan& plan) override {
        if(failCommit && std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write) {
            return write.field==Field::Contrast && write.value==2.0;
        })) {fields[Field::Contrast]=2.0;return false;}
        return true;
    }
    ControlPlan Prepare(ToolId tool,const QJsonObject& args,const QString&) const override {
        ControlPlan plan;
        plan.requiresApproval = exactApproval;
        plan.approvalDomain = configurationApproval ? ApprovalDomain::Configuration : ApprovalDomain::None;
        if (tool == ToolId::Zoom) plan.writes = {{Field::ZoomAmount,args["magnification"]}};
        if (tool == ToolId::Orientation) plan.writes = {{Field::Rotation,args["degreesClockwise"]}};
        if (tool == ToolId::Contrast) plan.writes = {{Field::Contrast,args["contrast"]},{Field::Brightness,args["brightness"]}};
        if (tool == ToolId::AutoAdjust) plan.writes = {{Field::Contrast,1.5}};
        if (tool == ToolId::RecordingStop) plan.targetIdentity = session;
        if (tool == ToolId::AssistantPreferencesSet) plan.writes = {{Field::Contrast,args["assistant_preferences.httpModel"]}};
        if (tool == ToolId::ImageSettingsGet) {
            for (const auto& value : args["fields"].toArray()) {
                if (value == "image.contrast") plan.observedFields.push_back(Field::Contrast);
                if (value == "image.brightness") plan.observedFields.push_back(Field::Brightness);
            }
        }
        if (tool == ToolId::ImageSettingsSet) {
            if (args.contains("image.contrast")) plan.writes.push_back({Field::Contrast,args["image.contrast"],{},retryUnacknowledged});
            if (args.contains("image.brightness")) plan.writes.push_back({Field::Brightness,args["image.brightness"]});
            if (args.contains("image.spatial.backend")) plan.writes.push_back({Field::Upscaler,args["image.spatial.backend"]=="nis"?1:0});
            if (args.contains("image.textClarity.polarity")) plan.writes.push_back({Field::Polarity,QStringList{"auto","dark-on-light","light-on-dark"}.indexOf(args["image.textClarity.polarity"].toString())});
        }
        if (tool == ToolId::CameraSettingsSet && args.contains("camera.acceleration"))
            plan.writes.push_back({Field::CameraAcceleration,args["camera.acceleration"],"cam-fixture"});
        return plan;
    }
    bool Apply(const FieldWrite& write) override {
        if(failCameraApply && write.field==Field::CameraAcceleration)return false;
        if(failContrastRollback && write.field==Field::Contrast && write.value==1.0)return false;
        if(failBrightness && write.field==Field::Brightness && write.value!=0.0)return false;
        ++effects; fields[write.field]=write.value;
        if(coupledContrast && write.field==Field::Contrast)fields[Field::Brightness]=0.1;
        return true;
    }
    bool Save() override { ++saves; return !saveFails; }
    QJsonObject Query(ToolId tool,const QJsonObject& args) const override {
        if(tool==ToolId::Operation)return operationStatus;
        QJsonObject values;
        if (tool == ToolId::ImageSettingsGet) for (const auto& value : args["fields"].toArray()) {
            if (value == "image.contrast") values["image.contrast"] = QJsonObject{{"requested",fields.at(Field::Contrast)}};
            if (value == "image.brightness") values["image.brightness"] = QJsonObject{{"requested",fields.at(Field::Brightness)}};
        }
        return values.isEmpty() ? QJsonObject{} : QJsonObject{{"parameters",values}};
    }
    void Execute(ToolId tool,const QJsonObject&,const QString&,ContextKind,ControlCompletion completion) override {
        if (tool == ToolId::Observe || tool == ToolId::Metrics) delayed = std::move(completion);
        else { ++captures; completion({{{"ok",true},{"pending",true},{"operationId","operation-a"},{"state","queued"}},{}}); }
    }
    bool ModalActive() const override { return modal; }
    bool UserInteracting() const override { return interacting; }
    bool RecordingActive() const override { return recording; }
    QString RecordingSession() const override { return session; }
    QString RecordingOwner() const override { return owner; }
    void StopOwnedRecording(const QString& requestedOwner) override {
        ++cleanupCalls;
        if (recording && owner == requestedOwner) { ++ownedStops; recording = false; }
    }
    // Exact temporary-source ownership: only the holding owner restores, once.
    void CancelOwnedSource(const QString& requested) override {
        ++sourceCleanupCalls;
        if (HasOwnedSource(requested)) { ownedSource.clear(); ++sourceRestores; }
    }
    bool HasOwnedSource(const QString& requested) const override { return !requested.isEmpty() && ownedSource == requested; }
};
class AgentControlTests : public QObject {
    Q_OBJECT
    QString Begin(AppControlService& service, ControlGrants grants = {}) {
        service.SetEnabled(true,true);
        return service.Begin({ContextKind::EmbeddedTurn,"trusted-session",1,grants});
    }
    ControlResult Call(AppControlService& service,const QString& context,const QString& key,
                       const QString& tool,QJsonObject args = {}) {
        std::optional<ControlResult> result;
        service.Submit({context,key,tool,args},[&](ControlResult value) { result = std::move(value); });
        for (int i = 0; i < 30 && !result; ++i) QCoreApplication::processEvents();
        if (!result) return ControlResult::Failure("fixture_no_reply");
        return *result;
    }
    QString Error(const ControlResult& r) { return r.content["error"].toObject()["code"].toString(); }
private slots:
    void embeddedCurrentViewPolicyKeepsFreshnessAndExternalIsolation() {
        Target target;AppControlService service(target,CatalogEdition::DevCamera);
        service.SetEnabled(true,true);
        const auto embedded=service.Begin({ContextKind::EmbeddedTurn,"user-send",1,{},true});
        QVERIFY(!embedded.isEmpty());QVERIFY(service.ContextStatus()["grants"].toObject()["frames"].toBool());
        const auto oldStamp=service.Stamp();QVERIFY(service.AuthorizeFrameDelivery(embedded,oldStamp));
        service.SourceChanged();
        QVERIFY(!service.AuthorizeFrameDelivery(embedded,oldStamp));
        QVERIFY(service.AuthorizeFrameDelivery(embedded,service.Stamp()));
        service.RevokeFrameGrant("user-send");
        const auto denied=Call(service,embedded,"revoked-observe","view_observe");
        QCOMPARE(Error(denied),"consent_denied");QVERIFY(service.PendingApproval().isEmpty());
        service.SourceChanged();QVERIFY(!service.ContextStatus()["grants"].toObject()["frames"].toBool());
        service.End(embedded);
        const auto external=service.Begin({ContextKind::ExternalLease,"external",2,{},true});
        QVERIFY(!external.isEmpty());QVERIFY(!service.ContextStatus()["grants"].toObject()["frames"].toBool());
        QCOMPARE(Error(Call(service,external,"external-observe","view_observe")),"denied_by_policy");
        QVERIFY(service.PendingApproval().isEmpty());
    }
    void approvedAsyncReceiptsTrackAdmissionAndActualOutcome_data() {
        QTest::addColumn<QString>("terminal");
        QTest::newRow("committed")<<QStringLiteral("committed");
        QTest::newRow("failed")<<QStringLiteral("failed");
        QTest::newRow("unknown")<<QStringLiteral("unknown");
    }
    void approvedAsyncReceiptsTrackAdmissionAndActualOutcome() {
        QFETCH(QString,terminal);
        Target target;AppControlService service(target,CatalogEdition::DevCapture);
        const auto context=Begin(service);
        QVector<QJsonObject> receipts;
        service.SetEventSink([&](const QJsonObject& event) {
            if(event["type"]=="operation_receipt")receipts.push_back(event);
        });
        const auto request=Call(service,context,"approval","capture_photo_take");
        const auto id=request.content["approvalId"].toString();QVERIFY(!id.isEmpty());
        QVERIFY(service.ResolveApproval(id,true).content["ok"].toBool());
        QTRY_COMPARE(receipts.size(),1);
        QVERIFY(receipts[0]["outcome"].toObject()["pending"].toBool());
        QCOMPARE(receipts[0]["outcome"].toObject()["state"].toString(),"queued");
        // A subsequent turn cannot relabel the approved action or steal its
        // final outcome. The read-only watcher survives context replacement.
        service.End(context);const auto successor=Begin(service);QVERIFY(successor!=context);
        if(terminal=="unknown")target.operationStatus=QJsonObject{{"ok",false}};
        else target.operationStatus["state"]=terminal;
        QTRY_COMPARE(receipts.size(),2);
        QCOMPARE(receipts[1]["contextId"].toString(),context);
        QCOMPARE(receipts[1]["approvalId"].toString(),id);
        const auto outcome=receipts[1]["outcome"].toObject();
        if(terminal=="unknown")QVERIFY(outcome["statusUnavailable"].toBool());
        else {
            QVERIFY(!outcome["pending"].toBool());
            QCOMPARE(outcome["ok"].toBool(),terminal=="committed");
        }
        QTest::qWait(300);QCOMPARE(receipts.size(),2); // one final receipt
    }
    void failedCommitRestoresNativeStateWithoutSaveOrUndo() {
        Target target;target.failCommit=true;
        AppControlService service(target,CatalogEdition::DevFull);const auto context=Begin(service);
        const auto failure=Call(service,context,"failed-commit","image_settings_set",{{"image.contrast",2.0}});
        QCOMPARE(Error(failure),"internal");QVERIFY(failure.content["rolledBack"].toBool());
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(1.0));QCOMPARE(target.saves,0);
        QVERIFY(service.StopAndUndo().content["restored"].toArray().isEmpty());
    }
    void decisionTimePollingProtectsDispatchApprovalAndUndo() {
        Target target;AppControlService service(target);
        auto observed=target.fields;
        target.synchronize=[&] {
            std::vector<Field> changed;
            for(const auto& [field,value]:target.fields)if(observed.at(field)!=value)changed.push_back(field);
            service.NativeFieldsChanged(changed);observed=target.fields;
        };
        const auto context=Begin(service);
        QVERIFY(Call(service,context,"first","view_zoom_set",{{"magnification",2.0}}).content["ok"].toBool());
        observed=target.fields;target.fields[Field::ZoomAmount]=1.75;
        QCOMPARE(Error(Call(service,context,"stale","view_zoom_set",{{"magnification",2.5}})),"stale_state");
        service.StopAndUndo();QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(1.75));
        const auto next=Begin(service);target.exactApproval=true;
        const auto pending=Call(service,next,"approval","view_zoom_set",{{"magnification",3.0}});
        QCOMPARE(Error(pending),"consent_required");target.fields[Field::ZoomAmount]=1.9;
        QCOMPARE(Error(service.ResolveApproval(pending.content["approvalId"].toString(),true)),"stale_state");
        QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(1.9));
    }
    void ownedSourceAdvancesFormatRevisionAndOperationMetadataIsFinite() {
        Target target;AppControlService service(target);Begin(service);
        const auto previous=service.FieldRevision(Field::CameraFormat);
        service.OwnedSourceRequested("trusted-session",{{Field::Camera,"cam-"+QString(32,'a')},{Field::CameraFormat,"automatic"}});
        QVERIFY(service.FieldRevision(Field::CameraFormat)>previous);
        OperationTracker tracker;const auto id=tracker.Create(OperationKind::Camera,"owner");
        QVERIFY(tracker.Advance(id,OperationState::Stopping,{{"requested",QJsonObject{{"camera.selection","cam-"+QString(32,'a')},
            {"camera.format","C:/private/format"}}},{"applied",QJsonObject{{"camera.acceleration","compatibility"}}},
            {"undoPolicy","temporary-source-owner"},{"savedStartupPreserved",true}}));
        QVERIFY(tracker.Advance(id,OperationState::Failed,{{"errorCode","canceled"}}));
        const auto status=tracker.SafeStatus(id);
        QVERIFY(!status["requested"].toObject().contains("camera.format"));
        QCOMPARE(status["applied"].toObject()["camera.acceleration"],QJsonValue("compatibility"));
        QCOMPARE(status["errorCode"],QJsonValue("canceled"));
        QVERIFY(!tracker.Advance(id,OperationState::FirstFrame));
    }
    void failedNativeRollbackInvalidatesFurtherWritesAndUndo() {
        Target target;target.failBrightness=true;target.failContrastRollback=true;
        AppControlService service(target,CatalogEdition::DevFull);const auto context=Begin(service);
        const auto failure=Call(service,context,"partial","image_settings_set",{{"image.contrast",2.0},{"image.brightness",0.25}});
        QCOMPARE(Error(failure),"internal");QVERIFY(!failure.content["rolledBack"].toBool());
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(2.0));QCOMPARE(target.saves,0);
        QCOMPARE(Error(Call(service,context,"stale-after-failed-inverse","image_settings_set",{{"image.contrast",3.0}})),"stale_state");
        QVERIFY(service.StopAndUndo().content["restored"].toArray().isEmpty());
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(2.0));
    }
    void groupedReceiptsReturnRegistryEnumTokens() {
        Target target; AppControlService service(target,CatalogEdition::DevFull);
        const auto context=Begin(service);
        const auto result=Call(service,context,"finite-enums","image_settings_set",{{"image.spatial.backend","nis"},{"image.textClarity.polarity","light-on-dark"}});
        QVERIFY(result.content["ok"].toBool());
        QCOMPARE(result.content["applied"].toObject()["image.spatial.backend"],QJsonValue("nis"));
        QCOMPARE(result.content["effective"].toObject()["image.textClarity.polarity"],QJsonValue("light-on-dark"));
        QCOMPARE(target.fields.at(Field::Upscaler),QJsonValue(1));
        QCOMPARE(target.fields.at(Field::Polarity),QJsonValue(2));
    }
    void trustedUnacknowledgedRetryRunsButApprovalCannotChangeItsMeaning() {
        Target target; target.retryUnacknowledged=true;
        AppControlService service(target,CatalogEdition::DevFull);
        const auto context=Begin(service);
        QVERIFY(Call(service,context,"retry","image_settings_set",{{"image.contrast",1.0}}).content["ok"].toBool());
        QCOMPARE(target.effects,1);
        target.exactApproval=true;
        const auto approval=Call(service,context,"review-retry","image_settings_set",{{"image.contrast",1.0}});
        QCOMPARE(Error(approval),"consent_required");
        target.retryUnacknowledged=false;
        QCOMPARE(Error(service.ResolveApproval(approval.content["approvalId"].toString(),true)),"stale_state");
        QCOMPARE(target.effects,1);
    }
    void approvalShowsCanonicalCoupledChangesBeforeAnyEffect() {
        Target target; target.exactApproval=true;
        AppControlService service(target,CatalogEdition::DevFull);
        const auto context=Begin(service);
        const auto result=Call(service,context,"review","image_settings_set",{{"image.contrast",2.0},{"image.brightness",0.25}});
        QCOMPARE(Error(result),"consent_required");
        const auto changes=service.PendingApproval()["normalizedChanges"].toObject();
        QCOMPARE(changes.size(),2);
        QCOMPARE(changes["image.contrast"],QJsonValue(2.0));
        QCOMPARE(changes["image.brightness"],QJsonValue(0.25));
        QVERIFY(!changes.contains("contrast"));
        QCOMPARE(target.effects,0);
        service.ResolveApproval(result.content["approvalId"].toString(),false);
        QCOMPARE(target.effects,0);
        QVERIFY(service.PendingApproval().isEmpty());
    }
    void leaseExpiryRetiresAsyncReservationBeforeLateDelivery() {
        qint64 now = 1; Target target;
        AppControlService service(target,CatalogEdition::DevCore,nullptr,[&] { return now; });
        service.SetEnabled(false,true);
        const auto context = service.Begin({ContextKind::ExternalLease,"lease-owner",1,{true,false,false,false}});
        QVERIFY(!context.isEmpty()); int replies = 0; ControlResult final;
        service.Submit({context,"held-image","view_observe",{}},[&](ControlResult result) { ++replies; final = std::move(result); });
        QCoreApplication::processEvents(); QVERIFY(target.delayed);
        now += 60000; target.delayed({{{"ok",true}},QByteArray("jpeg")});
        QTRY_COMPARE(replies,1); QCOMPARE(Error(final),"canceled"); QVERIFY(final.jpeg.isEmpty());
        QCoreApplication::processEvents(); QCOMPARE(replies,1); QCOMPARE(target.cleanupCalls,1);
    }
    void canceledCameraGenerationsCannotStartOrPresentAndNativeWins() {
        CameraSourceIntent intent; intent.LoadSaved({"native","format",0,true});
        const auto initial = intent.Request({"native","format",false},CameraSourceOrigin::Startup);
        QVERIFY(intent.Activate(initial,{"native","format",true}));
        QVERIFY(intent.RequestTemporary("owner",{"temporary","",false},"operation"));
        const auto canceled = intent.Requested().generation;
        QVERIFY(intent.ReleaseTemporary("owner"));
        QVERIFY(!intent.Activate(canceled,{"temporary","",false}));
        QVERIFY(!intent.MarkFirstFrame(canceled));
        QVERIFY(intent.RequestTemporary("owner",{"temporary","",false},"operation-2"));
        const auto superseded = intent.Requested().generation;
        intent.SelectManual("user-new",1);
        QVERIFY(!intent.Activate(superseded,{"temporary","",false}));
        QVERIFY(!intent.MarkFirstFrame(superseded));
        QVERIFY(!intent.ReleaseTemporary("owner"));
        QCOMPARE(intent.Saved().symbolicLink,"user-new");
        QCOMPARE(intent.Requested().identity.symbolicLink,"user-new");
        QVERIFY(intent.RequestTemporary("owner",{"temporary","",false},"format-operation"));
        const auto oldFormatGeneration = intent.Requested().generation;
        intent.SetSavedFormat("user-format");
        QVERIFY(!intent.Activate(oldFormatGeneration,{"temporary","",false}));
        QVERIFY(!intent.ReleaseTemporary("owner"));
        QCOMPARE(intent.Saved().symbolicLink,"user-new");
        QCOMPARE(intent.Saved().formatStableId,"user-format");
    }
    void cameraIntentSeparatesSavedRequestedAndActive() {
        CameraSourceIntent intent;
        intent.LoadSaved({"saved-webcam","saved-format",4,true});
        const auto start = intent.Request({"saved-webcam","saved-format",false},CameraSourceOrigin::Startup);
        QVERIFY(intent.Activate(start,{"saved-webcam","saved-format",false})); QVERIFY(intent.MarkFirstFrame(start));
        QVERIFY(intent.RequestTemporary("owner",{"okuflow-phone:fixture/rear","",false},"operation"));
        const auto temporary = intent.Requested().generation;
        QCOMPARE(intent.Saved().symbolicLink,"saved-webcam"); QCOMPARE(intent.Saved().formatStableId,"saved-format");
        QVERIFY(!intent.FillStartupIdentity("phone-delayed",0));
        QVERIFY(!intent.Activate(start,{"saved-webcam","saved-format",true}));
        QVERIFY(intent.Activate(temporary,{"okuflow-phone:fixture/rear","phone-default",false}));
        QVERIFY(intent.MarkFirstFrame(temporary)); QVERIFY(!intent.TemporaryPhoneIntent().isEmpty());
        intent.ReindexSaved({"other","saved-webcam","okuflow-phone:fixture/rear"});
        QCOMPARE(intent.Saved().legacyIndex,1);
        const auto restore = intent.RestoreChoice({"saved-webcam"}); QVERIFY(restore); QVERIFY(!restore->usesSavedStartup);
        QCOMPARE(restore->identity.symbolicLink,"saved-webcam");
        QVERIFY(!intent.ReleaseTemporary("foreign-owner")); QVERIFY(intent.ReleaseTemporary("owner"));
        QVERIFY(intent.TemporaryPhoneIntent().isEmpty());
        intent.SelectManual("new-webcam",2); QCOMPARE(intent.Saved().symbolicLink,"new-webcam");
        const auto native = intent.Request({"previous-active","active-format",false},CameraSourceOrigin::Manual);
        QVERIFY(intent.Activate(native,{"previous-active","active-format",true}));
        QVERIFY(intent.RequestTemporary("owner",{"phone","",false},"next"));
        const auto savedFallback = intent.RestoreChoice({"new-webcam"}); QVERIFY(savedFallback); QVERIFY(savedFallback->usesSavedStartup);
        const auto legacyBefore = intent.Saved().legacyIndex; intent.ReindexSaved({"missing"});
        QCOMPARE(intent.Saved().legacyIndex,legacyBefore);
    }
    void adjustmentOperationsBoundCapacityAndMetrics() {
        qint64 now = 1; OperationTracker tracker([&] { return now; });
        const auto operation = tracker.Create(OperationKind::Adjustment,"owner");
        QVERIFY(tracker.Advance(operation,OperationState::Starting));
        QVERIFY(tracker.Advance(operation,OperationState::FirstFrame,{{"metrics",QJsonObject{
            {"lumaMean",0.5},{"sceneClass","mixed"},{"focusBasis","viewport_laplacian"},{"secret","forbidden"}}}}));
        const auto metrics = tracker.SafeStatus(operation)["metrics"].toObject();
        QCOMPARE(metrics["lumaMean"].toDouble(),0.5); QVERIFY(!metrics.contains("secret"));
        for (int i = 1; i < 256; ++i) QVERIFY(!tracker.Create(OperationKind::Photo,"owner").isEmpty());
        QVERIFY(!tracker.HasCapacity()); QVERIFY(tracker.Create(OperationKind::Photo,"owner").isEmpty());
        now += 900000; QVERIFY(tracker.HasCapacity());
    }
    void grantWaitObservesOnlyFutureConsentChanges() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        QVERIFY(!Call(service,context,"immediate","app_wait_for",{{"condition","grant_changed"},{"timeoutMs",0}}).content["met"].toBool());
        std::optional<ControlResult> result;
        service.Submit({context,"grant-wait","app_wait_for",{{"condition","grant_changed"},{"timeoutMs",500}}},
                       [&](ControlResult value) { result = std::move(value); });
        QCoreApplication::processEvents(); QVERIFY(!result);
        service.SetGrants(context,{true,false,false,false});
        QTRY_VERIFY(result.has_value()); QVERIFY(result->content["met"].toBool());
        const auto state = Call(service,context,"state","app_state_get");
        QVERIFY(state.content["assistant"].toObject()["grants"].toObject()["frames"].toBool());
    }
    void cleanupWorkerOwnsResourcesAndRetainsFailure() {
        auto gate = std::make_shared<std::promise<void>>();
        auto ready = gate->get_future().share();
        auto resource = std::make_shared<int>(7); std::weak_ptr<int> weak = resource;
        auto cleanup = okuflow::AsyncCleanup::Start([resource,ready] { ready.wait(); return true; });
        resource.reset(); QVERIFY(!weak.expired());
        QCOMPARE(cleanup->Poll(),okuflow::AsyncCleanup::Status::Running);
        gate->set_value();
        QTRY_COMPARE(cleanup->Poll(),okuflow::AsyncCleanup::Status::Completed);
        QVERIFY(weak.expired());
        auto retainedResource = std::make_shared<int>(9); std::weak_ptr<int> retained = retainedResource;
        auto failed = okuflow::AsyncCleanup::Start([retainedResource]() -> bool { throw std::runtime_error("fixture"); });
        retainedResource.reset();
        QTRY_COMPARE(failed->Poll(),okuflow::AsyncCleanup::Status::Failed);
        failed.reset(); QVERIFY(!retained.expired());
    }
    void ownerRetirementStopsCompletedTurnRecordingOnly() {
        Target target; AppControlService service(target,CatalogEdition::DevCapture);
        const auto context = Begin(service,{false,false,true,false});
        target.recording = true; target.owner = QStringLiteral("trusted-session");
        service.End(context,true);
        QVERIFY(target.recording); QCOMPARE(target.ownedStops,0);
        service.RetireOwner(QStringLiteral("trusted-session"));
        QVERIFY(!target.recording); QCOMPARE(target.ownedStops,1);
        target.recording = true; target.owner = QStringLiteral("manual");
        service.RetireOwner(QStringLiteral("trusted-session"));
        QVERIFY(target.recording); QCOMPARE(target.ownedStops,1);
    }
    void monotonicLeaseExpiryAndNoWidening() {
        qint64 now = 1000; Target target; AppControlService service(target,CatalogEdition::DevCapture,nullptr,[&] { return now; });
        service.SetEnabled(true,true);
        const auto context = service.Begin({ContextKind::ExternalLease,"trusted-lease",1,{}}); QVERIFY(!context.isEmpty());
        service.SetGrants(context,{true,true,true,true});
        QVERIFY(!service.ContextStatus()["grants"].toObject()["frames"].toBool());
        QVERIFY(service.Begin({ContextKind::EmbeddedTurn,"trusted-session",1,{}}).isEmpty());
        now += 59999;
        QVERIFY(Call(service,context,"keep-alive","app_state_get").content["ok"].toBool());
        for (int keepAlive = 0; keepAlive < 4; ++keepAlive) {
            now += 59999;
            QVERIFY(Call(service,context,QStringLiteral("keep-alive-%1").arg(keepAlive),"app_state_get").content["ok"].toBool());
        }
        now = 300999;
        QVERIFY(Call(service,context,"last-allowed","app_state_get").content["ok"].toBool());
        now = 301000;
        QCOMPARE(Error(Call(service,context,"absolute-deadline","view_zoom_set",{{"magnification",2}})),"context_expired");
        QCOMPARE(target.effects,0); QCOMPARE(target.cleanupCalls,1); QCOMPARE(target.ownedStops,0);
    }
    void queuedCancelAndRevokedCachedImage() {
        Target target; AppControlService service(target); const auto context = Begin(service,{true,false,false,false});
        std::optional<ControlResult> canceled;
        service.Submit({context,"queued","view_zoom_set",{{"magnification",4}}},[&](ControlResult result) { canceled = result; });
        service.Cancel(context,"queued"); QCoreApplication::processEvents();
        QVERIFY(canceled); QCOMPARE(Error(*canceled),"canceled"); QCOMPARE(target.effects,0);
        std::optional<ControlResult> image;
        service.Submit({context,"image","view_observe",{}},[&](ControlResult result) { image = result; });
        QCoreApplication::processEvents(); QVERIFY(target.delayed);
        target.delayed({{{"ok",true}},QByteArray("jpeg")});
        QTRY_VERIFY(image.has_value()); QVERIFY(!image->jpeg.isEmpty());
        service.SetGrants(context,{});
        const auto cached = Call(service,context,"image","view_observe");
        QCOMPARE(Error(cached),"consent_denied"); QVERIFY(cached.jpeg.isEmpty());
    }
    void observationGeometryAndPayload() {
        ObservationService service;
        const auto transform = okuflow::ComputeViewTransform(1600,900,1000,1000,1,0.5,0.5,okuflow::ViewportFitMode::kFit);
        ObservationFrame frame; frame.stamp = {7,4,3,2}; frame.transform = transform; frame.sourceSize = QSize(1600,900);
        frame.viewportSize = QSize(1000,1000); frame.sequence = 9;
        QImage image(1000,1000,QImage::Format_ARGB32); image.fill(Qt::white);
        const auto encoded = ObservationService::Encode(image,frame,true,768);
        QVERIFY(encoded.content["ok"].toBool()); QVERIFY(encoded.jpeg.size() <= 192*1024);
        QVERIFY(encoded.observation);
        QVERIFY(encoded.observation->stamp == frame.stamp);
        QCOMPARE(encoded.observation->frameSequence, frame.sequence);
        QCOMPARE(encoded.observation->processedAt, frame.processedAt);
        QVERIFY(encoded.observation->contextId.isEmpty()); // the service adds authority later
        QVERIFY(!encoded.content.contains("jpeg")); QCOMPARE(encoded.content["versions"].toObject()["sourceEpoch"].toInt(),7);
        std::optional<ControlResult> completed;
        service.Request(true,768,frame.stamp,[&](ControlResult result) { completed = result; });
        auto stale = frame; --stale.stamp.processingVersion;
        QVERIFY(!service.WantsReadback(stale)); QVERIFY(service.WantsReadback(frame));
        service.Consume(image,frame); QTRY_VERIFY(completed.has_value());
        const auto id = completed->content["observationId"].toString();
        const auto width = completed->content["width"].toInt(); const auto height = completed->content["height"].toInt();
        const auto center = service.Resolve({{"space","observation"},{"observationId",id},{"px",width/2.0},{"py",height/2.0}},frame.stamp);
        QVERIFY(center); QVERIFY(std::abs(center->x-0.5f) < 0.001f); QVERIFY(std::abs(center->y-0.5f) < 0.001f);
        QVERIFY(!service.Resolve({{"space","observation"},{"observationId",id},{"px",10},{"py",0}},frame.stamp)); // letterbox
        auto geometry = frame.stamp; ++geometry.geometryEpoch;
        QVERIFY(!service.Resolve({{"space","observation"},{"observationId",id},{"px",width/2},{"py",height/2}},geometry));
        service.Cancel(); QVERIFY(service.Metrics(frame.stamp)["stale"].toBool());
    }
    void observationCacheRetainsNewestInsertionsRegardlessOfUuidOrder() {
        int next=100;
        ObservationService service(nullptr,{},[&]{return QString("obs-%1").arg(next--,32,16,QChar('0'));});
        ObservationFrame frame;frame.stamp={7,4,3,2};
        frame.transform=okuflow::ComputeViewTransform(64,64,64,64,1,0.5,0.5,okuflow::ViewportFitMode::kFit);
        frame.sourceSize=frame.viewportSize=QSize(64,64);
        QImage image(64,64,QImage::Format_ARGB32);image.fill(Qt::white);
        QStringList ids;
        for(int index=0;index<24;++index) {
            std::optional<ControlResult> result;
            service.Request(true,384,frame.stamp,[&](ControlResult value){result=std::move(value);});
            service.Consume(image,frame);QTRY_VERIFY(result.has_value());QVERIFY(result->content["ok"].toBool());
            const auto id=result->content["observationId"].toString();ids.push_back(id);
            QVERIFY(service.Resolve({{"space","observation"},{"observationId",id},{"px",192},{"py",192}},frame.stamp));
        }
        for(int index=0;index<ids.size();++index) {
            const auto resolved=service.Resolve({{"space","observation"},{"observationId",ids[index]},{"px",192},{"py",192}},frame.stamp);
            QCOMPARE(resolved.has_value(),index>=ids.size()-16);
        }
        service.Cancel();
        QVERIFY(!service.Resolve({{"space","observation"},{"observationId",ids.last()},{"px",192},{"py",192}},frame.stamp));
    }
    void observationFreshnessUsesElapsedTimeAndHeldTruth() {
        qint64 now = 5000;
        ObservationService service(nullptr,[&] { return now; });
        ObservationFrame frame; frame.stamp = {1,2,3,4}; frame.processedAt = now;
        frame.transform = okuflow::ComputeViewTransform(64,64,64,64,1,0.5,0.5,okuflow::ViewportFitMode::kFit);
        frame.held = false; frame.converging = false;
        QImage image(64,64,QImage::Format_ARGB32); image.fill(Qt::white);
        std::optional<ControlResult> result;
        service.Request(false,768,frame.stamp,[&](ControlResult value) { result = value; });
        service.Consume(image,frame); QTRY_VERIFY(result.has_value());
        QVERIFY(!service.Metrics(frame.stamp)["stale"].toBool());
        now += 1001;
        QCOMPARE(service.Metrics(frame.stamp)["frameAgeMs"].toInt(),1001);
        QVERIFY(service.Metrics(frame.stamp)["stale"].toBool());
        // Re-presenting the same cached GPU output cannot reset its age.
        result.reset(); service.Request(false,768,frame.stamp,[&](ControlResult value) { result = value; });
        service.Consume(image,frame); QTRY_VERIFY(result.has_value());
        QVERIFY(result->content["metrics"].toObject()["stale"].toBool());
        frame.processedAt = now; frame.held = true;
        result.reset(); service.Request(false,768,frame.stamp,[&](ControlResult value) { result = value; });
        service.Consume(image,frame); QTRY_VERIFY(result.has_value());
        QVERIFY(service.Metrics(frame.stamp)["stale"].toBool());
        frame.processedAt = now+1; frame.held = false;
        result.reset(); service.Request(false,768,frame.stamp,[&](ControlResult value) { result = value; });
        service.Consume(image,frame); QTRY_VERIFY(result.has_value());
        QVERIFY(service.Metrics(frame.stamp)["stale"].toBool());
    }
    void immutableObservationsFollowThePresentedFrameWithoutCameraFreshness() {
        qint64 now=50'000;
        ObservationService service(nullptr,[&]{return now;});
        ObservationFrame frame;frame.stamp={4,2,1,8};frame.sequence=9;
        frame.transform=okuflow::ComputeViewTransform(64,64,64,64,1,0.5,0.5,okuflow::ViewportFitMode::kFit);
        frame.sourceSize=frame.viewportSize=QSize(64,64);frame.held=true;frame.converging=false;
        frame.immutableSource=true;frame.documentId="media-"+QString(32,'a');
        frame.documentGeneration=frame.selectionGeneration=7;frame.mediaPts100ns=12'000'000;
        QImage image(64,64,QImage::Format_ARGB32);image.fill(Qt::white);
        std::optional<ControlResult> result;
        service.Request(true,384,frame.stamp,[&](ControlResult value){result=std::move(value);});
        service.Consume(image,frame);QTRY_VERIFY(result.has_value());
        QVERIFY(result->content["ok"].toBool());QVERIFY(result->observation.has_value());
        QVERIFY(result->observation->immutableSource);
        QCOMPARE(result->observation->mediaPts100ns,frame.mediaPts100ns);
        QCOMPARE(result->content["media"].toObject()["documentId"].toString(),frame.documentId);
        const auto id=result->content["observationId"].toString();
        const QJsonObject point{{"space","observation"},{"observationId",id},{"px",192},{"py",192}};
        now+=60'000;
        QVERIFY(service.Metrics(frame.stamp)["frameAgeMs"].isNull());
        QVERIFY(!service.Metrics(frame.stamp)["stale"].toBool());
        QVERIFY(service.Resolve(point,frame.stamp));
        // Playback advanced without changing document/seek authority. Old
        // coordinates must still be rejected against the actual new pixels.
        ++frame.sequence;frame.mediaPts100ns+=333'333;
        service.Consume(image,frame);
        QVERIFY(!service.Resolve(point,frame.stamp));
        QVERIFY(service.Metrics(frame.stamp)["stale"].toBool());
        service.Cancel();QVERIFY(!service.Resolve(point,frame.stamp));
    }
    void durableOperationsRejectLateAndUnsafeUpdates() {
        qint64 now = 1; OperationTracker tracker([&] { return now; });
        const auto photo = tracker.Create(OperationKind::Photo,"owner"); QVERIFY(!photo.isEmpty());
        QVERIFY(!tracker.Advance(photo,OperationState::Committed));
        QVERIFY(tracker.Advance(photo,OperationState::Readback));
        QVERIFY(tracker.Advance(photo,OperationState::Writing));
        QVERIFY(tracker.Advance(photo,OperationState::Committed,{{"originalName","C:/private/secret.jpg"},{"processedName","IMG_fixture_processed.jpg"},{"rawError","credential"}}));
        QVERIFY(!tracker.Advance(photo,OperationState::Failed));
        const auto status = tracker.SafeStatus(photo); QCOMPARE(status["state"].toString(),"committed");
        QVERIFY(!status.contains("originalName")); QVERIFY(!status.contains("rawError"));
        QCOMPARE(status["processedName"].toString(),"IMG_fixture_processed.jpg");
        tracker.Prune(now+899000); QVERIFY(tracker.Get(photo));
        tracker.Prune(now+900001); QVERIFY(!tracker.Get(photo));
        const auto recording = tracker.Create(OperationKind::Recording,"owner");
        QVERIFY(tracker.SetSession(recording,"exact-a")); QVERIFY(!tracker.SetSession(recording,"replacement-b"));
        QVERIFY(tracker.Advance(recording,OperationState::Stopping)); QVERIFY(tracker.Advance(recording,OperationState::Completed));
        QCOMPARE(tracker.SafeStatus(recording)["sessionId"].toString(),"exact-a");
    }
    void postTurnFrameApprovalNeverCaptures() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        const auto request = Call(service,context,"observe","view_observe");
        QCOMPARE(Error(request),"consent_required"); QVERIFY(!target.delayed);
        service.End(context,true);
        QVERIFY(service.ResolveApproval(request.content["approvalId"].toString(),true).content["ok"].toBool());
        QVERIFY(!target.delayed); QCOMPARE(target.captures,0);
        const auto next = Begin(service);
        int replies = 0;
        service.Submit({next,"observe","view_observe",{}},[&](ControlResult) { ++replies; });
        QCoreApplication::processEvents(); QVERIFY(bool(target.delayed));
        target.delayed({{{"ok",true}},QByteArray("jpeg")});
        QCoreApplication::processEvents(); QCoreApplication::processEvents(); QCOMPARE(replies,1);
    }
    void postTurnFrameGrantCanBeRevokedBeforeNextTurn() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        const auto request = Call(service,context,"observe","view_observe");
        service.End(context,true);
        QVERIFY(service.ResolveApproval(request.content["approvalId"].toString(),true).content["ok"].toBool());
        QVERIFY(service.ContextStatus()["nextTurnFrames"].toBool());
        service.RevokeFrameGrant("unrelated-owner");
        QVERIFY(service.ContextStatus()["nextTurnFrames"].toBool());
        service.RevokeFrameGrant("trusted-session");
        QVERIFY(!service.ContextStatus()["nextTurnFrames"].toBool());
        const auto next = Begin(service);
        QCOMPARE(Error(Call(service,next,"observe","view_observe")),"consent_required");
        QVERIFY(!target.delayed); QCOMPARE(target.captures,0);
    }
    void catalogAndSchemas() {
        for (const auto edition : {CatalogEdition::DevCore,CatalogEdition::DevCapture,CatalogEdition::DevCamera,CatalogEdition::V1}) {
            std::set<std::string_view> ids,names;
            for (const auto& tool : ControlRegistry(edition)) {
                QVERIFY(ids.insert(tool.canonicalId).second); QVERIFY(names.insert(tool.wireName).second);
                QVERIFY(QJsonDocument::fromJson(QByteArray::fromStdString(tool.inputSchema)).isObject());
                QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(tool.inputSchema)).object()["additionalProperties"].toBool(),false);
                QVERIFY(QJsonDocument::fromJson(QByteArray::fromStdString(tool.outputSchema)).object()["oneOf"].isArray());
                if (tool.id == ToolId::Observe) QVERIFY(!tool.readOnly);
            }
        }
        QVERIFY(!FindControlTool("capture_photo_take",CatalogEdition::DevCore));
        QVERIFY(FindControlTool("capture_photo_take",CatalogEdition::DevCapture));
        QVERIFY(!FindControlTool("camera_select",CatalogEdition::DevCapture));
        QVERIFY(FindControlTool("camera_select",CatalogEdition::DevCamera));
        QVERIFY(!FindControlTool("shell",CatalogEdition::V1));
    }
    void configurationApprovalCannotBypassSourceOrCaptureGrants() {
        Target target; target.configurationApproval = true; target.exactApproval = true;
        AppControlService service(target,CatalogEdition::DevFull); service.SetEnabled(true,true);
        const auto context = service.Begin({ContextKind::ExternalLease,"trusted-bridge",1,{}});
        const auto pending = Call(service,context,"config","assistant_preferences_set",{{"assistant_preferences.httpModel","reviewed-model"}});
        QCOMPARE(Error(pending),"consent_required"); QCOMPARE(target.effects,0);
        QVERIFY(service.ResolveApproval(pending.content["approvalId"].toString(),true).content["ok"].toBool());
        QCOMPARE(target.effects,1);
        QCOMPARE(Error(Call(service,context,"photo","capture_photo_take")),"denied_by_policy");
        QCOMPARE(Error(Call(service,context,"camera","camera_settings_set",{{"camera.orientation",90}})),"denied_by_policy");
        QCOMPARE(target.captures,0); QCOMPARE(target.effects,1); QVERIFY(service.PendingApproval().isEmpty());
        target.configurationApproval = false;
        QCOMPARE(Error(Call(service,context,"generic","assistant_preferences_set",{{"assistant_preferences.httpModel","other-model"}})),"denied_by_policy");
        QVERIFY(service.PendingApproval().isEmpty());
    }
    void groupedReadRefreshesOnlyReturnedFields() {
        Target target; AppControlService service(target,CatalogEdition::DevFull); const auto context = Begin(service);
        target.fields[Field::Contrast] = 1.5; target.fields[Field::Brightness] = 0.2;
        service.NativeFieldsChanged({Field::Contrast,Field::Brightness});
        const auto read = Call(service,context,"read","image_settings_get",{{"fields",QJsonArray{"image.contrast"}}});
        QVERIFY(read.content["ok"].toBool());
        QVERIFY(read.content["fieldRevisions"].toObject().contains("image.contrast"));
        QVERIFY(!read.content["fieldRevisions"].toObject().contains("image.brightness"));
        QVERIFY(Call(service,context,"set","image_settings_set",{{"image.contrast",2.0}}).content["ok"].toBool());
        QCOMPARE(Error(Call(service,context,"other","image_settings_set",{{"image.brightness",0.4}})),"stale_state");
        target.fields[Field::Contrast] = 1.7; service.NativeFieldsChanged({Field::Contrast});
        QVERIFY(Call(service,context,"metadata","app_controls_list").content["ok"].toBool());
        QCOMPARE(Error(Call(service,context,"unread","image_settings_set",{{"image.contrast",2.5}})),"stale_state");
    }
    void failedGroupRollsBackWithoutSavingOrRetainingInverse() {
        Target target;target.failBrightness=true;
        AppControlService service(target,CatalogEdition::DevFull);const auto context=Begin(service);
        const auto failed=Call(service,context,"fail","image_settings_set",{{"image.contrast",2.0},{"image.brightness",0.4}});
        QCOMPARE(Error(failed),"internal");QVERIFY(failed.content["rolledBack"].toBool());
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(1.0));QCOMPARE(target.fields.at(Field::Brightness),QJsonValue(0.0));
        QCOMPARE(target.saves,0);QVERIFY(service.StopAndUndo().content["restored"].toArray().isEmpty());
    }
    void coupledHandlerStillAppliesInitiallyEqualLaterField() {
        Target target;target.coupledContrast=true;
        AppControlService service(target,CatalogEdition::DevFull);const auto context=Begin(service);
        const auto result=Call(service,context,"coupled","image_settings_set",{{"image.contrast",2.0},{"image.brightness",0.0}});
        QVERIFY(result.content["ok"].toBool());QCOMPARE(target.fields.at(Field::Brightness),QJsonValue(0.0));
        QCOMPARE(result.content["effective"].toObject()["image.brightness"],QJsonValue(0.0));
        QVERIFY(!result.content["applied"].toObject().contains("contrast"));
        QCOMPARE(result.content["applied"].toObject()["image.contrast"],QJsonValue(2.0));
    }
    void configurationApprovalRechecksIdleLeaseAndNativeRevision() {
        qint64 now = 1000; Target target; target.configurationApproval = true; target.exactApproval = true;
        AppControlService service(target,CatalogEdition::DevFull,nullptr,[&] { return now; }); service.SetEnabled(true,true);
        auto context = service.Begin({ContextKind::ExternalLease,"trusted-bridge",1,{}});
        const auto expired = Call(service,context,"config","assistant_preferences_set",{{"assistant_preferences.httpModel","model-a"}});
        now += 60001;
        QCOMPARE(Error(service.ResolveApproval(expired.content["approvalId"].toString(),true)),"context_expired");
        QCOMPARE(target.effects,0);
        context = service.Begin({ContextKind::ExternalLease,"trusted-bridge",1,{}});
        const auto stale = Call(service,context,"config","assistant_preferences_set",{{"assistant_preferences.httpModel","model-b"}});
        target.fields[Field::Contrast] = "native-new-model"; service.NativeFieldsChanged({Field::Contrast});
        QCOMPARE(Error(service.ResolveApproval(stale.content["approvalId"].toString(),true)),"stale_state");
        QCOMPARE(target.effects,0);
    }
    void notificationListenersMayReplaceContextAfterMutationOrApproval() {
        for (const bool approval : {false,true}) {
            Target target; AppControlService service(target); const auto context = Begin(service);
            QString replacement; int delivered = 0;
            service.SetEventSink([&](const QJsonObject& event) {
                if (event["type"] == (approval ? "approval_requested" : "applied")) {
                    ++delivered; service.End(context); replacement = Begin(service);
                }
            });
            const auto result = approval ? Call(service,context,"call","camera_orientation_set",{{"degreesClockwise",90}})
                : Call(service,context,"call","view_zoom_set",{{"magnification",2.0}});
            QCOMPARE(delivered,1); QVERIFY(!replacement.isEmpty()); QCOMPARE(Error(result),"canceled");
            QCOMPARE(target.effects,approval ? 0 : 1);
            QCOMPARE(service.ContextStatus()["contextId"].toString(),replacement);
            QVERIFY(service.PendingApproval().isEmpty());
        }
    }
    void queuedNotificationDoesNotReachReplacedSinkOrContext() {
        Target target; AppControlService service(target); Begin(service); int oldSink = 0, newSink = 0;
        service.SetEventSink([&](const QJsonObject&) { ++oldSink; }); service.SourceChanged();
        service.SetEventSink([&](const QJsonObject&) { ++newSink; }); QCoreApplication::processEvents();
        QCOMPARE(oldSink,0); QCOMPARE(newSink,0);
        service.SourceChanged(); QCoreApplication::processEvents(); QCOMPARE(newSink,1);
        service.SourceChanged(); service.End(service.ContextStatus()["contextId"].toString()); Begin(service); QCoreApplication::processEvents();
        QCOMPARE(newSink,1); // Old source/cancellation notifications are fenced after replacement.
    }
    void recordingRevocationDoesNotRetireOwnedSource() {
        Target target; target.recording = true; target.owner = "trusted-session";
        AppControlService service(target); const auto context = Begin(service,{false,false,true,false});
        service.SetGrants(context,{}); QCOMPARE(target.ownedStops,1); QCOMPARE(target.sourceCleanupCalls,0);
        service.End(context); QCOMPARE(target.sourceCleanupCalls,1);
    }
    void defaultsAndMalformed() {
        Target target; AppControlService service(target);
        QVERIFY(service.Begin({ContextKind::EmbeddedTurn,"owner",1,{}}).isEmpty());
        const auto context = Begin(service);
        for (const auto args : {QJsonObject{{"magnification",2},{"path","secret"}},QJsonObject{{"magnification","2"}},QJsonObject{{"magnification",17}}})
            QCOMPARE(Error(Call(service,context,QString::number(target.effects)+QJsonDocument(args).toJson(),"view_zoom_set",args)),"invalid_argument");
        QCOMPARE(target.effects,0);
    }
    void noOpDedupeAndSingleSave() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        const auto result = Call(service,context,"a","view_zoom_set",{{"magnification",2.125}});
        QVERIFY(result.content["ok"].toBool()); QCOMPARE(target.fields[Field::ZoomAmount].toDouble(),2.13);
        QCOMPARE(target.effects,1); QCOMPARE(target.saves,1);
        QCOMPARE(Call(service,context,"a","view_zoom_set",{{"magnification",2.125}}).content,result.content);
        QCOMPARE(target.effects,1);
        QCOMPARE(Error(Call(service,context,"a","view_zoom_set",{{"magnification",3}})),"invalid_argument");
        QVERIFY(!Call(service,context,"b","view_zoom_set",{{"magnification",2.13}}).content["changed"].toBool());
        QCOMPARE(target.saves,1);
    }
    void userWinsAndConflictUndo() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        QVERIFY(Call(service,context,"a","image_contrast_set",{{"contrast",2},{"brightness",0.2}}).content["ok"].toBool());
        target.fields[Field::Contrast] = 3.0; service.NativeFieldsChanged({Field::Contrast});
        QCOMPARE(Error(Call(service,context,"b","image_contrast_set",{{"contrast",1},{"brightness",0.5}})),"stale_state");
        QCOMPARE(target.fields[Field::Brightness].toDouble(),0.2);
        const auto undo = service.StopAndUndo();
        QCOMPARE(target.fields[Field::Contrast].toDouble(),3.0);
        QCOMPARE(target.fields[Field::Brightness].toDouble(),0.0);
        QVERIFY(undo.content["skipped"].toArray().contains("contrast"));
    }
    void rereadAndModal() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        target.fields[Field::ZoomAmount] = 3.0; service.NativeFieldsChanged({Field::ZoomAmount});
        target.modal = true;
        QVERIFY(Call(service,context,"read","app_state_get").content["ok"].toBool());
        QCOMPARE(Error(Call(service,context,"blocked","view_zoom_set",{{"magnification",2}})),"modal_active");
        target.modal = false;
        QVERIFY(Call(service,context,"after","view_zoom_set",{{"magnification",2}}).content["ok"].toBool());
    }
    void exactApprovalAndReplacement() {
        Target target; AppControlService service(target,CatalogEdition::DevCapture); const auto context = Begin(service);
        const auto requested = Call(service,context,"a","capture_recording_stop");
        QCOMPARE(Error(requested),"consent_required"); QCOMPARE(target.captures,0);
        target.session = "session-b";
        QCOMPARE(Error(service.ResolveApproval(requested.content["approvalId"].toString(),true)),"stale_state");
        QCOMPARE(target.captures,0);
        const auto next = Call(service,context,"b","capture_photo_take");
        const auto id = next.content["approvalId"].toString(); QVERIFY(!id.isEmpty());
        QVERIFY(service.ResolveApproval(id,true).content["ok"].toBool()); QCOMPARE(target.captures,1);
        QCOMPARE(Error(service.ResolveApproval(id,true)),"not_found");
        QCOMPARE(Error(Call(service,context,"c","capture_photo_take")),"consent_required");
    }
    void denyDoesNotNag() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        const auto requested = Call(service,context,"a","camera_orientation_set",{{"degreesClockwise",90}});
        QCOMPARE(Error(service.ResolveApproval(requested.content["approvalId"].toString(),false)),"consent_denied");
        QCOMPARE(Error(Call(service,context,"b","camera_orientation_set",{{"degreesClockwise",90}})),"consent_denied");
        QCOMPARE(target.effects,0);
    }
    void canceledQueuedAndFrameEgress() {
        Target target; AppControlService service(target); const auto context = Begin(service,{true,false,false,false});
        int replies = 0; QString error;
        service.Submit({context,"frame","view_observe",{}},[&](ControlResult result) { ++replies; error = Error(result); });
        QCoreApplication::processEvents(); QVERIFY(bool(target.delayed));
        service.End(context); QCoreApplication::processEvents(); QCOMPARE(replies,1); QCOMPARE(error,"canceled");
        target.delayed({{{"ok",true}},QByteArray("jpeg")}); QCoreApplication::processEvents(); QCoreApplication::processEvents();
        QCOMPARE(replies,1);
        QVERIFY(!Call(service,context,"late","view_zoom_set",{{"magnification",4}}).content["ok"].toBool());
        QCOMPARE(target.effects,0);
    }
    void failureKeepsLiveState() {
        Target target; target.saveFails = true; AppControlService service(target); const auto context = Begin(service);
        QVERIFY(Call(service,context,"a","view_zoom_set",{{"magnification",2}}).content["ok"].toBool());
        QCOMPARE(target.fields[Field::ZoomAmount].toDouble(),2.0);
        const auto state = Call(service,context,"read","app_state_get",{{"sinceEvent",0}});
        const auto events = state.content["eventsAfter"].toArray();
        QVERIFY(std::any_of(events.begin(),events.end(),[](const auto& e) { return e.toObject()["type"] == "settings_not_saved"; }));
    }
    void stopAndUndoRequestsExactSourceRestoreBeforeProviderCleanup() {
        Target target; target.ownedSource = "trusted-session";
        AppControlService service(target); const auto context = Begin(service);
        service.OwnedSourceRequested("trusted-session",{{Field::Camera,"cam-"+QString(32,'a')},{Field::CameraFormat,"automatic"}});
        QVERIFY(Call(service,context,"zoom","view_zoom_set",{{"magnification",2.0}}).content["ok"].toBool());
        QJsonObject event;
        service.SetEventSink([&](const QJsonObject& value) { if (value["type"] == "undo") event = value; });
        // Stop and undo runs first, while the exact owner still holds the source.
        const auto undo = service.StopAndUndo().content;
        QVERIFY(undo["sourceRestoreRequested"].toBool()); QVERIFY(undo["changed"].toBool());
        QCOMPARE(undo["sourceRestoreFields"].toArray(),(QJsonArray{"camera.selection","camera.format"}));
        QVERIFY(!undo["skipped"].toArray().contains("camera.selection"));
        QVERIFY(!undo["skipped"].toArray().contains("camera.format"));
        QVERIFY(undo["restored"].toArray().contains("zoomAmount"));
        QCOMPARE(target.sourceRestores,1); QVERIFY(target.ownedSource.isEmpty());
        QTRY_VERIFY(!event.isEmpty());
        QCOMPARE(event["sourceRestoreFields"].toArray(),undo["sourceRestoreFields"].toArray());
        // The provider and lease cleanup that follows cannot re-end or revive the turn.
        const auto cleanup = target.sourceCleanupCalls;
        service.End(context); service.End(context,true);
        QCOMPARE(target.sourceCleanupCalls,cleanup); QVERIFY(!service.IsContextActive(context));
        QCOMPARE(Error(Call(service,context,"late","view_zoom_set",{{"magnification",3.0}})),"context_expired");
        QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(1.0));
        const auto again = service.StopAndUndo().content;
        QVERIFY(!again["changed"].toBool()); QVERIFY(again["sourceRestoreFields"].toArray().isEmpty());
        QVERIFY(again["skipped"].toArray().isEmpty()); QCOMPARE(target.sourceRestores,1);
    }
    void endedOwnerSourceRestoreIsNotLaterReportedAsSkipped() {
        Target target; target.ownedSource = "trusted-session";
        AppControlService service(target); const auto context = Begin(service);
        service.OwnedSourceRequested("trusted-session",{{Field::Camera,"cam-"+QString(32,'a')},{Field::CameraFormat,"automatic"}});
        service.End(context); // A provider cancel/failure callback ended the turn first.
        QCOMPARE(target.sourceRestores,1);
        const auto undo = service.StopAndUndo().content;
        QVERIFY(!undo["sourceRestoreRequested"].toBool());
        QVERIFY(!undo["skipped"].toArray().contains("camera.selection"));
        QVERIFY(!undo["skipped"].toArray().contains("camera.format"));
        // A source the user already replaced is still reported as skipped.
        Begin(service); target.ownedSource = "trusted-session";
        service.OwnedSourceRequested("trusted-session",{{Field::Camera,"cam-"+QString(32,'b')},{Field::CameraFormat,"automatic"}});
        target.ownedSource.clear(); service.NativeFieldsChanged({Field::Camera});
        const auto userWon = service.StopAndUndo().content;
        QVERIFY(!userWon["sourceRestoreRequested"].toBool());
        QVERIFY(userWon["skipped"].toArray().contains("camera.selection"));
        QCOMPARE(target.sourceRestores,1);
    }
    void emptyNativePollIsNotAUserChange() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        const auto state = [&](const QString& key) { return Call(service,context,key,"app_state_get",{{"sinceEvent",0}}).content; };
        const auto userChanges = [](const QJsonObject& value) {
            const auto events = value["eventsAfter"].toArray();
            return static_cast<int>(std::count_if(events.begin(),events.end(),[](const auto& e) { return e.toObject()["type"] == "user_changed"; }));
        };
        const auto before = state("before");
        for (int poll = 0; poll < 3; ++poll) service.NativeFieldsChanged({});
        const auto after = state("after");
        QCOMPARE(after["versions"],before["versions"]); QCOMPARE(userChanges(after),0);
        QVERIFY(Call(service,context,"write","image_contrast_set",{{"contrast",2},{"brightness",0.2}}).content["ok"].toBool());
        // A nonempty native change keeps its user-wins behavior.
        target.fields[Field::Contrast] = 3.0; service.NativeFieldsChanged({Field::Contrast});
        QCOMPARE(Error(Call(service,context,"stale","image_contrast_set",{{"contrast",1},{"brightness",0.5}})),"stale_state");
        const auto changed = state("changed"); // This read acknowledges the edit.
        QCOMPARE(userChanges(changed),1);
        QCOMPARE(changed["versions"].toObject()["state"].toInteger(),after["versions"].toObject()["state"].toInteger()+2);
    }
    void rejectedUndoBatchIsRetainedForRetryAndUserEditsStillWin() {
        Target target; AppControlService service(target); const auto context = Begin(service);
        QVERIFY(Call(service,context,"contrast","image_contrast_set",{{"contrast",2},{"brightness",0.2}}).content["ok"].toBool());
        QVERIFY(Call(service,context,"zoom","view_zoom_set",{{"magnification",2}}).content["ok"].toBool());
        const auto saves = target.saves;
        // GPU processing is briefly unavailable: validation refuses the inverse batch before any effect.
        target.rejectBegin = [](const ControlPlan& plan) {
            return std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write) {
                return write.field == Field::Contrast && write.value == 1.0; });
        };
        const auto rejected = service.StopAndUndo().content;
        QVERIFY(rejected["restored"].toArray().isEmpty()); QVERIFY(!rejected["changed"].toBool());
        for (const auto* field : {"contrast","brightness","zoomAmount"}) QVERIFY2(rejected["retained"].toArray().contains(field),field);
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(2.0)); QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(2.0));
        QCOMPARE(target.saves,saves);
        // The user edits one retained field before processing returns.
        target.fields[Field::ZoomAmount] = 1.5; service.NativeFieldsChanged({Field::ZoomAmount});
        target.rejectBegin = {};
        const auto retried = service.StopAndUndo().content;
        QVERIFY(retried["restored"].toArray().contains("contrast")); QVERIFY(retried["restored"].toArray().contains("brightness"));
        QVERIFY(retried["skipped"].toArray().contains("zoomAmount")); QVERIFY(retried["retained"].toArray().isEmpty());
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(1.0)); QCOMPARE(target.fields.at(Field::Brightness),QJsonValue(0.0));
        QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(1.5));
        const auto exhausted = service.StopAndUndo().content;
        QVERIFY(exhausted["restored"].toArray().isEmpty()); QVERIFY(exhausted["skipped"].toArray().isEmpty());
    }
    void failedCameraUndoIsRetainedUntilUserWins() {
        for (const bool userEdit : {false,true}) {
            Target target; target.fields[Field::CameraAcceleration] = "automatic";
            AppControlService service(target,CatalogEdition::DevFull); service.SetEnabled(true,true);
            const auto lease = service.Begin({ContextKind::ExternalLease,"trusted-lease",1,{false,false,false,true}});
            QVERIFY(Call(service,lease,"acceleration","camera_settings_set",{{"camera.acceleration","compatibility"}}).content["ok"].toBool());
            target.failCameraApply = true;
            const auto failed = service.StopAndUndo().content;
            QVERIFY(failed["retained"].toArray().contains("camera.acceleration")); QVERIFY(failed["restored"].toArray().isEmpty());
            QCOMPARE(target.fields.at(Field::CameraAcceleration),QJsonValue("compatibility"));
            target.failCameraApply = false;
            if (userEdit) { target.fields[Field::CameraAcceleration] = "accelerated"; service.NativeCameraAccelerationChanged("cam-fixture"); }
            const auto retried = service.StopAndUndo().content;
            QCOMPARE(retried["restored"].toArray().contains("camera.acceleration"),!userEdit);
            QCOMPARE(retried["skipped"].toArray().contains("camera.acceleration"),userEdit);
            QCOMPARE(target.fields.at(Field::CameraAcceleration),QJsonValue(userEdit ? "accelerated" : "automatic"));
            const auto exhausted = service.StopAndUndo().content;
            QVERIFY(exhausted["restored"].toArray().isEmpty()); QVERIFY(exhausted["skipped"].toArray().isEmpty());
        }
    }
    void beginAttributesUnpolledNativeEditsToTheStartingBaseline() {
        Target target; AppControlService service(target); auto observed = target.fields;
        target.synchronize = [&] {
            std::vector<Field> changed;
            for (const auto& [field,value] : target.fields) if (observed.at(field) != value) changed.push_back(field);
            service.NativeFieldsChanged(changed); observed = target.fields;
        };
        target.fields[Field::ZoomAmount] = 1.5; // Edited before the turn; not yet polled.
        const auto context = Begin(service);
        QVERIFY(Call(service,context,"zoom","view_zoom_set",{{"magnification",2.0}}).content["ok"].toBool());
        QCOMPARE(target.fields.at(Field::ZoomAmount),QJsonValue(2.0));
    }
    void delayedAutoAdjustRechecksNativeEditsBeforeApplying() {
        Target target; AppControlService service(target); auto observed = target.fields;
        target.synchronize = [&] {
            std::vector<Field> changed;
            for (const auto& [field,value] : target.fields) if (observed.at(field) != value) changed.push_back(field);
            service.NativeFieldsChanged(changed); observed = target.fields;
        };
        const auto context = Begin(service);
        std::optional<ControlResult> result;
        service.Submit({context,"auto","image_auto_adjust",{{"goal","read_text"},{"allowModeChange",false}}},
                       [&](ControlResult value) { result = std::move(value); });
        QCoreApplication::processEvents(); QVERIFY(bool(target.delayed)); // Metrics are pending.
        target.fields[Field::Contrast] = 3.0; // The user edits while metrics are pending.
        target.delayed({{{"ok",true}},{}});
        QTRY_VERIFY(result.has_value());
        QCOMPARE(Error(*result),"stale_state");
        QCOMPARE(target.fields.at(Field::Contrast),QJsonValue(3.0)); QCOMPARE(target.captures,0);
    }
    void receiptLabelsUseExistingCatalogCaptions() {
        const auto path = QFINDTESTDATA("../src/ui/translation_catalog.cpp");
        QFile file(path); QVERIFY2(file.open(QIODevice::ReadOnly),qPrintable(path));
        const auto catalog = QString::fromUtf8(file.readAll());
        const auto cataloged = [&](const QString& source) {
            return catalog.contains(QStringLiteral("QT_TRANSLATE_NOOP(\"OkuFlow\", \"%1\")").arg(source));
        };
        const auto label = [](const QString& key) {
            const auto bytes = key.toUtf8();
            const auto source = ReceiptLabelSource(std::string_view(bytes.constData(),static_cast<size_t>(bytes.size())));
            return QString::fromUtf8(source.data(),static_cast<qsizetype>(source.size()));
        };
        const auto fallback = QString::fromUtf8(kReceiptFallbackLabel.data(),static_cast<qsizetype>(kReceiptFallbackLabel.size()));
        QVERIFY(cataloged(fallback));
        // Layout-only preferences without their own caption keep the neutral label.
        const QSet<QString> neutral{"ui.mode","ui.activeTab","ui.sectionExpanded","video.selection"};
        QCOMPARE(ParameterRegistry().size(),size_t(100));
        for (const auto& descriptor : ParameterRegistry()) {
            const auto id = QString::fromUtf8(descriptor.id.data(),static_cast<qsizetype>(descriptor.id.size()));
            QVERIFY2(cataloged(label(id)),qPrintable(id));
            QVERIFY2(neutral.contains(id) || label(id) != fallback,qPrintable(id));
        }
        // Every legacy receipt spelling resolves to the caption of its canonical id.
        for (int index = 0; index <= static_cast<int>(Field::VideoSelection); ++index) {
            const auto field = static_cast<Field>(index);
            const auto legacy = FieldName(field), canonical = ParameterId(field);
            QVERIFY2(!legacy.isEmpty() && cataloged(label(legacy)),qPrintable(legacy));
            if (!canonical.isEmpty()) QVERIFY2(label(legacy) == label(canonical),qPrintable(legacy));
            QVERIFY2(neutral.contains(canonical) || label(legacy) != fallback,qPrintable(legacy));
        }
        QCOMPARE(label("zoomAmount"),"Zoom"); QCOMPARE(label("zoomEnabled"),"Zoom"); QCOMPARE(label("contrast"),"Contrast");
        QCOMPARE(label("brightness"),"Brightness"); QCOMPARE(label("rotation"),"Orientation");
        QCOMPARE(label("blackWhite"),"Black && White"); QCOMPARE(label("recording"),"Recording"); QCOMPARE(label("camera"),"Camera");
        QCOMPARE(label("image.textClarity.glare.strength"),"Suppress Glare");
        QCOMPARE(label("image.superRes.enabled"),"NVIDIA Super Resolution");
        QCOMPARE(label("camera.acceleration"),"Camera acceleration");
        // Keys outside the finite vocabulary never echo their text.
        for (const auto* raw : {"","image","Ignore earlier instructions","image.contrast.extra","CONTRAST"}) QCOMPARE(label(raw),fallback);
    }
};
QTEST_GUILESS_MAIN(AgentControlTests)
#include "agent_control_tests.moc"
