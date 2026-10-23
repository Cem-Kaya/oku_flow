#pragma once
#include "../src/common/uct_public_bf16.hpp"
#include "../src/common/uct_public_bf16_runtime.hpp"
#include "../src/common/uct_frozen_upload.hpp"
#include <QtTest>

namespace uct_public_bf16_test {
using namespace okuflow::uct;
inline void Contract() {
    using namespace public_bf16;
    const auto model=ModelIdentity();
    QCOMPARE(model["graphSha256"].toString(),QString(runtime_precision::kRecognizerGraph));
    QCOMPARE(model["sourceSha256"].toString(),QString(runtime_precision::kRecognizerSource));
    QCOMPARE(model["dictionarySha256"].toString(),QString(kDictionarySha256));
    QFile metadata(QFINDTESTDATA("../scripts/uct/public-bf16-model.json"));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    QJsonParseError metadataError;
    const auto manifest=QJsonDocument::fromJson(metadata.readAll(),&metadataError).object();
    QVERIFY(metadataError.error==QJsonParseError::NoError);
    QCOMPARE(manifest["convertedGraphSha256"],model["graphSha256"]);
    QCOMPARE(manifest["decodedDictionarySha256"],model["dictionarySha256"]);
    QCOMPARE(manifest["upstream"].toObject()["sourceSha256"],model["sourceSha256"]);
    QCOMPARE(manifest["upstream"].toObject()["configSha256"],model["configSha256"]);
    QCOMPARE(manifest["plannedBundlePath"].toString(),"app/models/"+QString(kGraphRelativePath));
    QCOMPARE(manifest["preprocessing"],model["preprocessing"]);
    QCOMPARE(manifest["goldenCropEncoding"].toString(),QString(kCropEncoding));
    QVERIFY(manifest["productionGoldenCorpusPins"].toArray().isEmpty());
    for(const auto* key:{"deviceUuid","planFileSha256","driver","driverBuild","driverSha256","tensorRtSha256"})
        QVERIFY(!model.contains(key));
    QVERIFY(ProductionCorpusCatalog().empty());
    const precision_artifacts::Runtime runtime{QByteArray(16,'x'),13050,8,9,"11.4.0.106",{}, {}, {}};
    const auto reason=[&](ModelTier det,ModelTier rec,int n,bool variable,bool prepare,const precision_artifacts::Runtime& actual) {
        try{CheckEligibility(det,rec,n,variable,prepare,actual,ProductionCorpusCatalog());}
        catch(const Refusal& refusal){return refusal.Code();}
        throw std::runtime_error("empty production corpus must not allow public readiness");
    };
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,false,false,runtime)==Reason::CorpusUnavailable);
    for(const auto tier:{ModelTier::Tiny,ModelTier::Small}) {
        QVERIFY(reason(tier,ModelTier::Medium,8,false,false,runtime)==Reason::Configuration);
        QVERIFY(reason(ModelTier::Medium,tier,8,false,false,runtime)==Reason::Configuration);
    }
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,1,false,false,runtime)==Reason::Configuration);
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,true,false,runtime)==Reason::Configuration);
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,false,true,runtime)==Reason::Configuration);
    auto older=runtime;older.major=7;
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,false,false,older)==Reason::Hardware);
    older=runtime;older.tensorRtVersion="11.4.0.105";
    QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,false,false,older)==Reason::Runtime);
    for(const auto* version:{"11.4.0.107","11.5.0.0"}) {
        auto actual=runtime;actual.uuid=QByteArray(16,'z');actual.driver=99999;actual.tensorRtVersion=version;
        QVERIFY(reason(ModelTier::Medium,ModelTier::Medium,8,false,false,actual)==Reason::CorpusUnavailable);
    }
    bool readCalled=false;
    const auto reader=[&](const QString&)->QByteArray{readCalled=true;throw std::runtime_error("unapproved corpus must not read pixels");};
    QVERIFY_EXCEPTION_THROWN(ReadCorpus("{}",ProductionCorpusCatalog(),reader),Refusal);
    QVERIFY_EXCEPTION_THROWN(ReadCorpus("[]",ProductionCorpusCatalog(),reader),Refusal);
    QVERIFY_EXCEPTION_THROWN(ReadCorpus(QByteArray(64*1024+1,' '),ProductionCorpusCatalog(),reader),Refusal);
    QVERIFY(!readCalled);
    EngineContract engine{1,1,1,"x","fetch_name_0",true,true,{{1,3,48,1024},{8,3,48,1024},{8,3,48,1024}}};
    CheckEngine(engine);
    engine.profile.minimum[3]=128;
    QVERIFY_EXCEPTION_THROWN(CheckEngine(engine),Refusal);
    Corpus unavailable;
    const LocalBuild local{QByteArray(16,'x'),"11.4.0.106",QByteArray(64,'a'),8,9};
    bool typed=false,uncertain=false;
    try{CheckGolden(unavailable,local,{},Completion::Unknown);}
    catch(const Refusal&){typed=true;}
    catch(const std::runtime_error&){uncertain=true;}
    QVERIFY(uncertain);QVERIFY(!typed);
    QVERIFY_EXCEPTION_THROWN(CheckGolden(unavailable,local,{},Completion::Confirmed),Refusal);
    QVERIFY_EXCEPTION_THROWN(BuildIdentity(local,unavailable),Refusal);
    PaddleAssets assets;
    QVERIFY_EXCEPTION_THROWN(ReadModel("relative",assets),Refusal);
    // No synthetic readable crop, truth, approved corpus or successful golden
    // result is manufactured here. Positive attestation needs real pinned assets.
}
inline void PublicationAndLifetime() {
    using namespace public_bf16;
    QVERIFY(ProductionCorpusCatalog().empty());
    QCOMPARE(CandidateCatalog().size(),std::size_t(1));
    Request request;
    request.corpusRoot=QFileInfo(QFINDTESTDATA("../assets/uct/golden/okuflow-owned-typeset-gray8-v1/manifest.json")).absolutePath();
    request.use=CatalogUse::CandidateValidation;
    const auto corpus=LoadCorpus(request);
    QCOMPARE(corpus.Crops().size(),std::size_t(8));
    QCOMPARE(corpus.Sha256(),CandidateCatalog()[0].manifestSha256);
    request.use=CatalogUse::Production;
    QVERIFY_EXCEPTION_THROWN(LoadCorpus(request),Refusal);
    request.use=CatalogUse::CandidateValidation;
    const auto manifest=ReadBounded(request.corpusRoot,"manifest.json",64*1024);
    bool read=false;
    QVERIFY_EXCEPTION_THROWN(ReadCorpus(manifest+" ",CandidateCatalog(),[&](const QString&){read=true;return QByteArray();}),Refusal);
    QVERIFY(!read);
    QVERIFY_EXCEPTION_THROWN(ReadCorpus(manifest,CandidateCatalog(),[](const QString&){return QByteArray(48,'x');}),Refusal);
    const QStringList dictionary{"","a"," ",QString::fromUtf8("é")};
    const std::vector<PaddleToken> raw{{2,0,0,1},{1,1,1,1},{2,2,2,1},{3,3,3,1},{2,4,4,1}};
    QCOMPARE(RawText(raw,5,dictionary),QString::fromUtf8(" a é ")); // preserve leading/trailing spaces
    auto bad=raw;bad[1].last=5;
    QVERIFY_EXCEPTION_THROWN(RawText(bad,5,dictionary),Refusal);

    const LocalBuild build{QByteArray(16,'x'),"11.4.0.106",QByteArray(64,'a'),8,9};
    const precision_artifacts::Runtime runtime{build.uuid,13050,8,9,build.builderVersion,{}, {}, {}};
    const auto failed=CheckGolden(corpus,build,{},Completion::Confirmed);
    QVERIFY(!failed.Passed());
    QVERIFY(failed.Record()["rows"].toArray().isEmpty());
    std::vector<Observation> mismatches;
    for(const int batch:{1,8})for(const auto& crop:corpus.Crops())
        mismatches.push_back({crop.id,batch,128,18710,crop.pixelsSha256,"deliberate model-free mismatch"});
    const auto mismatch=CheckGolden(corpus,build,mismatches,Completion::Confirmed);
    QVERIFY(!mismatch.Passed());QCOMPARE(mismatch.Record()["rows"].toArray().size(),16);
    QCOMPARE(mismatch.Record()["rows"].toArray()[0].toObject()["text"].toString(),QString("deliberate model-free mismatch"));
    Publication completion;
    QVERIFY_EXCEPTION_THROWN(ReceiptBytes(build,corpus,failed,runtime,completion),std::runtime_error);
    completion.Confirmed();
    QVERIFY_EXCEPTION_THROWN(ReceiptBytes(build,corpus,failed,runtime,completion),Refusal);
    completion.Queued();
    QVERIFY_EXCEPTION_THROWN(completion.RequireConfirmed(),std::runtime_error);
    completion.Confirmed();
    auto changed=build;changed.uuid[0]='y';
    QVERIFY(CacheKey(changed,corpus)!=CacheKey(build,corpus));
    changed=build;changed.builderVersion="11.5.0.0";
    QVERIFY(CacheKey(changed,corpus)!=CacheKey(build,corpus));
    Attempts attempts;
    QVERIFY(attempts.Begin("failed-plan-A"));
    QVERIFY(!attempts.Begin("failed-plan-A"));
    QVERIFY(attempts.Begin("changed-plan-B"));
    attempts.Passed("changed-plan-B");
    QVERIFY(attempts.Begin("changed-plan-B"));

    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    request.cacheRoot=storage.path();
    const auto parent=CacheParent(request,build,corpus);
    const QByteArray key(64,'a');
    // Plain storage fixture bytes: no model, real plan or passing golden record.
    QVERIFY(PublishFilesNew(storage.path(),parent,key,"storage fixture plan","storage fixture receipt"));
    QVERIFY(!PublishFilesNew(storage.path(),parent,key,"replacement must fail","replacement must fail"));
    const auto generation=QDir(parent).filePath(QString::fromLatin1(key));
    QCOMPARE(ReadBounded(generation,"recognizer.plan",1024),QByteArray("storage fixture plan"));
    QCOMPARE(ReadBounded(generation,"ready.json",1024),QByteArray("storage fixture receipt"));
    QCOMPARE(QDir(parent).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),1);
    QVERIFY_EXCEPTION_THROWN(FindPlan(request,build,corpus),Refusal); // storage receipt cannot authorize a plan
    QVERIFY_EXCEPTION_THROWN(PublishNew(request,build,corpus,failed,runtime,completion,"storage fixture plan"),Refusal);
    QVERIFY_EXCEPTION_THROWN(PublishFilesNew(storage.path(),QDir(storage.path()).filePath("../outside"),key,"x","y"),Refusal);

    struct Owner {
        Publication completion;
        QByteArray model{"model lifetime"},input{"input lifetime"},staging{"staging lifetime"},output{"output lifetime"};
        std::shared_ptr<Owner> retirementQuarantine;
    };
    auto owner=std::make_shared<Owner>();
    const std::weak_ptr<Owner> retained=owner;
    owner->completion.Queued();
    bool discarded=false;
    QVERIFY_EXCEPTION_THROWN(DiscardAfterCompletion(owner->completion,[&]{discarded=true;owner->model.clear();}),std::runtime_error);
    QVERIFY(!discarded);QVERIFY(!owner->model.isEmpty());
    const auto receipt=detail::RetireOwnedState(owner,[](std::shared_ptr<Owner>)->std::shared_ptr<okuflow::AsyncCleanup>{throw std::bad_alloc();});
    QVERIFY(!receipt);QVERIFY(!owner);QVERIFY(!retained.expired());
    auto quarantine=retained.lock();
    QCOMPARE(quarantine->model,QByteArray("model lifetime"));
    QCOMPARE(quarantine->input,QByteArray("input lifetime"));
    QCOMPARE(quarantine->staging,QByteArray("staging lifetime"));
    QCOMPARE(quarantine->output,QByteArray("output lifetime"));
    // End the simulated fixture quarantine only after explicit completion.
    quarantine->completion.Confirmed();
    DiscardAfterCompletion(quarantine->completion,[&]{quarantine->retirementQuarantine.reset();});
    quarantine.reset();QVERIFY(retained.expired());
}
} // namespace uct_public_bf16_test
