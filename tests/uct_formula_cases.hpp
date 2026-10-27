#pragma once
#include "../src/common/uct_formula_internal.hpp"
#include "../src/common/uct_formula_catalog.hpp"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <cstring>
#include <future>
#include <thread>

namespace uct_formula_test {
using namespace okuflow::uct::formula;
inline void CatalogContract() {
    QFile source(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()).filePath("../scripts/uct/formula-packs.json"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(source.readAll()),QJsonDocument::fromJson(kCatalog));
    QCOMPARE(QJsonDocument::fromJson(kCatalog).object()["models"].toArray().size(),4);
    for(auto id:{Model::PlusS,Model::PlusM,Model::L,Model::PlusL}) {
        const auto model=Describe(id);
        QCOMPARE(model.files.size(),4);QCOMPARE(model.revision.size(),40);
        for(const auto& pin:model.files) {
            const auto hash=pin.toObject()["sha256"].toString().toLatin1();
            QCOMPARE(hash.size(),64);QCOMPARE(QByteArray::fromHex(hash).toHex(),hash);
            QVERIFY(pin.toObject()["bytes"].toInteger()>0 && pin.toObject()["bytes"].toInteger()<kPlanLimit);
        }
        for(const auto& p:model.states) {
            QCOMPARE(TensorBytes(p.initial,p.dtype),p.initialData.size());QVERIFY(ShapeWithin(p.initial,p));
            auto shape=p.maximum;shape[0]++;QVERIFY(!ShapeWithin(shape,p));
        }
        const auto key=CacheKey(model,"decoder",QByteArray(16,'a'),"11.4.0.106");
        QCOMPARE(key.size(),64);
        QVERIFY(key!=CacheKey(model,"encoder",QByteArray(16,'a'),"11.4.0.106"));
        QVERIFY(key!=CacheKey(model,"decoder",QByteArray(16,'b'),"11.4.0.106"));
        QVERIFY(key!=CacheKey(model,"decoder",QByteArray(16,'a'),"11.4.1.1"));
    }
    QVERIFY_EXCEPTION_THROWN(TensorBytes({1,16,4096,4096},"float32"),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(TensorBytes({-1},"float32"),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(TensorBytes({1},"float16"),std::runtime_error);
}
inline void TokenizerContract() {
    const QJsonObject vocab{{"<s>",0},{"<pad>",1},{"</s>",2},{"k",3},{"Ġ",4},{"=",5},{"\\frac",6},{"{",7},{"}",8},{"1",9},{"2",10}};
    const QJsonArray special{QJsonObject{{"id",0},{"special",true}},QJsonObject{{"id",1},{"special",true}},QJsonObject{{"id",2},{"special",true}}};
    const QJsonObject tokenizer{{"model",QJsonObject{{"type","BPE"},{"vocab",vocab}}},
        {"decoder",QJsonObject{{"type","ByteLevel"}}},{"added_tokens",special}};
    const auto config=QJsonDocument(QJsonObject{{"PostProcess",QJsonObject{{"character_dict",QJsonObject{{"fast_tokenizer_file",tokenizer}}}}}}).toJson();
    const auto dictionary=LoadVocabulary(config);
    QCOMPARE(DecodeTokens({0,3,4,5,4,6,7,9,8,7,10,8,2},dictionary),QStringLiteral("k = \\frac{1}{2}"));
    QVERIFY_EXCEPTION_THROWN(DecodeTokens({3,5},dictionary),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(DecodeTokens({65536,2},dictionary),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(DecodeTokens({100,2},dictionary),std::runtime_error);
    auto invalid=dictionary;invalid[3]=QByteArray("\xc3",1);
    QVERIFY_EXCEPTION_THROWN(DecodeTokens({3,2},invalid),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(LoadVocabulary("{}"),std::runtime_error);
}
inline QByteArray ReadNpy(const QString& path,const QByteArray& type,qint64 bytes) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()>bytes+1024)throw std::runtime_error("Invalid native formula reference file.");
    const auto data=file.readAll();
    if(data.size()<10 || data.first(6)!=QByteArray("\x93NUMPY",6) || data[6]!=1)throw std::runtime_error("Unsupported reference NPY header.");
    const int header=static_cast<unsigned char>(data[8])+(static_cast<unsigned char>(data[9])<<8);
    if(data.size()!=10+header+bytes || !data.sliced(10,header).contains(type) ||
        !data.sliced(10,header).contains("'fortran_order': False"))throw std::runtime_error("Invalid reference NPY shape or type.");
    return data.sliced(10+header);
}
inline void NativeDiagnostic() {
    if(qEnvironmentVariable("OKUFLOW_UCT_FORMULA_TEST")!="1")QSKIP("Explicit native formula GPU diagnostic not selected.");
#if OKUFLOW_UCT_TRT
    const auto source=qEnvironmentVariable("OKUFLOW_UCT_FORMULA_SOURCES"),reference=qEnvironmentVariable("OKUFLOW_UCT_FORMULA_REFERENCE");
    const auto evidence=qEnvironmentVariable("OKUFLOW_UCT_FORMULA_EVIDENCE"),cache=qEnvironmentVariable("OKUFLOW_UCT_FORMULA_CACHE");
    QVERIFY(QDir::isAbsolutePath(source) && QDir::isAbsolutePath(reference) && QDir::isAbsolutePath(evidence) && QDir::isAbsolutePath(cache));
    QVERIFY(!QFileInfo::exists(evidence));QVERIFY(QDir().mkpath(evidence));
    std::atomic<bool> cancel=false;QElapsedTimer setup;setup.start();
    auto prepared=Prepare(Model::PlusL,source,cache,cancel,std::chrono::steady_clock::now()+std::chrono::minutes(10));
    QJsonObject receipt{{"precision","FP32 graph; TF32 permitted"},{"native",true},{"setupMs",setup.elapsed()},
        {"setupCode",prepared.code},{"setupDetail",prepared.detail}};
    const auto save=[&]{QFile out(QDir(evidence).filePath("native-formula.json"));if(out.open(QIODevice::WriteOnly))out.write(QJsonDocument(receipt).toJson());};
    save();QVERIFY2(prepared.engine,qPrintable(prepared.detail));QJsonArray cases;
    for(const auto& name:{QString("chalk-kinetic-normalized"),QString("chalk-work-normalized"),QString("chalk-delta-normalized"),QString("official-complex")}) {
        const auto pixels=ReadNpy(QDir(reference).filePath(name+"-input.npy"),"<f4",768*768*4);
        std::vector<float> tensor(768*768);std::memcpy(tensor.data(),pixels.constData(),pixels.size());
        // Move the ready engine to another worker; agreement is evidence only.
        auto future=std::async(std::launch::async,[engine=std::move(prepared.engine),&cancel,&tensor]() mutable {
            auto result=engine->Recognize(tensor,cancel,std::chrono::steady_clock::now()+std::chrono::seconds(30));
            return std::make_pair(std::move(engine),std::move(result));
        });
        auto pair=future.get();prepared.engine=std::move(pair.first);const auto& result=pair.second;
        QJsonArray tokens;for(auto id:result.tokens)tokens.append(id);
        QFile expected(QDir(reference).filePath(name+"-tokens.npy"));QVERIFY(expected.open(QIODevice::ReadOnly));const auto raw=expected.readAll();
        QVERIFY(raw.size()>=10);const int h=10+static_cast<unsigned char>(raw[8])+(static_cast<unsigned char>(raw[9])<<8);
        const QByteArray actual(reinterpret_cast<const char*>(result.tokens.constData()),result.tokens.size()*8);
        QVector<double> timings;
        for(int run=0;run<9;++run) {
            const auto warm=prepared.engine->Recognize(tensor,cancel,std::chrono::steady_clock::now()+std::chrono::seconds(30));
            QVERIFY2(warm.ok,qPrintable(warm.detail));
            if(run>=2)timings.push_back(warm.milliseconds);
        }
        std::sort(timings.begin(),timings.end());
        cases.append(QJsonObject{{"case",name},{"ok",result.ok},{"code",result.code},{"detail",result.detail},{"latex",result.latex},
            {"tokens",tokens},{"iterations",result.iterations},{"milliseconds",result.milliseconds},{"sameTokensAsReference",actual==raw.sliced(h)}});
        auto entry=cases.last().toObject();entry["warmMedianMs"]=timings[timings.size()/2];entry["warmMeasuredRuns"]=timings.size();
        cases[cases.size()-1]=entry;
        receipt["cases"]=cases;save();QVERIFY2(result.ok,qPrintable(result.detail));
    }
    const auto pixels=ReadNpy(QDir(reference).filePath("chalk-kinetic-normalized-input.npy"),"<f4",768*768*4);
    std::vector<float> tensor(768*768);std::memcpy(tensor.data(),pixels.constData(),pixels.size());
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    QCOMPARE(prepared.engine->Recognize(tensor,cancel,end,1).code,QString("formula_token_limit"));
    cancel=true;QVERIFY(prepared.engine->Recognize(tensor,cancel,end).cancelled);cancel=false;
    QCOMPARE(prepared.engine->Recognize(tensor,cancel,std::chrono::steady_clock::now()-std::chrono::seconds(1)).code,QString("formula_deadline"));
    const auto complex=ReadNpy(QDir(reference).filePath("official-complex-input.npy"),"<f4",768*768*4);
    std::vector<float> complexTensor(768*768);std::memcpy(complexTensor.data(),complex.constData(),complex.size());
    {
        std::jthread stop([&]{std::this_thread::sleep_for(std::chrono::milliseconds(10));cancel=true;});
        const auto interrupted=prepared.engine->Recognize(complexTensor,cancel,end);
        QVERIFY(interrupted.cancelled);
    }
    cancel=false;
    const auto resumed=prepared.engine->Recognize(tensor,cancel,end);QVERIFY2(resumed.ok,qPrintable(resumed.detail));
    receipt["tokenBoundAndCancellationKeepEngineUsable"]=true;receipt["completed"]=true;save();
#else
    QSKIP("Native TensorRT CUDA build required.");
#endif
}
}
