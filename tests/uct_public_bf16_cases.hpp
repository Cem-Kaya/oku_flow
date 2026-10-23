#pragma once
#include "../src/common/uct_public_bf16.hpp"
#include "../src/common/uct_public_bf16_runtime.hpp"
#include "../src/common/uct_frozen_upload.hpp"
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#include <winioctl.h>
#endif

namespace uct_public_bf16_test {
using namespace okuflow::uct;
#ifdef Q_OS_WIN
inline bool SetFixtureJunction(const QString& path,const QString& target,DWORD* nativeError=nullptr) {
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    const auto handle=CreateFileW(native.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE) {if(nativeError)*nativeError=GetLastError();return false;}
    struct MountPoint {
        DWORD tag;WORD dataLength,reserved,substituteOffset,substituteLength,printOffset,printLength;
        wchar_t paths[1];
    };
    const auto print=QDir::toNativeSeparators(target).toStdWString(),substitute=L"\\??\\"+print;
    std::vector<unsigned char> bytes(offsetof(MountPoint,paths)+(substitute.size()+1+print.size()+1)*sizeof(wchar_t),0);
    auto* point=reinterpret_cast<MountPoint*>(bytes.data());
    point->tag=IO_REPARSE_TAG_MOUNT_POINT;point->dataLength=WORD(bytes.size()-8);
    point->substituteLength=WORD(substitute.size()*sizeof(wchar_t));
    point->printOffset=WORD((substitute.size()+1)*sizeof(wchar_t));point->printLength=WORD(print.size()*sizeof(wchar_t));
    std::copy(substitute.begin(),substitute.end(),point->paths);
    std::copy(print.begin(),print.end(),point->paths+substitute.size()+1);
    DWORD returned{};
    const bool result=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,point,DWORD(bytes.size()),nullptr,0,&returned,nullptr)!=FALSE;
    const auto error=result?ERROR_SUCCESS:GetLastError();
    CloseHandle(handle);if(nativeError)*nativeError=error;return result;
}
inline bool CreateFixtureJunction(const QString& path,const QString& target) {
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    if(!CreateDirectoryW(native.c_str(),nullptr))return false;
    return SetFixtureJunction(path,target);
}
#endif
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
    // The real preflight accepts only explicit candidate validation here;
    // production remains a typed refusal before any BF16 attempt starts.
    CheckEligibility(ModelTier::Medium,ModelTier::Medium,8,false,false,runtime,CandidateCatalog());
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
    QVERIFY(!failed.Record()["complete"].toBool());
    std::vector<Observation> mismatches;
    for(const int batch:{1,8})for(const auto& crop:corpus.Crops())
        mismatches.push_back({crop.id,batch,128,18710,crop.pixelsSha256,"deliberate model-free mismatch"});
    const auto mismatch=CheckGolden(corpus,build,mismatches,Completion::Confirmed);
    QVERIFY(!mismatch.Passed());QCOMPARE(mismatch.Record()["rows"].toArray().size(),16);
    QVERIFY(mismatch.Record()["complete"].toBool());
    QCOMPARE(mismatch.Record()["rows"].toArray()[0].toObject()["text"].toString(),QString("deliberate model-free mismatch"));
    const auto partial=PartialRecord(BuildIdentity(build,corpus),std::span<const Observation>(mismatches).first(3),"unknown","later call failed");
    QVERIFY(!partial["complete"].toBool());QVERIFY(!partial["passed"].toBool());
    QCOMPARE(partial["completion"].toString(),QString("unknown"));
    QCOMPARE(partial["rows"].toArray().size(),3);
    QCOMPARE(partial["rows"].toArray()[0].toObject()["text"].toString(),QString("deliberate model-free mismatch"));
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

    const QByteArray nonce(32,'1');
    const auto fixtureReceipt=[&](const LocalBuild& recipe) {
        auto identity=BuildIdentity(recipe,corpus);
        identity["planFileSha256"]=QString::fromLatin1(recipe.planFileSha256);
        // Integrity-only labelled bytes: no model, golden/pass field or approval.
        return QJsonDocument(QJsonObject{{"schema","uct-local-bf16-ready-v2"},{"identity",identity},
            {"identitySha256",QString::fromLatin1(precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact)))},
            {"fixture","non-model lookup only; no golden attestation"}}).toJson(QJsonDocument::Compact);
    };

    {
        // Positive integrity-only lookup, never a model or golden approval.
        // Both builder generations share the parent; either enumeration order
        // must return only the requested actual builder's unchanged bytes.
        QTemporaryDir lookupStorage;
        QVERIFY(lookupStorage.isValid());
        auto lookupRequest=request;lookupRequest.cacheRoot=lookupStorage.path();
        const QByteArray olderBytes("non-model lookup fixture: older builder"),currentBytes("non-model lookup fixture: current builder");
        auto olderBuild=build,currentBuild=build;
        currentBuild.builderVersion="11.5.0.0";
        olderBuild.planFileSha256=precision_artifacts::Digest(olderBytes);
        currentBuild.planFileSha256=precision_artifacts::Digest(currentBytes);
        const auto lookupParent=CacheParent(lookupRequest,currentBuild,corpus);
        QCOMPARE(CacheParent(lookupRequest,olderBuild,corpus),lookupParent);
        const auto olderReceipt=fixtureReceipt(olderBuild),currentReceipt=fixtureReceipt(currentBuild);
        qInfo("public BF16 fixture phase: two-builder/publication");
        QVERIFY(PublishFilesNew(lookupRequest,olderBuild,corpus,olderBytes,olderReceipt,nonce));
        QVERIFY(PublishFilesNew(lookupRequest,currentBuild,corpus,currentBytes,currentReceipt,nonce));
        const auto inventory=QDir(lookupParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden);
        QCOMPARE(inventory.size(),2);
        qInfo("public BF16 fixture phase: two-builder/current");
        const auto selected=FindPlan(lookupRequest,currentBuild,corpus);
        QCOMPARE(selected.bytes,currentBytes);QCOMPARE(selected.builderVersion,currentBuild.builderVersion);
        QCOMPARE(QFileInfo(selected.directory).canonicalFilePath(),
            QFileInfo(QDir(lookupParent).filePath(GenerationName(CacheKey(currentBuild,corpus),nonce))).canonicalFilePath());
        qInfo("public BF16 fixture phase: two-builder/older");
        QCOMPARE(FindPlan(lookupRequest,olderBuild,corpus).bytes,olderBytes);
        auto missingBuild=currentBuild;missingBuild.builderVersion="11.6.0.0";
        qInfo("public BF16 fixture phase: two-builder/missing");
        const auto missing=FindPlan(lookupRequest,missingBuild,corpus);
        QVERIFY(missing.bytes.isEmpty());QVERIFY(missing.builderVersion.isEmpty());QVERIFY(missing.directory.isEmpty());
        QCOMPARE(QDir(lookupParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden),inventory);
        const auto olderDirectory=QDir(lookupParent).filePath(GenerationName(CacheKey(olderBuild,corpus),nonce));
        const auto currentDirectory=QDir(lookupParent).filePath(GenerationName(CacheKey(currentBuild,corpus),nonce));
        QCOMPARE(ReadBounded(olderDirectory,"recognizer.plan",1024),olderBytes);
        QCOMPARE(ReadBounded(olderDirectory,"ready.json",16*1024),olderReceipt);
        QCOMPARE(ReadBounded(currentDirectory,"recognizer.plan",1024),currentBytes);
        QCOMPARE(ReadBounded(currentDirectory,"ready.json",16*1024),currentReceipt);

        // Force a full 32-generation walk, beyond the ten-handle path bound.
        // No match must finish without accumulating skipped child handles.
        for(int i=0;i<30;++i) {
            auto siblingBuild=currentBuild;siblingBuild.builderVersion=QByteArray("11.5.1.")+QByteArray::number(i);
            const auto siblingBytes=QByteArray("non-model lookup sibling ")+QByteArray::number(i);
            siblingBuild.planFileSha256=precision_artifacts::Digest(siblingBytes);
            QVERIFY(PublishFilesNew(lookupRequest,siblingBuild,corpus,siblingBytes,fixtureReceipt(siblingBuild),nonce));
        }
        const auto fullInventory=QDir(lookupParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden);
        QCOMPARE(fullInventory.size(),32);
        qInfo("public BF16 fixture phase: 32-generations/missing");
        QVERIFY(FindPlan(lookupRequest,missingBuild,corpus).bytes.isEmpty());
        qInfo("public BF16 fixture phase: 32-generations/current");
        QCOMPARE(FindPlan(lookupRequest,currentBuild,corpus).bytes,currentBytes);
        QCOMPARE(QDir(lookupParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden),fullInventory);
        auto overflowBuild=currentBuild;overflowBuild.builderVersion="11.5.2.0";
        const QByteArray overflowBytes("non-model lookup overflow sibling");
        overflowBuild.planFileSha256=precision_artifacts::Digest(overflowBytes);
        QVERIFY_EXCEPTION_THROWN(PublishFilesNew(lookupRequest,overflowBuild,corpus,overflowBytes,fixtureReceipt(overflowBuild),nonce),Refusal);
        QCOMPARE(QDir(lookupParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden),fullInventory);
        // An uncooperative external addition must also fail closed at33. It is
        // a recognized but uncommitted v2 generation, not a name-policy waiver.
        QVERIFY(QDir().mkdir(QDir(lookupParent).filePath(GenerationName(CacheKey(overflowBuild,corpus),nonce))));
        qInfo("public BF16 fixture phase: 33-generations/expected-refusal");
        QVERIFY_EXCEPTION_THROWN(FindPlan(lookupRequest,missingBuild,corpus),Refusal); // unchanged 32-entry bound

        // A nonmatching generation still undergoes full hash validation before
        // being skipped. Corruption is a refusal, never silently ignored.
        QTemporaryDir corruptStorage;
        QVERIFY(corruptStorage.isValid());
        auto corruptRequest=lookupRequest;corruptRequest.cacheRoot=corruptStorage.path();
        QVERIFY(PublishFilesNew(corruptStorage.path(),CacheParent(corruptRequest,olderBuild,corpus),
            CacheKey(olderBuild,corpus),"corrupt non-model fixture bytes",olderReceipt));
        qInfo("public BF16 fixture phase: corrupted-plan/expected-refusal");
        QVERIFY_EXCEPTION_THROWN(FindPlan(corruptRequest,currentBuild,corpus),Refusal);

        QTemporaryDir unknownStorage;
        QVERIFY(unknownStorage.isValid());
        auto unknownRequest=lookupRequest;unknownRequest.cacheRoot=unknownStorage.path();
        const auto unknownParent=CacheParent(unknownRequest,currentBuild,corpus);
        QVERIFY(QDir().mkpath(QDir(unknownParent).filePath("unknown-generation")));
        const auto unknownInventory=QDir(unknownParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden);
        qInfo("public BF16 fixture phase: unknown-generation/expected-refusal");
        bool unknownRefused=false;
        try { FindPlan(unknownRequest,currentBuild,corpus); }
        catch(const Refusal& error) {
            unknownRefused=true;QVERIFY(error.Code()==Reason::Model);
            const QByteArray prefix("unknown public BF16 cache generation: "),message(error.what());
            QVERIFY(message.startsWith(prefix));
            const auto context=QJsonDocument::fromJson(message.mid(prefix.size())).object();
            QCOMPARE(context["phase"].toString(),QString("FindPlan/generation-name"));
            QCOMPARE(context["ordinal"].toInt(),1);
            QCOMPARE(context["entry"].toString(),QString("unknown-generation"));
            QCOMPARE(context["entryChars"].toInt(),18);
            QCOMPARE(QFileInfo(context["path"].toString()).canonicalFilePath(),QFileInfo(unknownParent).canonicalFilePath());
            QVERIFY(context["entry"].toString().size()<=128 && context["path"].toString().size()<=512);
        }
        QVERIFY(unknownRefused);
        QCOMPARE(QDir(unknownParent).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden),unknownInventory);
    }

