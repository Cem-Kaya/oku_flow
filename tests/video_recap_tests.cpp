#include "okuflow/common/video_recap.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/app/frozen_camera_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <psapi.h>
#include <strmif.h>
#include <codecapi.h>

using namespace okuflow;
using namespace std::chrono_literals;
namespace okuflow {struct VideoMemoryStreamTestAccess {
    static const void* Identity(const VideoMemorySource& source){return source.backing_.get();}
};
struct VideoRecorderTestAccess {
    static void PrintAdmission(VideoRecorder& recorder,unsigned frame) {
        MF_SINK_WRITER_STATISTICS stats{};stats.cb=sizeof(stats);
        const auto hr=recorder.sinkWriter_->GetStatistics(recorder.streamIndex_,&stats);
        std::fprintf(stderr,"RAM admission frame=%u hr=%ld received=%llu encoded=%llu queuedBytes=%u submitted=%llu pending=%d\n",
            frame,hr,stats.qwNumSamplesReceived,stats.qwNumSamplesEncoded,stats.dwByteCountQueued,
            recorder.videoSamplesWritten_,bool(recorder.pendingSample_));
        Microsoft::WRL::ComPtr<ICodecAPI> codec;
        const auto service=recorder.sinkWriter_->GetServiceForStream(recorder.streamIndex_,GUID_NULL,IID_PPV_ARGS(&codec));
        VARIANT value{};const auto latency=codec?codec->GetValue(&CODECAPI_AVLowLatencyMode,&value):service;
        std::fprintf(stderr,"RAM codec service=%ld lowLatencyHr=%ld type=%u value=%ld\n",service,latency,value.vt,value.lVal);
        VariantClear(&value);
        Microsoft::WRL::ComPtr<IMFSinkWriterEx> extended;
        if(SUCCEEDED(recorder.sinkWriter_.As(&extended)))for(DWORD i=0;i<8;++i) {
            Microsoft::WRL::ComPtr<IMFTransform> transform;GUID category{};
            if(FAILED(extended->GetTransformForStream(recorder.streamIndex_,i,&category,&transform)))break;
            Microsoft::WRL::ComPtr<IMFAttributes> attrs;GUID clsid{};wchar_t name[128]{},id[40]{};
            if(SUCCEEDED(transform->GetAttributes(&attrs))) {
                attrs->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&clsid);attrs->GetString(MFT_FRIENDLY_NAME_Attribute,name,128,nullptr);
            }
            StringFromGUID2(clsid,id,40);std::fprintf(stderr,"RAM transform[%u] clsid=%ls name=%ls\n",i,id,name);
        }
    }
};}
namespace {
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void PrintMemory(const char* stage) {
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
        std::printf("process_memory stage=%s working_bytes=%zu private_commit_bytes=%zu peak_working_bytes=%zu peak_commit_bytes=%zu\n",
            stage,std::size_t(memory.WorkingSetSize),std::size_t(memory.PrivateUsage),
            std::size_t(memory.PeakWorkingSetSize),std::size_t(memory.PeakPagefileUsage));
}
template<class Predicate> bool Wait(Predicate predicate,std::chrono::milliseconds limit=5000ms) {
    const auto end=std::chrono::steady_clock::now()+limit;
    do {if(predicate())return true;std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<end);
    return predicate();
}
std::shared_ptr<const VideoMemorySource> Memory(unsigned size=1024) {
    auto memory=std::make_shared<VideoMemoryStream>(std::max<std::size_t>(VideoRecapPolicy::kSegmentBytes,size));
    Microsoft::WRL::ComPtr<IMFByteStream> stream;Require(SUCCEEDED(memory->OpenWriteStream(&stream)),"memory output opens");
    std::vector<std::uint8_t> payload(size,71);ULONG written{};
    Require(SUCCEEDED(stream->Write(payload.data(),size,&written)) && written==size,"memory output writes");
    stream.Reset();auto source=memory->Seal();Require(bool(source),"memory seals after writer cursor release");return source;
}
std::shared_ptr<const VideoRecapSegment> Segment(std::uint64_t epoch,std::uint64_t id,
    std::int64_t begin,std::int64_t end,std::shared_ptr<const VideoMemorySource> source) {
    auto s=std::make_shared<VideoRecapSegment>();s->source=std::move(source);s->epoch=epoch;s->id=id;
    s->begin100ns=begin;s->end100ns=end;s->captureOrigin100ns=1'000'000'000+begin;
    s->admissions.push_back({id*100,s->captureOrigin100ns,0});
    if(end>begin)s->admissions.push_back({id*100+1,1'000'000'000+end,end-begin});
    return s;
}
void FrozenSceneContract() {
    ViewTransform view;view.valid=true;FrozenSceneRecipe recipe;
    std::vector<std::uint8_t> pixels(4*2*4),out;
    for(unsigned i=0;i<8;++i){pixels[i*4]=i+10;pixels[i*4+1]=i+30;pixels[i*4+2]=i+50;pixels[i*4+3]=255;}
    Require(ComposeFrozenCameraScene(pixels,4,2,view,4,2,recipe,out) && out==pixels,"CPU exact scene identity preserves displayed pixels");
    recipe.rotationQuarterTurns=1;
    Require(ComposeFrozenCameraScene(pixels,4,2,view,2,4,recipe,out) && out[0]==pixels[4*4] && out[7*4]==pixels[3*4],
        "rotation reuses held pixels without reapplying appearance or temporal effects");
    recipe={};view.sourceX=0.5f;view.sourceWidth=0.5f;
    Require(ComposeFrozenCameraScene(pixels,4,2,view,2,2,recipe,out) && out[0]==pixels[2*4],"pan/zoom samples the held scene");
    pixels.assign(64*64*4,47);view={};view.valid=true;recipe={0,true,true,0.5f,0.5f};
    Require(ComposeFrozenCameraScene(pixels,64,64,view,64,64,recipe,out),"frozen GPU display recipe composes");
    const auto inner=(32*64+32)*4,outer=(32*64+44)*4;
    Require(out[inner]==255 && out[inner+1]==255 && out[inner+2]==255 &&
        out[outer]==20 && out[outer+1]==20 && out[outer+2]==242 && out[0]==47,"actual GPU focus marker recipe and background preserved");
    Require(!ComposeFrozenCameraScene(pixels,64,64,view,UINT32_MAX,UINT32_MAX,recipe,out),"viewport overflow fails before allocation");
}
void RingContract() {
    FrozenSceneContract();
    auto source=Memory();const auto bytes=source->Capacity();VideoRecapRing ring(bytes*3,30'000'000);ring.Reset(9);
    auto empty=std::make_shared<VideoMemoryStream>();
    Require(!ring.Append(Segment(9,1,0,1,empty->Seal())),"zero-byte source cannot create uncharged metadata history");
    VideoRecapRing largeRing;largeRing.Reset(9);
    Require(!largeRing.Append(Segment(9,1,0,1,Memory(VideoRecapPolicy::kSegmentBytes+1))),"segment allocation may not exceed eight MiB");
    auto tiny=Memory(1);
    for(std::uint64_t i=0;i<400;++i)Require(largeRing.Append(Segment(9,i+2,i*2,i*2+1,tiny)),"tiny-segment accounting seam");
    Require(largeRing.Snapshot().compressedBytes<=VideoRecapPolicy::kSegments,"one-byte segments retain a bounded metadata inventory");
    Require(ring.Append(Segment(9,1,0,9'000'000,source)),"first independent segment accepted");
    auto lease=ring.Acquire(0);Require(bool(lease),"leased segment acquired");
    Require(ring.Append(Segment(9,2,10'000'000,19'000'000,source)),"second segment");
    Require(ring.Append(Segment(9,3,20'000'000,29'000'000,source)),"third segment");
    Require(ring.Append(Segment(9,4,30'000'000,39'000'000,source)),"whole-keyframe byte eviction");
    Require(ring.Snapshot().compressedBytes<=bytes*3 && ring.Snapshot().first100ns>=10'000'000,"byte/window bound");
    Require(lease->id==1 && VideoMemoryStreamTestAccess::Identity(*lease->source)==VideoMemoryStreamTestAccess::Identity(*source),
        "evicted lease retains exact immutable bytes");
    auto second=ring.Acquire(0);Require(second && second->id==2,"expired seek clamps to actual first segment");
    Require(!ring.Acquire(0),"third pin is refused without dangling old leases");
    auto sourceOnly=lease->source;lease.reset();
    Require(!ring.Acquire(0),"copying source alone cannot evade the pin bound");
    Microsoft::WRL::ComPtr<IMFByteStream> cursor;VideoReadStreamClone clone;
    Require(SUCCEEDED(sourceOnly->OpenReadStream(&cursor,&clone)),"pinned source read cursor opens");
    sourceOnly.reset();Require(!ring.Acquire(0),"read cursor and clone callback retain their counted pin");
    cursor.Reset();Require(!ring.Acquire(0),"clone callback alone retains the counted pin");
    clone={};lease=ring.Acquire(0);Require(bool(lease),"retiring final cursor pin restores credit");
    ring.Reset(10);Require(!ring.Snapshot().available && lease->id==2,"epoch reset retires range not pinned bytes");
    Require(!ring.Append(Segment(9,5,40'000'000,49'000'000,source)),"old source epoch is refused");
    lease.reset();second.reset();
    Require(ring.Append(Segment(10,6,0,9'000'000,source)),"new source can start at zero");
    Require(!ring.Append(Segment(10,7,8'000'000,19'000'000,source)),"overlapping PTS is refused");
    Require(!VideoRecapPolicy::ForSize(7680,4320),"unsupported source is not called five-minute capable");
    Require(VideoRecapPolicy::ForSize(1920,1080)->fps==30 && VideoRecapPolicy::ForSize(3840,2160)->fps==30,"owner-selected 4K30 cadence policy");
    Require(VideoRecapPolicy::ForSize(1080,1920)->bitrate==8'000'000,"portrait policy retains same pixel budget");
    // At the reviewed rates, five minutes of payload plus full segment reserve
    // fits; no raw five-minute pixel queue is admitted.
    Require(16'000'000ULL*300/8+VideoRecapPolicy::kSegmentBytes<VideoRecapPolicy::kCompressedBytes,"five-minute nominal byte margin");
    VideoRecapRing durationRing(bytes*400);durationRing.Reset(30);
    for(std::uint64_t i=0;i<150;++i)Require(durationRing.Append(Segment(30,i+1,i*20'000'000,i*20'000'000+19'000'000,source)),"five-minute synthetic segment inventory");
    auto durationPin=durationRing.Acquire(0);durationRing.SetWindow(60LL*10'000'000);
    Require(durationRing.Snapshot().end100ns-durationRing.Snapshot().first100ns<=60LL*10'000'000 && durationPin->source,
        "reducing duration immediately trims history while preserving bounded old lease");
    Require(durationPin->begin100ns<durationRing.Snapshot().first100ns,"four-minute-old hold keeps its true time outside one-minute history");
    auto shrinkSeek=durationRing.Acquire(durationRing.Snapshot().first100ns);
    Require(shrinkSeek && !durationRing.Acquire(durationRing.Snapshot().end100ns),"duration shrink and rapid seek retain only two counted pins");
    shrinkSeek.reset();
    durationRing.SetWindow(VideoRecapPolicy::kWindow100ns);
    Require(durationRing.Snapshot().warming,"increasing duration does not invent evicted video");
}
std::uint32_t TrackTimeScale(std::span<const std::uint8_t> bytes,unsigned depth=0) {
    if(depth>4)return 0;
    const auto be32=[](const std::uint8_t* p){return(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];};
    for(std::size_t pos=0;pos+8<=bytes.size();) {
        const auto size=be32(bytes.data()+pos),type=be32(bytes.data()+pos+4);
        if(size<8 || size>bytes.size()-pos)return 0;
        const auto payload=bytes.subspan(pos+8,size-8);
        if(type==0x6d646864 && payload.size()>=16) { // mdhd
            const std::size_t offset=payload[0]==1?20:12;
            return payload.size()>=offset+4?be32(payload.data()+offset):0;
        }
        if(type==0x6d6f6f76 || type==0x7472616b || type==0x6d646961) { // moov/trak/mdia
            if(const auto scale=TrackTimeScale(payload,depth+1))return scale;
        }
        pos+=size;
    }
    return 0;
}
std::shared_ptr<const VideoMemorySource> Encode(unsigned width=192,unsigned height=128,unsigned frames=65) {
    auto output=std::make_shared<VideoMemoryStream>();
    {
        VideoRecorder writer;
        Require(writer.StartMemory(output,width,height,30,1,std::uint64_t(width)*height>1920u*1080u?16'000'000:8'000'000),writer.LastError().c_str());
        std::vector<std::uint8_t> pixels(std::size_t(width)*height*4,255);
        for(unsigned i=0;i<frames;++i) {
            // Include a genuine one-second capture gap: PTS must not compact.
            const auto pts=std::int64_t(i)*10'000'000/30+(i>=32?10'000'000:0);
            for(std::size_t j=0;j<pixels.size();j+=4){pixels[j]=std::uint8_t(i*3);pixels[j+1]=std::uint8_t(i);pixels[j+2]=std::uint8_t(255-i*2);}
            RecordingFrameIdentity identity{pts,500'000'000+pts,i+1,30,1};
            const bool admitted=Wait([&]{return writer.CanAcceptVideoFrame();});
            if(!admitted)VideoRecorderTestAccess::PrintAdmission(writer,i);
            Require(admitted,"native encoder admission advances");
            Require(writer.AddFrame(pixels.data(),width*4,identity),writer.LastError().c_str());
            Require(writer.LastAdmittedVideoTimestamp100ns()==pts,"capture admission remains exact at 100 ns resolution");
        }
        Require(writer.Stop().HasPlayableVideo(),"silent RAM encoder finalizes actual video");
        Require(writer.OutputPath().empty() && !writer.Archive(),"RAM writer creates no file/archive");
    }
    auto source=output->Seal();Require(source && source->ByteSize()>0 && source->Capacity()<=VideoRecapPolicy::kSegmentBytes,"finalized coded heap bytes");
    Microsoft::WRL::ComPtr<IMFByteStream> header;
    Require(SUCCEEDED(source->OpenReadStream(&header)),"read native container header");
    std::vector<std::uint8_t> prefix(std::min<std::size_t>(64*1024,source->ByteSize()));ULONG bytesRead{};
    Require(SUCCEEDED(header->Read(prefix.data(),static_cast<ULONG>(prefix.size()),&bytesRead)),"inspect native container timebase");
    prefix.resize(bytesRead);const auto timescale=TrackTimeScale(prefix);
    Require(timescale>0,"actual in-memory mdhd supplies the representation timebase");
    Microsoft::WRL::ComPtr<IMFByteStream> input;Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    Require(SUCCEEDED(source->OpenReadStream(&input)) &&
        SUCCEEDED(MFCreateSourceReaderFromByteStream(input.Get(),nullptr,&reader)),"inspect actual compressed frame inventory");
    unsigned count{};std::int64_t minimumDelta{},maximumDelta{},firstError{},previousNative=-1,previousCapture=-1,lastDuration{};
    // Compare in rational container units: one measured mdhd tick, plus one
    // 100-ns integer conversion unit. This is not a guessed millisecond margin.
    const auto quantizationUnits=10'000'000LL+timescale;
    for(unsigned reads=0;reads<frames+4;++reads) {
        DWORD flags{};LONGLONG pts{};Microsoft::WRL::ComPtr<IMFSample> sample;
        Require(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&pts,&sample)),"read actual compressed frame");
        if(sample) {
            Require(count<frames,"RAM encoder cannot duplicate captures across a gap");
            const auto expected=std::int64_t(count)*10'000'000/30+(count>=32?10'000'000:0);
            std::printf("capture_frame=%u capture100ns=%lld native100ns=%lld signed_error100ns=%lld container_tick_floor=%lld\n",
                count,expected,pts,pts-expected,expected*timescale/10'000'000);
            if(pts!=expected && !firstError)firstError=pts-expected;
            minimumDelta=std::min(minimumDelta,pts-expected);maximumDelta=std::max(maximumDelta,pts-expected);
            Require(pts>previousNative,"native sample PTS strictly advances without duplicate pictures");
            Require(std::llabs(pts-expected)*timescale<=quantizationUnits,"native PTS stays within one actual container tick plus conversion rounding");
            if(previousNative>=0)Require(std::llabs((pts-previousNative)-(expected-previousCapture))*timescale<=2*quantizationUnits,
                "real capture intervals and holes survive container conversion");
            Require(SUCCEEDED(sample->GetSampleDuration(&lastDuration)) && lastDuration>0,"native sample has real duration");
            previousNative=pts;previousCapture=expected;
            ++count;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
    }
    Require(count==frames,"every submitted capture produces exactly one compressed picture");
    Require(std::llabs(previousNative+lastDuration-previousCapture-10'000'000/30)*timescale<=2*quantizationUnits,
        "native end duration and drift stay bounded across all fragments");
    std::printf("native coded inventory=%u actual captures=%u timescale=%u tick100ns=10000000/%u first_signed_error=%lld PTS_min_delta=%lld PTS_max_delta=%lld native_end100ns=%lld\n",
        count,frames,timescale,timescale,firstError,minimumDelta,maximumDelta,previousNative+lastDuration);
    return source;
}
void NativeContract() {
    auto source=Encode();VideoPlayback player;
    auto generation=player.OpenMemory(source);
    Require(Wait([&]{const auto s=player.Snapshot();return s.frame && s.frame->requestGeneration==generation;}),player.Snapshot().error.c_str());
    auto first=player.Snapshot();Require(!first.file.valid && first.memorySource==source,"opaque memory identity, no fake file ID");
    Require(!first.hasAudio && !first.audioAvailable && player.TakeAudio().empty(),"recap has no audio track or audio output");
    Require(first.frame->pts100ns==0 && first.frame->bgra,"first coded access point decodes");
    auto shown=first.frame;
    generation=player.Seek100ns(15'000'000);
    const auto newest=player.Seek100ns(25'000'000);
    Require(newest>generation,"seek generations advance");
    Require(Wait([&]{const auto s=player.Snapshot();return s.frame && s.frame->requestGeneration==newest;}),"latest RAM seek completes");
    const auto sought=player.Snapshot();Require(sought.frame->pts100ns>=25'000'000,"capture gap is not compacted");
    Require(!player.PauseAtFrame(shown),"pre-seek frame cannot authorize current pause");
    Require(player.PauseAtFrame(sought.frame),"exact issued current memory frame pauses");
    auto copy=std::make_shared<VideoPlaybackFrame>(*sought.frame);Require(!player.PauseAtFrame(copy),"copied metadata cannot forge decoder lease");
    Require(Wait([&]{auto s=player.Snapshot();return s.neighborFrame || s.state==VideoPlaybackState::Ended;}),"real bounded neighbour preparation completes");
    const auto paused=player.Snapshot();
    if(paused.neighborFrame)Require(paused.neighborFrame->pts100ns>paused.frame->pts100ns &&
        paused.neighborFrame->pts100ns-paused.frame->pts100ns<=2'500'000,"neighbour has distinct genuine <=250ms PTS");
    player.Close();Require(sought.frame->bgra && !sought.frame->bgra->empty(),"decoder cancellation cannot dangle pinned pixels");
    // Production async producer: drive synthetic captures while its viewer is
    // paused; no file, device, application window or audio endpoint is opened.
    RamVideoRecap recap;recap.Reset(100,0);
    auto pixels=std::make_shared<const std::vector<std::uint8_t>>(192*128*4,73);
    std::int64_t clock=1'000'000'000;unsigned accepted{};
    for(unsigned i=0;i<150;++i,clock+=333'334) {
        VideoRecapFrame f{pixels,{},192,128,{clock,clock,i+1,30,1}};
        if(recap.Submit(std::move(f)))++accepted;
        if(i==0)Require(Wait([&]{return recap.Snapshot().admittedFrames>0 || !recap.Snapshot().error.empty();}),"async encoder startup completes");
        std::this_thread::sleep_for(5ms);
    }
    Require(accepted>2,"bounded async producer accepts synthetic camera frames");
    Require(Wait([&]{return recap.Snapshot().available;}),recap.Snapshot().error.c_str());
    auto range=recap.Snapshot();auto pinned=recap.Acquire(range.first100ns);Require(bool(pinned),"paused recap lease");
    const auto before=range.admittedFrames;
    for(unsigned i=150;i<180;++i,clock+=333'334) {
        recap.Submit({pixels,{},192,128,{clock,clock,i+1,30,1}});std::this_thread::sleep_for(5ms);
    }
    Require(Wait([&]{return recap.Snapshot().admittedFrames>before;}),"camera keeps buffering while consumer holds old scene");
    Require(recap.Snapshot().queuedBytes<=VideoRecapPolicy::kQueueBytes &&
        recap.Snapshot().queuedFrames<=VideoRecapPolicy::kQueueFrames,"async queue stays bounded");
    recap.Reset(101,1);Require(!recap.Snapshot().available && pinned->source,"reset revokes old timeline without dangling pinned source");
    std::printf("native max call us=%lld; dropped=%llu; completed=%llu\n",range.maximumNativeCallUs,
        static_cast<unsigned long long>(range.droppedFrames),static_cast<unsigned long long>(range.completedSegments));
}
void Native4k30Contract() {
    PrintMemory("before-4k-codec");
    const auto start=std::chrono::steady_clock::now();auto source=Encode(3840,2160,65);
    PrintMemory("after-4k-encode");
    VideoPlayback player;const auto generation=player.OpenMemory(source);
    Require(Wait([&]{const auto s=player.Snapshot();return s.frame && s.frame->requestGeneration==generation;},15000ms),player.Snapshot().error.c_str());
    const auto decoded=player.Snapshot();Require(decoded.frame->width==3840 && decoded.frame->height==2160,"native 4K30 pixels preserved");
    Require(decoded.frame->pts100ns==0 && decoded.frame->duration100ns>=333'000 && decoded.frame->duration100ns<=334'000,"native 30 FPS sample cadence");
    Require(!decoded.hasAudio && !decoded.file.valid,"native 4K30 stays silent heap media");
    PrintMemory("after-4k-decode");
    std::printf("65-frame native 4K30 capability elapsed_ms=%lld compressed=%zu capacity=%zu decoded_bytes=%zu peak_decoded_bytes=%zu decode_working_bytes=%zu (not sustained camera FPS)\n",
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count(),source->ByteSize(),source->Capacity(),
        decoded.decodedBytes,decoded.peakDecodedBytes,decoded.decodeWorkingBytes);
}
}
int main(int argc,char** argv) {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const auto mf=SUCCEEDED(com)?MFStartup(MF_VERSION):E_FAIL;
    int result=0;
    try {Require(SUCCEEDED(mf),"Media Foundation fixture prerequisite");
        if(argc>1 && std::string(argv[1])=="--native-codec")NativeContract();
        else if(argc>1 && std::string(argv[1])=="--native-4k30")Native4k30Contract();else RingContract();
        std::puts("PASS: live RAM recap production seams");
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());result=1;}
    if(SUCCEEDED(mf))MFShutdown();if(SUCCEEDED(com))CoUninitialize();return result;
}
