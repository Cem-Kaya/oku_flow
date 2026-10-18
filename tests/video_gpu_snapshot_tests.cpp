#ifdef _WIN32
#include "okuflow/d3d12/presenter.hpp"
#include <QCoreApplication>
#include <dxgi1_6.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
void Require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void Hr(HRESULT hr,const char* message) {Require(SUCCEEDED(hr),message);}
}
namespace okuflow {
// Exercises production snapshot commands, shaders, fences and readback ring.
// No HWND, visible composition target, camera or user settings is created.
struct VideoGpuPresenterFixture {
    static void Initialize(D3D12Presenter& p,ID3D12Device* device) {
        p.device_=device;p.CreateCommandObjects();p.CreateFenceObjects();p.CreateViewportPipeline();p.initialized_=true;
    }
    static ComPtr<ID3D12Resource> Source(D3D12Presenter& p,UINT extent,std::uint32_t color) {
        Require(p.WaitForIdle(),"private GPU queue could not drain");
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=extent;desc.Height=extent;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> source;
        Hr(p.device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&source)),"source allocation failed");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};p.device_->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> upload;
        Hr(p.device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"source upload allocation failed");
        void* mapped{};Hr(upload->Map(0,nullptr,&mapped),"source upload map failed");
        for(UINT y=0;y<extent;++y)std::fill_n(reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch),extent,color);
        upload->Unmap(0,nullptr);
        Hr(p.readbackCommandAllocator_->Reset(),"private allocator reset failed");Hr(p.commandList_->Reset(p.readbackCommandAllocator_.Get(),nullptr),"private command reset failed");
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        p.commandList_->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Hr(p.commandList_->Close(),"private upload close failed");
        ID3D12CommandList* list=p.commandList_.Get();p.commandQueue_->ExecuteCommandLists(1,&list);
        Require(p.WaitForIdle(),"source upload did not retire");return source;
    }
    static void DropReadbacksAfterDrain(D3D12Presenter& p) {
        Require(p.WaitForIdle(),"resize-style drain failed");
        for(auto& slot:p.asyncReadbackSlots_) {slot.inFlight=false;slot.sourceLease.reset();}
    }
    static std::size_t RetainedSources(const D3D12Presenter& p) {return p.submittedSourceTextures_.size();}
    static void Composition(D3D12Presenter& p, IDXGIFactory6* factory) {
        // An unattached composition swap chain exercises the production draw
        // submission without creating or changing any desktop window.
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=64;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=p.kFrameCount;
        desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> swap;
        Hr(factory->CreateSwapChainForComposition(p.commandQueue_.Get(),&desc,nullptr,&swap),
           "private composition swap chain failed");
        Hr(swap.As(&p.swapChain_),"private composition interface failed");
        p.width_=p.height_=64;p.backBuffers_.resize(p.kFrameCount);p.AcquireBackBuffers();
    }
    static void BusyCurrentSlot(D3D12Presenter& p, bool busy) {
        p.frameFenceValues_[p.swapChain_->GetCurrentBackBufferIndex()]=busy?UINT64_MAX-1:0;
    }
};
}
namespace {
std::vector<std::uint8_t> Read(okuflow::D3D12Presenter& p,okuflow::GpuVideoFrame frame) {
    UINT64 id{};Require(p.RequestReadback(frame,&id),"owned snapshot readback was not admitted");
    Require(id && p.HasReadbackRequest(id),"readback receipt is missing");
    frame={}; // only the ring now retains the producer lease
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    std::vector<std::uint8_t> pixels;UINT width{},height{};UINT64 done{};
    while(std::chrono::steady_clock::now()<end) {
        if(p.TryGetCompletedReadback(pixels,width,height,&done))break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(done==id && width==64 && height==64 && pixels.size()==64*64*4,"owned snapshot readback returned another payload");
    Require(!p.HasReadbackRequest(id),"completed readback receipt remained active");return pixels;
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;DXGI_ADAPTER_DESC1 description{};
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
           FAILED(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))) ||
           FAILED(adapter->GetDesc1(&description)) || (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) ||
           FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
            std::cout<<"SKIP hardware D3D12 device unavailable\n";return 77;
        }
        okuflow::D3D12Presenter presenter;okuflow::VideoGpuPresenterFixture::Initialize(presenter,device.Get());
        auto red=okuflow::VideoGpuPresenterFixture::Source(presenter,64,0xffff0000u);
        ComPtr<ID3D12Fence> privateFence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&privateFence)),"private producer fence failed");
        const okuflow::RecordingViewTransform view{0,0,1,1,0,0,1,1,true};
        const okuflow::FenceSyncParams sync{true,700,701,privateFence.Get()};UINT64 readSignal{};
        const auto started=std::chrono::steady_clock::now();
        auto snapshot=presenter.RequestRecordingFrame(red.Get(),64,64,view,64,64,nullptr,0,nullptr,0,&sync,&readSignal);
        Require(snapshot.IsValid() && readSignal==701,"independent snapshot was not submitted with exact read signal");
        Require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(250),"snapshot waited for the producer on the CPU");
        Require(privateFence->GetCompletedValue()<700,"private wait was not actually outstanding");
        Hr(privateFence->Signal(700),"private producer completion failed");
        auto pixels=Read(presenter,snapshot);
        Require(privateFence->GetCompletedValue()>=701,"independent read-completion signal was not reached");
        Require(presenter.GetLastSignaledFenceValue()<700,"private signal polluted the global presenter timeline");
        for(std::size_t i=0;i<pixels.size();i+=4)Require(pixels[i]==0 && pixels[i+1]==0 && pixels[i+2]==255 && pixels[i+3]==255,"snapshot source color was corrupted");
        auto green=okuflow::VideoGpuPresenterFixture::Source(presenter,64,0xff00ff00u);
        (void)green;
        Require(Read(presenter,snapshot)==pixels,"later scene activity rewrote the clicked snapshot");
        UINT64 dropped{};Require(presenter.RequestReadback(snapshot,&dropped),"resize fixture readback rejected");
        okuflow::VideoGpuPresenterFixture::DropReadbacksAfterDrain(presenter);
        Require(!presenter.HasReadbackRequest(dropped),"resize-style drain kept dropped receipt");
        Require(Read(presenter,snapshot)==pixels,"retry after resize changed the owned snapshot");
        std::vector<std::uint32_t> overlay(64*64,0);
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x)overlay[y*64+x]=0xff0000ffu;
        const auto composite=presenter.RequestRecordingFrame(red.Get(),64,64,view,64,64,reinterpret_cast<const std::uint8_t*>(overlay.data()),64*4);
        const auto combined=Read(presenter,composite);
        Require(combined[0]==255 && combined[1]==0 && combined[2]==0,"snapshot omitted its exact overlay");
        const auto center=(32*64+32)*4;Require(combined[center+2]==255,"transparent overlay replaced source pixels");
        std::vector<ComPtr<ID3D12Resource>> sources;
        for(unsigned n=0;n<140;++n)sources.push_back(okuflow::VideoGpuPresenterFixture::Source(presenter,64,0xff000000u|n));
        for(const auto& source:sources) {
            const auto image=presenter.RequestRecordingFrame(source.Get(),64,64,view,64,64);
            Require(image.IsValid(),"completed source churn exhausted the presenter lifetime holds");
            (void)Read(presenter,image);
        }
        Require(okuflow::VideoGpuPresenterFixture::RetainedSources(presenter)<128,
            "completed historical source references accumulated until the failure limit");
        okuflow::VideoGpuPresenterFixture::Composition(presenter,factory.Get());
        Hr(privateFence->Signal(900),"private draw producer completion failed");
        const auto drawView=okuflow::ComputeViewTransform(64,64,64,64,1,0.5f,0.5f,okuflow::ViewportFitMode::kFit);
        const okuflow::FenceSyncParams drawSync{true,900,901,privateFence.Get()};
        okuflow::ViewportPresentationOptions drawOptions{};drawOptions.requestReadback=true;
        UINT64 drawReadback{},drawSignal{};
        Require(presenter.PresentSceneTexture(red.Get(),64,64,drawView,&drawSync,&drawOptions,&drawReadback,&drawSignal),
            "window-free production scene draw failed");
        Require(drawReadback && drawSignal==901,"scene draw lost its exact copy/private-fence receipt");
        std::vector<std::uint8_t> drawn;UINT drawnWidth{},drawnHeight{};UINT64 drawnId{};
        const auto drawDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(std::chrono::steady_clock::now()<drawDeadline) {
            if(presenter.TryGetCompletedReadback(drawn,drawnWidth,drawnHeight,&drawnId))break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(drawnId==drawReadback && drawnWidth==64 && drawnHeight==64 && drawn==pixels,
            "scene draw pixels do not match the exact submitted source");
        Require(privateFence->GetCompletedValue()>=901 && presenter.GetLastSignaledFenceValue()<900,
            "scene draw mixed independent and global fence timelines");
        const auto privateBefore=privateFence->GetCompletedValue();
        const auto globalBefore=presenter.GetLastSignaledFenceValue();
        const okuflow::FenceSyncParams busySync{true,1000,1001,privateFence.Get()};
        drawSignal=drawReadback=999;
        okuflow::VideoGpuPresenterFixture::BusyCurrentSlot(presenter,true);
        Require(!presenter.PresentSceneTexture(red.Get(),64,64,drawView,&busySync,&drawOptions,&drawReadback,&drawSignal),
            "busy draw unexpectedly submitted GPU work");
        Require(drawSignal==0 && drawReadback==0 && privateFence->GetCompletedValue()==privateBefore &&
            presenter.GetLastSignaledFenceValue()==globalBefore,"busy draw queued a future wait or published a false receipt");
        okuflow::VideoGpuPresenterFixture::BusyCurrentSlot(presenter,false);
        Require(presenter.WaitForIdle(),"busy draw left an unsignaled producer wait on the queue");
        std::cout<<"PASS independent GPU snapshot/draw, exact owned pixels, overlay, resize retry, busy admission and global timeline isolation\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
#endif