#ifdef Q_OS_WIN
    {
        QTemporaryDir readyStorage;
        QVERIFY(readyStorage.isValid());
        auto readyRequest=request;readyRequest.cacheRoot=readyStorage.path();
        const QByteArray readyBytes("non-model v2 marker-last fixture");
        auto readyBuild=build;readyBuild.planFileSha256=precision_artifacts::Digest(readyBytes);
        const auto readyReceipt=fixtureReceipt(readyBuild);
        const auto readyParent=CacheParent(readyRequest,readyBuild,corpus);
        bool absentBefore=false,missingBefore=false,readableAfter=false,denied=true;
        QString created;
        qInfo("public BF16 fixture phase: v2/marker-last-and-file-pins");
        QVERIFY(PublishFilesNew(readyRequest,readyBuild,corpus,readyBytes,readyReceipt,nonce,
            [&](const QString& directory,bool committed) {
                created=directory;
                if(committed) {readableAfter=FindPlan(readyRequest,readyBuild,corpus).bytes==readyBytes;return;}
                absentBefore=!QFileInfo::exists(QDir(directory).filePath(kCommitName));
                missingBefore=FindPlan(readyRequest,readyBuild,corpus).bytes.isEmpty();
                for(const auto* name:{"recognizer.plan","ready.json"})for(const DWORD access:{DWORD(GENERIC_WRITE),DWORD(DELETE)}) {
                    const auto path=NativeCachePath(QDir(directory).filePath(name));
                    const auto handle=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
                        OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                    const auto error=GetLastError();
                    denied&=handle==INVALID_HANDLE_VALUE && error==ERROR_SHARING_VIOLATION;
                    if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
                }
            }));
        QVERIFY(absentBefore && missingBefore && readableAfter && denied);
        QCOMPARE(ReadBounded(created,"recognizer.plan",1024),readyBytes);
        QCOMPARE(ReadBounded(created,"ready.json",16*1024),readyReceipt);
        QCOMPARE(ReadBounded(created,kCommitName,1024),QByteArray(kCommitBytes));
        const auto inventory=QDir(readyParent).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System);
        QVERIFY(!PublishFilesNew(readyRequest,readyBuild,corpus,readyBytes,readyReceipt,QByteArray(32,'2')));
        QCOMPARE(QDir(readyParent).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System),inventory);

        // A syntactically recognized generation cannot lie about its recipe.
        QTemporaryDir wrongRecipeStorage;
        QVERIFY(wrongRecipeStorage.isValid());
        auto wrongRequest=readyRequest;wrongRequest.cacheRoot=wrongRecipeStorage.path();
        QVERIFY(PublishFilesNew(wrongRequest.cacheRoot,CacheParent(wrongRequest,readyBuild,corpus),QByteArray(64,'f'),
            readyBytes,readyReceipt,nonce));
        QVERIFY_EXCEPTION_THROWN(FindPlan(wrongRequest,readyBuild,corpus),Refusal);

        // Marker/type/receipt/plan corruption remains a committed refusal, even
        // when another same-key generation could otherwise be returned.
        for(const auto* corrupted:{kCommitName,"recognizer.plan","ready.json"}) {
            QTemporaryDir corruptStorage;
            QVERIFY(corruptStorage.isValid());
            auto corruptRequest=readyRequest;corruptRequest.cacheRoot=corruptStorage.path();
            QVERIFY(PublishFilesNew(corruptRequest,readyBuild,corpus,readyBytes,readyReceipt,nonce));
            const auto directory=QDir(CacheParent(corruptRequest,readyBuild,corpus)).filePath(GenerationName(CacheKey(readyBuild,corpus),nonce));
            QFile file(QDir(directory).filePath(corrupted));
            QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
            QByteArray corruptBytes("corrupted labelled fixture");
            if(QByteArray(corrupted)=="ready.json") {
                auto invalid=QJsonDocument::fromJson(readyReceipt).object();
                invalid["identitySha256"]=QString(64,'0'); // valid JSON, invalid binding
                corruptBytes=QJsonDocument(invalid).toJson(QJsonDocument::Compact);
            }
            QCOMPARE(file.write(corruptBytes),qint64(corruptBytes.size()));file.close();
            QVERIFY_EXCEPTION_THROWN(FindPlan(corruptRequest,readyBuild,corpus),Refusal);
            QVERIFY_EXCEPTION_THROWN(PublishFilesNew(corruptRequest,readyBuild,corpus,readyBytes,readyReceipt,QByteArray(32,'2')),Refusal);
        }

        QTemporaryDir hardLinkStorage;
        QVERIFY(hardLinkStorage.isValid());
        auto linkRequest=readyRequest;linkRequest.cacheRoot=hardLinkStorage.path();
        QVERIFY(PublishFilesNew(linkRequest,readyBuild,corpus,readyBytes,readyReceipt,nonce));
        const auto linkedPlan=QDir(CacheParent(linkRequest,readyBuild,corpus)).filePath(GenerationName(CacheKey(readyBuild,corpus),nonce)+"/recognizer.plan");
        const auto source=NativeCachePath(linkedPlan);
        const auto alias=NativeCachePath(QDir(hardLinkStorage.path()).filePath("fixture-alias.plan"));
        QVERIFY(CreateHardLinkW(alias.c_str(),source.c_str(),nullptr));
        QVERIFY_EXCEPTION_THROWN(FindPlan(linkRequest,readyBuild,corpus),Refusal);

        QTemporaryDir fileEntryStorage;
        QVERIFY(fileEntryStorage.isValid());
        auto fileRequest=readyRequest;fileRequest.cacheRoot=fileEntryStorage.path();
        const auto fileParent=CacheParent(fileRequest,readyBuild,corpus);
        QVERIFY(QDir().mkpath(fileParent));
        // Even a recognized name cannot masquerade as an incomplete directory.
        QFile impostor(QDir(fileParent).filePath(GenerationName(CacheKey(readyBuild,corpus),nonce)));
        QVERIFY(impostor.open(QIODevice::WriteOnly|QIODevice::NewOnly));QVERIFY(impostor.write("not a directory")>0);impostor.close();
        QVERIFY_EXCEPTION_THROWN(FindPlan(fileRequest,readyBuild,corpus),Refusal);

        QTemporaryDir longStorage;
        QVERIFY(longStorage.isValid());
        auto longRequest=readyRequest;
        longRequest.cacheRoot=QDir(longStorage.path()).filePath(QString(100,'x'));
        QVERIFY(QDir().mkdir(longRequest.cacheRoot));
        const auto longGeneration=QDir(CacheParent(longRequest,readyBuild,corpus)).filePath(GenerationName(CacheKey(readyBuild,corpus),nonce));
        QVERIFY(longGeneration.size()>260);
        QVERIFY(PublishFilesNew(longRequest,readyBuild,corpus,readyBytes,readyReceipt,nonce));
        QCOMPARE(FindPlan(longRequest,readyBuild,corpus).bytes,readyBytes);
    }
    {
        QTemporaryDir crashStorage;
        QVERIFY(crashStorage.isValid());
        auto crashRequest=request;crashRequest.cacheRoot=crashStorage.path();
        const QByteArray bytes("non-model v2 interrupted fixture");
        auto recipe=build;recipe.planFileSha256=precision_artifacts::Digest(bytes);
        const auto receipt=fixtureReceipt(recipe);
        const auto parent=CacheParent(crashRequest,recipe,corpus);
        qInfo("public BF16 fixture phase: v2/interrupted-fresh-nonce");
        QVERIFY_EXCEPTION_THROWN(PublishFilesNew(crashRequest,recipe,corpus,bytes,receipt,nonce,
            [](const QString&,bool committed){if(!committed)throw std::runtime_error("fixture interrupted before marker");}),std::runtime_error);
        const auto abandoned=QDir(parent).filePath(GenerationName(CacheKey(recipe,corpus),nonce));
        QVERIFY(!QFileInfo::exists(QDir(abandoned).filePath(kCommitName)));
        QCOMPARE(ReadBounded(abandoned,"recognizer.plan",1024),bytes);
        QVERIFY(FindPlan(crashRequest,recipe,corpus).bytes.isEmpty());
        // The exact abandoned directory is never adopted, repaired or removed.
        QVERIFY_EXCEPTION_THROWN(PublishFilesNew(crashRequest,recipe,corpus,bytes,receipt,nonce),Refusal);
        QVERIFY(PublishFilesNew(crashRequest,recipe,corpus,bytes,receipt,QByteArray(32,'2')));
        QCOMPARE(FindPlan(crashRequest,recipe,corpus).bytes,bytes);
        QCOMPARE(QDir(parent).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),2);
        QVERIFY(!QFileInfo::exists(QDir(abandoned).filePath(kCommitName)));
        QCOMPARE(ReadBounded(abandoned,"ready.json",16*1024),receipt);

        // v1 is neither a fallback nor a source for rekey/migration.
        QTemporaryDir legacyStorage;
        QVERIFY(legacyStorage.isValid());
        QVERIFY(QDir().mkpath(QDir(legacyStorage.path()).filePath("public-bf16/unknown-v1")));
        auto legacyRequest=request;legacyRequest.cacheRoot=legacyStorage.path();
        QVERIFY(FindPlan(legacyRequest,recipe,corpus).bytes.isEmpty());
        QVERIFY(QFileInfo::exists(QDir(legacyStorage.path()).filePath("public-bf16/unknown-v1")));
        QVERIFY(CacheParent(legacyRequest,recipe,corpus).contains("/public-bf16-v2/"));
    }
    {
        QTemporaryDir overlapStorage;
        QVERIFY(overlapStorage.isValid());
        auto overlapRequest=request;overlapRequest.cacheRoot=overlapStorage.path();
        const QByteArray outerBytes("non-model overlapping outer"),innerBytes("non-model overlapping inner");
        auto outerBuild=build,innerBuild=build;
        outerBuild.planFileSha256=precision_artifacts::Digest(outerBytes);innerBuild.planFileSha256=precision_artifacts::Digest(innerBytes);
        QCOMPARE(CacheKey(outerBuild,corpus),CacheKey(innerBuild,corpus)); // recipe, not plan hash
        bool innerPublished=false,outerReadable=false;
        qInfo("public BF16 fixture phase: v2/overlap-and-deterministic-selection");
        QVERIFY(PublishFilesNew(overlapRequest,outerBuild,corpus,outerBytes,fixtureReceipt(outerBuild),QByteArray(32,'2'),
            [&](const QString&,bool committed) {
                if(committed) {outerReadable=FindPlan(overlapRequest,outerBuild,corpus).bytes==innerBytes;return;}
                innerPublished=PublishFilesNew(overlapRequest,innerBuild,corpus,innerBytes,fixtureReceipt(innerBuild),nonce);
            }));
        QVERIFY(innerPublished && outerReadable);
        const auto parent=CacheParent(overlapRequest,outerBuild,corpus);
        QCOMPARE(QDir(parent).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),2);
        QCOMPARE(FindPlan(overlapRequest,outerBuild,corpus).bytes,innerBytes); // lower nonce despite later insertion
        QVERIFY(!PublishFilesNew(overlapRequest,outerBuild,corpus,outerBytes,fixtureReceipt(outerBuild),QByteArray(32,'3')));
        const auto outer=QDir(parent).filePath(GenerationName(CacheKey(outerBuild,corpus),QByteArray(32,'2')));
        QCOMPARE(ReadBounded(outer,"recognizer.plan",1024),outerBytes);
        // A valid first match cannot hide corruption in a later sorted sibling.
        QFile corrupt(QDir(outer).filePath("recognizer.plan"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate));QVERIFY(corrupt.write("corrupt later sibling")>0);corrupt.close();
        QVERIFY_EXCEPTION_THROWN(FindPlan(overlapRequest,outerBuild,corpus),Refusal);
        QVERIFY_EXCEPTION_THROWN(PublishFilesNew(overlapRequest,outerBuild,corpus,outerBytes,fixtureReceipt(outerBuild),QByteArray(32,'3')),Refusal);
    }
    {
        QTemporaryDir capacityStorage;
        QVERIFY(capacityStorage.isValid());
        auto capacityRequest=request;capacityRequest.cacheRoot=capacityStorage.path();
        const QByteArray bytes("non-model capacity reservation fixture");
        auto recipe=build;recipe.planFileSha256=precision_artifacts::Digest(bytes);
        const auto receipt=fixtureReceipt(recipe);
        const auto parent=CacheParent(capacityRequest,recipe,corpus);
        QVERIFY(QDir().mkpath(parent));
        for(int i=0;i<31;++i) {
            const auto abandonedNonce=QByteArray::number(i,16).rightJustified(32,'0');
            QVERIFY(QDir().mkdir(QDir(parent).filePath(GenerationName(CacheKey(recipe,corpus),abandonedNonce))));
        }
        bool fullRefused=false;
        qInfo("public BF16 fixture phase: v2/capacity-reservation");
        QVERIFY(PublishFilesNew(capacityRequest,recipe,corpus,bytes,receipt,QByteArray(32,'e'),
            [&](const QString&,bool committed) {
                if(committed)return;
                try {PublishFilesNew(capacityRequest,recipe,corpus,bytes,receipt,QByteArray(32,'f'));}
                catch(const Refusal&) {fullRefused=true;}
            }));
        QVERIFY(fullRefused);
        QCOMPARE(QDir(parent).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System).size(),32);
        QCOMPARE(FindPlan(capacityRequest,recipe,corpus).bytes,bytes);
        QVERIFY(!PublishFilesNew(capacityRequest,recipe,corpus,bytes,receipt,QByteArray(32,'f')));
    }
    {
        // Post-close observer failure is already committed, never incomplete.
        QTemporaryDir committedStorage;
        QVERIFY(committedStorage.isValid());
        auto committedRequest=request;committedRequest.cacheRoot=committedStorage.path();
        const QByteArray bytes("non-model committed observer failure");
        auto recipe=build;recipe.planFileSha256=precision_artifacts::Digest(bytes);
        const auto receipt=fixtureReceipt(recipe);
        QVERIFY_EXCEPTION_THROWN(PublishFilesNew(committedRequest,recipe,corpus,bytes,receipt,nonce,
            [](const QString&,bool committed){if(committed)throw std::runtime_error("fixture already committed");}),std::runtime_error);
        QCOMPARE(FindPlan(committedRequest,recipe,corpus).bytes,bytes);
        QVERIFY(!PublishFilesNew(committedRequest,recipe,corpus,bytes,receipt,QByteArray(32,'2')));
    }
