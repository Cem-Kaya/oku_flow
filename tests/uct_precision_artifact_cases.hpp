#pragma once
#include "../src/common/uct_precision_artifacts.hpp"
#include <QtTest>
#include <QTemporaryDir>

namespace uct_precision_artifact_test {
using namespace okuflow::uct;
using namespace precision_artifacts;
inline QJsonObject FixtureIdentity(Role role, bool variable = false) {
    const auto hash = QString(64,'a');
    const auto* pin = FindPaddleModelPin(role == Role::Detector,ModelTier::Medium);
    const auto source = QString::fromUtf8(pin->modelSha256.data(),qsizetype(pin->modelSha256.size()));
    const auto config = QString::fromUtf8(pin->configSha256.data(),qsizetype(pin->configSha256.size()));
    const bool det = role == Role::Detector;
    return {{"id",det ? "fixture-detector" : variable ? "fixture-rec-ordered" : "fixture-rec-fixed"},
        {"role",RoleName(role)},{"tier","medium"},{"precision",det ? "fp16-linear" : "bf16-linear"},
        {"sourceSha256",source},{"graphSha256",hash},{"configSha256",config},{"dictionarySha256",hash},
        {"conversionRecipeSha256",hash},{"graphAuditSha256",hash},{"planFileSha256",hash},
        {"deviceUuid","230556cb9d98d5c17318d9ac52e6697d"},{"tensorRtVersion","11.4.0.106"},
        {"tensorRtSha256",hash},{"inputName","x"},{"outputName","fetch_name_0"},
        {"driverBuild","fixture-only"},{"driverSha256",hash},
        {"graphRelativePath","graphs-v2/fixture.onnx"},
        {"planRelativePath",det ? "plans-v2/det.plan" : variable ? "plans-v2/ordered.plan" : "plans-v2/fixed.plan"},
        {"inputLocation","DEVICE"},{"outputLocation","DEVICE"},{"inputFormat","LINEAR"},{"outputFormat","LINEAR"},
        {"inputType","FLOAT"},{"outputType","FLOAT"},
        {"outputAbi",det ? "detector-map-v1" : "ctc-probabilities-v1"},
        {"widthPolicy",det ? "detector-dynamic-v1" : variable ? "bucketed-ordered-v1" : "fixed1024"},
        {"tf32","default"},{"driver",13050},{"computeMajor",8},{"computeMinor",9},
        {"capacity",det ? 1 : 8},{"classes",det ? 1 : 18710},
        {"optimizationLevel",2},{"workspaceMiB",256},{"auxStreams",0},
        {"minimum",QJsonArray{1,3,det ? 32 : 48,det ? 32 : variable ? 128 : 1024}},
        {"optimum",QJsonArray{det ? 1 : 8,3,det ? 544 : 48,det ? 960 : variable ? 512 : 1024}},
        {"maximum",QJsonArray{det ? 1 : 8,3,det ? 960 : 48,det ? 960 : 1024}}};
}
inline void Contract() {
    const auto fixed = FixtureIdentity(Role::Recognizer), ordered = FixtureIdentity(Role::Recognizer,true);
    const auto det = FixtureIdentity(Role::Detector);
    const std::array<QJsonObject,3> catalog{fixed,ordered,det};
    const auto rec = ApprovedIdentity(ordered,catalog), fixedId = ParseIdentity(fixed);
    QVERIFY((rec.profile.minimum == Shape{1,3,48,128}));
    QVERIFY((rec.profile.optimum == Shape{8,3,48,512}));
    QCOMPARE(rec.capacity,8);
    QVERIFY(rec.width == Width::OrderedBuckets);
    QVERIFY(ParseIdentity(det).abi == OutputAbi::DetectorMapV1);
    QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(ordered,{}),ArtifactError);
    const std::array<QJsonObject,2> duplicated{ordered,ordered};
    QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(ordered,duplicated),ArtifactError);
    for (const auto* key : {"planFileSha256","graphSha256","dictionarySha256","configSha256",
                            "tensorRtSha256","driverSha256","conversionRecipeSha256","graphAuditSha256"}) {
        auto changed=ordered; changed[key]=QString(64,'b');
        QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(changed,catalog),ArtifactError);
    }
    for (const auto* key : {"driver","computeMajor","computeMinor"}) {
        auto changed=ordered; changed[key]=ordered[key].toInt()+1;
        QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(changed,catalog),ArtifactError);
    }
    for (const auto* key : {"role","precision","inputType","outputType","outputAbi","widthPolicy"}) {
        auto changed=ordered; changed[key]="invalid";
        QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError);
    }
    auto changed=ordered; changed["capacity"]=8.5;
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError);
    changed=ordered; changed["minimum"]=QJsonArray{1,3,48,127};
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError);
    changed=ordered; changed["extra"]="ignored?";
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError);
    QVERIFY(IdentityKey(fixedId) != IdentityKey(rec));
    auto fp32=fixed; fp32["precision"]="fp32"; fp32["graphSha256"]=fp32["sourceSha256"];
    QVERIFY(ParseIdentity(fp32).width == Width::Fixed1024);
    fp32["widthPolicy"]="bucketed-ordered-v1";
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(fp32),ArtifactError);
    QVERIFY(DictionaryHash({"","a:b"," "}) != DictionaryHash({"","a","b"," "}));
    const Runtime runtime{QByteArray::fromHex("230556cb9d98d5c17318d9ac52e6697d"),13050,8,9,"11.4.0.106",QByteArray(64,'a'),"fixture-only",QByteArray(64,'a')};
    CheckRuntime(rec,runtime);
    auto old=runtime; old.driver=13040;
    QVERIFY_EXCEPTION_THROWN(CheckRuntime(rec,old),ArtifactError);
    auto other=runtime; other.tensorRtSha256[0]='b';
    QVERIFY_EXCEPTION_THROWN(CheckRuntime(rec,other),ArtifactError);
    const auto checkProfile = [&](const Identity& id,bool detection,const Profile& p) {
        CheckEngineProfile(id,detection,1,"x","fetch_name_0",true,true,true,true,p.minimum.data(),4,p.optimum.data(),4,p.maximum.data(),4);
    };
    checkProfile(rec,false,rec.profile);
    QVERIFY_EXCEPTION_THROWN(checkProfile(rec,false,fixedId.profile),ArtifactError);
    QVERIFY_EXCEPTION_THROWN(checkProfile(rec,true,rec.profile),ArtifactError);
    const std::array<std::int64_t,3> shape{8,16,18710}, bad{8,16,18709};
    CheckOutput(rec,shape,8,48,128);
    bool typedArtifact=false, runtimeFailure=false;
    try { CheckOutput(rec,bad,8,48,128); }
    catch (const ArtifactError&) { typedArtifact=true; }
    catch (const std::runtime_error&) { runtimeFailure=true; }
    QVERIFY(runtimeFailure); QVERIFY(!typedArtifact);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto entry = [&](const QJsonObject& id) {
        return QJsonObject{{"identity",id},{"graphPath",root.filePath(id["graphRelativePath"].toString())},
            {"planPath",root.filePath(id["planRelativePath"].toString())}};
    };
    QJsonObject manifest{{"schema",2},{"prepare",false},{"artifactRoot",root.path()},
                          {"detector",entry(det)},{"recognizer",entry(ordered)}};
    const auto encode=[](const QJsonObject& value){return QJsonDocument(value).toJson();};
    const auto selected=ParseManifest(encode(manifest),catalog);
    QVERIFY(selected.recognizer.identity.width == Width::OrderedBuckets);
    auto invalid=manifest; invalid["prepare"]=true;
    QVERIFY_EXCEPTION_THROWN(ParseManifest(encode(invalid),catalog),ArtifactError);
    invalid=manifest; invalid["detector"]=entry(ordered);
    QVERIFY_EXCEPTION_THROWN(ParseManifest(encode(invalid),catalog),ArtifactError);
    invalid=manifest; auto e=entry(det); e["planPath"]=root.filePath("../escape.plan"); invalid["detector"]=e;
    QVERIFY_EXCEPTION_THROWN(ParseManifest(encode(invalid),catalog),ArtifactError);
    const auto before=QDir(root.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot);
    QVERIFY_EXCEPTION_THROWN(ReadPlan(root.path(),selected.recognizer,runtime),ArtifactError);
    QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot),before);
    PaddleAssets assets; assets.dictionary={""," "};
    QVERIFY_EXCEPTION_THROWN(ReadSelection(selected,runtime,assets),ArtifactError);
    assets.detectorSha256=selected.detector.identity.metadata["sourceSha256"].toString().toLatin1();
    assets.recognizerSha256=selected.recognizer.identity.metadata["sourceSha256"].toString().toLatin1();
    assets.dictionary.fill("fixture",18710); // count alone cannot substitute token meanings
    QVERIFY_EXCEPTION_THROWN(ReadSelection(selected,runtime,assets),ArtifactError);
    const QByteArray graphBytes("synthetic converted graph bytes; no inference"), payload("synthetic plan payload");
    const auto envelope=QCryptographicHash::hash(payload,QCryptographicHash::Sha256)+payload;
    auto synthetic=ordered;
    synthetic["graphRelativePath"]="graph.onnx"; synthetic["planRelativePath"]="plan.bin";
    synthetic["graphSha256"]=QString::fromLatin1(Digest(graphBytes));
    synthetic["planFileSha256"]=QString::fromLatin1(Digest(envelope));
    const auto syntheticId=ParseIdentity(synthetic);
    const Entry syntheticEntry{syntheticId,root.filePath("graph.onnx"),root.filePath("plan.bin")};
    { QFile file(syntheticEntry.graphPath); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(graphBytes),qint64(graphBytes.size())); }
    { QFile file(syntheticEntry.planPath); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(envelope),qint64(envelope.size())); }
    QCOMPARE(ReadPlan(root.path(),syntheticEntry,runtime).payload,payload);
    auto badEnvelope=envelope; badEnvelope[0]^=1;
    synthetic["planFileSha256"]=QString::fromLatin1(Digest(badEnvelope));
    { QFile file(syntheticEntry.planPath); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(badEnvelope); }
    const Entry badEntry{ParseIdentity(synthetic),syntheticEntry.graphPath,syntheticEntry.planPath};
    QVERIFY_EXCEPTION_THROWN(ReadPlan(root.path(),badEntry,runtime),ArtifactError);
    VerifiedSelection fallback{{ParseIdentity(det),{}},{fixedId,{}},{fixedId.width,fixedId.capacity,fixedId.abi}};
    QVERIFY(fallback.policy.width == Width::Fixed1024);
    QCOMPARE(fallback.policy.capacity,8);
    QVERIFY(fallback.policy.abi == OutputAbi::CtcProbabilitiesV1);
    QVERIFY(ActualStatus(fallback).contains("width=fixed1024"));
    QCOMPARE(fallback.Receipt()["recognizer"].toObject(),fixed);
}
} // namespace uct_precision_artifact_test
