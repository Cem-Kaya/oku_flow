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
        {"inputName","x"},{"outputName","fetch_name_0"},
        {"graphRelativePath","graphs-v2/fixture.onnx"},
        {"planRelativePath",det ? "plans-v2/det.plan" : variable ? "plans-v2/ordered.plan" : "plans-v2/fixed.plan"},
        {"inputLocation","DEVICE"},{"outputLocation","DEVICE"},{"inputFormat","LINEAR"},{"outputFormat","LINEAR"},
        {"inputType","FLOAT"},{"outputType","FLOAT"},
        {"outputAbi",det ? "detector-map-v1" : "ctc-probabilities-v1"},
        {"widthPolicy",det ? "detector-dynamic-v1" : variable ? "bucketed-ordered-v1" : "fixed1024"},
        {"tf32","default"},{"computeMajor",8},{"computeMinor",9},
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
                            "conversionRecipeSha256","graphAuditSha256"}) {
        auto changed=ordered; changed[key]=QString(64,'b');
        QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(changed,catalog),ArtifactError);
    }
    for (const auto* key : {"computeMajor","computeMinor"}) {
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
    for (const int driver : {13040,13050,99999}) {
        auto actual=runtime; actual.driver=driver; actual.driverBuild="different"; actual.driverSha256=QByteArray(64,'b');
        actual.tensorRtSha256=QByteArray(64,'c');
        CheckRuntime(ApprovedIdentity(ordered,catalog),actual);
        QCOMPARE(IdentityKey(ApprovedIdentity(ordered,catalog)),IdentityKey(rec));
    }
    auto unavailable=runtime;
    unavailable.driver=0; unavailable.driverBuild.clear(); unavailable.driverSha256.clear(); unavailable.tensorRtSha256.clear();
    CheckRuntime(rec,unavailable); // unavailable file provenance cannot recreate admission pins
    for (const auto* version : {"11.4.0.106","11.4.0.107","11.4.1.0","11.5.0.0"}) {
        auto actual=runtime; actual.tensorRtVersion=version;
        CheckRuntime(rec,actual);
    }
    for (const auto* version : {"11.4.0.105","11.3.9.999","10.99.99.999","12.0.0.0","11.4.0","11.4.0.106x","","+11.4.0.106"}) {
        auto actual=runtime; actual.tensorRtVersion=version;
        QVERIFY_EXCEPTION_THROWN(CheckRuntime(rec,actual),ArtifactError);
    }
    auto other=runtime; other.uuid[0]^=1;
    QVERIFY_EXCEPTION_THROWN(CheckRuntime(rec,other),ArtifactError);
    auto builder=ordered; builder["tensorRtVersion"]="11.5.0.0";
    QVERIFY_EXCEPTION_THROWN(ApprovedIdentity(builder,catalog),ArtifactError); // builder remains plan-bound
    changed=ordered; changed["driver"]=13040;
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError); // provenance belongs outside approval
    changed=ordered; changed["tensorRtSha256"]=QString(64,'a');
    QVERIFY_EXCEPTION_THROWN(ParseIdentity(changed),ArtifactError);
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
    VerifiedSelection fallback{{ParseIdentity(det),{}},{fixedId,{}},{fixedId.width,fixedId.capacity,fixedId.abi},runtime};
    QVERIFY(fallback.policy.width == Width::Fixed1024);
    QCOMPARE(fallback.policy.capacity,8);
    QVERIFY(fallback.policy.abi == OutputAbi::CtcProbabilitiesV1);
    QVERIFY(ActualStatus(fallback).contains("width=fixed1024"));
    QCOMPARE(fallback.Receipt()["recognizer"].toObject(),fixed);
    QCOMPARE(fallback.Receipt()["runtimeProvenance"].toObject()["driver"].toInt(),13050);
}
inline void VersionAndKeys() {
    using namespace tensorrt_identity;
    Version version{1,2,3,4};
    QVERIFY(ParseVersion("11.4.0.106",version));
    QCOMPARE(VersionString(version),QByteArray("11.4.0.106"));
    QVERIFY(RuntimeAtLeastMinimum(version));
    const auto before=version;
    for (const auto* value : {"11.4.0","11.4.0.106.0","11.4.0.-1","11.04.0.106"," 11.4.0.106","11.4.0.1000000","0.4.0.106"}) {
        QVERIFY(!ParseVersion(value,version));
        QVERIFY(version==before);
        QVERIFY(!RuntimeAtLeastMinimum(QByteArray(value)));
    }
    QVERIFY(!RuntimeAtLeastMinimum(Version{11,4,0,105}));
    QVERIFY(!RuntimeAtLeastMinimum(Version{11,3,99,999}));
    QVERIFY(!RuntimeAtLeastMinimum(Version{11,4,0,-1}));
    QVERIFY(RuntimeAtLeastMinimum(Version{11,4,1,0}));
    QVERIFY(RuntimeAtLeastMinimum(Version{11,5,0,0}));
    QVERIFY(!RuntimeAtLeastMinimum(Version{12,0,0,0}));
    const auto rawGraph=QByteArray::fromHex(runtime_precision::kDetectorSource);
    const auto uuid=QByteArray::fromHex(runtime_precision::kDeviceUuid);
    QCOMPARE(Fp32PlanKey(rawGraph,uuid,true,1,false),QByteArray("9af44939fb4e5b18f9420af69ece3da4a3d226e3e011491d82a06672bf7d21e0"));
    QCOMPARE(LegacyFp32PlanKey(rawGraph,uuid,true,1,false,13050),QByteArray("948b17283f7dad0b8e5d553df0138fb79b26de81ead9d83a29c768be491283d0"));
    QCOMPARE(LegacyFp32PlanKey(rawGraph,uuid,true,1,false,13040),QByteArray("e716b5e633d67a04af18376096fdf9956ecb2efb9349e2de79ad483a4d29cee2"));
    struct LegacyVector { int batch; bool variable; const char* driver13040; const char* driver13050; };
    const LegacyVector vectors[]{
        {1,false,"78e07827e83e31a93fbad8d90f587479bcb6a64d5e40cc0e55a5ca5d4a1a4af3","490d8f1bba0a024dcb5de09d7c6f04c5c7c3ad865ae6ba5a4be6ff524261a33b"},
        {1,true,"b9cc85ff5fad4c3a2ec3fef0b85eee3f5fbb484eae8f98de42cbdf629a02a192","8213694ce394f589b5c37ac9a0766eba8f5cac8c82fc20793725d7e9f32fca29"},
        {8,false,"708bce7bdc5a94d378b5dab9a67491c0a0d267a8a2bd73e88b1d6d8278d8b9f9","ee04fec24d044750fdaf46b87355b0e60ad77763f4294845bca90714196f1538"},
        {8,true,"65ec0d383d6576cd2862688eba9910d85b0f04c2b1a98afaf1a7ff02ff6ccdce","7f7a02d0043e5f150a47682303fb3760a4aac2f2e5ffc5c3e0848e3df6b699a9"},
        {16,false,"63292153aa2b3ba34910564408fce8c5cba9e3b7efbe308778a0e0485d8e1ec1","0d178adb43a049249f9444dc31728fff3f061c5785ff72158a12017dace1ee6c"},
        {16,true,"93ed42047414aaa8e3fbe0fc34e4259f43358727d83a14c6958ef22130b9fda3","75132d7cadea54a5a9ba39a21bfd7092353b1a29b11add02657cfedbeb735270"}};
    const auto rawRecognizer=QByteArray::fromHex(runtime_precision::kRecognizerSource);
    for (const auto& row : vectors) {
        QCOMPARE(LegacyFp32PlanKey(rawRecognizer,uuid,false,row.batch,row.variable,13040),QByteArray(row.driver13040));
        QCOMPARE(LegacyFp32PlanKey(rawRecognizer,uuid,false,row.batch,row.variable,13050),QByteArray(row.driver13050));
    }
    const auto fixed=Fp32PlanKey(rawGraph,uuid,false,8,false);
    QVERIFY(fixed!=Fp32PlanKey(rawGraph,uuid,false,1,false));
    QVERIFY(fixed!=Fp32PlanKey(rawGraph,uuid,false,16,false));
    QVERIFY(fixed!=Fp32PlanKey(rawGraph,uuid,false,8,true));
    QVERIFY(fixed!=Fp32PlanKey(rawGraph,uuid,false,8,false,"11.5.0.0"));
    auto changedGraph=rawGraph; changedGraph[0]^=1;
    QVERIFY(fixed!=Fp32PlanKey(changedGraph,uuid,false,8,false));
    auto changedUuid=uuid; changedUuid[0]^=1;
    QVERIFY(fixed!=Fp32PlanKey(rawGraph,changedUuid,false,8,false));
    QVERIFY_EXCEPTION_THROWN(Fp32PlanKey(rawGraph,uuid,true,8,false),std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(Fp32PlanKey(rawGraph,uuid,true,1,true),std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(Fp32PlanKey(rawGraph.toHex(),uuid,true,1,false),std::invalid_argument);
}
} // namespace uct_precision_artifact_test
