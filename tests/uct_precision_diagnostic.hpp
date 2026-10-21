#pragma once

// Unregistered, explicit offline experiment. Reuses production SubmitDevice,
// metering, profile validation and exact State retirement from the resident
// diagnostic. It does not change the existing strict N1/N8 comparisons.
#include "uct_resident_diagnostic.hpp"
#include <QProcess>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace okuflow::uct {
// Read only after the serial baseline worker has retired. Export CPU reference
// tensors from original CPU source pixels and compact quads, never GPU images.
struct TensorRtPrecisionCalibrationAccess {
    static const std::vector<TensorRtCropDiagnostic::Plan>& Plans(const TensorRtCropDiagnostic& value) { return value.plans_; }
};
}

namespace uct_precision_test {
using namespace uct_resident_test;

inline QJsonObject Activity()
{
    QJsonObject record{{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"contended",qEnvironmentVariable("OKUFLOW_UCT_PRECISION_CONTENDED")=="1"}};
    const auto executable=qEnvironmentVariable("OKUFLOW_UCT_PRECISION_NVIDIA_SMI");
    if (!QDir::isAbsolutePath(executable)) return record;
    const auto query=[&](const QString& key,const QStringList& args) {
        QProcess process;
#if defined(_WIN32)
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* value){value->flags|=CREATE_NO_WINDOW;});
#endif
        process.start(executable,args);
        const bool started=process.waitForStarted();
        // Read-only owned helper; wait for normal exit, never terminate it.
        const bool ended=started && process.waitForFinished(-1);
        record[key]=QJsonObject{{"started",started},{"ended",ended},{"exitCode",process.exitCode()},
            {"stdout",QString::fromUtf8(process.readAllStandardOutput()).left(32768)},
            {"stderr",QString::fromUtf8(process.readAllStandardError()).left(4096)}};
    };
    query("gpu",{"--query-gpu=index,name,uuid,utilization.gpu,utilization.memory,memory.used,memory.total,power.draw","--format=csv,noheader,nounits"});
    query("computeProcesses",{"--query-compute-apps=pid,process_name,used_gpu_memory","--format=csv,noheader,nounits"});
    return record;
}

#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
inline void ExportCalibration(const Run& run, const QString& output)
{
    QVERIFY(run.cropDiagnostic);
    const auto& plans=TensorRtPrecisionCalibrationAccess::Plans(*run.cropDiagnostic);
    QVERIFY(plans.size()==run.sources.size());
    QJsonArray samples;
    const auto save=[&](const std::vector<float>& data,const QString& role,const Source& source,QJsonArray shape) {
        const auto path=QDir(output).filePath(QString("%1-%2.f32").arg(role).arg(samples.size()));
        const auto size=qsizetype(data.size()*sizeof(float));
        QFile file(path);
        if (data.empty() || !file.open(QIODevice::WriteOnly|QIODevice::NewOnly) ||
            file.write(reinterpret_cast<const char*>(data.data()),size)!=size) return false;
        file.close();
        samples.push_back(QJsonObject{{"role",role},{"path",path},{"sha256",FileSha256(path)},
            {"shape",shape},{"dtype","little-endian-float32"},{"source",source.path},{"sourceSha256",source.fileSha256},
            {"preprocessing","production CPU PaddleNormalize/PaddleRectify reference; same compact pinned-detector quads"}});
        return true;
    };
    for (std::size_t i=0;i<plans.size();++i) {
        const auto& source=run.sources[i];
        int detections=0,recognitions=0;
        for (const auto& region:plans[i].regions) {
            const auto pixels=source.image.copy(region.bounds);
            if (detections++<2) {
                const auto size=PaddleDetectionSize(pixels.size());
                QVERIFY(save(PaddleNormalize(pixels,size,true),"det",source,{1,3,size.height(),size.width()}));
            }
            const auto step=std::max<std::size_t>(1,region.crops.size()/4);
            for (std::size_t n=0;n<region.crops.size() && recognitions<4;n+=step,++recognitions) {
                const auto& crop=region.crops[n]; PaddleQuad quad;
                for (int p=0;p<4;++p) quad.points[p]={crop.xy[2*p],crop.xy[2*p+1]};
                quad.score=crop.score;quad.internalEdges=crop.internalEdges;
                QVERIFY(save(PaddleRectify(pixels,quad,1024,crop.validWidth),"rec",source,{1,3,48,1024}));
            }
        }
    }
    QVERIFY(uct_batch_test::Save(QDir(output).filePath("calibration-inputs.json"),
        {{"schema",1},{"samples",samples},{"models",run.pairs.front().models},
         {"notAccuracy","These source frames must be disjoint from evaluation; inspect their recording/PTS provenance."}}));
}
#endif

