#include "okuflow/common/maxine_async_producer.hpp"
#include "maxine_fake_sdk.hpp"
#include <QCoreApplication>
#include <cuda_runtime.h>
#include <QFileInfo>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using Producer = okuflow::MaxineAsyncProducer;
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void Cuda(cudaError_t value) { if (value != cudaSuccess) throw std::runtime_error(cudaGetErrorString(value)); }
struct Module {
    HMODULE handle{};
    MaxineFakeRead read{};
    MaxineFakeReset reset{};
    MaxineFakeBlockRun block{};
    explicit Module(const char* path) {
        handle = LoadLibraryExW(QFileInfo(QString::fromUtf8(path)).absoluteFilePath().toStdWString().c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(handle != nullptr, "private fake SDK load failed");
        read = reinterpret_cast<MaxineFakeRead>(GetProcAddress(handle,"OkuFlowFakeRead"));
        reset = reinterpret_cast<MaxineFakeReset>(GetProcAddress(handle,"OkuFlowFakeReset"));
        block = reinterpret_cast<MaxineFakeBlockRun>(GetProcAddress(handle,"OkuFlowFakeBlockRun"));
        Require(read && reset && block,"private SDK controls missing"); reset();
    }
    ~Module() { if(handle) FreeLibrary(handle); }
    MaxineFakeSnapshot Stats() const { MaxineFakeSnapshot s; read(&s); return s; }
};
struct Gate {
    struct State {
        HANDLE entered{CreateEventW(nullptr,TRUE,FALSE,nullptr)}, release{CreateEventW(nullptr,TRUE,FALSE,nullptr)},
               returned{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    };
    std::unique_ptr<State> state{std::make_unique<State>()};
    HANDLE entered{state->entered}, release{state->release}, returned{state->returned};
    Gate() { Require(entered && release && returned,"gate allocation failed"); }
    ~Gate() {
        SetEvent(release);
        if (WaitForSingleObject(returned,2500) != WAIT_OBJECT_0) {
            (void)state.release(); return; // a late callback may not have entered yet
        }
        CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    }
    void Await() { Require(WaitForSingleObject(entered,2500) == WAIT_OBJECT_0,"private backend did not enter its gate"); }
    void Open() { SetEvent(release); }
    static void CUDART_CB Hold(void* pointer) {
        auto* gate = static_cast<State*>(pointer);
        SetEvent(gate->entered); WaitForSingleObject(gate->release,INFINITE); SetEvent(gate->returned);
    }
};
struct Fixture {
    cudaArray_t source{};
    cudaStream_t stream{};
    std::unique_ptr<Producer> producer;
    Producer::Config config{256,256,512,512,.65f};
    Fixture() {
        Cuda(cudaSetDevice(0)); Cuda(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        const auto desc = cudaCreateChannelDesc<uchar4>();
        Cuda(cudaMallocArray(&source,&desc,256,256)); producer = std::make_unique<Producer>(0);
    }
    ~Fixture() {
        producer.reset();
        // Fixture-owned source may disappear once its snapshot copy retires.
        // No private model/stream/output is released on this thread.
        if(cudaStreamSynchronize(stream) == cudaSuccess) { cudaFreeArray(source); cudaStreamDestroy(stream); }
    }
    void Fill(unsigned value) {
        std::vector<unsigned> pixels(256*256,value);
        Cuda(cudaMemcpy2DToArrayAsync(source,0,0,pixels.data(),256*4,256*4,256,cudaMemcpyHostToDevice,stream));
        Cuda(cudaStreamSynchronize(stream));
    }
    void Submit(const Producer::Ticket& ticket) {
        producer->Request(ticket,config);
        const auto deadline = GetTickCount64()+2500;
        bool submitted = false;
        while(!(submitted = producer->TrySubmit(source,0,0,stream,ticket)) && GetTickCount64()<deadline) {
            Require(!producer->IsFaulted(),producer->LastError().c_str()); Sleep(1);
        }
        Require(submitted,"private snapshot not admitted");
    }
    Producer::Output Take(const Producer::Ticket& ticket) {
        Producer::Output result;
        const auto deadline = GetTickCount64()+2500;
        while(!producer->TryTake(ticket,result) && GetTickCount64()<deadline) {
            Require(!producer->IsFaulted(),producer->LastError().c_str()); Sleep(1);
        }
        Require(result.success,result.error.empty()?"exact result not available":result.error.c_str()); return result;
    }
    unsigned First(const Producer::Output& output) {
        unsigned result{}; Cuda(cudaMemcpy(&result,output.pixels,sizeof(result),cudaMemcpyDeviceToHost)); return result;
    }
};
Producer::Ticket Ticket(unsigned serial) { return {41,100+serial,serial,71,serial}; }
void AwaitCleanup(const Module& image,const Module& effects) {
    const auto deadline=GetTickCount64()+2500;
    while((image.Stats().allocated!=image.Stats().deallocated || effects.Stats().created!=effects.Stats().destroyed) &&
          GetTickCount64()<deadline) Sleep(1);
    Require(image.Stats().allocated==image.Stats().deallocated && effects.Stats().created==effects.Stats().destroyed,
            "worker did not retire private model/storage after released gates");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        int count{}; auto result=cudaGetDeviceCount(&count);
        if(result==cudaErrorNoDevice || (result==cudaSuccess && !count)) return 77; Cuda(result);
        qputenv("OKUFLOW_MAXINE_PATH",QFileInfo(QString::fromUtf8(OKUFLOW_MAXINE_FAKE_IMAGE)).absolutePath().toUtf8());
        Module image(OKUFLOW_MAXINE_FAKE_IMAGE), effects(OKUFLOW_MAXINE_FAKE_EFFECTS);
        auto fill=reinterpret_cast<MaxineFakeFillOutput>(GetProcAddress(image.handle,"OkuFlowFakeFillOutput"));
        auto echo=reinterpret_cast<MaxineFakeEchoInput>(GetProcAddress(image.handle,"OkuFlowFakeEchoInput"));
        Require(fill && echo,"private pixel controls missing"); fill(true,0); echo(true);
        {
            Fixture f; f.Fill(0xff070707u); f.Submit(Ticket(1));
            f.Fill(0xff999999u); // mutate source only after its private snapshot was ordered
            auto output=f.Take(Ticket(1));
            Require(f.First(output)==0xff070707u && image.Stats().inputFirstPixel==0xff070707u,
                    "input snapshot aliased later scene storage");
            Require(f.producer->ReleaseOutput(f.stream),"consumer lease retirement failed");
            Require(f.producer->Stats().submitted==1,"one scene was submitted repeatedly");
        }
        AwaitCleanup(image,effects);
        {
            Fixture f; Gate sdk; effects.block(sdk.entered,sdk.release,sdk.returned);
            const auto runs=effects.Stats().ran;
            f.Fill(0xff111111u); f.Submit(Ticket(1)); sdk.Await();
            f.Fill(0xff222222u);
            for(unsigned n=2;n<=20;++n) f.producer->Request(Ticket(n),f.config);
            Require(!f.producer->TrySubmit(f.source,0,0,f.stream,Ticket(20)),"a second GPU job was queued");
            Require(effects.Stats().ran==runs+1,"inference overlap escaped one-slot admission");
            sdk.Open();
            Producer::Output stale;
            const auto deadline=GetTickCount64()+2500;
            while(f.producer->Stats().staleDiscarded==0 && GetTickCount64()<deadline) {
                Require(!f.producer->TryTake(Ticket(20),stale),"old-frame output was retargeted"); Sleep(1);
            }
            Require(f.producer->Stats().staleDiscarded==1,"obsolete result was not discarded exactly once");
            f.Submit(Ticket(20)); auto latest=f.Take(Ticket(20));
            Require(f.First(latest)==0xff222222u,"latest descriptor used stale input pixels");
            Require(f.producer->Stats().submitted==2 && f.producer->Stats().desiredOverwritten>=17,
                    "descriptor coalescing queued GPU work or lost replacement accounting");
            Require(f.producer->ReleaseOutput(f.stream),"latest lease retirement failed");
            effects.block(nullptr,nullptr,nullptr);
        }
        AwaitCleanup(image,effects);
        {
            Fixture f; Gate consumer; f.Fill(0xff333333u); f.Submit(Ticket(1)); (void)f.Take(Ticket(1));
            Cuda(cudaLaunchHostFunc(f.stream,&Gate::Hold,consumer.state.get())); consumer.Await();
            Require(f.producer->ReleaseOutput(f.stream),"consumer event could not be recorded");
            const auto loaded=effects.Stats().loaded;
            f.config.strength=.75f; f.producer->Request(Ticket(2),f.config);
            Require(!f.producer->TrySubmit(f.source,0,0,f.stream,Ticket(2)) && effects.Stats().loaded==loaded,
                    "leased private output was reused/reconfigured before consumer completion");
            consumer.Open(); f.Submit(Ticket(2)); (void)f.Take(Ticket(2));
            Require(f.producer->ReleaseOutput(f.stream),"second consumer event failed");
        }
        AwaitCleanup(image,effects);
        {
            Fixture f; Gate sdk; effects.block(sdk.entered,sdk.release,sdk.returned);
            f.Fill(0xff444444u); f.Submit(Ticket(1)); sdk.Await();
            const auto destroyed=effects.Stats().destroyed;
            f.producer->Invalidate(); f.producer->Request(Ticket(2),f.config);
            Require(!f.producer->TrySubmit(f.source,0,0,f.stream,Ticket(2)),"invalidation detached live slot ownership");
            f.producer.reset(); // must return before the uninterruptible backend is released
            Require(WaitForSingleObject(sdk.release,0)==WAIT_TIMEOUT && effects.Stats().destroyed==destroyed,
                    "caller teardown waited for or destroyed an active SDK operation");
            sdk.Open(); effects.block(nullptr,nullptr,nullptr);
        }
        AwaitCleanup(image,effects);
        {
            Fixture f; f.Fill(0xff555555u);
            for (unsigned field = 0; field < 5; ++field) {
                const auto actual = Ticket(30+field);
                auto obsolete = actual;
                switch (field) {
                case 0: ++obsolete.cameraEpoch; break;
                case 1: ++obsolete.frameSequence; break;
                case 2: ++obsolete.processingSerial; break;
                case 3: ++obsolete.settingsGeneration; break;
                case 4: ++obsolete.requestGeneration; break;
                }
                f.Submit(actual);
                const auto before = f.producer->Stats().staleDiscarded;
                Producer::Output rejected;
                const auto deadline = GetTickCount64()+2500;
                while(f.producer->Stats().staleDiscarded==before && GetTickCount64()<deadline) {
                    Require(!f.producer->TryTake(obsolete,rejected),"partial presentation ticket accepted a stale result");
                    Sleep(1);
                }
                Require(f.producer->Stats().staleDiscarded==before+1,"ticket mismatch did not retire its exact result");
                f.producer->Invalidate();
            }
        }
        AwaitCleanup(image,effects);
        {
            Fixture f; f.Fill(0xff666666u);
            auto fail = reinterpret_cast<MaxineFakeFailLoad>(GetProcAddress(effects.handle,"OkuFlowFakeFailLoad"));
            Require(fail != nullptr,"load failure control missing"); fail(-109);
            const auto failed = Ticket(70);
            f.producer->Request(failed,f.config);
            Producer::Output error;
            const auto deadline = GetTickCount64()+2500;
            bool taken = false;
            while(!(taken=f.producer->TryTake(failed,error)) && GetTickCount64()<deadline) {
                (void)f.producer->TrySubmit(f.source,0,0,f.stream,failed); Sleep(1);
            }
            Require(taken && !error.success && !error.error.empty() && !f.producer->IsFaulted(),
                    "ordinary model failure did not produce a recoverable exact receipt");
            Require(f.producer->ReleaseOutput(f.stream),"failed configuration receipt lease not retired");
            const auto failedLoads=effects.Stats().loaded;
            f.producer->Invalidate(false); // a settings edit only revokes; it cannot defeat the failure latch
            const auto stillFailed=Ticket(71);
            f.producer->Request(stillFailed,f.config);
            Producer::Output secondError;
            const auto retryDeadline=GetTickCount64()+2500;
            bool secondTaken=false;
            while(!(secondTaken=f.producer->TryTake(stillFailed,secondError)) && GetTickCount64()<retryDeadline) {
                (void)f.producer->TrySubmit(f.source,0,0,f.stream,stillFailed); Sleep(1);
            }
            Require(secondTaken && !secondError.success && effects.Stats().loaded==failedLoads,
                    "ordinary invalidation silently retried a latched model failure");
            Require(f.producer->ReleaseOutput(f.stream),"latched error lease not retired");
            f.producer->Invalidate(); // explicit retry must re-probe the same configuration off caller
            f.Submit(Ticket(72)); auto recovered = f.Take(Ticket(72));
            Require(f.First(recovered)==0xff666666u,"explicit retry did not recover the same private configuration");
            Require(f.producer->ReleaseOutput(f.stream),"recovered output lease not retired");
        }
        AwaitCleanup(image,effects);
        std::puts("PASS: independent producer snapshots, bounded admission/coalescing, stale rejection, consumer lease and revoked shutdown");
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