#endif

    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    request.cacheRoot=storage.path();
    request.enabled=true;
    const auto baseline=BaselinePolicy(request);
    QVERIFY(!baseline.allowPreparation);QCOMPARE(baseline.root,storage.path());
    request.use=CatalogUse::Production;
    QVERIFY(BaselinePolicy(request).allowPreparation);QVERIFY(BaselinePolicy(request).root.isEmpty());
    request.use=CatalogUse::CandidateValidation;request.enabled=false;request.cacheRoot="relative";
    QVERIFY(BaselinePolicy(request).allowPreparation);QVERIFY(BaselinePolicy(request).root.isEmpty());
    request.enabled=true;
    QVERIFY_EXCEPTION_THROWN(BaselinePolicy(request),Refusal);
    request.cacheRoot=storage.path();
    const auto parent=CacheParent(request,build,corpus);
    const QByteArray key(64,'a');
    // Plain storage fixture bytes: no model, real plan or passing golden record.
    qInfo("public BF16 fixture phase: storage/publication");
    QVERIFY(PublishFilesNew(storage.path(),parent,key,"storage fixture plan","storage fixture receipt",nonce));
    QVERIFY(!PublishFilesNew(storage.path(),parent,key,"replacement must fail","replacement must fail",nonce));
    const auto generation=QDir(parent).filePath(GenerationName(key,nonce));
    QCOMPARE(ReadBounded(generation,"recognizer.plan",1024),QByteArray("storage fixture plan"));
    QCOMPARE(ReadBounded(generation,"ready.json",1024),QByteArray("storage fixture receipt"));
    QCOMPARE(ReadBounded(generation,kCommitName,1024),QByteArray(kCommitBytes));
    QCOMPARE(QDir(parent).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),1);
    QVERIFY_EXCEPTION_THROWN(FindPlan(request,build,corpus),Refusal); // storage receipt cannot authorize a plan
    QVERIFY_EXCEPTION_THROWN(PublishNew(request,build,corpus,failed,runtime,completion,"storage fixture plan"),Refusal);
    QVERIFY_EXCEPTION_THROWN(PublishFilesNew(storage.path(),QDir(storage.path()).filePath("../outside"),key,"x","y"),Refusal);