inline void Diagnostic()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_PRECISION_DIAGNOSTIC") != "1")
        QSKIP("Explicit precision diagnostic not selected.");
#if !(OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT)
    QFAIL("Precision experiment requires the compiled CUDA/TensorRT adapter.");
#else
    try {
        const auto paths = qEnvironmentVariable("OKUFLOW_UCT_PRECISION_MANIFESTS").split(';',Qt::SkipEmptyParts);
        QVERIFY2(!paths.isEmpty() && paths.size() <= 5, "Select one to five explicit manifests.");
        std::vector<std::shared_ptr<TensorRtPrecisionExperiment>> experiments;
        for (const auto& path : paths)
            experiments.push_back(std::make_shared<TensorRtPrecisionExperiment>(LoadPrecisionExperiment(path)));
        const bool prepare = experiments.front()->prepare;
        QVERIFY2(!prepare || experiments.size() == 1, "Preparation selects exactly one model pair.");
        for (const auto& e : experiments) QVERIFY(e->prepare == prepare);
        const auto sdk = qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT");
        const auto output = qEnvironmentVariable("OKUFLOW_UCT_PRECISION_OUTPUT");
        QVERIFY(QDir::isAbsolutePath(output) && !QFile::exists(output));
        QVERIFY(QDir().mkpath(output));
        const QDir bin(QDir(sdk).filePath("bin"));
        QVERIFY(QFile::exists(bin.filePath("nvinfer_11.dll")));
        if (!prepare) {
            QVERIFY2(!QFile::exists(bin.filePath("nvonnxparser_11.dll")) &&
                     bin.entryList({"nvinfer_builder_resource*.dll"},QDir::Files).isEmpty(),
                     "Cached measurement requires the inference-only SDK.");
        }
        uct_batch_test::Environment batch("OKUFLOW_UCT_TRT_BATCH","8");
        uct_batch_test::Environment width("OKUFLOW_UCT_TRT_WIDTH","fixed1024");
        uct_batch_test::Environment noWidthPreparation("OKUFLOW_UCT_TRT_PREPARE_WIDTH","0");
        const auto assets = LoadPaddleAssets(DefaultPaddleRoot(),ModelTier::Medium,ModelTier::Medium);
        QVERIFY2(assets.IsValid(),qPrintable(assets.error));
        int devices{}; QVERIFY(cudaGetDeviceCount(&devices)==cudaSuccess && devices>0);
        QVERIFY(cudaSetDevice(0)==cudaSuccess);
        cudaDeviceProp gpu{}; QVERIFY(cudaGetDeviceProperties(&gpu,0)==cudaSuccess);
        int driver{},runtime{}; QVERIFY(cudaDriverGetVersion(&driver)==cudaSuccess);
        QVERIFY(cudaRuntimeGetVersion(&runtime)==cudaSuccess);
        const QByteArray uuid(reinterpret_cast<const char*>(&gpu.uuid),sizeof(gpu.uuid));
        const auto baselineCache = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-engines");
        const auto baselineBefore = CacheInventory(baselineCache);
        QVERIFY(uct_batch_test::Save(QDir(output).filePath("baseline-before.json"),{{"files",baselineBefore}}));
        QJsonArray graphManifests;
        for (std::size_t i=0;i<experiments.size();++i) {
            auto& e=*experiments[i];
            e.receiptRoot=QDir(output).filePath(QString("engines/%1").arg(i));
            for (const bool detection : {true,false}) {
                const auto& graph=detection?e.detector:e.recognizer;
                const auto source=detection?assets.detectorSha256:assets.recognizerSha256;
                QVERIFY(graph.sourceSha256==source);
                QVERIFY(FileSha256(graph.path).toLatin1()==graph.sha256);
                const bool converted=graph.precision!="fp32";
                const auto key=converted?QString::fromLatin1(PrecisionPlanKey(graph,uuid,driver,detection,detection?1:8))
                                        :PlanKey(source,uuid,driver,detection,detection?1:8);
                const auto cache=converted?QDir(e.artifactRoot).filePath("engines/"+graph.precision):baselineCache;
                const auto plan=InspectPlan(cache,detection?"detector":"recognizer",ModelTier::Medium,
                                            detection?1:8,key);
                if (!prepare || !converted) QVERIFY2(plan.valid,qPrintable("Missing/invalid cache: "+plan.path));
                graphManifests.push_back(QJsonObject{{"configuration",int(i)},{"role",detection?"detector":"recognizer"},
                    {"precision",graph.precision},{"graph",graph.path},{"graphSha256",QString::fromLatin1(graph.sha256)},
                    {"sourceSha256",QString::fromLatin1(source)},{"planPreflight",plan.Json()}});
            }
        }
        QVERIFY(uct_batch_test::Save(QDir(output).filePath("manifest.json"),
            {{"graphs",graphManifests},{"prepare",prepare},{"sdk",sdk},
             {"gpu",QJsonObject{{"name",QString::fromUtf8(gpu.name)},{"computeMajor",gpu.major},{"computeMinor",gpu.minor},
                 {"uuid",Hex(uuid)},{"memoryBytes",qint64(gpu.totalGlobalMem)},{"cudaDriver",driver},{"cudaRuntime",runtime}}},
             {"executableSha256",FileSha256(QCoreApplication::applicationFilePath())},
             {"note","Baseline agreement is not ground-truth accuracy. No display/FPS/other-GPU claim."}}));

        if (prepare) {
            QVERIFY(uct_batch_test::Save(QDir(output).filePath("activity-before.json"),Activity()));
            uct_batch_test::RetirementReceipts retirement;
            auto recognizer=CreateTensorRtPrecisionExperiment(assets.root,sdk,experiments.front(),retirement.Observer());
            const auto status=recognizer->Initialize();
            QVERIFY2(status.state==BackendState::kReady,qPrintable(status.detail));
            const auto deadline=Clock::now()+std::chrono::minutes(15);
            while (recognizer->Preparing() && Clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto final=recognizer->Status();
            const bool ready=!recognizer->Preparing() && final.state==BackendState::kReady;
            recognizer.reset();
            const auto receipt=retirement.Wait();
            QVERIFY(uct_batch_test::Save(QDir(output).filePath("preparation.json"),
                {{"ready",ready},{"detail",final.detail},{"retirement",receipt.Json()},
                 {"inferenceCalls",0},{"baselineUnchanged",CacheInventory(baselineCache)==baselineBefore}}));
            QVERIFY2(ready,qPrintable(final.detail)); QVERIFY(receipt.Passed());
            QVERIFY(uct_batch_test::Save(QDir(output).filePath("activity-after.json"),Activity()));
        } else {
            // Borrow the resident selector's finite bounds, then choose N8
            // explicitly here. Existing N1-required parsing/strict gates remain.
            auto selection=ParseSelection(&ProcessEnvironment);
            QVERIFY2(selection.error.isEmpty(),qPrintable(selection.error));
            QVERIFY(selection.pairs.size()==1 && selection.pairs.front()==Pair{});
            QVERIFY(selection.rounds==1 && selection.passes<=3 && selection.widthPolicy=="fixed1024");
            selection.capacities={8};
            selection.media=true;
            QVERIFY(!selection.shadowPlanner);
            Run run; run.selection=selection; run.started=Clock::now();
            PairAssets pair; pair.assets=assets; pair.models=ModelsJson(Pair{},assets); run.pairs.push_back(pair);
            for (const auto& path : selection.images) {
                Source source; source.path=path; source.image=QImage(path).convertToFormat(QImage::Format_ARGB32);
                QVERIFY(!source.image.isNull() && source.image.width()<=8192 && source.image.height()<=8192 &&
                        source.image.sizeInBytes()<=128ll*1024*1024);
                source.rowSha256=uct_test::SourcePixelsDigest(source.image); source.treeSha256=TreeDigest(source.image);
                source.fileSha256=FileSha256(path); run.sources.push_back(std::move(source));
            }
            run.pool=std::make_unique<AnalysisPool>(); run.uploads.stream=run.stream.value;
            if (selection.frozenCrops) {
                QVERIFY(experiments.front()->detector.precision=="fp32" && experiments.front()->recognizer.precision=="fp32");
                run.cropDiagnostic=std::make_shared<TensorRtCropDiagnostic>(); run.frozenInputs.resize(run.sources.size());
                for (std::size_t i=0;i<run.sources.size();++i) run.frozenPools.push_back(std::make_unique<AnalysisPool>());
            }
            QJsonArray sources;
            for (auto& source : run.sources) {
                sources.push_back(source.Json());
                const auto bytes=std::size_t(source.image.sizeInBytes()); source.device=run.uploads.Allocate(bytes);
                QVERIFY(source.device);
                QVERIFY(cudaMemcpyAsync(source.device,source.image.constBits(),bytes,cudaMemcpyHostToDevice,run.stream.value)==cudaSuccess);
            }
            QVERIFY(cudaStreamSynchronize(run.stream.value)==cudaSuccess);
            QVERIFY(uct_batch_test::Save(QDir(output).filePath("sources.json"),{{"sources",sources},{"selection",selection.Json()}}));
            uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE","1"); ProfileCapture capture;
            QJsonArray configurations;
            for (std::size_t i=0;i<experiments.size();++i) {
                run.precisionExperiment=experiments[i];
                run.evidence=QDir(output).filePath(QString("configuration-%1").arg(i));
                QVERIFY(QDir().mkpath(QDir(run.evidence).filePath("jobs")) && QDir().mkpath(QDir(run.evidence).filePath("blocks")));
                const auto cache=QDir(experiments[i]->artifactRoot).filePath("engines");
                QJsonObject inventoryBefore, inventoryAfter;
                for (const auto& precision : QDir(cache).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
                    inventoryBefore[precision]=CacheInventory(QDir(cache).filePath(precision));
                const auto memory=MemoryJson();
                QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("activity-before.json"),Activity()));
                auto block=PlanBlocks(selection).front(); block.index=int(i);
                RunBlock(run,block,capture);
                if (QTest::currentTestFailed()) return;
                QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("activity-after.json"),Activity()));
                for (const auto& precision : QDir(cache).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
                    inventoryAfter[precision]=CacheInventory(QDir(cache).filePath(precision));
                QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("cache.json"),
                    {{"before",inventoryBefore},{"after",inventoryAfter},{"unchanged",inventoryBefore==inventoryAfter}}));
                QVERIFY(inventoryBefore==inventoryAfter);
                configurations.push_back(QJsonObject{{"index",int(i)},{"detector",experiments[i]->detector.precision},
                    {"recognizer",experiments[i]->recognizer.precision},{"memoryBeforeBlock",memory},{"memoryAfterRetirement",MemoryJson()}});
            }
            if (qEnvironmentVariable("OKUFLOW_UCT_PRECISION_CALIBRATION_EXPORT")=="1") {
                QVERIFY(experiments.size()==1 && selection.frozenCrops &&
                        experiments.front()->detector.precision=="fp32" && experiments.front()->recognizer.precision=="fp32");
                ExportCalibration(run,output);
                if (QTest::currentTestFailed()) return;
            }
            QVERIFY(uct_batch_test::Save(QDir(output).filePath("summary.json"),
                {{"complete",true},{"configurations",configurations},{"blocks",run.blocks},
                 {"frozenReplayCalls",run.cropDiagnostic?qint64(run.cropDiagnostic->ReplayCalls()):0},
                 {"caveats",Caveats()},{"strictN1N8","Not run by this N8-only experiment; original diagnostic is unchanged."}}));
        }
        const auto baselineAfter=CacheInventory(baselineCache);
        QVERIFY(uct_batch_test::Save(QDir(output).filePath("baseline-after.json"),{{"files",baselineAfter}}));
        QVERIFY(baselineAfter==baselineBefore);
    } catch (const std::exception& error) { QFAIL(error.what()); }
#endif
}
} // namespace uct_precision_test
