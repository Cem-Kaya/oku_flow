// Included only by media_gpu_scene_tests.cpp, after its fixture helpers.
#include "okuflow/d3d12/presenter.hpp"
#include <deque>
#include <numeric>

namespace okuflow {
struct VideoGpuPresenterFixture {
    static void Initialize(D3D12Presenter& p, ID3D12Device* device) {
        p.device_ = device;
        p.CreateCommandObjects(); p.CreateFenceObjects(); p.CreateViewportPipeline();
        p.initialized_ = true;
    }
};
}
namespace {
void AwaitProducer(okuflow::MediaGpuScene& scene) {
    const auto sync = scene.PresentationSync();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (sync.presentationFence->GetCompletedValue() < sync.waitValue) {
        Require(std::chrono::steady_clock::now() < end, "alternate producer did not finish independently of graphics");
        Sleep(1);
    }
    Require(sync.presentationFence->GetCompletedValue() != UINT64_MAX, "alternate producer device removed");
}
void DoubleOutputTests(Harness& harness) {
    okuflow::MediaGpuScene scene(2);
    PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
    auto first = harness.MakeFrame(kWidth, kHeight, 80, 8000);
    okuflow::ProcessingSettings settings;
    settings.enableTemporalSmoothing = true; settings.temporalSmoothingAlpha = .5f;
    ProcessUntilReady(scene, first, 0, settings, 800, 1, false);
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth,kHeight,80),kWidth,0,"double output first pixels");
    const auto resets = scene.HistoryResetCount();
    const ComPtr<ID3D12Resource> firstOutput = scene.Texture();
    const ComPtr<ID3D12Fence> firstFence = scene.PresentationSync().presentationFence;
    auto firstGate = harness.BlockConsumer(scene);
    struct Gates {
        std::vector<ComPtr<ID3D12Fence>> fences;
        ~Gates(){ for(auto& fence:fences) fence->Signal(1); }
    } gates{{firstGate}};
    auto second = harness.MakeFrame(kWidth,kHeight,81,8001);
    ProcessUntilReady(scene, second, 0, settings,800,1,false);
    Require(scene.Texture()!=firstOutput.Get() && scene.PresentationSync().presentationFence!=firstFence.Get(),
            "second output did not have independent texture/fence ownership");
    AwaitProducer(scene); // must complete while the older graphics gate is still closed
    Require(scene.HistoryResetCount()==resets,"alternating output reset shared temporal history");
    Hr(firstGate->Signal(1),"first output graphics release failed");
    ComparePixels(harness.ReadScene(scene,false,false,firstOutput.Get()).pixels,
                  Pattern(kWidth,kHeight,80),kWidth,0,"older output overwritten by newer processing");
    auto expected=Pattern(kWidth,kHeight,81), prior=Pattern(kWidth,kHeight,80);
    for(size_t i=0;i<expected.size();++i) {
        expected[i].b=std::uint8_t(.5f*expected[i].b+.5f*prior[i].b+.5f);
        expected[i].g=std::uint8_t(.5f*expected[i].g+.5f*prior[i].g+.5f);
        expected[i].r=std::uint8_t(.5f*expected[i].r+.5f*prior[i].r+.5f);
    }
    ComparePixels(harness.ReadScene(scene).pixels,expected,kWidth,0,"shared two-output temporal history");
    auto secondGate=harness.BlockConsumer(scene);gates.fences.push_back(secondGate);
    auto third=harness.MakeFrame(kWidth,kHeight,82,8002);
    ProcessUntilReady(scene,third,0,settings,800,1,false);
    AwaitProducer(scene);
    auto thirdGate=harness.BlockConsumer(scene);gates.fences.push_back(thirdGate);
    const auto generation=scene.SceneGeneration(), before=scene.HistoryResetCount();
    auto obsolete=harness.MakeFrame(kWidth,kHeight,83,8003);
    obsolete.requestGeneration=90;
    Require(scene.Process(obsolete,0,settings,900,2,false)==Status::Pending &&
            scene.SceneGeneration()==generation && scene.HistoryResetCount()==before,
            "full two-output pool admitted obsolete seek/history");
    auto latest=harness.MakeFrame(kWidth,kHeight,84,8004);
    latest.requestGeneration=91;
    Require(scene.Process(latest,0,settings,901,3,false)==Status::Pending &&
            scene.SceneGeneration()==generation && scene.HistoryResetCount()==before,
            "waiting seek changed output or processing history");
    Hr(secondGate->Signal(1),"second output release failed");
    ProcessUntilReady(scene,latest,0,settings,901,3,false);
    AwaitProducer(scene);
    Require(scene.SceneEpoch()==901 && scene.SceneFrameSequence()==8004 && scene.HistoryResetCount()==before+1,
            "reopened output did not process only the newest seek");
    Hr(thirdGate->Signal(1),"third output release failed");
    ComparePixels(harness.ReadScene(scene).pixels,Pattern(kWidth,kHeight,84),kWidth,0,"newest seek did not reset history");
    // Same-frame replay remains exact after output switches and seek reset.
    const auto seekResets=scene.HistoryResetCount();
    settings.contrast=1.3f;
    ProcessUntilReady(scene,latest,0,settings,901,4,false);
    Require(scene.HistoryResetCount()==seekResets,"two-output view replay reset history");
    harness.ReadScene(scene);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(true) {
        const auto status=scene.Release();
        Require(status!=Status::Failed,"two-output scene retirement failed");
        if(status==Status::Ready)break;
        Require(std::chrono::steady_clock::now()<deadline,"two-output scene did not retire");
        Sleep(1);
    }
}

