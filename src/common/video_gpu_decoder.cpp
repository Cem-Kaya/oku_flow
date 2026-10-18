#ifdef _WIN32
#include "okuflow/common/video_gpu_decoder.hpp"
#include <d3d10.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace okuflow {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr,const char* message) {if(FAILED(hr))throw VideoGpuIncompatible(message);}
[[noreturn]] void Fail(const char* message) {throw VideoGpuIncompatible(message);}
struct TextureBudget {std::atomic<std::size_t> bytes{};std::atomic<unsigned> count{};};
// Global decoder-owned texture admission also counts retired resources whose
// driver release has not completed. A blocked release cannot grow new pools.
std::shared_ptr<TextureBudget> Budget() {static auto budget=std::make_shared<TextureBudget>();return budget;}
struct TextureStorage {
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<TextureBudget> budget;
    std::size_t bytes{};
    ~TextureStorage() {
        texture.Reset(); // only the release worker destroys this object
        budget->bytes.fetch_sub(bytes);budget->count.fetch_sub(1);
    }
};
class TextureRetirement {
public:
    static TextureRetirement& Instance() {static auto* owner=new TextureRetirement;return *owner;}
    void Retire(TextureStorage* storage) noexcept {
        // At most eight charged allocations can reach this queue. Link-free
        // native destruction stays on the one process-owned release worker.
        std::lock_guard lock(mutex_);
        for(auto& entry:queue_)if(!entry) {entry=storage;changed_.notify_one();return;}
        std::terminate(); // admission proves this fixed queue cannot overflow
    }
private:
    TextureRetirement() {
        std::thread([this]{
            const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            for(;;) {
                TextureStorage* storage{};
                {std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return std::any_of(std::begin(queue_),std::end(queue_),[](auto* p){return p;});});
                 for(auto& entry:queue_)if(entry) {storage=entry;entry=nullptr;break;}}
                delete storage;
            }
            if(SUCCEEDED(hr))CoUninitialize();
        }).detach();
    }
    std::mutex mutex_;std::condition_variable changed_;
    TextureStorage* queue_[VideoGpuDecoder::kTextureSlotLimit]{};
};
std::shared_ptr<TextureStorage> AllocateTexture(ID3D11Device* device,UINT width,UINT height,bool shared=true) {
    // Initialise the releaser before creating any native resources; creation
    // failure never leaves an allocation whose last release would run on UI.
    auto& retirement=TextureRetirement::Instance();
    const auto budget=Budget();const auto bytes=std::size_t(width)*height*4;
    auto count=budget->count.load();
    do {if(count>=VideoGpuDecoder::kTextureSlotLimit)throw VideoGpuBackpressure("GPU playback texture slots are retained by consumers.");}
    while(!budget->count.compare_exchange_weak(count,count+1));
    auto used=budget->bytes.load();
    do {if(bytes>VideoGpuDecoder::kTextureByteLimit || used>VideoGpuDecoder::kTextureByteLimit-bytes) {
            budget->count.fetch_sub(1);throw VideoGpuBackpressure("GPU playback texture credit is retained by consumers.");}}
    while(!budget->bytes.compare_exchange_weak(used,used+bytes));
    auto* owned=new(std::nothrow) TextureStorage;
    if(!owned) {budget->bytes.fetch_sub(bytes);budget->count.fetch_sub(1);throw std::bad_alloc();}
    owned->budget=budget;owned->bytes=bytes;
    std::shared_ptr<TextureStorage> storage{owned,[&retirement](TextureStorage* value){retirement.Retire(value);}};
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags=shared?D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE:0;
    Check(device->CreateTexture2D(&desc,nullptr,&storage->texture),"GPU playback could not allocate a shareable BGRA texture.");
    return storage;
}
bool IsRgb(DXGI_FORMAT f) {return f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8X8_UNORM ||
    f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R10G10B10A2_UNORM || f==DXGI_FORMAT_R16G16B16A16_UNORM || f==DXGI_FORMAT_R16G16B16A16_FLOAT;}