#ifdef Q_OS_WIN
    QTemporaryDir redirected,outside;
    QVERIFY(redirected.isValid() && outside.isValid());
    const auto junction=QDir(redirected.path()).filePath("public-bf16-v2");
    struct RemoveJunction {std::wstring path;~RemoveJunction(){RemoveDirectoryW(path.c_str());}} removeJunction{QDir::toNativeSeparators(junction).toStdWString()};
    QVERIFY(CreateFixtureJunction(junction,outside.path()));
    const auto before=QDir(outside.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden);
    auto redirectedRequest=request;redirectedRequest.cacheRoot=redirected.path();
    QVERIFY_EXCEPTION_THROWN(FindPlan(redirectedRequest,build,corpus),Refusal);
    QVERIFY_EXCEPTION_THROWN(PublishFilesNew(redirected.path(),QDir(junction).filePath(QString::fromLatin1(key)),key,"x","y"),Refusal);
    QCOMPARE(QDir(outside.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),before);
    // Existing ancestors must remain unretargetable through the transaction.
    DirectoryPins pinned;
    pinned.Prepare(storage.path(),parent);
    const auto parentNative=QDir::toNativeSeparators(parent).toStdWString();
    const auto replacement=CreateFileW(parentNative.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    const bool denied=replacement==INVALID_HANDLE_VALUE;
    if(!denied)CloseHandle(replacement);
    QVERIFY(denied);
    const auto deleteOnly=CreateFileW(parentNative.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    const bool deleteDenied=deleteOnly==INVALID_HANDLE_VALUE;
    if(!deleteDenied)CloseHandle(deleteOnly);
    QVERIFY(deleteDenied);
    // A writer could mutate this directory into a junction without DELETE.
    // Child creation is distinct: the production publisher still writes AFTER
    // holding the generation, with ready-last publication under the pinned parent.
    const auto noDelete=CreateFileW(parentNative.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    const bool writeDenied=noDelete==INVALID_HANDLE_VALUE;
    if(!writeDenied)CloseHandle(noDelete);
    QVERIFY(writeDenied);
    const QByteArray pinnedKey(64,'b');
    qInfo("public BF16 fixture phase: pinned-parent/publication");
    QVERIFY(PublishFilesNew(storage.path(),parent,pinnedKey,"pinned-parent non-model fixture","pinned-parent storage receipt",nonce));
    QVERIFY(!PublishFilesNew(storage.path(),parent,pinnedKey,"replacement must fail","replacement must fail",nonce));
    const auto pinnedGeneration=QDir(parent).filePath(GenerationName(pinnedKey,nonce));
    QCOMPARE(ReadBounded(pinnedGeneration,"recognizer.plan",1024),QByteArray("pinned-parent non-model fixture"));
    QCOMPARE(ReadBounded(pinnedGeneration,"ready.json",1024),QByteArray("pinned-parent storage receipt"));

    qInfo("public BF16 fixture phase: post-hold-reparse/ancestor-and-generation");
    for(const auto* kind:{"ancestor","generation"}) {
        QTemporaryDir mutationRoot,mutationOutside;
        QVERIFY(mutationRoot.isValid() && mutationOutside.isValid());
        const auto emptyDirectory=QDir(mutationRoot.path()).filePath("empty-"+QString::fromLatin1(kind));
        QVERIFY(QDir().mkdir(emptyDirectory));
        const auto outsideInventory=QDir(mutationOutside.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden);
        // Close the pin before removing a junction if a regression permits one.
        // RemoveDirectoryW removes that link without traversing its target.
        RemoveJunction removeMutation{QDir::toNativeSeparators(emptyDirectory).toStdWString()};
        DirectoryPins mutationPin;
        mutationPin.Hold(emptyDirectory);
        DWORD mutationError{};
        QVERIFY(!SetFixtureJunction(emptyDirectory,mutationOutside.path(),&mutationError));
        QCOMPARE(mutationError,DWORD(ERROR_SHARING_VIOLATION));
        const auto child=QDir(emptyDirectory).filePath("child");
        QVERIFY(QDir().mkdir(child));
        QFile inside(QDir(child).filePath("inside.txt"));
        const QByteArray insideBytes("private post-hold non-model write");
        QVERIFY(inside.open(QIODevice::WriteOnly|QIODevice::NewOnly));
        QCOMPARE(inside.write(insideBytes),qint64(insideBytes.size()));inside.close();
        QCOMPARE(ReadBounded(child,"inside.txt",1024),insideBytes);
        QCOMPARE(QDir(mutationOutside.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),outsideInventory);
    }
#endif

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
