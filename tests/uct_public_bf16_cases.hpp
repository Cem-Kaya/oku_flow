#pragma once
#include "../src/common/uct_public_bf16.hpp"
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
} // namespace uct_public_bf16_test