double Percentile(std::vector<double> values,double q) {
    Require(!values.empty(),"benchmark has no latency samples");
    std::sort(values.begin(),values.end());
    return values[std::min(values.size()-1,size_t(std::ceil(q*values.size())-1))];
}

// Explicit one-time benchmark, not a recurring test or screen-latency claim.
// Native decode is excluded: the same four immutable GPU inputs are reused.
// Production viewport shaders render into owned offscreen GPU snapshot targets;
// completion is observed through the real graphics fence, never API submission.
void BufferBenchmark(Harness& harness) {
    using Clock=std::chrono::steady_clock;
    struct Pending {okuflow::GpuVideoFrame frame;Clock::time_point requested;int phase;};
    std::puts("BUFFER_CSV,width,height,profile,repeat,buffers,steady_fps,frame_p50_ms,frame_p95_ms,seek_p50_ms,seek_p95_ms,pan_p50_ms,pan_p95_ms,api_p95_us,extra_output_bytes");
    for (const auto dimensions : {std::pair<UINT,UINT>{1920,1080},{3840,2160}}) {
        const auto [width,height]=dimensions;
        std::vector<okuflow::VideoPlaybackFrame> sources;
        for(unsigned i=0;i<4;++i)sources.push_back(harness.MakeFrame(width,height,100+i,1+i));
        for(int profile=0;profile<2;++profile)for(int repeat=0;repeat<4;++repeat) {
            for(unsigned order=0;order<2;++order) {
                const unsigned buffers=(repeat==0 || repeat==3)?order+1:2-order;
                {
                okuflow::MediaGpuScene scene(buffers);
                PrepareUntilReady(scene,harness.Device(),width,height);
                okuflow::D3D12Presenter presenter;
                okuflow::VideoGpuPresenterFixture::Initialize(presenter,harness.Device());
                std::deque<Pending> pending;
                std::array<std::vector<double>,4> latencies;
                std::vector<double> api;
                std::uint64_t epoch=1000,request=1,sequence=1;
                auto view=okuflow::ComputeViewTransform(width,height,width,height,1,.5f,.5f,okuflow::ViewportFitMode::kFit);
                auto poll=[&] {
                    const auto completed=presenter.GetFence()->GetCompletedValue();
                    Require(completed!=UINT64_MAX,"benchmark graphics device removed");
                    while(!pending.empty() && completed>=pending.front().frame.readyFenceValue) {
                        const auto item=std::move(pending.front());pending.pop_front();
                        latencies[item.phase].push_back(std::chrono::duration<double,std::milli>(Clock::now()-item.requested).count());
                    }
                };
                auto run=[&](int phase,unsigned frames,double minimumSeconds=0) {
                    const auto phaseStart=Clock::now();
                    for(unsigned i=0;i<frames || std::chrono::duration<double>(Clock::now()-phaseStart).count()<minimumSeconds;++i) {
                        auto frame=sources[i%sources.size()];
                        if(phase==2) {epoch+=5;request+=5;sequence+=5;} // four older waiting requests discarded
                        else ++sequence;
                        frame.sampleId=sequence;frame.pts100ns=std::int64_t(sequence)*333333;
                        frame.requestGeneration=request;
                        if(phase==3)view=okuflow::ComputeViewTransform(width,height,width,height,4,
                            .15f+.7f*float(i%17)/16,.5f,okuflow::ViewportFitMode::kFit);
                        else view=okuflow::ComputeViewTransform(width,height,width,height,1,.5f,.5f,okuflow::ViewportFitMode::kFit);
                        const auto requested=Clock::now();
                        const auto deadline=requested+std::chrono::seconds(5);
                        bool ready=false;
                        while(true) {
                            poll();
                            Require(Clock::now()<deadline,"buffer benchmark frame deadline exceeded");
                            const auto callStart=Clock::now();
                            if(!ready) {
                                okuflow::ProcessingSettings settings;
                                if(profile) {
                                    settings.enableAutoTextClarity=true;settings.enableSmartSharpen=true;
                                    settings.enableAutoContrast=true;settings.enableTemporalSmoothing=true;
                                    settings.temporalSmoothingAlpha=.35f;
                                }
                                const auto status=scene.Process(frame,0,settings,epoch,1,false);
                                Require(status!=Status::Failed,scene.Error().c_str());
                                ready=status==Status::Ready;
                            }
                            bool submitted=false;
                            if(ready) {
                                const auto sync=scene.PresentationSync();
                                const okuflow::RecordingViewTransform transform{view.sourceX,view.sourceY,view.sourceWidth,view.sourceHeight,
                                    view.destinationX,view.destinationY,view.destinationWidth,view.destinationHeight,view.valid};
                                UINT64 readSignal{};
                                auto output=presenter.RequestRecordingFrame(scene.Texture(),width,height,transform,width,height,
                                    nullptr,0,nullptr,0,&sync,&readSignal);
                                if(readSignal)scene.RecordPresented(readSignal);
                                if(output.IsValid()) {
                                    Require(scene.SceneEpoch()==epoch && scene.SceneFrameSequence()==sequence,"benchmark rendered obsolete target");
                                    pending.push_back({std::move(output),requested,phase});submitted=true;
                                }
                                Require(!presenter.IsFaulted(),"benchmark viewport shader submission faulted");
                            }
                            api.push_back(std::chrono::duration<double,std::micro>(Clock::now()-callStart).count());
                            if(submitted)break;
                            std::this_thread::yield();
                        }
                    }
                    while(!pending.empty()) {
                        poll(); Require(Clock::now()-phaseStart<std::chrono::seconds(45),"benchmark phase failed to drain");
                        std::this_thread::yield();
                    }
                    return std::chrono::duration<double>(Clock::now()-phaseStart).count();
                };
                run(0,32,.25); api.clear();
                const auto elapsed=run(1,240,2);
                run(2,120);run(3,120);
                Require(presenter.WaitForIdle(),"benchmark final graphics drain failed");
                const auto cacheWidth=std::min(width,2560u),cacheHeight=std::min(height,1440u);
                const auto extra=buffers==2?(std::uint64_t(width)*height+std::uint64_t(cacheWidth)*cacheHeight)*4:0;
                std::printf("BUFFER_CSV,%u,%u,%s,%d,%u,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%llu\n",
                    width,height,profile?"text-effects":"neutral",repeat,buffers,latencies[1].size()/elapsed,
                    Percentile(latencies[1],.5),Percentile(latencies[1],.95),
                    Percentile(latencies[2],.5),Percentile(latencies[2],.95),
                    Percentile(latencies[3],.5),Percentile(latencies[3],.95),Percentile(api,.95),
                    static_cast<unsigned long long>(extra));
                std::fflush(stdout);
                const auto retireDeadline=Clock::now()+std::chrono::seconds(5);
                while(true) {
                    const auto status=scene.Release();
                    Require(status!=Status::Failed,"benchmark scene retirement could not prove completion");
                    if(status==Status::Ready)break;
                    Require(Clock::now()<retireDeadline,"benchmark scene retirement deadline exceeded");
                    Sleep(1);
                }
                }
                // Shader/runtime thread pools may stay alive. Release above
                // waits only the exact scene closure that can still call CUDA.
            }
        }
    }
}
}
