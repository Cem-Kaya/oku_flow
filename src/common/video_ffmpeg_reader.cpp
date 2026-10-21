#include "video_ffmpeg_reader.hpp"
#ifdef _WIN32
#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

#if OKUFLOW_HAS_FFMPEG_PLAYBACK
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace okuflow::detail {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
bool FfmpegPlaybackRequested() {
    wchar_t value[16]{};
    return GetEnvironmentVariableW(L"OKUFLOW_VIDEO_BACKEND",value,16)==6 &&
        std::wstring(value)==L"ffmpeg";
}
#if OKUFLOW_HAS_FFMPEG_PLAYBACK
namespace {
// Retain DLLs for the process lifetime. Never unload while another retained
// decoder/frame/retirement could still execute library code. Qt uses other
// major versions; this loader never searches PATH or writes into Qt's tree.
#define FF_API(X) \
 X(avcodec_version,1) X(avcodec_configuration,1) X(avcodec_license,1) \
 X(avcodec_find_decoder,1) X(avcodec_find_decoder_by_name,1) \
 X(avcodec_alloc_context3,1) X(avcodec_parameters_to_context,1) \
 X(avcodec_open2,1) X(avcodec_free_context,1) X(avcodec_flush_buffers,1) \
 X(avcodec_send_packet,1) X(avcodec_receive_frame,1) \
 X(av_packet_alloc,1) X(av_packet_free,1) X(av_packet_unref,1) \
 X(avformat_alloc_context,2) X(avformat_open_input,2) X(avformat_find_stream_info,2) \
 X(av_find_best_stream,2) X(avformat_close_input,2) X(av_read_frame,2) \
 X(avformat_seek_file,2) X(av_stream_get_side_data,2) \
 X(avio_alloc_context,2) X(avio_context_free,2) \
 X(av_frame_alloc,0) X(av_frame_free,0) X(av_frame_unref,0) \
 X(av_malloc,0) X(av_free,0) X(av_rescale_q,0) X(av_strerror,0) \
 X(av_dict_set_int,0) X(av_dict_free,0) \
 X(av_channel_layout_copy,0) X(av_channel_layout_uninit,0) X(av_display_rotation_get,0) \
 X(sws_getCachedContext,3) X(sws_freeContext,3) X(sws_scale,3) \
 X(sws_getCoefficients,3) X(sws_setColorspaceDetails,3) \
 X(swr_alloc_set_opts2,4) X(swr_init,4) X(swr_free,4) X(swr_convert,4)
struct Api {
#define DECLARE(name,dll) decltype(&::name) name{};
    FF_API(DECLARE)
#undef DECLARE
    HMODULE modules[6]{};
    explicit Api(const std::filesystem::path& root) {
        const wchar_t* names[]={L"avutil-59.dll",L"avcodec-61.dll",L"avformat-61.dll",
            L"swscale-8.dll",L"swresample-5.dll",L"libdav1d.dll"};
        // dav1d must be resident before avcodec imports it.
        for(const auto i:{5,0,4,3,1,2}) {
            modules[i]=LoadLibraryExW((root/names[i]).c_str(),nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if(!modules[i])throw std::runtime_error("Pinned FFmpeg SDK runtime is incomplete.");
        }
#define LOAD(name,dll) name=reinterpret_cast<decltype(name)>(GetProcAddress(modules[dll],#name)); \
    if(!name)throw std::runtime_error("FFmpeg SDK API mismatch: " #name);
        FF_API(LOAD)
#undef LOAD
        const std::string config=avcodec_configuration();
        if(avcodec_version()!=AV_VERSION_INT(61,19,101) ||
           std::string(avcodec_license())!="LGPL version 2.1 or later" ||
           config.find("--enable-gpl")!=std::string::npos ||
           config.find("--enable-nonfree")!=std::string::npos ||
           config.find("--enable-libdav1d")==std::string::npos ||
           !avcodec_find_decoder_by_name("libdav1d"))
            throw std::runtime_error("Only the reviewed LGPL FFmpeg 7.1.1/dav1d development build is supported.");
    }
};
Api& Library() {
    // Serialized initialization, with one sticky successful runtime identity.
    static std::mutex mutex;static Api* loaded{};static std::string failure;
    std::lock_guard lock(mutex);
    if(!failure.empty())throw std::runtime_error(failure);
    if(!loaded) {
        wchar_t root[32768]{};
        const auto n=GetEnvironmentVariableW(L"OKUFLOW_FFMPEG_RUNTIME",root,32768);
        if(!n || n>=32768 || !std::filesystem::path(root).is_absolute())
            throw std::runtime_error("Set OKUFLOW_FFMPEG_RUNTIME to the private SDK bin directory.");
        try {loaded=new Api(root);}catch(const std::exception& e){failure=e.what();throw;}
    }
    return *loaded;
}
void Require(HRESULT hr) {if(FAILED(hr))throw std::runtime_error("FFmpeg held-stream or sample operation failed.");}
void AvCheck(Api& a,int result) {
    if(result>=0)return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};a.av_strerror(result,text,sizeof(text));
    throw std::runtime_error(std::string("FFmpeg: ")+text);
}
constexpr AVRational ticks{1,10'000'000};
int Matrix(AVColorSpace space) {
    switch(space) {
    case AVCOL_SPC_BT709:return SWS_CS_ITU709;
    case AVCOL_SPC_FCC:return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M:return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:return SWS_CS_BT2020;
    case AVCOL_SPC_RGB:case AVCOL_SPC_UNSPECIFIED:case AVCOL_SPC_BT470BG:case AVCOL_SPC_SMPTE170M:return SWS_CS_DEFAULT;
    default:throw std::runtime_error("Unsupported embedded color matrix; native conversion is required.");
    }
}
// Saved fragmented MP4 currently has a measured timeline-precision difference
// from the Windows reader. Refuse that container class until cross-backend
// frame/PTS equivalence is resolved. This bounded structural probe never
// inspects codec names, payload bytes or a reopened pathname.
bool FragmentedMp4(IMFByteStream* bytes,const std::function<bool()>& current,Clock::time_point deadline) {
    const auto live=[&]{if(!current() || Clock::now()>=deadline)throw std::runtime_error("Embedded container probe canceled or expired.");};
    live();
    QWORD length{};Require(bytes->GetLength(&length));
    const auto box=[&](std::uint64_t at,std::uint64_t end,std::uint64_t& size,unsigned& header,char* type) {
        live();
        if(end-at<8)return false;
        std::uint8_t data[16]{};ULONG actual{};Require(bytes->SetCurrentPosition(at));
        Require(bytes->Read(data,8,&actual));live();if(actual!=8)return false;
        size=0;for(unsigned i=0;i<4;++i)size=(size<<8)|data[i];
        std::memcpy(type,data+4,4);header=8;
        if(size==1) {
            if(end-at<16)return false;Require(bytes->Read(data+8,8,&actual));live();if(actual!=8)return false;
            size=0;for(unsigned i=8;i<16;++i)size=(size<<8)|data[i];header=16;
        } else if(!size)size=end-at;
        return size>=header && size<=end-at;
    };
    for(std::uint64_t at=0,attempts=0;at<length && attempts<1024;++attempts) {
        std::uint64_t size{};unsigned header{};char type[4]{};
        if(!box(at,length,size,header,type))return false;
        if(!std::memcmp(type,"moof",4))return true;
        if(!std::memcmp(type,"moov",4)) {
            if(size>8*1024*1024)throw std::runtime_error("Embedded MP4 metadata exceeds the prototype probe budget.");
            const auto end=at+size;auto child=at+header;
            for(unsigned children=0;child<end && children<1024;++children) {
                std::uint64_t childSize{};unsigned childHeader{};char childType[4]{};
                if(!box(child,end,childSize,childHeader,childType))return false;
                if(!std::memcmp(childType,"mvex",4))return true;
                child+=childSize;
            }
            if(child<end)throw std::runtime_error("Embedded MP4 metadata exceeds the bounded box probe.");
            return false;
        }
        at+=size;
    }
    return false;
}
struct Track {
    Api& a;ComPtr<IMFByteStream> bytes;
    AVFormatContext* format{};AVIOContext* io{};AVCodecContext* codec{};
    AVFrame* frame{};AVPacket* packet{};SwsContext* scale{};SwrContext* resample{};
    ComPtr<IMFMediaType> type;
    int index{-1};bool audio{},draining{},ended{};
    std::int64_t origin{},duration{};
    std::uint32_t width{},height{},rate{},channels{};
    std::function<bool()> current;
    Clock::time_point deadline{};
    std::int64_t copyFloor{-1};
    explicit Track(Api& api):a(api){}
    ~Track() {
        a.swr_free(&resample);a.sws_freeContext(scale);
        a.av_packet_free(&packet);a.av_frame_free(&frame);a.avcodec_free_context(&codec);
        a.avformat_close_input(&format);
        if(io){a.av_free(io->buffer);a.avio_context_free(&io);}
    }
    bool Canceled() const {return !current || !current() || Clock::now()>=deadline;}
    static int Interrupt(void* opaque) {return static_cast<Track*>(opaque)->Canceled();}
    static int Read(void* opaque,std::uint8_t* data,int size) {
        auto& t=*static_cast<Track*>(opaque);if(t.Canceled())return AVERROR_EXIT;
        ULONG actual{};
        const auto hr=t.bytes->Read(data,static_cast<ULONG>(std::min(size,64*1024)),&actual);
        if(t.Canceled())return AVERROR_EXIT;
        if(FAILED(hr))return AVERROR(EIO);
        return actual?static_cast<int>(actual):AVERROR_EOF;
    }
    static std::int64_t SeekIo(void* opaque,std::int64_t offset,int whence) {
        auto& t=*static_cast<Track*>(opaque);if(t.Canceled())return AVERROR_EXIT;
        QWORD length{},position{};
        if(FAILED(t.bytes->GetLength(&length)) || length>INT64_MAX)return AVERROR(EIO);
        if(whence&AVSEEK_SIZE)return static_cast<std::int64_t>(length);
        whence&=~AVSEEK_FORCE;
        std::int64_t base{};
        if(whence==SEEK_CUR) {
            if(FAILED(t.bytes->GetCurrentPosition(&position)) || position>INT64_MAX)return AVERROR(EIO);
            base=static_cast<std::int64_t>(position);
        } else if(whence==SEEK_END)base=static_cast<std::int64_t>(length);
        else if(whence!=SEEK_SET)return AVERROR(EINVAL);
        if((offset>0 && base>INT64_MAX-offset) || offset< -base)return AVERROR(EINVAL);
        const auto target=base+offset;
        if(target<0 || static_cast<std::uint64_t>(target)>length)return AVERROR(EINVAL);
        return SUCCEEDED(t.bytes->SetCurrentPosition(target))?target:AVERROR(EIO);
    }
    static int DenyExternal(AVFormatContext*,AVIOContext**,const char*,int,AVDictionary**) {
        return AVERROR(EACCES); // no playlists, secondary paths or network I/O
    }
    void Open(IMFByteStream* held,bool isAudio,std::function<bool()> live,Clock::time_point end,
              std::int64_t timelineOrigin=0) {
        bytes=held;audio=isAudio;current=std::move(live);deadline=end;
        Require(bytes->SetCurrentPosition(0));
        auto* buffer=static_cast<unsigned char*>(a.av_malloc(64*1024));
        if(!buffer)throw std::bad_alloc();
        io=a.avio_alloc_context(buffer,64*1024,0,this,Read,nullptr,SeekIo);
        if(!io){a.av_free(buffer);throw std::bad_alloc();}
        format=a.avformat_alloc_context();if(!format)throw std::bad_alloc();
        format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback={Interrupt,this};format->io_open=DenyExternal;
        format->probesize=8*1024*1024;format->max_analyze_duration=2*AV_TIME_BASE;
        format->max_streams=8;format->max_index_size=8*1024*1024;
        AvCheck(a,a.avformat_open_input(&format,nullptr,nullptr,nullptr));
        // Probe-created decoders need the same allocation/thread guards as
        // the persistent contexts. A header-less stream cannot supply that
        // per-stream option array yet; leave it to native fallback for now.
        if(!format->nb_streams || format->nb_streams>8)
            throw std::runtime_error("Embedded input needs stream metadata before bounded probing.");
        struct ProbeOptions {
            Api& a;std::vector<AVDictionary*> values;
            ~ProbeOptions(){for(auto*& value:values)a.av_dict_free(&value);}
        } options{a,std::vector<AVDictionary*>(format->nb_streams)};
        for(auto*& value:options.values) {
            AvCheck(a,a.av_dict_set_int(&value,"threads",4,0));
            AvCheck(a,a.av_dict_set_int(&value,"max_pixels",16'777'216,0));
            AvCheck(a,a.av_dict_set_int(&value,"max_samples",131072,0));
        }
        AvCheck(a,a.avformat_find_stream_info(format,options.values.data()));
        index=a.av_find_best_stream(format,audio?AVMEDIA_TYPE_AUDIO:AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        AvCheck(a,index);
        for(unsigned i=0;i<format->nb_streams;++i)
            if(i!=static_cast<unsigned>(index))format->streams[i]->discard=AVDISCARD_ALL;
        auto* stream=format->streams[index];auto* parameters=stream->codecpar;
        const auto* decoder=parameters->codec_id==AV_CODEC_ID_AV1
            ?a.avcodec_find_decoder_by_name("libdav1d"):a.avcodec_find_decoder(parameters->codec_id);
        if(!decoder)throw std::runtime_error("No embedded decoder for this selected stream.");
        codec=a.avcodec_alloc_context3(decoder);if(!codec)throw std::bad_alloc();
        AvCheck(a,a.avcodec_parameters_to_context(codec,parameters));
        codec->thread_count=4;codec->max_pixels=16'777'216;codec->max_samples=131072;
        AvCheck(a,a.avcodec_open2(codec,decoder,nullptr));
        frame=a.av_frame_alloc();packet=a.av_packet_alloc();if(!frame || !packet)throw std::bad_alloc();
        origin=audio?timelineOrigin:(stream->start_time==AV_NOPTS_VALUE?0:a.av_rescale_q(stream->start_time,stream->time_base,ticks));
        if(origin< -INT64_MAX/100 || origin>INT64_MAX/100)
            throw std::runtime_error("Embedded timeline origin exceeds the host clock.");
        duration=stream->duration==AV_NOPTS_VALUE?0:a.av_rescale_q(stream->duration,stream->time_base,ticks);
        if(!duration && format->duration!=AV_NOPTS_VALUE)
            duration=a.av_rescale_q(format->duration,AVRational{1,AV_TIME_BASE},ticks);
        Require(MFCreateMediaType(&type));
        if(audio) {
            rate=codec->sample_rate;channels=codec->ch_layout.nb_channels;
            if(rate<8000 || rate>192000 || !channels || channels>8)throw std::runtime_error("Unsupported embedded PCM format.");
            Require(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));
            Require(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));
            type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,rate);
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,channels);
            AVChannelLayout output{};AvCheck(a,a.av_channel_layout_copy(&output,&codec->ch_layout));
            const auto result=a.swr_alloc_set_opts2(&resample,&output,AV_SAMPLE_FMT_S16,rate,
                &codec->ch_layout,codec->sample_fmt,rate,0,nullptr);
            a.av_channel_layout_uninit(&output);AvCheck(a,result);AvCheck(a,a.swr_init(resample));
        } else {
            (void)Matrix(parameters->color_space);
            width=codec->width;height=codec->height;
            if(!width || !height || std::uint64_t(width)*height>16'777'216)
                throw std::runtime_error("Embedded video dimensions exceed admission.");
            Require(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
            Require(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32));
            MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,width,height);type->SetUINT32(MF_MT_DEFAULT_STRIDE,width*4);
            auto par=stream->sample_aspect_ratio;if(par.num<=0 || par.den<=0)par={1,1};
            MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,par.num,par.den);
            std::size_t matrixSize{};const auto* matrix=a.av_stream_get_side_data(stream,AV_PKT_DATA_DISPLAYMATRIX,&matrixSize);
            if(matrix && matrixSize>=9*sizeof(std::int32_t)) {
                const auto* transform=reinterpret_cast<const std::int32_t*>(matrix);
                if(double(transform[0])*transform[4]-double(transform[1])*transform[3]<=0)
                    throw std::runtime_error("Mirrored embedded display transforms require native fallback.");
                const auto degrees=-a.av_display_rotation_get(transform);
                const auto rounded=std::lround(degrees/90)*90;
                if(!std::isfinite(degrees) || std::abs(degrees-rounded)>0.01)
                    throw std::runtime_error("Unsupported embedded display transform.");
                type->SetUINT32(MF_MT_VIDEO_ROTATION,static_cast<UINT32>((rounded%360+360)%360));
            }
        }
    }
    void Seek(std::int64_t target) {
        io->error=0;io->eof_reached=0; // a superseded custom read is recoverable by explicit seek
        const auto raw=a.av_rescale_q(target+origin,ticks,format->streams[index]->time_base);
        AvCheck(a,a.avformat_seek_file(format,index,INT64_MIN,raw,raw,AVSEEK_FLAG_BACKWARD));
        a.avcodec_flush_buffers(codec);a.av_frame_unref(frame);a.av_packet_unref(packet);
        draining=ended=false;
        if(audio){a.swr_free(&resample);AVChannelLayout layout{};
            AvCheck(a,a.av_channel_layout_copy(&layout,&codec->ch_layout));
            const auto result=a.swr_alloc_set_opts2(&resample,&layout,AV_SAMPLE_FMT_S16,rate,&layout,codec->sample_fmt,rate,0,nullptr);
            a.av_channel_layout_uninit(&layout);AvCheck(a,result);AvCheck(a,a.swr_init(resample));}
    }
    bool Decode() {
        a.av_frame_unref(frame);
        for(unsigned attempts=0;attempts<65536 && !Canceled();++attempts) {
            const auto received=a.avcodec_receive_frame(codec,frame);
            if(received>=0)return true;
            if(received==AVERROR_EOF){ended=true;return false;}
            if(received!=AVERROR(EAGAIN))AvCheck(a,received);
            if(draining)throw std::runtime_error("Embedded decoder did not drain.");
            const auto read=a.av_read_frame(format,packet);
            if(read==AVERROR_EOF){AvCheck(a,a.avcodec_send_packet(codec,nullptr));draining=true;continue;}
            AvCheck(a,read);
            if(packet->size<0 || packet->size>64*1024*1024){a.av_packet_unref(packet);throw std::runtime_error("Embedded packet exceeds read admission.");}
            if(packet->stream_index==index) {
                const auto sent=a.avcodec_send_packet(codec,packet);a.av_packet_unref(packet);AvCheck(a,sent);
            } else a.av_packet_unref(packet);
        }
        throw std::runtime_error("Embedded decode canceled or reached its finite read deadline.");
    }
    void Sample(IMFSample** output,LONGLONG* timestamp,DWORD* flags) {
        *output=nullptr;
        if(ended || !Decode()){*flags|=MF_SOURCE_READERF_ENDOFSTREAM;return;}
        auto* stream=format->streams[index];
        if(frame->best_effort_timestamp==AV_NOPTS_VALUE)throw std::runtime_error("Embedded sample has no presentation timestamp.");
        const auto raw=a.av_rescale_q(frame->best_effort_timestamp,stream->time_base,ticks);
        if(raw< -INT64_MAX/100 || raw>INT64_MAX/100)throw std::runtime_error("Embedded sample timestamp exceeds the host clock.");
        *timestamp=raw-origin;
        auto duration100ns=a.av_rescale_q(frame->duration,stream->time_base,ticks);
        ComPtr<IMFSample> sample;Require(MFCreateSample(&sample));
        sample->SetSampleTime(*timestamp);sample->SetSampleDuration(std::max<std::int64_t>(0,duration100ns));
        if(audio || copyFloor<0 || *timestamp>=copyFloor) {
            std::uint64_t capacity=audio?std::uint64_t(frame->nb_samples)*channels*2:std::uint64_t(width)*height*4;
            if(!capacity || capacity>(audio?256u*1024:64u*1024*1024))throw std::runtime_error("Embedded output exceeds staging admission.");
            ComPtr<IMFMediaBuffer> buffer;Require(MFCreateMemoryBuffer(static_cast<DWORD>(capacity),&buffer));
            BYTE* destination{};Require(buffer->Lock(&destination,nullptr,nullptr));
            int produced{};
            if(audio) {
                if(frame->sample_rate!=static_cast<int>(rate) || frame->ch_layout.nb_channels!=static_cast<int>(channels) || frame->format!=codec->sample_fmt) {
                    buffer->Unlock();throw std::runtime_error("Embedded PCM format changed.");}
                produced=a.swr_convert(resample,&destination,frame->nb_samples,const_cast<const std::uint8_t**>(frame->extended_data),frame->nb_samples);
                if(produced>=0)produced*=channels*2;
            } else {
                if(frame->width!=static_cast<int>(width) || frame->height!=static_cast<int>(height)) {
                    buffer->Unlock();throw std::runtime_error("Embedded video dimensions changed.");}
                scale=a.sws_getCachedContext(scale,width,height,static_cast<AVPixelFormat>(frame->format),width,height,AV_PIX_FMT_BGRA,
                    SWS_BILINEAR,nullptr,nullptr,nullptr);
                if(!scale){buffer->Unlock();throw std::runtime_error("Embedded color conversion unavailable.");}
                int matrix{};
                try {matrix=Matrix(frame->colorspace);}catch(...){buffer->Unlock();throw;}
                const auto* coefficients=a.sws_getCoefficients(matrix);
                const auto color=a.sws_setColorspaceDetails(scale,coefficients,frame->color_range==AVCOL_RANGE_JPEG,coefficients,1,0,1<<16,1<<16);
                if(color<0){buffer->Unlock();AvCheck(a,color);}
                std::uint8_t* destinations[]={destination,nullptr,nullptr,nullptr};const int strides[]={static_cast<int>(width*4),0,0,0};
                produced=a.sws_scale(scale,frame->data,frame->linesize,0,height,destinations,strides);
                produced=produced==static_cast<int>(height)?static_cast<int>(capacity):AVERROR(EINVAL);
            }
            buffer->Unlock();AvCheck(a,produced);Require(buffer->SetCurrentLength(produced));Require(sample->AddBuffer(buffer.Get()));
        }
        if(Canceled())throw std::runtime_error("Embedded sample was superseded.");
        *output=sample.Detach();
    }
};
// QueryInterface is required here: foreign system COM objects need not carry
// MSVC C++ RTTI and must never be inspected with dynamic_cast.
struct __declspec(uuid("20f19c99-45a8-43e2-b9b7-5707f9fb46f3")) EmbeddedControl:public IUnknown {
    virtual void Arm(std::function<bool()> current,Clock::time_point deadline,std::int64_t floor)=0;
    virtual std::string Failure() const=0;
};
class Reader final:public IMFSourceReader,public EmbeddedControl {
    std::atomic<ULONG> references_{1};
public:
    Track video;std::unique_ptr<Track> audio;bool videoSelected{},audioSelected{};
    std::string error;
    explicit Reader(Api& a):video(a){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(EmbeddedControl))*out=static_cast<EmbeddedControl*>(this);
        else if(id==IID_IUnknown || id==__uuidof(IMFSourceReader))*out=static_cast<IMFSourceReader*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--references_;if(!n)delete this;return n;}
    void Arm(std::function<bool()> current,Clock::time_point deadline,std::int64_t floor) override {
        video.current=current;video.deadline=deadline;video.copyFloor=floor;
        if(audio){audio->current=std::move(current);audio->deadline=deadline;}
    }
    std::string Failure() const override {return error;}
    Track* Select(DWORD index) {
        if(index==MF_SOURCE_READER_FIRST_VIDEO_STREAM || index==static_cast<DWORD>(video.index))return &video;
        if(audio && (index==MF_SOURCE_READER_FIRST_AUDIO_STREAM || index==static_cast<DWORD>(audio->index)))return audio.get();
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSelection(DWORD i,BOOL* selected) override {
        if(!selected)return E_POINTER;auto* t=Select(i);if(!t)return MF_E_INVALIDSTREAMNUMBER;
        *selected=t->audio?audioSelected:videoSelected;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSelection(DWORD i,BOOL selected) override {
        if(i==MF_SOURCE_READER_ALL_STREAMS){videoSelected=audioSelected=selected;return S_OK;}
        auto* t=Select(i);if(!t)return MF_E_INVALIDSTREAMNUMBER;(t->audio?audioSelected:videoSelected)=selected;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNativeMediaType(DWORD i,DWORD n,IMFMediaType** out) override {
        if(n)return MF_E_NO_MORE_TYPES;return GetCurrentMediaType(i,out);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentMediaType(DWORD i,IMFMediaType** out) override {
        auto* t=Select(i);if(!t)return MF_E_INVALIDSTREAMNUMBER;return t->type.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentMediaType(DWORD i,DWORD*,IMFMediaType* type) override {
        auto* t=Select(i);if(!t || !type)return MF_E_INVALIDSTREAMNUMBER;
        GUID subtype{};if(FAILED(type->GetGUID(MF_MT_SUBTYPE,&subtype)))return MF_E_INVALIDMEDIATYPE;
        return subtype==(t->audio?MFAudioFormat_PCM:MFVideoFormat_RGB32)?S_OK:MF_E_INVALIDMEDIATYPE;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(REFGUID,const PROPVARIANT& position) override {
        if(position.vt!=VT_I8 || position.hVal.QuadPart<0)return E_INVALIDARG;
        try {video.Seek(position.hVal.QuadPart);if(audio)audio->Seek(position.hVal.QuadPart);return S_OK;}
        catch(const std::exception& e){error=e.what();return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE ReadSample(DWORD i,DWORD,DWORD* actual,DWORD* flags,LONGLONG* pts,IMFSample** out) override {
        if(!actual || !flags || !pts || !out)return E_POINTER;
        *flags=0;*out=nullptr;auto* t=Select(i);if(!t)return MF_E_INVALIDSTREAMNUMBER;*actual=t->index;
        if(!(t->audio?audioSelected:videoSelected))return MF_E_INVALIDREQUEST;
        try {t->Sample(out,pts,flags);return S_OK;}
        catch(const std::exception& e){error=e.what();return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE Flush(DWORD) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD,REFGUID,REFIID,LPVOID*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPresentationAttribute(DWORD,REFGUID key,PROPVARIANT* value) override {
        if(key!=MF_PD_DURATION || !value)return E_NOTIMPL;
        return InitPropVariantFromInt64(video.duration,value);
    }
};
}
#endif
HRESULT CreateFfmpegVideoReader(IMFByteStream* bytes,const VideoReadStreamClone& clone,
    std::function<bool()> current,Clock::time_point deadline,IMFSourceReader** out,std::string& reason) {
    if(!out)return E_POINTER;*out=nullptr;
#if OKUFLOW_HAS_FFMPEG_PLAYBACK
    try {
        if(!current() || Clock::now()>=deadline)throw std::runtime_error("Embedded open canceled or expired.");
        if(FragmentedMp4(bytes,current,deadline))throw std::runtime_error("Saved fragmented MP4 retains native frame/PTS authority; using Media Foundation.");
        auto reader=std::make_unique<Reader>(Library());
        reader->video.Open(bytes,false,current,deadline);
        ComPtr<IMFByteStream> audioBytes;Require(clone(&audioBytes));
        auto audio=std::make_unique<Track>(reader->video.a);
        // No audio stream is legitimate; an unsupported existing track must
        // fail open into native fallback, never become silently muted media.
        try {audio->Open(audioBytes.Get(),true,current,deadline,reader->video.origin);reader->audio=std::move(audio);}
        catch(...) {
            if(audio->format && std::any_of(audio->format->streams,audio->format->streams+audio->format->nb_streams,
                [](const AVStream* s){return s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO;}))throw;
            if(!current() || Clock::now()>=deadline)throw;
        }
        *out=reader.release();reason="Embedded FFmpeg CPU; libdav1d for AV1; BGRA matrix conversion, no HDR tone mapping.";
        return S_OK;
    } catch(const std::exception& e){reason=e.what();return E_FAIL;}
#else
    (void)bytes;(void)clone;(void)current;(void)deadline;
    reason="Embedded FFmpeg playback SDK was not compiled; using Media Foundation.";return E_NOTIMPL;
#endif
}
void ArmFfmpegVideoReader(IMFSourceReader* source,std::function<bool()> current,Clock::time_point deadline,std::int64_t copyFloor) {
#if OKUFLOW_HAS_FFMPEG_PLAYBACK
    ComPtr<EmbeddedControl> control;
    if(source && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&control))))
        control->Arm(std::move(current),deadline,copyFloor);
#else
    (void)source;(void)current;(void)deadline;(void)copyFloor;
#endif
}
std::string FfmpegVideoReaderError(IMFSourceReader* source) {
#if OKUFLOW_HAS_FFMPEG_PLAYBACK
    ComPtr<EmbeddedControl> control;
    if(source && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&control))))return control->Failure();
#else
    (void)source;
#endif
    return {};
}
}
#endif