DXGI_COLOR_SPACE_TYPE ColorSpace(IMFMediaType* type,bool rgb) {
    const auto transfer=MFGetAttributeUINT32(type,MF_MT_TRANSFER_FUNCTION,MFVideoTransFunc_Unknown);
    const auto matrix=MFGetAttributeUINT32(type,MF_MT_YUV_MATRIX,MFVideoTransferMatrix_Unknown);
    const auto range=MFGetAttributeUINT32(type,MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_Unknown);
    const auto primaries=MFGetAttributeUINT32(type,MF_MT_VIDEO_PRIMARIES,MFVideoPrimaries_Unknown);
    const bool full=range==MFNominalRange_0_255;
    if(range!=MFNominalRange_Unknown && range!=MFNominalRange_0_255 && range!=MFNominalRange_16_235)
        Fail("GPU playback nominal range needs CPU compatibility conversion.");
    if(transfer==MFVideoTransFunc_2084) {
        if(rgb)return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if(full)Fail("Full-range PQ YUV requires a converter with an exact advertised color space.");
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if(transfer==MFVideoTransFunc_HLG) {
        if(rgb)Fail("HLG RGB conversion is not advertised by this playback processor.");
        return full?DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
    }
    if(transfer!=MFVideoTransFunc_Unknown && transfer!=MFVideoTransFunc_709 &&
       transfer!=MFVideoTransFunc_sRGB && transfer!=MFVideoTransFunc_22 && transfer!=MFVideoTransFunc_20)
        Fail("GPU playback transfer function requires the CPU compatibility converter.");
    if(rgb) {
        if(primaries==MFVideoPrimaries_BT2020) {
            if(!full && range!=MFNominalRange_Unknown)Fail("Studio-range BT.2020 RGB requires CPU compatibility conversion.");
            return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020;
        }
        return full || range==MFNominalRange_Unknown?DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709;
    }
    if(primaries==MFVideoPrimaries_BT2020 && matrix!=MFVideoTransferMatrix_BT2020_10 && matrix!=MFVideoTransferMatrix_BT2020_12)
        Fail("BT.2020 YUV requires an explicit matching matrix for GPU conversion.");
    if(matrix==MFVideoTransferMatrix_BT2020_10 || matrix==MFVideoTransferMatrix_BT2020_12)
        return full?DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    if(matrix==MFVideoTransferMatrix_BT601)return full?DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601:DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    if(matrix!=MFVideoTransferMatrix_Unknown && matrix!=MFVideoTransferMatrix_BT709)
        Fail("GPU playback YUV matrix requires the CPU compatibility converter.");
    UINT width{},height{};MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&width,&height);
    const bool standardDefinition=matrix==MFVideoTransferMatrix_Unknown && primaries!=MFVideoPrimaries_BT709 && height && height<=576;
    return standardDefinition?(full?DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601:DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601):
        (full?DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
}
}
struct VideoGpuDecoder::State {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video;ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoContext1> videoContext1;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFMediaType> type;VideoGpuFormat format;RECT aperture{};
    struct Slot {std::shared_ptr<TextureStorage> storage;std::weak_ptr<const VideoPlaybackGpuImage> lease;};
    std::vector<Slot> textures;
    std::shared_ptr<TextureStorage> privateTarget;
    bool forcePrivateConversion{};
    bool directOutputViewProven{};
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;ComPtr<ID3D11VideoProcessor> processor;
    DXGI_FORMAT processorFormat{DXGI_FORMAT_UNKNOWN};
    ComPtr<IMFSourceReader> attachedReader;
    // Exact closure a submitted conversion may still read or write. The
    // reader reference is copied from attachedReader at submission time, so
    // the producing reader outlives this sample however long completion takes.
    struct Pending {
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> dxgi;
        ComPtr<ID3D11Texture2D> source;
        ComPtr<ID3D11VideoProcessorInputView> input;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        std::shared_ptr<TextureStorage> outputStorage;
        std::shared_ptr<TextureStorage> privateStorage;
        ComPtr<ID3D11Query> query;
        bool queryEnded{};
    };
    std::unique_ptr<Pending> pending;
    bool unsafe{};
};
VideoGpuDecoder::VideoGpuDecoder(std::unique_ptr<State> state):state_(std::move(state)){}
VideoGpuDecoder::~VideoGpuDecoder()=default;
void VideoGpuDecoder::MarkUnsafe() noexcept {
    state_->unsafe=true;
    // An unsafe latch without in-flight work retains nothing. With a pending
    // conversion the self-reference quarantines the whole closure (and the
    // device/processor it was submitted to) until process exit.
    if(state_->pending && !quarantine_) {
        try {quarantine_=shared_from_this();} catch(...) {}
    }
}
void VideoGpuDecoder::ClearPending() noexcept {
    auto& pending=*state_->pending;
    // Deliberate order: views first (they reference both textures), then the
    // completed sample and its buffer chain, then output storage, and the
    // producing source reader last.
    pending.output.Reset();pending.input.Reset();
    pending.sample.Reset();pending.buffer.Reset();pending.dxgi.Reset();pending.source.Reset();
    pending.outputStorage.reset();pending.privateStorage.reset();
    pending.query.Reset();pending.reader.Reset();
    state_->pending.reset();
}
bool VideoGpuDecoder::PollPending() {
    auto& s=*state_;
    if(!s.pending)return true;
    if(s.unsafe || !s.pending->queryEnded) {
        // GetData must never be called on a query whose End was not reached
        // (failed Blt), and an unsafe converter is never polled again.
        MarkUnsafe();
        Fail("GPU playback converter is latched unsafe after an incomplete conversion; its sample and reader are retained until process exit.");
    }
    BOOL complete{};const auto hr=s.context->GetData(s.pending->query.Get(),&complete,sizeof(complete),0);
    if(hr==S_FALSE)return false;
    if(FAILED(hr)) {
        MarkUnsafe();
        Check(hr,"GPU playback conversion completion failed; its sample and reader are retained until process exit.");
    }
    if(!complete)return false;
    ClearPending();return true;
}
void VideoGpuDecoder::DrainPending(Clock::time_point deadline) {
    while(!PollPending()) {
        if(Clock::now()>=deadline) {
            MarkUnsafe();
            Fail("GPU playback conversion exceeded its cooperative deadline; its exact sample and reader are retained until process exit.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void VideoGpuDecoder::AttachSourceReader(IMFSourceReader* reader) {state_->attachedReader=reader;}
void VideoGpuDecoder::DetachSourceReader() noexcept {
    auto& s=*state_;
    try {if(s.pending)(void)PollPending();} catch(...) {MarkUnsafe();}
    if(!s.pending)s.attachedReader.Reset();
    else MarkUnsafe(); // pending closure keeps its own reader reference
}
bool VideoGpuDecoder::HasUnsafePending() const {return state_->unsafe && bool(state_->pending);}
std::shared_ptr<VideoGpuDecoder> VideoGpuDecoder::Create(LUID adapter) {
    auto state=std::make_unique<State>();ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> selected;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"GPU playback could not enumerate adapters.");
    Check(factory->EnumAdapterByLuid(adapter,IID_PPV_ARGS(&selected)),"GPU playback adapter does not match the presenter.");
    Check(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
        &state->device,nullptr,&state->context),"GPU playback could not create its decoder device.");
    return CreateFromDevice(state->device.Get());
}
std::shared_ptr<VideoGpuDecoder> VideoGpuDecoder::CreateFromDevice(ID3D11Device* device,bool forcePrivateConversion) {
    if(!device || !(device->GetCreationFlags()&D3D11_CREATE_DEVICE_VIDEO_SUPPORT))
        Fail("GPU playback requires a video-capable D3D11 device.");
    auto state=std::make_unique<State>();state->device=device;state->forcePrivateConversion=forcePrivateConversion;
    device->GetImmediateContext(&state->context);
    ComPtr<ID3D10Multithread> multi;
    Check(state->context.As(&multi),"GPU playback device lacks decoder multithread protection.");multi->SetMultithreadProtected(TRUE);
    Check(state->device.As(&state->video),"GPU playback video processing is unavailable.");
    Check(state->context.As(&state->videoContext),"GPU playback video context is unavailable.");
    state->context.As(&state->videoContext1);
    UINT token{};Check(MFCreateDXGIDeviceManager(&token,&state->manager),"GPU playback could not create its DXGI manager.");
    Check(state->manager->ResetDevice(state->device.Get(),token),"GPU playback could not attach its decoder device.");
    return std::shared_ptr<VideoGpuDecoder>(new VideoGpuDecoder(std::move(state)));
}
std::vector<GUID> VideoGpuDecoder::DecoderPreferredOutputs(IMFMediaType* codedType) {
    std::vector<GUID> result;
    if(!codedType || state_->unsafe)return result;
    GUID coded{};
    if(FAILED(codedType->GetGUID(MF_MT_SUBTYPE,&coded)))return result;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video,coded};
    IMFActivate** activates{};UINT32 count{};
    if(FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT|MFT_ENUM_FLAG_ASYNCMFT|MFT_ENUM_FLAG_HARDWARE|
        MFT_ENUM_FLAG_LOCALMFT|MFT_ENUM_FLAG_SORTANDFILTER,
        &input,nullptr,&activates,&count)))return result;
    // Merit order matches the source reader's decoder choice. Bound the
    // activations tried; the first decoder accepting the exact coded type
    // is the one whose preference order matters.
    for(UINT32 i=0;i<count && i<8 && result.empty();++i) {
        ComPtr<IMFTransform> transform;
        if(FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))))continue;
        ComPtr<IMFAttributes> attributes;
        if(SUCCEEDED(transform->GetAttributes(&attributes)) && attributes &&
           MFGetAttributeUINT32(attributes.Get(),MF_TRANSFORM_ASYNC,FALSE))
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,TRUE);
        transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(state_->manager.Get()));
        if(SUCCEEDED(transform->SetInputType(0,codedType,0))) {
            for(DWORD index=0;index<64;++index) {
                ComPtr<IMFMediaType> offered;
                if(FAILED(transform->GetOutputAvailableType(0,index,&offered)))break;
                GUID subtype{};
                if(SUCCEEDED(offered->GetGUID(MF_MT_SUBTYPE,&subtype)))result.push_back(subtype);
            }
        }
        transform.Reset();activates[i]->ShutdownObject();
    }
    for(UINT32 i=0;i<count;++i)activates[i]->Release();
    CoTaskMemFree(activates);
    return result;
}
void VideoGpuDecoder::ConfigureReader(IMFAttributes* attrs) {
    Check(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,state_->manager.Get()),"GPU playback could not attach the reader manager.");
    Check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"GPU playback could not enable hardware transforms.");
    Check(attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA,FALSE),"GPU playback could not enable DXVA.");
}
VideoGpuFormat VideoGpuDecoder::SetFormat(IMFMediaType* type) {
    auto& s=*state_;
    if(s.unsafe || s.pending) {
        MarkUnsafe();
        Fail("GPU playback format cannot change while an unsafe conversion is retained until process exit.");
    }
    s.type=type;s.format={};
    Check(MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&s.format.width,&s.format.height),"GPU playback has no decoded dimensions.");
    Check(type->GetGUID(MF_MT_SUBTYPE,&s.format.subtype),"GPU playback has no decoded subtype.");
    if(!s.format.width || !s.format.height || s.format.width>16384 || s.format.height>16384)
        Fail("GPU playback decoded dimensions exceed admission.");
    const auto interlace=MFGetAttributeUINT32(type,MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    if(interlace!=MFVideoInterlace_Unknown && interlace!=MFVideoInterlace_Progressive && interlace!=MFVideoInterlace_MixedInterlaceOrProgressive)
        Fail("GPU playback requires a progressive sample or the CPU deinterlacer.");
    s.aperture={0,0,LONG(s.format.width),LONG(s.format.height)};
    MFVideoArea area{};UINT size{};
    if(SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8*>(&area),sizeof(area),&size)) ||
       SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE,reinterpret_cast<UINT8*>(&area),sizeof(area),&size))) {
        if(size!=sizeof(area) || area.OffsetX.fract || area.OffsetY.fract || area.OffsetX.value<0 || area.OffsetY.value<0 ||
           area.Area.cx<=0 || area.Area.cy<=0 || UINT(area.OffsetX.value)>s.format.width || UINT(area.OffsetY.value)>s.format.height ||
           UINT(area.Area.cx)>s.format.width-UINT(area.OffsetX.value) || UINT(area.Area.cy)>s.format.height-UINT(area.OffsetY.value))
            Fail("GPU playback cannot represent the decoded display aperture exactly.");
        s.aperture={area.OffsetX.value,area.OffsetY.value,area.OffsetX.value+area.Area.cx,area.OffsetY.value+area.Area.cy};
    }
    UINT n=1,d=1;MFGetAttributeRatio(type,MF_MT_PIXEL_ASPECT_RATIO,&n,&d);if(!n || !d)n=d=1;
    const auto square=(std::uint64_t(s.aperture.right-s.aperture.left)*n+d/2)/d;
    if(!square || square>16384)Fail("GPU playback square-pixel width is invalid.");
    s.format.squareWidth=UINT(square);s.format.height=UINT(s.aperture.bottom-s.aperture.top);
    s.format.rotation=MFGetAttributeUINT32(type,MF_MT_VIDEO_ROTATION,0);
    if(s.format.rotation!=0 && s.format.rotation!=90 && s.format.rotation!=180 && s.format.rotation!=270)
        Fail("GPU playback rotation metadata is invalid.");
    s.enumerator.Reset();s.processor.Reset();s.processorFormat=DXGI_FORMAT_UNKNOWN;
    if(s.privateTarget) {
        D3D11_TEXTURE2D_DESC working{};s.privateTarget->texture->GetDesc(&working);
        if(working.Width!=s.format.squareWidth || working.Height!=s.format.height)s.privateTarget.reset();
    }
    return s.format;
}
std::shared_ptr<const VideoPlaybackGpuImage> VideoGpuDecoder::Convert(IMFSample* sample,
    const std::function<bool()>& current,Clock::time_point deadline) {
    auto& s=*state_;
    if(s.unsafe)Fail("GPU playback converter is latched unsafe after an incomplete conversion; its sample and reader are retained until process exit.");
    DrainPending(deadline);if(!current())return {};
    DWORD count{};Check(sample->GetBufferCount(&count),"GPU playback could not inspect a sample.");
    if(count!=1)Fail("GPU playback requires one DXGI sample buffer.");
    if(MFGetAttributeUINT32(sample,MFSampleExtension_Interlaced,FALSE))Fail("GPU playback sample needs CPU deinterlacing.");
    ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFDXGIBuffer> dxgi;ComPtr<ID3D11Texture2D> source;
    Check(sample->GetBufferByIndex(0,&buffer),"GPU playback could not access its sample buffer.");
    Check(buffer.As(&dxgi),"The selected decoder returned system-memory output; using CPU playback.");
    Check(dxgi->GetResource(IID_PPV_ARGS(&source)),"GPU playback sample has no video texture.");
    UINT subresource{};Check(dxgi->GetSubresourceIndex(&subresource),"GPU playback sample has no texture subresource.");
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    ComPtr<ID3D11Device> origin;source->GetDevice(&origin);
    if(origin.Get()!=s.device.Get() || !desc.MipLevels || subresource/desc.MipLevels>=desc.ArraySize ||
       subresource%desc.MipLevels>=32 || UINT(s.aperture.right)>std::max(1u,desc.Width>>(subresource%desc.MipLevels)) ||
       UINT(s.aperture.bottom)>std::max(1u,desc.Height>>(subresource%desc.MipLevels)))
        Fail("GPU playback sample allocation does not match its admitted device/aperture.");
    std::shared_ptr<TextureStorage> target;
    for(auto& slot:s.textures)if(slot.lease.expired()) {
        D3D11_TEXTURE2D_DESC old{};slot.storage->texture->GetDesc(&old);
        if(old.Width==s.format.squareWidth && old.Height==s.format.height) {target=slot.storage;break;}
    }
    if(!target) {
        s.textures.erase(std::remove_if(s.textures.begin(),s.textures.end(),[](const auto& slot){return slot.lease.expired();}),s.textures.end());
        target=AllocateTexture(s.device.Get(),s.format.squareWidth,s.format.height);s.textures.push_back({target,{}});
    }
    if(!s.processor || s.processorFormat!=desc.Format) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth=desc.Width;content.InputHeight=desc.Height;content.OutputWidth=s.format.squareWidth;content.OutputHeight=s.format.height;
        content.InputFrameRate={30,1};content.OutputFrameRate={30,1};content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        Check(s.video->CreateVideoProcessorEnumerator(&content,&s.enumerator),"GPU playback could not create a format converter.");
        UINT input{},output{};
        Check(s.enumerator->CheckVideoProcessorFormat(desc.Format,&input),"GPU playback input format is unavailable.");
        Check(s.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,&output),"GPU playback BGRA output is unavailable.");
        if(!(input&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(output&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            Fail("GPU playback converter does not support this pixel format.");
        Check(s.video->CreateVideoProcessor(s.enumerator.Get(),0,&s.processor),"GPU playback could not create its video processor.");
        s.processorFormat=desc.Format;
    }
    const auto inputColor=ColorSpace(s.type.Get(),IsRgb(desc.Format));
    constexpr auto outputColor=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    ComPtr<ID3D11VideoProcessorEnumerator1> enum1;
    if(s.videoContext1 && SUCCEEDED(s.enumerator.As(&enum1))) {
        BOOL supported{};Check(enum1->CheckVideoProcessorFormatConversion(desc.Format,inputColor,DXGI_FORMAT_B8G8R8A8_UNORM,outputColor,&supported),
            "GPU playback color conversion could not be checked.");
        if(!supported)Fail("GPU playback color/transfer conversion is unsupported by this device.");
        s.videoContext1->VideoProcessorSetStreamColorSpace1(s.processor.Get(),0,inputColor);
        s.videoContext1->VideoProcessorSetOutputColorSpace1(s.processor.Get(),outputColor);
    } else {
        if(inputColor!=DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 && inputColor!=DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 &&
           inputColor!=DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 && inputColor!=DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 &&
           inputColor!=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)Fail("GPU playback needs an extended color-space processor.");
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{},out{};
        in.YCbCr_Matrix=inputColor==DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 || inputColor==DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
        in.Nominal_Range=inputColor==DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 || inputColor==DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 ||
            inputColor==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709?D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255:D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        out.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        s.videoContext->VideoProcessorSetStreamColorSpace(s.processor.Get(),0,&in);s.videoContext->VideoProcessorSetOutputColorSpace(s.processor.Get(),&out);
    }
    ComPtr<ID3D11VideoProcessorInputView> input;ComPtr<ID3D11VideoProcessorOutputView> output;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};in.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
    in.Texture2D.MipSlice=subresource%desc.MipLevels;in.Texture2D.ArraySlice=subresource/desc.MipLevels;
    Check(s.video->CreateVideoProcessorInputView(source.Get(),s.enumerator.Get(),&in,&input),"GPU playback could not view its decoded texture.");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};out.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
    const auto direct=s.forcePrivateConversion?E_NOINTERFACE:
        s.video->CreateVideoProcessorOutputView(target->texture.Get(),s.enumerator.Get(),&out,&output);
    if(SUCCEEDED(direct))s.directOutputViewProven=true;
    if(FAILED(direct)) {
        // Some drivers reject a video-processor view of an NT-shared target.
        // Keep conversion and the following copy on this same GPU/context;
        // the extra working texture consumes the same bounded global credit.
        if(!s.privateTarget)s.privateTarget=AllocateTexture(s.device.Get(),s.format.squareWidth,s.format.height,false);
        Check(s.video->CreateVideoProcessorOutputView(s.privateTarget->texture.Get(),s.enumerator.Get(),&out,&output),
            "GPU playback could not view its private converted texture.");
    }
    const RECT destination{0,0,LONG(s.format.squareWidth),LONG(s.format.height)};
    s.videoContext->VideoProcessorSetStreamFrameFormat(s.processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    s.videoContext->VideoProcessorSetStreamAutoProcessingMode(s.processor.Get(),0,FALSE);
    s.videoContext->VideoProcessorSetStreamSourceRect(s.processor.Get(),0,TRUE,&s.aperture);
    s.videoContext->VideoProcessorSetStreamDestRect(s.processor.Get(),0,TRUE,&destination);
    s.videoContext->VideoProcessorSetOutputTargetRect(s.processor.Get(),TRUE,&destination);
    s.videoContext->VideoProcessorSetOutputAlphaFillMode(s.processor.Get(),D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE,0);
    ComPtr<ID3D11Query> query;
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};Check(s.device->CreateQuery(&queryDesc,&query),"GPU playback could not track conversion completion.");
    // From here the pending closure, not these locals, owns everything the
    // GPU may touch; a later deadline or Blt failure cannot release any of it
    // while completion is unknown.
    auto pending=std::make_unique<State::Pending>();
    pending->reader=s.attachedReader;pending->sample=sample;pending->buffer=buffer;pending->dxgi=dxgi;
    pending->source=source;pending->input=input;pending->output=output;
    pending->outputStorage=target;pending->privateStorage=FAILED(direct)?s.privateTarget:nullptr;
    pending->query=query;
    s.pending=std::move(pending);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=input.Get();
    const auto converted=s.videoContext->VideoProcessorBlt(s.processor.Get(),output.Get(),0,1,&stream);
    if(FAILED(converted)) {
        // A driver may have accepted partial work before reporting failure.
        // The event was not ended, so neither pool credit nor decoder sample
        // may be released on the assumption that conversion never started.
        // Latch unsafe: this query is never polled and the closure above is
        // quarantined with the decoder until process exit.
        MarkUnsafe();
        Check(converted,"GPU playback color conversion failed; its exact sample and reader are retained until process exit.");
    }
    if(FAILED(direct))s.context->CopyResource(target->texture.Get(),s.privateTarget->texture.Get());
    s.context->End(s.pending->query.Get());s.pending->queryEnded=true;s.context->Flush();
    // Cancellation is polled by the caller's post-drain check: quickly
    // completed stale work drains without poisoning this healthy converter,
    // while work still pending at the deadline latches it unsafe above.
    DrainPending(deadline);if(!current())return {};
    auto image=std::make_shared<VideoPlaybackGpuImage>();image->texture=target->texture.Get();image->storage=target;image->bytes=target->bytes;
    for(auto& slot:s.textures)if(slot.storage==target) {slot.lease=image;break;}
    return image;
}
std::size_t VideoGpuDecoder::RetainedBytes() {return Budget()->bytes.load();}
bool VideoGpuDecoder::HasConversionCredit() {
    auto& s=*state_;
    bool reusable{};
    for(const auto& slot:s.textures)if(slot.lease.expired()) {
        D3D11_TEXTURE2D_DESC desc{};slot.storage->texture->GetDesc(&desc);
        if(desc.Width==s.format.squareWidth && desc.Height==s.format.height) {reusable=true;break;}
    }
    // Old-format free slots must retire before their credit can admit the new
    // geometry; otherwise an idle pool could wait forever on its own storage.
    if(!reusable)
        s.textures.erase(std::remove_if(s.textures.begin(),s.textures.end(),[](const auto& slot){return slot.lease.expired();}),s.textures.end());
    const auto bytes=std::size_t(s.format.squareWidth)*s.format.height*4;
    if(!bytes || bytes>kTextureByteLimit)return false;
    // Until a direct processor view of the shared target has succeeded on
    // this driver, Convert may also need the private conversion target.
    // Credit must cover both allocations up front, or an admitted read
    // could meet backpressure only after native work was accepted.
    const unsigned allocations=(reusable?0u:1u)+((!s.privateTarget && !s.directOutputViewProven)?1u:0u);
    const auto needed=bytes*allocations;
    const auto budget=Budget();
    return budget->count.load()+allocations<=kTextureSlotLimit && needed<=kTextureByteLimit &&
        budget->bytes.load()<=kTextureByteLimit-needed;
}
} // namespace okuflow
#endif
