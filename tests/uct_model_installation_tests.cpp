#include "okuflow/common/uct_model_installation.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "../src/common/uct_model_installation_internal.hpp"
#include "../src/common/uct_download_catalog.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace okuflow::uct;
using namespace okuflow::uct::installation;
namespace {
const QByteArray kSource="a small immutable model fixture";
Model Fixture() {
    return {"ocr-det-medium","fixture","det/medium",QString(40,'a'),true,ModelTier::Medium,
        {{"inference.onnx",QUrl("https://huggingface.co/fixture"),kSource.size(),
          QCryptographicHash::hash(kSource,QCryptographicHash::Sha256).toHex()}}};
}
void Write(const QString& path,const QByteArray& bytes) {
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly) || f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");
}
detail::Fetch Fetch(int& count,bool corrupt=false,std::atomic<bool>* cancel=nullptr) {
    return [&,corrupt,cancel](const File&,const QString& path,const std::atomic<bool>&,qint64,qint64,const Notify&) {
        ++count;Write(path,corrupt ? QByteArray("bad") : kSource);if(cancel)cancel->store(true);
    };
}
}
class ModelInstallationTests:public QObject {
    Q_OBJECT
private slots:
    void catalogMatchesReviewedSourcePins() {
        QFile file(QStringLiteral(OKUFLOW_SOURCE_ROOT "/scripts/uct/download-models.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()),QJsonDocument::fromJson(kDownloadCatalog));
        const auto models=Catalog();QCOMPARE(models.size(),6);
        QSet<QString> ids;
        for(const auto& model:models) {
            QVERIFY(!ids.contains(model.id));ids.insert(model.id);
            const auto* pin=FindPaddleModelPin(model.detection,model.tier);QVERIFY(pin);
            QCOMPARE(model.directory,QString::fromUtf8(pin->directory.data(),pin->directory.size()));
            QCOMPARE(model.revision,QString::fromUtf8(pin->revision.data(),pin->revision.size()));
            QCOMPARE(model.files[0].sha256,QByteArray(pin->modelSha256.data(),pin->modelSha256.size()));
            QCOMPARE(model.files[1].sha256,QByteArray(pin->configSha256.data(),pin->configSha256.size()));
            QVERIFY(model.Bytes()>0);
        }
        QCOMPARE(Select({}).size(),2);
    }
    void mathPackagePinsNoticesAndJoinsDraftSelection() {
        for(const auto model:{formula::Model::PlusS,formula::Model::PlusM,formula::Model::L,formula::Model::PlusL}) {
            const auto package=FormulaPackage(model);QCOMPARE(package.files.size(),6);QVERIFY(package.Bytes()>200000000);
            QSet<QString> names;for(const auto& file:package.files){names.insert(file.name);QCOMPARE(file.sha256.size(),64);QVERIFY(file.url.scheme()=="https");}
            QVERIFY(names.contains("LICENSE.txt") && names.contains("NOTICE.txt"));
            ModelSettings settings;settings.installMath=true;settings.mathModel=model;
            const auto selected=Select(settings);QCOMPARE(selected.size(),3);QCOMPARE(selected.last().id,package.id);
        }
    }
    void optionalPinnedSourceDownload() {
        if(qEnvironmentVariable("OKUFLOW_UCT_DOWNLOAD_TEST")!="1")QSKIP("Explicit real model download not selected.");
        const auto evidence=qEnvironmentVariable("OKUFLOW_UCT_DOWNLOAD_EVIDENCE");
        QVERIFY(QDir::isAbsolutePath(evidence));QVERIFY(!QFileInfo::exists(evidence));
        auto models=Catalog();const auto found=std::find_if(models.begin(),models.end(),[](const auto& m){
            return m.detection && m.tier==ModelTier::Tiny;
        });QVERIFY(found!=models.end());const auto model=*found;
        std::atomic<bool> cancelled{false};int prepares=0;
        const auto cache=QDir(evidence).filePath("sources");
        const auto result=Run({model},QDir(evidence).filePath("empty-bundle"),cache,cancelled,{},
            [&](const std::atomic<bool>&){++prepares;return Preparation{VerifyDirectory(CachedDirectory(cache,model),model),"source transport check only"};});
        QVERIFY(QDir().mkpath(evidence));
        Write(QDir(evidence).filePath("transport.json"),QJsonDocument(QJsonObject{{"ok",result.ok},
            {"code",result.code},{"detail",result.detail},{"bytes",model.Bytes()},{"id",model.id}}).toJson());
        QVERIFY2(result.ok,qPrintable(result.detail));QCOMPARE(prepares,1);
        QVERIFY(VerifyDirectory(CachedDirectory(cache,model),model));
    }
    void completeSourcesPrecedePreparationAndAreReused() {
        QTemporaryDir temp;QVERIFY(temp.isValid());std::atomic<bool> cancelled{false};int downloads=0,prepares=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        const auto prepare=[&](const std::atomic<bool>&) {
            ++prepares;return Preparation{VerifyDirectory(CachedDirectory(cache,model),model),"fixture ready"};
        };
        auto result=detail::RunPinned({model},temp.filePath("bundle"),cache,cancelled,{},prepare,Fetch(downloads));
        QVERIFY2(result.ok,qPrintable(result.detail));QCOMPARE(downloads,1);QCOMPARE(prepares,1);
        QCOMPARE(QDir(cache).entryList({"ready-*.json"},QDir::Files).size(),1);
        result=detail::RunPinned({model},temp.filePath("bundle"),cache,cancelled,{},prepare,Fetch(downloads));
        QVERIFY(result.ok);QCOMPARE(downloads,1);QCOMPARE(prepares,2);
    }
    void damagedDownloadCannotPublishOrPrepare() {
        QTemporaryDir temp;std::atomic<bool> cancelled{false};int downloads=0,prepares=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        Write(temp.filePath("active"),"previous selection");
        const auto result=detail::RunPinned({model},temp.filePath("bundle"),cache,cancelled,{},
            [&](const std::atomic<bool>&){++prepares;return Preparation{true,{}};},Fetch(downloads,true));
        QVERIFY(!result.ok);QCOMPARE(result.code,QString("download_verification"));QCOMPARE(prepares,0);
        QVERIFY(!QFileInfo::exists(CachedDirectory(cache,model)));
        QCOMPARE(QDir(cache).entryList({"download-*","ready-*"},QDir::Dirs|QDir::Files|QDir::NoDotAndDotDot).size(),0);
        QFile active(temp.filePath("active"));QVERIFY(active.open(QIODevice::ReadOnly));QCOMPARE(active.readAll(),QByteArray("previous selection"));
    }
    void preparationFailureKeepsVerifiedSourcesWithoutReadyReceipt() {
        QTemporaryDir temp;std::atomic<bool> cancelled{false};int downloads=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        const auto result=detail::RunPinned({model},{},cache,cancelled,{},
            [](const std::atomic<bool>&){return Preparation{false,"backend refused"};},Fetch(downloads));
        QVERIFY(!result.ok);QCOMPARE(result.code,QString("preparation_failed"));
        QVERIFY(VerifyDirectory(CachedDirectory(cache,model),model));
        QVERIFY(QDir(cache).entryList({"ready-*"},QDir::Files).isEmpty());
    }
    void damagedOwnedCacheIsPreservedAndRepaired() {
        QTemporaryDir temp;std::atomic<bool> cancelled{false};int downloads=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        const auto prepare=[](const std::atomic<bool>&){return Preparation{true,"fixture ready"};};
        QVERIFY(detail::RunPinned({model},{},cache,cancelled,{},prepare,Fetch(downloads)).ok);
        Write(QDir(CachedDirectory(cache,model)).filePath("inference.onnx"),"broken");
        const auto result=detail::RunPinned({model},{},cache,cancelled,{},prepare,Fetch(downloads));
        QVERIFY2(result.ok,qPrintable(result.detail));QCOMPARE(downloads,2);
        QVERIFY(VerifyDirectory(CachedDirectory(cache,model),model));
        const auto damaged=QDir(cache).entryList({"damaged-*"},QDir::Dirs|QDir::NoDotAndDotDot);
        QCOMPARE(damaged.size(),1);
        QFile old(QDir(cache).filePath(damaged.first()+"/inference.onnx"));
        QVERIFY(old.open(QIODevice::ReadOnly));QCOMPARE(old.readAll(),QByteArray("broken"));
    }
    void unknownCacheIsNotAdoptedOrRemoved() {
        QTemporaryDir temp;std::atomic<bool> cancelled{false};int downloads=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        QVERIFY(QDir().mkpath(CachedDirectory(cache,model)));
        Write(QDir(CachedDirectory(cache,model)).filePath("keep.txt"),"owner data");
        const auto result=detail::RunPinned({model},{},cache,cancelled,{},
            [](const std::atomic<bool>&){return Preparation{true,{}};},Fetch(downloads));
        QVERIFY(!result.ok);QCOMPARE(result.code,QString("cache_damaged"));QCOMPARE(downloads,0);
        QVERIFY(QFileInfo::exists(QDir(CachedDirectory(cache,model)).filePath("keep.txt")));
    }
    void cancellationHasNoActivationAuthority_data() {
        QTest::addColumn<int>("phase");QTest::newRow("before")<<0;QTest::newRow("download")<<1;QTest::newRow("preparation")<<2;
    }
    void cancellationHasNoActivationAuthority() {
        QFETCH(int,phase);QTemporaryDir temp;std::atomic<bool> cancelled{phase==0};int downloads=0,prepares=0;
        const auto model=Fixture();const auto cache=temp.filePath("cache");
        const auto result=detail::RunPinned({model},{},cache,cancelled,{},[&](const std::atomic<bool>&) {
            ++prepares;if(phase==2)cancelled=true;return Preparation{true,"prepared"};
        },Fetch(downloads,false,phase==1?&cancelled:nullptr));
        QVERIFY(!result.ok);QVERIFY(result.cancelled);QCOMPARE(prepares,phase==2?1:0);
        QVERIFY(QDir(cache).entryList({"ready-*"},QDir::Files).isEmpty());
        if(phase<2)QVERIFY(!QFileInfo::exists(CachedDirectory(cache,model)));
    }
    void concurrentSetupRefusesBeforeTransport() {
        QTemporaryDir temp;QLockFile lock(temp.filePath("installation.lock"));QVERIFY(lock.tryLock());
        std::atomic<bool> cancelled{false};int downloads=0;
        const auto result=detail::RunPinned({Fixture()},{},temp.path(),cancelled,{},
            [](const std::atomic<bool>&){return Preparation{true,{}};},Fetch(downloads));
        QVERIFY(!result.ok);QCOMPARE(result.code,QString("setup_busy"));QCOMPARE(downloads,0);
    }
    void alteredCatalogRequestNeverStartsNetworkOrWrites() {
        QTemporaryDir temp;std::atomic<bool> cancelled{false};auto models=Select({});
        models[0].files[0].url=QUrl("http://127.0.0.1/changed");
        const auto path=temp.filePath("untouched");
        const auto result=Run(models,{},path,cancelled,{},[](const std::atomic<bool>&){return Preparation{true,{}};});
        QVERIFY(!result.ok);QCOMPARE(result.code,QString("invalid_request"));QVERIFY(!QFileInfo::exists(path));
    }
};
QTEST_GUILESS_MAIN(ModelInstallationTests)
#include "uct_model_installation_tests.moc"
