#pragma once
#include "uct_batch_diagnostic.hpp"
#include "../src/common/uct_public_bf16_runtime.hpp"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <mutex>

namespace uct_public_runtime_test {
inline void Prepare() {
    using namespace okuflow::uct;
    if(qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_DIAGNOSTIC")!="1")QSKIP("Explicit public BF16 runtime diagnostic not selected.");
    const auto tierToken=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_TIER");
    QVERIFY(tierToken=="tiny" || tierToken=="small" || tierToken=="medium");
    const auto detectorToken=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_DETECTOR","medium");
    const auto detector=ParseModelTier(detectorToken);QVERIFY(detector);
    const auto parsed=ParseModelTier(tierToken);QVERIFY(parsed);
    const auto tier=*parsed;
    const bool fp32Control=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_FP32_CONTROL")=="1";
    const bool runtimePreference=qEnvironmentVariable("OKUFLOW_UCT_RUNTIME_BF16_PREFERENCE")=="1";
    QVERIFY(!fp32Control || !runtimePreference);
    const auto modelRoot=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_MODELS");
    const auto corpusRoot=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_CORPUS");
    const auto cacheRoot=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_CACHE");
    const auto evidence=qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_EVIDENCE");
    for(const auto& path:{modelRoot,corpusRoot,cacheRoot,evidence})QVERIFY(runtime_precision::LocalAbsolute(path));
    QVERIFY(!QFileInfo::exists(evidence));
    struct Records {std::mutex mutex;QJsonArray values;};
    const auto records=std::make_shared<Records>();
    uct_batch_test::RetirementReceipts retirement;
    auto recognizer=CreatePublicBf16ValidationRecognizer(DefaultPaddleRoot(),qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT"),
        modelRoot,corpusRoot,cacheRoot,[records](const QJsonObject& row){
            std::lock_guard lock(records->mutex);
            if(records->values.size()<32)records->values.push_back(row);
        },retirement.Observer(),*detector,tier,qEnvironmentVariable("OKUFLOW_UCT_PUBLIC_BF16_DETECTOR")=="1",fp32Control,runtimePreference);
    QVERIFY(recognizer);
    const auto initialized=recognizer->Initialize();
    QVERIFY(initialized.usesGpu);
    QImage image(96,96,QImage::Format_ARGB32);image.fill(Qt::white);
    std::atomic<bool> cancelled{false};
    RecognitionOutput output;
    QElapsedTimer elapsed;elapsed.start();
    do {
        output=recognizer->Recognize(image,cancelled);
        if(!output.backendPreparing)break;
        QTest::qWait(10);
    }while(elapsed.elapsed()<800000);
    const auto backend=recognizer->Status();
    cancelled=true;
    recognizer.reset();
    const auto closed=retirement.Wait();
    QJsonArray observations;
    {std::lock_guard lock(records->mutex);observations=records->values;}
    QJsonObject result{{"tier",tierToken},{"detectorTier",detectorToken},{"candidateValidation",true},{"productionActivated",false},{"fp32Control",fp32Control},{"runtimePreference",runtimePreference},
        {"ok",output.ok},{"error",output.error},{"preparing",output.backendPreparing},{"elapsedMs",elapsed.elapsed()},
        {"usesGpu",backend.usesGpu},{"backend",backend.name},{"detail",backend.detail},{"attestations",observations},
        {"retirement",closed.Json()}};
    QFile saved(evidence);QVERIFY(saved.open(QIODevice::WriteOnly|QIODevice::NewOnly));
    const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Indented);
    QCOMPARE(saved.write(bytes),qint64(bytes.size()));QVERIFY(saved.flush());saved.close();
    qInfo().noquote()<<"Public BF16 candidate evidence="<<evidence<<"tier="<<tierToken<<"detail="<<backend.detail;
    QVERIFY2(output.ok,qPrintable(output.error));
    QVERIFY(!output.backendPreparing);
    QVERIFY(backend.usesGpu);
    QVERIFY(closed.Passed());
    if(fp32Control)QVERIFY(backend.detail.contains("FP32 control only"));
    else QVERIFY(backend.detail.contains("Recognition graph=BF16-linear,") || backend.detail.contains("Public BF16 unavailable") || backend.detail.contains("Runtime BF16 unavailable"));
}
} // namespace uct_public_runtime_test
