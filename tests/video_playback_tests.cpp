#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/common/media_writer.hpp"
#include "../src/common/video_playback_seek_budget.hpp"
#include "../src/common/video_ffmpeg_reader.hpp"
#include <mfapi.h>
#include <mfreadwrite.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <numeric>
#include <thread>

namespace okuflow {
struct VideoMemoryStreamTestAccess {
    static std::shared_ptr<const VideoMemorySource> SealBytes(const std::vector<char>& bytes) {
        VideoMemoryStream output(bytes.size()+64);
        if (FAILED(output.BeginRecording())) return {};
        Microsoft::WRL::ComPtr<IStream> cursor;
        if (FAILED(output.OpenWriteCursor(&cursor))) return {};
        ULONG written{};
        const auto status = cursor->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&written);
        cursor.Reset(); output.FinishRecording(SUCCEEDED(status) && written==bytes.size());
        return output.Seal();
    }
};
}

namespace {
using namespace okuflow;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
int failures{};
void Expect(bool ok,const char* message);
std::optional<LUID> gpuAdapter;
void ConfigureGpu(VideoPlayback& player) {if(gpuAdapter)player.SetGpuAdapter(gpuAdapter);}
void RequireGpu(const VideoPlaybackSnapshot& snapshot) {
    if(!gpuAdapter)return;
    if(!snapshot.gpuResident)std::cerr<<"GPU fallback diagnostic: "<<snapshot.backendReason<<" error="<<snapshot.error<<'\n';
    Expect(snapshot.gpuResident && snapshot.frame && snapshot.frame->gpu && !snapshot.frame->bgra,
        "GPU opt-in fixture publishes actual resident texture pixels without CPU BGRA");
}
class PrewriteProbe final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFByteStream> {
public:
    PrewriteProbe(IMFByteStream* inner, std::function<void()> probe, bool unknownPosition = false)
        : inner_(inner), probe_(std::move(probe)), unknownPosition_(unknownPosition) {}
    STDMETHODIMP GetCapabilities(DWORD* x) override { return inner_->GetCapabilities(x); }
    STDMETHODIMP GetLength(QWORD* x) override { return inner_->GetLength(x); }
    STDMETHODIMP SetLength(QWORD x) override { probe_(); return inner_->SetLength(x); }
    STDMETHODIMP GetCurrentPosition(QWORD* x) override { return unknownPosition_ ? E_FAIL : inner_->GetCurrentPosition(x); }
    STDMETHODIMP SetCurrentPosition(QWORD x) override { return inner_->SetCurrentPosition(x); }
    STDMETHODIMP IsEndOfStream(BOOL* x) override { return inner_->IsEndOfStream(x); }
    STDMETHODIMP Read(BYTE* p, ULONG n, ULONG* got) override { return inner_->Read(p, n, got); }
    STDMETHODIMP BeginRead(BYTE* p, ULONG n, IMFAsyncCallback* c, IUnknown* s) override { return inner_->BeginRead(p,n,c,s); }
    STDMETHODIMP EndRead(IMFAsyncResult* r, ULONG* n) override { return inner_->EndRead(r,n); }
    STDMETHODIMP Write(const BYTE* p, ULONG n, ULONG* got) override { probe_(); return inner_->Write(p,n,got); }
    STDMETHODIMP BeginWrite(const BYTE* p, ULONG n, IMFAsyncCallback* c, IUnknown* s) override { probe_(); return inner_->BeginWrite(p,n,c,s); }
    STDMETHODIMP EndWrite(IMFAsyncResult* r, ULONG* n) override { return inner_->EndWrite(r,n); }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN o, LONGLONG n, DWORD f, QWORD* p) override { return inner_->Seek(o,n,f,p); }
    STDMETHODIMP Flush() override { return inner_->Flush(); }
    STDMETHODIMP Close() override { return inner_->Close(); }
private: ComPtr<IMFByteStream> inner_; std::function<void()> probe_; bool unknownPosition_{};
};
void Expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
std::filesystem::path TempPath() {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    GUID id{}; CoCreateGuid(&id); wchar_t token[40]{}; StringFromGUID2(id, token, 40);
    return std::filesystem::path(temp) / (std::wstring(L"okuflow-playback-") + token + L".mp4");
}
template<class Predicate> bool Wait(Predicate predicate, int milliseconds = 4000) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while (Clock::now() < end);
    return predicate();
}
void LayoutAndIdentity() {
    Expect(VideoPlaybackLayoutFits(1920, 1080, 7680), "1080p admitted");
    Expect(VideoPlaybackLayoutFits(3840, 2160, -15360), "bounded negative stride admitted");
    Expect(!VideoPlaybackLayoutFits(0, 1080, 7680), "zero width rejected");
    Expect(!VideoPlaybackLayoutFits(8192, 8192, 32768), "oversized decoded frame rejected");
    Expect(!VideoPlaybackLayoutFits(1920, 1080, 400), "short stride rejected");
    const auto uhd=PlanVideoPlaybackMemory(3840,2160,15360);
    const auto dci=PlanVideoPlaybackMemory(4096,2160,16384+256);
    const auto archivePlan=PlanVideoPlaybackMemory(3840,2160,15360,1,1,true);
    Expect(uhd && dci && archivePlan,"UHD and padded DCI working sets admitted");
    if(uhd && dci && archivePlan) {
        Expect(uhd->byteLimit>96u*1024*1024 && dci->byteLimit<=VideoPlayback::kDecodedByteLimit,
            "adaptive 4K capacity has working and retained-scene headroom under hard ceiling");
        Expect(uhd->retainedBytes>=uhd->frameBytes*3 &&
            uhd->byteLimit-uhd->retainedBytes>=uhd->stagingBytes+uhd->frameBytes,
            "retained capacity leaves explicit staging plus replacement reserve");
        Expect(uhd->lookaheadFrames==2 && archivePlan->lookaheadFrames==1,
            "saved video and archive retain distinct bounded lookahead policies");
    }
    const auto large=PlanVideoPlaybackMemory(4096,2560,16384);
    Expect(large && large->lookaheadFrames==1 && large->byteLimit<=VideoPlayback::kDecodedByteLimit,
        "40 MiB frames reduce optional lookahead rather than over-promising the working set");
    const auto expanded=PlanVideoPlaybackMemory(1920,1080,7680,2,1);
    Expect(expanded && expanded->frameBytes>expanded->stagingBytes &&
        expanded->workingBytes==expanded->frameBytes+expanded->stagingBytes,
        "square-pixel expansion reserves destination separately from native staging");
    Expect(PlanVideoPlaybackMemory(4096,2730,16384).has_value() &&
        !PlanVideoPlaybackMemory(4096,2731,16384),"minimum working-set boundary rejects the next row before allocation");
    Expect(!PlanVideoPlaybackMemory(7680,4320,30720),"8K rejected before decoding an unaffordable working set");
    Expect(!PlanVideoPlaybackMemory(3840,2160,15360,UINT32_MAX,1) &&
        !PlanVideoPlaybackMemory(3840,2160,INT64_MIN),"aspect expansion and stride overflow rejected");
    constexpr auto maximumClock = std::numeric_limits<std::int64_t>::max() / 100;
    Expect(VideoPlaybackTimestampFits(0) && VideoPlaybackTimestampFits(maximumClock) &&
        VideoPlaybackTimestampFits(maximumClock - 1, 1), "representable timestamp endpoints admitted");
    Expect(!VideoPlaybackTimestampFits(maximumClock, 1) && !VideoPlaybackTimestampFits(-1) &&
        !VideoPlaybackTimestampFits(0, -1) && !VideoPlaybackTimestampFits(0, INT64_MAX),
        "timestamp sums reject overflow and unrepresentable clock ranges");
    const auto path = TempPath();
    { std::ofstream file(path, std::ios::binary); file << "identity fixture"; }
    VideoFileIdentity first, second; ComPtr<IMFByteStream> a, b;
    VideoReadStreamClone clone;
    Expect(SUCCEEDED(OpenStableVideoReadStream(path.wstring(), &a, &first,&clone)), "stable held handle opened");
    Expect(SUCCEEDED(OpenStableVideoReadStream(path.wstring(), &b, &second)) && first.SameFile(second), "same held identity");
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Expect(writer == INVALID_HANDLE_VALUE, "saved source denied concurrent writes");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    if (a) { BYTE bytes[32]{}; ULONG read{}; Expect(SUCCEEDED(a->Read(bytes, 16, &read)) && read == 16, "same admitted handle is readable"); }
    ComPtr<IMFByteStream> independent;
    Expect(clone && SUCCEEDED(clone(&independent)),"CPU fallback clones the held backing without reopening its pathname");
    if(a && independent) {
        QWORD position{};BYTE bytes[4]{};ULONG read{};
        Expect(SUCCEEDED(a->SetCurrentPosition(7)) && SUCCEEDED(independent->Read(bytes,4,&read)) &&
            read==4 && std::memcmp(bytes,"iden",4)==0 && SUCCEEDED(a->GetCurrentPosition(&position)) && position==7,
            "GPU-retained reader and CPU successor have independent logical cursors");
        a.Reset();b.Reset();
        Expect(SUCCEEDED(independent->SetCurrentPosition(0)) && SUCCEEDED(independent->Read(bytes,4,&read)) && read==4,
            "cloned read stream keeps the exact held file alive after original wrappers retire");
    }
    independent.Reset();clone={};a.Reset();b.Reset();std::error_code ec;std::filesystem::remove(path,ec);
}
void MutationObservation() {
    const auto path = TempPath();
    auto archive = std::make_shared<RecordingArchive>(path.wstring());
    ComPtr<IMFByteStream> output, observed;
    Expect(SUCCEEDED(CreateRecordingArchiveOutputStream(path.wstring(), &output)), "exclusive shared-readable archive fixture created");
    std::uint64_t generationSeenBeforeNativeWrite{};
    auto probe = Microsoft::WRL::Make<PrewriteProbe>(output.Get(), [&] {
        generationSeenBeforeNativeWrite = archive->Snapshot().generation;
    });
    Expect(SUCCEEDED(RecordingArchive::Observe(probe.Get(), archive, &observed)), "observer installed");
    if (!observed) return;
    // An incomplete box must never become a seekable frame merely because
    // Write succeeded or the physical file grew.
    const BYTE partial[]{0,0,0,32, 'm','o','o','f', 0,0,0,0}; ULONG written{};
    Expect(SUCCEEDED(observed->Write(partial, sizeof(partial), &written)), "partial fragment write forwarded");
    const auto before = archive->Snapshot();
    Expect(before.committedBytes == sizeof(partial), "successful physical extent recorded");
    Expect(before.seekableEnd100ns == -1 && before.verifiedMediaBytes == 0, "partial metadata not seekable");
    Expect(!archive->ValidateRange(before.generation, 0, sizeof(partial)), "no unverified read leases");
    observed->SetCurrentPosition(4);
    const BYTE replacement[]{'f','r','e','e'};
    Expect(SUCCEEDED(observed->Write(replacement, sizeof(replacement), &written)), "native backwrite forwarded");
    Expect(generationSeenBeforeNativeWrite > before.generation,
        "mutation generation revoked before native backwrite starts, without waiting on parser");
    Expect(archive->Snapshot().generation > before.generation, "backwrite synchronously revoked previous generation");
    archive->WriterAccepted(123);
    Expect(archive->Snapshot().latestWriterAccepted100ns == 123 && archive->Snapshot().seekableEnd100ns == -1,
        "writer acceptance is separate from seekable PTS");
    archive->Abandoned();
    Expect(archive->Snapshot().availability == ActiveRewindAvailability::Failed, "abandonment revokes active proof");
    observed.Reset(); probe.Reset(); output.Reset(); archive.reset();
    std::error_code ec; std::filesystem::remove(path, ec);
}
void UnsupportedAndUnknownWrites() {
    const auto path = TempPath();
    auto archive = std::make_shared<RecordingArchive>(path.wstring());
    ComPtr<IMFByteStream> output, observed;
    Expect(SUCCEEDED(CreateRecordingArchiveOutputStream(path.wstring(), &output)), "unsupported-box fixture created");
    Expect(SUCCEEDED(RecordingArchive::Observe(output.Get(), archive, &observed)), "unsupported-box observer installed");
    if (!observed) return;
    const BYTE unsupported[]{0,0,0,8,'m','o','o','v'}; ULONG written{};
    Expect(SUCCEEDED(observed->Write(unsupported, sizeof(unsupported), &written)), "complete unsupported moov submitted");
    Expect(Wait([&] { return archive->Snapshot().availability == ActiveRewindAvailability::UnsupportedContainer; }),
        "unsupported complete metadata is classified");
    const auto before = archive->Snapshot();
    const BYTE append[]{0,0,0,8,'f','r','e','e'};
    Expect(SUCCEEDED(observed->Write(append, sizeof(append), &written)), "unrelated append after unsupported metadata");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto after = archive->Snapshot();
    Expect(after.generation == before.generation && after.parserBoxAttempts == before.parserBoxAttempts &&
        after.parserBoxAttempts <= 1, "unsupported box is not reparsed on timer or same-generation appends");
    observed.Reset(); output.Reset(); archive.reset();
    std::error_code ec; std::filesystem::remove(path, ec);

    const auto unknownPath = TempPath();
    archive = std::make_shared<RecordingArchive>(unknownPath.wstring());
    Expect(SUCCEEDED(CreateRecordingArchiveOutputStream(unknownPath.wstring(), &output)), "unknown-position fixture created");
    std::uint64_t fencedGeneration{};
    auto probe = Microsoft::WRL::Make<PrewriteProbe>(output.Get(), [&] {
        fencedGeneration = archive->Snapshot().generation;
    }, true);
    Expect(SUCCEEDED(RecordingArchive::Observe(probe.Get(), archive, &observed)), "unknown-position observer installed");
    const auto initialGeneration = archive->Snapshot().generation;
    if (observed) {
        Expect(SUCCEEDED(observed->Write(append, sizeof(append), &written)) && written == sizeof(append),
            "unobservable write still reaches the Original output");
        const auto unknown = archive->Snapshot();
        Expect(fencedGeneration > initialGeneration && unknown.availability == ActiveRewindAvailability::Invalidated &&
            unknown.committedBytes == 0 && unknown.verifiedMediaBytes == 0 &&
            !archive->ValidateRange(unknown.generation, 0, 0),
            "unknown position revokes before native write and never grants fake prefix credit");
    }
    observed.Reset(); probe.Reset(); output.Reset(); archive.reset();
    std::filesystem::remove(unknownPath, ec);
}
void FailedOpenSupersession() {
    VideoPlayback player;ConfigureGpu(player);
    const auto first = player.OpenFile(TempPath().wstring());
    const auto second = player.OpenFile(TempPath().wstring());
    Expect(second > first, "open generations monotonic");
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == second && s.state == VideoPlaybackState::Unavailable; }),
        "latest failed open owns publication");
    Expect(!player.Snapshot().frame, "failed opens invent no frame");
    player.Close(); Expect(player.Snapshot().state == VideoPlaybackState::Closed, "close is immediate and worker-owned");
}
bool nativeSkipped{};
void FinalizationRetainsRevokedMapping(const std::filesystem::path& source) {
    std::ifstream input(source,std::ios::binary|std::ios::ate);
    const auto length=input.tellg();
    Expect(length>0 && length<16*1024*1024,"private finalized fixture fits bounded copy");
    if(length<=0 || length>=16*1024*1024)return;
    std::vector<BYTE> bytes(static_cast<std::size_t>(length));input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    const auto path=TempPath();auto archive=std::make_shared<RecordingArchive>(path.wstring());
    ComPtr<IMFByteStream> file,observed;
    Expect(SUCCEEDED(CreateRecordingArchiveOutputStream(path.wstring(),&file)),"private mapping fixture created");
    if(!file)return;
    Expect(SUCCEEDED(RecordingArchive::Observe(file.Get(),archive,&observed)),"mapping observer installed");
    if(!observed)return;
    ULONG written{};
    Expect(SUCCEEDED(observed->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&written)) && written==bytes.size(),"complete native fixture copied through write observer");
    const bool proved=Wait([&]{return archive->Snapshot().duration100ns>0;});
    Expect(proved,"native copied fragments establish duration before mutation");
    const auto before=archive->Snapshot();
    // A producer rewrite revokes bytes before the parser can re-establish
    // them. Truncation prevents a race from accidentally restoring the proof.
    Expect(SUCCEEDED(observed->SetLength(0)),"producer truncation revokes read ranges");
    archive->Finalized(false);
    const auto after=archive->Snapshot();
    Expect(!proved || after.duration100ns>=before.duration100ns,"finalization restores prior proven mapping after write revocation");
    Expect(after.seekableEnd100ns==-1 && !archive->ValidateRange(before.generation,0,1),"retained mapping cannot authorize revoked media bytes");
    observed.Reset();file.Reset();archive.reset();
    std::error_code error;std::filesystem::remove(path,error);
}

void LongGopSeeking(VideoPlayback& player) {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"data"/"long-gop-h264.mp4";
    auto request=player.OpenFile(path.wstring());
    VideoPlaybackSnapshot snapshot;
    const auto settled=[&] {
        snapshot=player.Snapshot();
        return snapshot.requestGeneration==request &&
            (snapshot.state==VideoPlaybackState::Paused || snapshot.state==VideoPlaybackState::Unavailable);
    };
    Expect(Wait(settled) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused,
           "committed long-GOP source opens without an encoder or GPU");
    if(!snapshot.frame)return;
    {
        const auto openingFrame = snapshot.frame;
        const auto openingFile = snapshot.file;
        Expect(openingFrame->pts100ns == 0 && openingFrame->sampleId == 0,
               "initial paused selection retains real frame-zero PTS and sample identity");
        Expect(Wait([&] {
            snapshot = player.Snapshot();
            return snapshot.requestGeneration == request && snapshot.neighborFrame;
        }), "initial paused Open obtains a real following neighbour without Play or Pause");
        Expect(snapshot.frame == openingFrame && snapshot.state == VideoPlaybackState::Paused &&
            snapshot.file.SameFile(openingFile) && snapshot.neighborFrame &&
            snapshot.neighborFrame->requestGeneration == request &&
            snapshot.neighborFrame->sampleId != openingFrame->sampleId &&
            snapshot.neighborFrame->pts100ns > openingFrame->pts100ns &&
            snapshot.neighborFrame->pts100ns - openingFrame->pts100ns <= 2'500'000,
            "initial neighbour arrival preserves the exact selected lease and bounded real PTS gap");
        Expect(snapshot.readyFrames <= snapshot.lookaheadFrames && snapshot.decodedBytes <= snapshot.decodedByteLimit &&
            player.TakeAudio().empty(), "initial paused neighbour stays in the existing silent decoded quotas");
    }
    snapshot={};request=player.Seek100ns(30'000'000);
    Expect(Wait(settled) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused && snapshot.frame->pts100ns>=30'000'000,
           "long-GOP seek reaches three seconds beyond 120 preroll reads");
    std::cout<<"Long-GOP regression reads="<<snapshot.lastRequestReadCount<<" latencyUs="<<snapshot.lastRequestLatencyUs<<" error="<<snapshot.error<<'\n';
    player.Seek100ns(10'000'000);request=player.Seek100ns(40'000'000);snapshot={};
    Expect(Wait(settled) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused && snapshot.frame->pts100ns>=40'000'000,
           "new long-GOP seek supersedes a waiting target");
    if(!detail::FfmpegPlaybackRequested()) {
        request=player.UseCpuFallback("fixture consumer interop refusal");snapshot={};
        Expect(Wait(settled) && snapshot.state==VideoPlaybackState::Paused &&
            snapshot.backendReason=="fixture consumer interop refusal","native CPU fallback preserves its exact existing diagnostic reason");
    }
    player.Close();
}

// Original raw AVI, three distinct colour grids two seconds apart. This
// tests native sample duration/issuance without an encoder or private media.
std::vector<char> SparseAviBytes() {
    using Bytes = std::vector<char>;
    const auto u32 = [](Bytes& bytes,std::uint32_t value) {
        for (int shift=0;shift<32;shift+=8) bytes.push_back(char((value>>shift)&255));
    };
    const auto append = [](Bytes& bytes,const Bytes& other) { bytes.insert(bytes.end(),other.begin(),other.end()); };
    const auto chunk = [&](const char* kind,Bytes data) {
        Bytes result(kind,kind+4); u32(result,std::uint32_t(data.size())); append(result,data);
        if (data.size()%2) result.push_back(0); return result;
    };
    const auto list = [&](const char* kind,const Bytes& data) {
        Bytes contents(kind,kind+4); append(contents,data); return chunk("LIST",std::move(contents));
    };
    constexpr std::uint32_t width=32,height=16,frameBytes=width*height*4,count=3;
    Bytes main; for (auto value : {2'000'000u,frameBytes/2,0u,0x10u,count,0u,1u,frameBytes,width,height,0u,0u,0u,0u}) u32(main,value);
    const char streamType[]="vidsDIB "; Bytes stream(streamType,streamType+8); u32(stream,0); u32(stream,0);
    for (auto value : {0u,2u,1u,0u,count,frameBytes,0xffffffffu,0u}) u32(stream,value);
    for (char value : {char(0),char(0),char(0),char(0),char(width),char(0),char(height),char(0)}) stream.push_back(value);
    Bytes format; for (auto value : {40u,width,height}) u32(format,value);
    for (char value : {char(1),char(0),char(32),char(0)}) format.push_back(value);
    for (auto value : {0u,frameBytes,0u,0u,0u,0u}) u32(format,value);
    Bytes strl=chunk("strh",stream); append(strl,chunk("strf",format));
    Bytes header=chunk("avih",main); append(header,list("strl",strl));
    Bytes frames,index;
    for (std::uint32_t frame=0;frame<count;++frame) {
        Bytes pixels(frameBytes,0);
        for (std::uint32_t y=0;y<height;++y) for (std::uint32_t x=0;x<width;++x) {
            const auto at=(y*width+x)*4;
            pixels[at]=char(30+60*frame);pixels[at+1]=char(x*7);pixels[at+2]=char(y*13);pixels[at+3]=char(255);
        }
        for (const char* tag={"00db"}; *tag; ++tag) index.push_back(*tag);
        u32(index,0x10);u32(index,std::uint32_t(frames.size())+4);u32(index,frameBytes);
        append(frames,chunk("00db",std::move(pixels)));
    }
    const char type[]="AVI "; Bytes body(type,type+4);append(body,list("hdrl",header));
    append(body,list("movi",frames));append(body,chunk("idx1",index));return chunk("RIFF",std::move(body));
}

void InitialSparseSavedFileNeighbour(VideoPlayback& player) {
    const auto bytes=SparseAviBytes();auto path=TempPath();path.replace_extension(L".avi");
    { std::ofstream file(path,std::ios::binary);file.write(bytes.data(),std::streamsize(bytes.size()));
      Expect(bool(file),"first-party sparse raw AVI fixture is written"); }
    auto generation=player.OpenFile(path.wstring());
    const auto settled=[&] { const auto s=player.Snapshot(); return s.requestGeneration==generation && s.frame && s.state==VideoPlaybackState::Paused; };
    Expect(Wait(settled),"sparse saved file opens paused without Play");
    auto selected=player.Snapshot().frame;
    if (!selected) {player.Close();return;}
    Expect(selected->sampleId==0 && selected->pts100ns==0 && selected->duration100ns==20'000'000,
        "native first sample supplies exact zero PTS and two-second duration");
    const auto policy=player.CorroborationPolicy(selected);
    Expect(policy.Allows(0,0,20'000'000,2000),"native saved-file cadence authorizes its actual two-second following sample");
    Expect(Wait([&] { const auto s=player.Snapshot(); return s.frame==selected && s.neighborFrame && s.neighborFrame->pts100ns==20'000'000; }),
        "initial sparse selected frame receives the genuine nearest following frame without Play");
    const auto after=player.Snapshot();
    Expect(after.frame==selected && after.state==VideoPlaybackState::Paused && after.neighborFrame &&
        after.neighborFrame->sampleId==20'000'000 && after.readyFrames<=after.lookaheadFrames &&
        after.decodedBytes<=after.decodedByteLimit,"sparse preparation preserves selected identity and existing quotas");
    const auto copied=std::make_shared<VideoPlaybackFrame>(*selected);
    const auto borrowed=std::shared_ptr<const VideoPlaybackFrame>(selected.get(),[](const VideoPlaybackFrame*){});
    Expect(player.CorroborationPolicy(copied)==uct::ImmutableCorroborationPolicy{} &&
        player.CorroborationPolicy(borrowed)==uct::ImmutableCorroborationPolicy{},
        "copied metadata and a borrowed ownership block cannot issue saved cadence");
    {VideoPlayback foreign;Expect(foreign.CorroborationPolicy(selected)==uct::ImmutableCorroborationPolicy{},
        "a foreign player cannot authorize this native lease");}
    copied->requestGeneration++;copied->duration100ns=25'000'000;
    Expect(player.CorroborationPolicy(copied)==uct::ImmutableCorroborationPolicy{},"forged generation/duration cannot expand proof");
    generation=player.Seek100ns(0);Expect(Wait(settled),"same-frame seek obtains a new exact native ticket");
    Expect(player.CorroborationPolicy(selected)==uct::ImmutableCorroborationPolicy{} &&
        player.CorroborationPolicy(player.Snapshot().frame)!=policy,"seek revokes old cadence issuance even when pixels are reused");
    player.Close();selected.reset();
    const auto memory=VideoMemoryStreamTestAccess::SealBytes(bytes);
    Expect(bool(memory),"same first-party sparse bytes seal as immutable memory");
    if (memory) {
        generation=player.OpenMemory(memory);Expect(Wait(settled),"sparse RAM source opens paused");
        const auto held=player.Snapshot().frame;
        Expect(held && player.CorroborationPolicy(held)==uct::ImmutableCorroborationPolicy{},
            "RAM/recap origin never acquires saved-file cadence");
        Expect(Wait([&] {return player.Snapshot().readyFrames>0;}),"RAM decoder observes its genuine sparse following sample");
        Expect(player.Snapshot().frame==held && !player.Snapshot().neighborFrame,
            "RAM retains the ordinary 250-ms limit for the identical two-second gap");
    }
    player.Close();
    std::error_code error;std::filesystem::remove(path,error);
}

void PausedSceneCutNeighbor(VideoPlayback& player) {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"data"/"uct-scene-cut-h264.mp4";
    auto request=player.OpenFile(path.wstring());
    const auto settled=[&] {
        const auto s=player.Snapshot();
        return s.requestGeneration==request && s.frame && s.frame->requestGeneration==request &&
            s.state==VideoPlaybackState::Paused;
    };
    Expect(Wait(settled),"first-party scene-cut fixture opens on the system CPU decoder");
    if(!settled())return;
    request=player.Seek100ns(9'000'000);
    Expect(Wait(settled),"scene-cut fixture selects a real pre-cut frame");
    const auto beforeCut=player.Snapshot().frame;
    if(!beforeCut)return;
    const auto blue=[](const std::shared_ptr<const VideoPlaybackFrame>& frame) {
        if(!frame || !frame->bgra || frame->bgra->size()<4)return false;
        const auto at=std::size_t(frame->height/2)*frame->strideBytes+std::size_t(frame->width/2)*4;
        return at+3<frame->bgra->size() && (*frame->bgra)[at] > 180 && (*frame->bgra)[at+2] < 70;
    };
    const auto red=[](const std::shared_ptr<const VideoPlaybackFrame>& frame) {
        if(!frame || !frame->bgra || frame->bgra->size()<4)return false;
        const auto at=std::size_t(frame->height/2)*frame->strideBytes+std::size_t(frame->width/2)*4;
        return at+3<frame->bgra->size() && (*frame->bgra)[at+2] > 180 && (*frame->bgra)[at] < 70;
    };
    const auto describe=[](const char* label,const std::shared_ptr<const VideoPlaybackFrame>& frame) {
        std::cout<<"Scene-cut "<<label<<" pts="<<(frame?frame->pts100ns:-1)
            <<" sample="<<(frame?frame->sampleId:0);
        if(frame && frame->bgra) {
            const auto at=std::size_t(frame->height/2)*frame->strideBytes+std::size_t(frame->width/2)*4;
            if(at+3<frame->bgra->size())std::cout<<" center BGRA="<<unsigned((*frame->bgra)[at])<<','
                <<unsigned((*frame->bgra)[at+1])<<','<<unsigned((*frame->bgra)[at+2])<<','<<unsigned((*frame->bgra)[at+3]);
        }
        std::cout<<'\n';
    };
    describe("before",beforeCut);
    Expect(beforeCut->pts100ns==9'000'000 && red(beforeCut),"pre-cut source is the real red 0.9-second frame");
    player.SetMuted(false);
    player.SetPlaying(true);
    std::shared_ptr<const VideoPlaybackFrame> selected;
    auto preceding=beforeCut;
    Expect(Wait([&] {
        const auto s=player.Snapshot();
        if(s.frame && s.frame->sampleId!=preceding->sampleId) {
            if(blue(s.frame))selected=s.frame;
            else {
                Expect(red(s.frame),"every observed pre-cut image contains genuine red pixels");
                preceding=s.frame;
            }
        }
        return bool(selected);
    }),"playback reaches the first post-cut image");
    if(!selected)return;
    describe("last-red",preceding);
    describe("selected",selected);
    const auto selectedPts=selected->pts100ns;
    std::cout<<"Scene-cut authored PTS=10000000 native first-blue PTS="<<selectedPts
        <<" delta100ns="<<selectedPts-10'000'000<<'\n';
    Expect(red(preceding) && blue(selected) && preceding->sampleId!=selected->sampleId &&
        selectedPts-preceding->pts100ns==1'000'000,
        "the selected native blue image has a genuinely distinct red predecessor exactly 100 ms earlier");
    const auto selectedId=selected->sampleId;
    Expect(player.PauseAtFrame(selected),"pause accepts its exact issued displayed frame");
    Expect(player.TakeAudio().empty(),"the silent fixture has no queued PCM after pause");
    Expect(Wait([&] {
        const auto s=player.Snapshot();
        Expect(s.frame==selected && s.frame->sampleId==selectedId && s.frame->pts100ns==selectedPts,
            "optional following proof never advances the held image, sample or PTS");
        return s.neighborFrame && s.neighborFrame->pts100ns>selected->pts100ns;
    }),"pause publishes a genuinely decoded following neighbour instead of the pre-cut predecessor");
    auto paused=player.Snapshot();
    describe("following",paused.neighborFrame);
    Expect(paused.state==VideoPlaybackState::Paused && paused.requestGeneration==request &&
        paused.neighborFrame && paused.neighborFrame->sampleId!=selectedId &&
        paused.neighborFrame->pts100ns==selectedPts+1'000'000 && blue(paused.neighborFrame),
        "post-cut proof is a real blue sample exactly 100 ms later with its unchanged source identity");
    Expect(paused.readyFrames<=paused.lookaheadFrames && paused.decodedBytes<=paused.decodedByteLimit,
        "neighbour shares the bounded ready-frame leases and existing byte charges");
    const auto following=paused.neighborFrame;
    Expect(player.PauseAtFrame(selected),"an exact repeated pause retains its original acquisition");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    paused=player.Snapshot();
    Expect(paused.frame==selected && paused.neighborFrame==following && player.TakeAudio().empty(),
        "repeated pause retains the same selected/following leases and the silent PCM queue stays empty");

    auto copied=std::make_shared<VideoPlaybackFrame>(*selected);
    Expect(!player.PauseAtFrame(copied),"copied numeric identity and opaque issuance record cannot authorize another frame object");
    auto otherOwner=std::make_shared<int>(0);
    const std::shared_ptr<const VideoPlaybackFrame> borrowed(otherOwner,selected.get());
    Expect(!player.PauseAtFrame(borrowed),"a borrowed pointer with a different ownership block is not the issued frame lease");
    VideoPlayback foreign;
    Expect(!foreign.PauseAtFrame(selected),"an issued frame from a foreign player is refused");
    Expect(player.Snapshot().frame==selected,"refused pause inputs leave the selected image untouched");

    player.SetPlaying(true);
    Expect(Wait([&] {
        const auto s=player.Snapshot();
        return s.frame && s.frame->pts100ns>selected->pts100ns;
    }),"explicit Resume consumes the actual future timeline after paused proof");
    Expect(player.PauseAtFrame(selected),"pause can hold its last actually displayed issued image while the decoder is ahead");
    Expect(player.Snapshot().frame==selected,"decoder-ahead pause restores only the exact held lease, not a numeric substitute");

    // Keep the same request and issued earlier frame through real playback
    // EOS, then replay its actual tail. No new Seek request retickets it.
    player.SetPlaying(true);
    const bool reachedFirstEnd=Wait([&] {return player.Snapshot().state==VideoPlaybackState::Ended;});
    const auto firstEnd=player.Snapshot();
    if(!reachedFirstEnd)std::cerr<<"Held-EOF initial state="<<static_cast<int>(firstEnd.state)
        <<" pts="<<(firstEnd.frame?firstEnd.frame->pts100ns:-1)
        <<" ready="<<firstEnd.readyFrames<<" pressure="<<firstEnd.memoryPressure
        <<" error="<<firstEnd.error<<'\n';
    Expect(reachedFirstEnd,"same-generation playback reaches native EOS before testing the held earlier frame");
    Expect(firstEnd.frame && firstEnd.frame->pts100ns>selectedPts &&
        firstEnd.seekableEnd100ns==firstEnd.frame->pts100ns,
        "initial EOS records the actual final video sample beyond the held selection");
    Expect(player.PauseAtFrame(selected),"an earlier same-generation issued frame remains eligible after decoder EOS");
    const auto rewoundAtEnd=player.Snapshot();
    Expect(rewoundAtEnd.frame==selected && rewoundAtEnd.state==VideoPlaybackState::Paused &&
        rewoundAtEnd.requestGeneration==request && rewoundAtEnd.seekableEnd100ns==firstEnd.seekableEnd100ns,
        "holding the earlier exact frame preserves request identity and the already observed tail bound");
    player.SetPlaying(true);
    std::set<std::int64_t> replayedTail;
    Expect(Wait([&] {
        const auto s=player.Snapshot();
        if(s.frame && s.frame->pts100ns>selectedPts)replayedTail.insert(s.frame->pts100ns);
        Expect(s.requestGeneration==request,"tail replay keeps the original request generation");
        return s.state==VideoPlaybackState::Ended;
    }),"Resume of the held earlier frame completes playback again");
    const auto replayedEnd=player.Snapshot();
    Expect(!replayedTail.empty() && replayedEnd.frame && firstEnd.frame &&
        replayedEnd.frame->pts100ns==firstEnd.frame->pts100ns &&
        replayedEnd.frame->sampleId==firstEnd.frame->sampleId &&
        replayedEnd.seekableEnd100ns==firstEnd.seekableEnd100ns,
        "Resume replays genuine later samples to the same final source sample and unchanged endpoint");
    std::cout<<"Held-EOF replay selected="<<selectedPts<<" firstEnd="<<firstEnd.seekableEnd100ns
        <<" replayedEnd="<<replayedEnd.seekableEnd100ns<<" laterSamples="<<replayedTail.size()<<'\n';

    request=player.Seek100ns(20'000'000);
    Expect(Wait(settled),"a real new seek supersedes paused-neighbour preparation");
    const auto newer=player.Snapshot().frame;
    Expect(!player.PauseAtFrame(selected),"a prior seek generation's genuine frame lease is refused");
    Expect(player.Snapshot().frame==newer && player.Snapshot().requestGeneration==request,
        "late old neighbour/held requests cannot replace the new selected authority");
    request=player.JumpToEnd();
    Expect(Wait([&] {
        const auto s=player.Snapshot();return s.requestGeneration==request && s.frame && s.state==VideoPlaybackState::Ended;
    }),"scene-cut source reaches its actual last decoded image");
    const auto last=player.Snapshot().frame;
    Expect(last && player.PauseAtFrame(last),"EOF retains the exact issued final frame");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto ended=player.Snapshot();
    Expect(ended.frame==last && ended.state==VideoPlaybackState::Ended && !ended.neighborFrame,
        "EOF retains the exact final lease and has no following neighbour");
    player.SetPlaying(true);
    Expect(player.Snapshot().state==VideoPlaybackState::Ended,"proof preparation preserves explicit Resume-at-EOF refusal");
    player.Close();
}

void NativeAvcAac(VideoRecorder::Codec codec = VideoRecorder::Codec::H264, bool strictActive = false) {
    if (!GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC", nullptr, 0)) {
        std::cout << "SKIP opt-in AVC/AAC and active archive: set OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1\n"; return;
    }
    const int priorFailures = failures;
    const auto path = TempPath(); std::filesystem::path clockFencePath;
    VideoRecorder recorder; VideoRecorder::AudioFormat audioFormat;
    // Native AV1 hardware has a larger minimum extent than the CPU decoder.
    // Keep tiny CPU fixtures, but use an admitted hardware-sized GPU sample.
    const UINT width=gpuAdapter?320u:160u,height=gpuAdapter?192u:96u;
    std::wcout << L"Native codec fixture: " << path.wstring() << L'\n';
    const bool started = recorder.Start(path.wstring(), width, height, 30, 1, codec, &audioFormat);
    if (!started && codec == VideoRecorder::Codec::Av1) {
        std::cout << "SKIP native AV1 unavailable: " << recorder.LastError() << '\n'; nativeSkipped = true; return;
    }
    Expect(started, "native selected video codec/AAC recorder starts");
    if (!recorder.IsRecording()) { std::cerr << recorder.LastError() << '\n'; return; }
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4, 255);
    std::vector<std::int16_t> pcm(1600);
    const auto submitFrame = [&](int frame) {
        for (std::size_t p = 0; p < pixels.size(); p += 4) { pixels[p] = static_cast<std::uint8_t>(frame); pixels[p + 1] = 70; pixels[p + 2] = 180; }
        for (std::size_t p = 0; p < pcm.size(); ++p)
            pcm[p] = static_cast<std::int16_t>(4000 * std::sin((frame * 1600 + p) * 0.057595865));
        RecordingFrameIdentity id; id.captureTimestamp100ns = frame * 333333ll; id.captureClock100ns = id.captureTimestamp100ns;
        id.sequenceNumber = frame + 1; id.frameRateNumerator = 30; id.frameRateDenominator = 1;
        const bool videoReady = Wait([&] { return recorder.CanAcceptVideoFrame(); }, 2000);
        Expect(videoReady, "bounded encoder video admission");
        if (!videoReady) return false;
        const bool videoWritten = recorder.AddFrame(pixels.data(), width * 4, id);
        Expect(videoWritten, "native video submitted");
        if (!videoWritten) return false;
        const bool audioReady = Wait([&] { return recorder.CanAcceptAudioFrame(pcm.size() * 2, id.captureTimestamp100ns, 333333); }, 2000);
        Expect(audioReady, "bounded encoder audio admission");
        if (!audioReady) return false;
        const bool audioWritten = recorder.AddAudioFrame(reinterpret_cast<const BYTE*>(pcm.data()), pcm.size() * 2, id.captureTimestamp100ns, 333333);
        Expect(audioWritten, "native audio submitted");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return audioWritten;
    };
    for (int frame = 0; frame < 100 && recorder.IsRecording(); ++frame)
        if (!submitFrame(frame)) break;
    auto archive = recorder.Archive();
    Wait([&] { const auto s = archive->Snapshot(); return s.availability != ActiveRewindAvailability::WaitingForMedia; });
    auto active = archive->Snapshot();
    VideoPlayback player;ConfigureGpu(player);
    std::cout << "Active archive availability=" << static_cast<int>(active.availability) << " pts=" << active.seekableEnd100ns << " reason=" << active.error << '\n';
    if (strictActive) Expect(active.availability == ActiveRewindAvailability::Available,
        "production active fMP4 metadata/config/payload proof before Finalize");
    if (active.availability == ActiveRewindAvailability::Available) {
        const auto g = player.OpenArchive(archive);
        Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == g && s.state != VideoPlaybackState::Opening; }), "active decoder completes bounded open");
        Expect(player.Snapshot().frame != nullptr, "verified active fragment decodes actual pixels");
        Expect(player.CorroborationPolicy(player.Snapshot().frame)==uct::ImmutableCorroborationPolicy{},
            "recording archive never acquires saved-file cadence");
        if(detail::FfmpegPlaybackRequested())Expect(player.Snapshot().decoderBackend=="media-foundation",
            "active recording retains producer-fenced native fallback under embedded opt-in");
        RequireGpu(player.Snapshot());
        const auto activeSeek = player.Seek100ns(5'000'000);
        Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == activeSeek && s.state == VideoPlaybackState::Paused; }),
            "actual active fragmented MP4 seek before Finalize");
        const auto held = player.Snapshot().frame;
        Expect(held && held->pts100ns >= 5'000'000, "active seek reports actual decoded PTS");
        const auto beforeAppend = archive->Snapshot();
        for (int frame = 100; frame < 112 && recorder.IsRecording(); ++frame)
            if (!submitFrame(frame)) break;
        Expect(recorder.IsRecording() && archive->Snapshot().latestWriterAccepted100ns > beforeAppend.latestWriterAccepted100ns,
            "Original WriteSample continues while same-session paused reader owns its handle");
        Expect(player.Snapshot().frame == held && player.Snapshot().state == VideoPlaybackState::Paused,
            "archive growth does not replace the held frame");
        Expect(Wait([&] { return archive->Snapshot().seekableEnd100ns > beforeAppend.seekableEnd100ns; }),
            "new complete fragment extends the verified seekable endpoint");
        const auto newTarget = beforeAppend.seekableEnd100ns + 1;
        const auto growingSeek = player.Seek100ns(newTarget);
        Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == growingSeek && s.state == VideoPlaybackState::Paused; }),
            "same worker refreshes growing archive prefix for a new seek");
        Expect(player.Snapshot().frame && player.Snapshot().frame->pts100ns >= newTarget,
            "extended archive seek decodes actual newly recorded media");
        // Writer admission and one new fragment do not drain delayed native
        // encoder/mux/parser output. Fence the clock assertion to an exact
        // already verified prefix, then expose new real bytes explicitly.
        clockFencePath = TempPath();
        std::wcout << L"Controlled active prefix fixture: " << clockFencePath.wstring() << L'\n';
        auto clockArchive = std::make_shared<RecordingArchive>(clockFencePath.wstring());
        ComPtr<IMFByteStream> clockOutput, clockObserved;
        Expect(SUCCEEDED(CreateRecordingArchiveOutputStream(clockFencePath.wstring(), &clockOutput)),
            "exclusive test-owned active clock archive created");
        Expect(clockOutput && SUCCEEDED(RecordingArchive::Observe(clockOutput.Get(), clockArchive, &clockObserved)),
            "active clock archive uses the production write observer");
        std::uint64_t copiedBytes{};
        RecordingArchiveSnapshot copiedSource;
        const auto appendVerifiedPrefix = [&] {
            ComPtr<IMFByteStream> input;
            RecordingArchiveSnapshot admitted;
            if (!clockObserved || FAILED(archive->OpenReadStream(&input, &admitted)) ||
                admitted.verifiedMediaBytes <= copiedBytes || admitted.verifiedMediaBytes > 16u * 1024 * 1024 ||
                (copiedBytes && (admitted.generation != copiedSource.generation ||
                                !admitted.file.SameFile(copiedSource.file)))) return false;
            const auto beforeCopy = clockArchive->Snapshot();
            if (beforeCopy.committedBytes != copiedBytes || beforeCopy.verifiedMediaBytes != copiedBytes ||
                FAILED(input->SetCurrentPosition(copiedBytes)) ||
                FAILED(clockObserved->SetCurrentPosition(copiedBytes))) return false;
            std::array<BYTE, 64 * 1024> buffer{};
            while (copiedBytes < admitted.verifiedMediaBytes) {
                const auto bytes = static_cast<ULONG>(std::min<std::uint64_t>(buffer.size(),
                    admitted.verifiedMediaBytes - copiedBytes));
                ULONG read{}, written{};
                if (FAILED(input->Read(buffer.data(), bytes, &read)) || read != bytes ||
                    FAILED(clockObserved->Write(buffer.data(), bytes, &written)) || written != bytes) return false;
                copiedBytes += bytes;
            }
            if (!archive->ValidateRange(admitted.generation, 0, copiedBytes) ||
                FAILED(clockObserved->Flush()) || !Wait([&] {
                    return clockArchive->Snapshot().verifiedMediaBytes == copiedBytes;
                })) return false;
            const auto copied = clockArchive->Snapshot();
            if (copied.generation != beforeCopy.generation || copied.committedBytes != copiedBytes ||
                copied.finalized || copied.availability != ActiveRewindAvailability::Available ||
                !copied.file.valid || copied.file.SameFile(admitted.file) ||
                copied.videoSamples != admitted.videoSamples || copied.seekableEnd100ns != admitted.seekableEnd100ns ||
                copied.videoTrackId != admitted.videoTrackId || copied.codecConfigurationId != admitted.codecConfigurationId)
                return false;
            copiedSource = admitted;
            std::cout << "Controlled active prefix sourceGeneration=" << admitted.generation
                << " bytes=" << copiedBytes << " mirrorGeneration=" << copied.generation
                << " mutation=" << copied.mutationSequence << " samples=" << copied.videoSamples
                << " seekableEnd=" << copied.seekableEnd100ns << '\n';
            return true;
        };
        const bool copied = appendVerifiedPrefix();
        Expect(copied, "bounded real native prefix is independently verified in the controlled active archive");
        if (copied) {
            const auto clockOpen = player.OpenArchive(clockArchive);
            Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == clockOpen &&
                s.frame && s.state == VideoPlaybackState::Paused; }), "controlled active prefix decodes real native pixels");
            RequireGpu(player.Snapshot());
            const auto tail = player.JumpToEnd();
            Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == tail && s.state == VideoPlaybackState::Ended; }),
                "active prefix end selects its actual last decoded frame");
            std::int64_t tailPts{-1}; std::uint64_t tailSample{};
            { const auto tailSnapshot = player.Snapshot();
              if (tailSnapshot.frame) { tailPts = tailSnapshot.frame->pts100ns; tailSample = tailSnapshot.frame->sampleId; } }
            player.SetPlaying(true);
            Expect(Wait([&] { return player.Snapshot().state == VideoPlaybackState::Waiting; }),
                "active-tail Resume explicitly waits for a complete fragment");
            {
                const auto frozen = clockArchive->Snapshot();
                const auto sourceBeforeWait = archive->Snapshot();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                const auto afterWait = clockArchive->Snapshot();
                const auto waiting = player.Snapshot();
                const auto sourceAfterWait = archive->Snapshot();
                std::cout << "Active clock fence generation=" << frozen.generation << "->" << afterWait.generation
                    << " bytes=" << frozen.verifiedMediaBytes << "->" << afterWait.verifiedMediaBytes
                    << " mutation=" << frozen.mutationSequence << "->" << afterWait.mutationSequence
                    << " nativeBytes=" << sourceBeforeWait.verifiedMediaBytes << "->" << sourceAfterWait.verifiedMediaBytes
                    << " state=" << static_cast<int>(waiting.state) << " tailPts=" << tailPts
                    << " actualPts=" << (waiting.frame ? waiting.frame->pts100ns : -1) << '\n';
                Expect(!frozen.finalized && !afterWait.finalized && frozen.committedBytes == copiedBytes &&
                    afterWait.committedBytes == copiedBytes && frozen.verifiedMediaBytes == copiedBytes &&
                    afterWait.verifiedMediaBytes == copiedBytes && afterWait.generation == frozen.generation &&
                    afterWait.mutationSequence == frozen.mutationSequence && afterWait.videoSamples == frozen.videoSamples &&
                    afterWait.seekableEnd100ns == frozen.seekableEnd100ns && afterWait.file.SameFile(frozen.file),
                    "active clock wait owns one exact immutable verified prefix independent of delayed native output");
                Expect(waiting.requestGeneration == tail && waiting.state == VideoPlaybackState::Waiting && waiting.frame &&
                    waiting.frame->sampleId == tailSample && waiting.frame->pts100ns == tailPts,
                    "active wait holds exact media PTS while wall time advances");
            } // Release held diagnostic frame before testing new decode credit.
            for (int frame = 112; frame < 124 && recorder.IsRecording(); ++frame)
                if (!submitFrame(frame)) break;
            Expect(Wait([&] { return archive->Snapshot().verifiedMediaBytes > copiedBytes; }),
                "native writer proves a new complete suffix after the controlled wait");
            const auto beforeGrowth = clockArchive->Snapshot();
            const bool grew = appendVerifiedPrefix();
            Expect(grew && clockArchive->Snapshot().generation == beforeGrowth.generation &&
                clockArchive->Snapshot().verifiedMediaBytes > beforeGrowth.verifiedMediaBytes,
                "explicit real native suffix grows only the same controlled active archive generation");
            Expect(Wait([&] { const auto s = player.Snapshot(); return s.state == VideoPlaybackState::Playing && s.frame && s.frame->pts100ns > tailPts; }),
                "waiting active playback resumes from newly verified recorded bytes");
            const auto resumedAt = Clock::now();
            const auto resumedFrame = player.Snapshot().frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            const auto paced = player.Snapshot();
            const auto elapsedTicks = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - resumedAt).count() / 100;
            Expect(paced.requestGeneration == tail && resumedFrame && paced.frame &&
                paced.frame->pts100ns >= resumedFrame->pts100ns &&
                paced.frame->pts100ns - resumedFrame->pts100ns <= elapsedTicks + 1'000'000,
                "automatic active growth resumes at media pace without consuming accumulated waiting wall time");
        }
        player.SetPlaying(false);
    } else Expect(active.seekableEnd100ns == -1, "unproved native archive truthfully unavailable");
    const auto provenDurationBeforeStop = archive->Snapshot().duration100ns;
    const auto result = recorder.Stop(); Expect(result.FullyCompleted(), "native Original finalized");
    Expect(archive->Snapshot().duration100ns >= provenDurationBeforeStop,
        "finalization retains proven duration for stable multipart receipt offsets");
    if(result.FullyCompleted())FinalizationRetainsRevokedMapping(path);
    {
        const auto g = player.OpenFile(path.wstring());
        Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == g && s.state != VideoPlaybackState::Opening; }), "saved file first frame opens");
        auto snapshot = player.Snapshot();
        Expect(snapshot.frame && snapshot.state == VideoPlaybackState::Paused, "saved first frame paused");
        if(detail::FfmpegPlaybackRequested())Expect(snapshot.decoderBackend=="media-foundation" &&
            snapshot.backendReason.find("fragmented MP4")!=std::string::npos,
            "saved fragmented MP4 retains native timing with an explicit unsupported-prototype reason");
        RequireGpu(snapshot);
        Expect(snapshot.hasAudio && snapshot.audioAvailable, "same SourceReader exposes AAC decoded PCM");
        if (snapshot.neighborFrame) Expect(snapshot.frame->pts100ns != snapshot.neighborFrame->pts100ns, "owned neighbor has distinct real PTS");
        const auto a = player.Seek100ns(20'000'000); const auto b = player.Seek100ns(5'000'000);
        Expect(b > a && Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == b && s.state == VideoPlaybackState::Paused; }), "latest seek supersedes earlier target");
        Expect(player.Snapshot().frame && player.Snapshot().frame->pts100ns >= 5'000'000, "seek reports actual selected PTS");
        const auto resumeFloor = player.Snapshot().frame ? player.Snapshot().frame->pts100ns : 0;
        player.SetMuted(false); player.SetPlaying(true);
        Expect(Wait([&] {
            auto packets = player.TakeAudio();
            for (const auto& packet : packets) Expect(packet.pts100ns >= resumeFloor, "resumed PCM excludes seek preroll before held cursor");
            return !packets.empty();
        }), "bounded PCM emitted only on explicit unmuted Resume");
        player.SetPlaying(false); Expect(player.TakeAudio().empty(), "Pause silences pending PCM");
        const auto end = player.JumpToEnd();
        const bool atEnd = Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == end && s.state == VideoPlaybackState::Ended; });
        if (!atEnd) {
            const auto s = player.Snapshot();
            std::cerr << "Saved EOF diagnostic: state=" << static_cast<int>(s.state)
                << " generation=" << s.requestGeneration << " expected=" << end
                << " pts=" << (s.frame ? s.frame->pts100ns : -1)
                << " duration=" << s.duration100ns << " seekableEnd=" << s.seekableEnd100ns
                << " error=" << s.error << '\n';
        }
        Expect(atEnd, "jump to actual decoder EOS");
        const auto endPts = player.Snapshot().frame ? player.Snapshot().frame->pts100ns : -1;
        Expect(endPts >= 0 && player.Snapshot().seekableEnd100ns == endPts, "actual last decodable frame retained");
        player.SetPlaying(true); Expect(player.Snapshot().state == VideoPlaybackState::Ended, "Resume at EOF never restarts");
        const auto beforeEnd=player.Seek100ns(endPts-666666);
        Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==beforeEnd && s.state==VideoPlaybackState::Paused;}),
            "a seek before EOF does not inherit the end scan's broad anchor interval");
        Expect(player.Snapshot().frame && player.Snapshot().frame->pts100ns<endPts &&
            !player.Snapshot().lastRequestReusedFrame,"end cache does not substitute the final frame for an earlier target");
        player.SetPlaying(true);
        Expect(Wait([&]{return player.Snapshot().state==VideoPlaybackState::Ended;}),"playback resumes from a real pre-end seek");
        if(gpuAdapter) {
            const auto original=player.Snapshot();
            const auto fallback=player.UseCpuFallback("fixture consumer interop refusal");
            Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==fallback && s.frame &&
                s.frame->requestGeneration==fallback && (s.state==VideoPlaybackState::Paused || s.state==VideoPlaybackState::Ended);}),
                "GPU consumer failure re-seeks the same stable document through CPU compatibility");
            const auto cpu=player.Snapshot();
            Expect(!cpu.gpuResident && cpu.frame && cpu.frame->bgra && !cpu.frame->gpu &&
                cpu.file.SameFile(original.file) && cpu.frame->pts100ns==original.frame->pts100ns &&
                cpu.backendReason=="fixture consumer interop refusal","fallback preserves held PTS/file identity and reports actual CPU pixels");
            player.SetGpuAdapter(LUID{0xdeadbeefu,LONG(0x7fffffff)});
            const auto refused=player.OpenFile(path.wstring());
            Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==refused && s.state==VideoPlaybackState::Paused;}),
                "an unavailable exact GPU adapter falls back before publishing the open request");
            const auto unavailable=player.Snapshot();
            Expect(!unavailable.gpuResident && unavailable.frame && unavailable.frame->bgra && !unavailable.backendReason.empty(),
                "failed GPU admission never claims a resident frame");
        }
        player.Close();
    }
    archive.reset();
    if (failures == priorFailures) {
        std::error_code ec; std::filesystem::remove(path, ec);
        if (ec) std::wcout << L"Retained codec fixture pending handle retirement: " << path.wstring() << L'\n';
    } else std::wcout << L"Retained failed codec fixture: " << path.wstring() << L'\n';
    if (!clockFencePath.empty()) {
        if (failures == priorFailures) {
            std::error_code ec; std::filesystem::remove(clockFencePath, ec);
            if (ec) std::wcout << L"Retained active clock fixture pending handle retirement: " << clockFencePath.wstring() << L'\n';
        } else std::wcout << L"Retained failed active clock fixture: " << clockFencePath.wstring() << L'\n';
    }
}
std::vector<std::int64_t> SparseNativeTailReference(const std::filesystem::path& path) {
    // Observe the decoded native clock directly. The MP4 track stores 30-kHz
    // packet PTS, while this machine's MF reader reports slightly shifted
    // 100-ns presentation times across sparse gaps. This independent reader has
    // no playback/ready queue or Pause path, and retires before the player
    // opens; neither a guessed tolerance nor authored-clock equality is used.
    ComPtr<IMFByteStream> bytes;VideoFileIdentity file;
    ComPtr<IMFAttributes> attrs;ComPtr<IMFSourceReader> reader;ComPtr<IMFMediaType> output;
    const auto setup=OpenStableVideoReadStream(path.wstring(),&bytes,&file);
    bool ready=SUCCEEDED(setup) && SUCCEEDED(MFCreateAttributes(&attrs,2)) &&
        SUCCEEDED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE)) &&
        SUCCEEDED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,FALSE)) &&
        SUCCEEDED(MFCreateSourceReaderFromByteStream(bytes.Get(),attrs.Get(),&reader)) &&
        SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE)) &&
        SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE)) &&
        SUCCEEDED(MFCreateMediaType(&output)) &&
        SUCCEEDED(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video)) &&
        SUCCEEDED(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32)) &&
        SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,output.Get()));
    PROPVARIANT target{};target.vt=VT_I8;target.hVal.QuadPart=1; // same explicit target as the player
    ready=ready && SUCCEEDED(reader->SetCurrentPosition(GUID_NULL,target));
    Expect(ready,"the independent CPU native-clock reference opens the generated sparse source");
    std::vector<std::int64_t> tail;
    if(!ready)return tail;
    bool eos{};const auto deadline=Clock::now()+std::chrono::seconds(2);
    for(unsigned reads=0;reads<VideoPlayback::kSeekSampleLimit && Clock::now()<deadline;++reads) {
        DWORD stream{},flags{};LONGLONG pts{};ComPtr<IMFSample> sample;
        const auto hr=reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&stream,&flags,&pts,&sample);
        if(FAILED(hr) || (flags&MF_SOURCE_READERF_ERROR))break;
        if(sample && pts>=40'000'000)tail.push_back(pts);
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){eos=true;break;}
    }
    Expect(eos && tail.size()==5 && std::adjacent_find(tail.begin(),tail.end(),std::greater_equal<>())==tail.end(),
        "the independent native decoder observes exactly five distinct strictly ordered tail samples through EOS");
    std::cout<<"Sparse independent native tail PTS:";for(const auto pts:tail)std::cout<<' '<<pts;std::cout<<'\n';
    return tail;
}

void NativeSparseVideoAudio(VideoRecorder::Codec codec) {
    const auto path = TempPath();
    std::wcout << L"Sparse-video/continuous-AAC fixture: " << path.wstring() << L'\n';
    const int priorFailures = failures;
    VideoRecorder recorder; VideoRecorder::AudioFormat audioFormat;
    const UINT width=gpuAdapter?320u:160u,height=gpuAdapter?192u:96u;
    Expect(recorder.Start(path.wstring(), width, height, 30, 1, codec, &audioFormat), "sparse-video AAC recorder starts");
    if (!recorder.IsRecording()) return;
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4, 180);
    std::vector<std::int16_t> pcm(1600);
    // Preserve the sparse VFR samples, then continue AAC alone so container
    // duration is not a video-sample position and end seeking must observe it.
    for (int slot = 0; slot < 190 && recorder.IsRecording(); ++slot) {
        const auto time = slot * 333333ll;
        if (slot == 0 || slot == 1 || slot == 61 || (slot >= 121 && slot < 126)) {
            RecordingFrameIdentity id; id.captureTimestamp100ns = id.captureClock100ns = time;
            id.sequenceNumber = slot + 1; id.frameRateNumerator = 30; id.frameRateDenominator = 1;
            const bool admitted = Wait([&] { return recorder.CanAcceptVideoFrame(); }, 2000);
            Expect(admitted && recorder.AddFrame(pixels.data(), width * 4, id), "sparse video sample accepted");
            if (!admitted) break;
        }
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::int16_t>(4000 * std::sin((slot * 1600 + i) * 0.057595865));
        const bool admitted = Wait([&] { return recorder.CanAcceptAudioFrame(pcm.size() * 2, time, 333333); }, 2000);
        Expect(admitted && recorder.AddAudioFrame(reinterpret_cast<const BYTE*>(pcm.data()), pcm.size() * 2, time, 333333),
            "continuous AAC accepted between sparse video frames");
        if (!admitted) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Expect(recorder.Stop().FullyCompleted(), "sparse-video fixture finalized by recorder");
    const auto nativeTail=SparseNativeTailReference(path);
    VideoPlayback player;ConfigureGpu(player); const auto open = player.OpenFile(path.wstring());
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == open && s.state == VideoPlaybackState::Paused; }),
        "sparse-video saved file opens paused");
    RequireGpu(player.Snapshot());
    const auto seek = player.Seek100ns(1);
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == seek && s.state == VideoPlaybackState::Paused; }),
        "sparse-video fixture selects the frame preceding its two-second gap");
    const auto held = player.Snapshot().frame;
    Expect(held && held->pts100ns < 1'000'000, "sparse-video selected early frame has actual PTS");
    player.SetMuted(false); player.SetPlaying(true);
    std::int64_t audioDuration{};
    Expect(Wait([&] {
        for (const auto& packet : player.TakeAudio()) {
            if (held) Expect(packet.pts100ns >= held->pts100ns, "sparse resumed audio excludes preroll");
            audioDuration += static_cast<std::int64_t>(std::uint64_t(packet.pcm.size()) * 10'000'000 /
                (packet.sampleRate * packet.channels * 2));
        }
        return audioDuration >= 7'000'000;
    }, 1200), "continuous PCM progresses while next sparse video PTS is still in the future");
    Expect(player.Snapshot().frame == held, "audio pumping during a VFR gap retains the exact video frame");
    Expect(Wait([&] {const auto s=player.Snapshot();return s.frame==held &&
        s.readyFrames==s.lookaheadFrames && s.readyFrames>0;},500),
        "the sparse VFR gap fills its bounded future-frame queue while the same early image is displayed");
    player.SetPlaying(false); Expect(player.TakeAudio().empty(), "Pause discards sparse-file PCM");
    const auto pausedSparse=player.Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto afterProof=player.Snapshot();
    Expect(afterProof.frame==held && afterProof.state==VideoPlaybackState::Paused && !afterProof.neighborFrame &&
        afterProof.readyFrames==pausedSparse.readyFrames && afterProof.decodedBytes==pausedSparse.decodedBytes,
        "out-of-window VFR proof leaves the exact held image, charged leases and queued playback samples intact");
    if(!gpuAdapter)Expect(afterProof.peakDecodedBytes==pausedSparse.peakDecodedBytes,
        "a full CPU ready queue causes no extra owned-image allocation during optional proof preparation");
    // Muted Resume reuses queued video instead of audio realignment masking
    // a discarded native sample. Every independent native tail sample must play.
    player.SetMuted(true);player.SetPlaying(true);
    std::set<std::int64_t> sparseTail;
    std::vector<std::int64_t> observedTail;
    auto latestObserved=held?held->pts100ns:-1;
    Expect(Wait([&] {
        const auto s=player.Snapshot();
        if(s.frame && s.frame->pts100ns!=latestObserved) {
            Expect(s.frame->pts100ns>latestObserved,"sparse replay presents strictly increasing source timestamps");
            latestObserved=s.frame->pts100ns;
            sparseTail.insert(latestObserved);
            if(latestObserved>=40'000'000)observedTail.push_back(latestObserved);
        }
        Expect(s.readyFrames<=s.lookaheadFrames && s.decodedBytes<=s.decodedByteLimit,
            "VFR tail replay preserves the bounded queue and existing decoded-byte quota");
        return s.state==VideoPlaybackState::Ended || s.state==VideoPlaybackState::Unavailable;
    },6000) && player.Snapshot().state==VideoPlaybackState::Ended,
        "explicit muted Resume drains the sparse queued timeline through actual video EOS");
    std::cout<<"Sparse paused proof ready="<<pausedSparse.readyFrames<<"->"<<afterProof.readyFrames
        <<" bytes="<<pausedSparse.decodedBytes<<"->"<<afterProof.decodedBytes
        <<" peak="<<pausedSparse.peakDecodedBytes<<"->"<<afterProof.peakDecodedBytes<<" tail PTS:";
    for(const auto pts:sparseTail)std::cout<<' '<<pts;
    std::cout<<'\n';
    Expect(nativeTail.size()==5 && observedTail==nativeTail,
        "all five independent native tail samples replay one-to-one in exact timestamp order after paused proof");
    const auto end = player.JumpToEnd();
    const bool atEnd = Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == end && s.state == VideoPlaybackState::Ended; });
    if (!atEnd) {
        const auto s = player.Snapshot();
        std::cerr << "Sparse audio-tail EOF diagnostic: state=" << static_cast<int>(s.state)
            << " duration=" << s.duration100ns << " error=" << s.error << '\n';
    }
    Expect(atEnd, "VFR video with a long AAC-only tail reaches observed video EOS within shared seek limits");
    { const auto s = player.Snapshot();
      Expect(s.frame && s.frame->pts100ns >= 40'000'000 && s.frame->pts100ns < 43'000'000 &&
          s.seekableEnd100ns == s.frame->pts100ns && s.duration100ns > s.frame->pts100ns + 10'000'000,
          "sparse-file endpoint is the actual last video frame rather than the later audio/container duration"); }
    player.Close();
    if (failures == priorFailures) {
        std::error_code ec; std::filesystem::remove(path, ec);
        if (ec) std::wcout << L"Retained sparse fixture pending handle retirement: " << path.wstring() << L'\n';
    } else std::wcout << L"Retained failed sparse fixture: " << path.wstring() << L'\n';
}
void NativeFourKPool(bool dci=false) {
    const bool embedded=detail::FfmpegPlaybackRequested();
    const auto path = embedded?std::filesystem::path(__FILE__).parent_path()/"data/embedded-pool-4k.mp4":TempPath();
    const int priorFailures = failures;
    std::wcout << L"4K decoded-pool fixture: " << path.wstring() << L'\n';
    VideoRecorder recorder;
    const std::uint32_t width=dci?4096u:3840u,height=2160;
    if(!embedded) {
    Expect(recorder.Start(path.wstring(), width, height, 5, 1, VideoRecorder::Codec::H264), "4K AVC fixture starts");
    if (!recorder.IsRecording()) return;
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4, 255);
    for (int frame = 0; frame < 12 && recorder.IsRecording(); ++frame) {
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4) pixels[offset] = static_cast<BYTE>(frame * 15);
        RecordingFrameIdentity id; id.captureTimestamp100ns = id.captureClock100ns = frame * 2'000'000ll;
        id.sequenceNumber = frame + 1; id.frameRateNumerator = 5; id.frameRateDenominator = 1;
        const bool admitted = Wait([&] { return recorder.CanAcceptVideoFrame(); }, 4000);
        Expect(admitted && recorder.AddFrame(pixels.data(), width * 4, id), "4K video admitted");
        if (!admitted) break;
    }
    Expect(recorder.Stop().FullyCompleted(), "4K fixture finalized");
    pixels.clear(); pixels.shrink_to_fit();
    }
    VideoPlayback player;ConfigureGpu(player); const auto open = player.OpenFile(path.wstring());
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == open && s.state == VideoPlaybackState::Paused; }),
        "4K first frame opens with an admitted complete working set");
    auto held=player.Snapshot(); // Actual app retains a displayed pair across decoder work.
    if(detail::FfmpegPlaybackRequested())Expect(held.decoderBackend=="ffmpeg-dav1d","4K pressure fixture uses embedded decoder, without native fallback");
    RequireGpu(held);
    player.SetPlaying(true); std::set<std::int64_t> displayed;
    Expect(Wait([&] {
        const auto s = player.Snapshot();
        Expect(s.state != VideoPlaybackState::Unavailable, "ordinary 4K playback remains available");
        Expect(s.decodedBytes <= s.decodedByteLimit && s.peakDecodedBytes <= VideoPlayback::kDecodedByteLimit &&
            s.gpuTextureBytes<=VideoGpuDecoder::kTextureByteLimit,
            "4K working and immutable leases stay inside adaptive/hard limits");
        if (s.frame) displayed.insert(s.frame->pts100ns);
        return displayed.size() >= 5 || s.state == VideoPlaybackState::Unavailable;
    }, 3000) && displayed.size() >= 5, "at least five distinct 4K frames play without pool exhaustion");
    Expect(Wait([&]{return player.Snapshot().readyFrames==2;},1000),"saved file prepares two genuine future frames");
    player.SetPlaying(false);
    held={};
    const auto seek = player.Seek100ns(7'000'000);
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == seek && s.state == VideoPlaybackState::Paused; }),
        "4K multi-sample seek drops optional prior frames before allocating replacements");
    { const auto s = player.Snapshot();
      Expect(s.frame && s.frame->width == width && s.frame->height == height && s.frame->pts100ns >= 7'000'000 &&
          s.peakDecodedBytes <= VideoPlayback::kDecodedByteLimit, "4K seek keeps actual selected pixels and exact quota"); }
    std::vector<std::int64_t> seekTimes;unsigned mostSeekReads{};
    for(const auto target:{16'000'000ll,0ll,10'000'000ll,2'000'000ll,18'000'000ll,4'000'000ll,12'000'000ll,6'000'000ll}) {
        for(int burst=0;burst<5;++burst)player.Seek100ns((target+burst*2'000'000)%20'000'000);
        const auto latest=player.Seek100ns(target);
        const auto pending=player.Snapshot();
        Expect(pending.requestGeneration==latest &&
            (pending.state!=VideoPlaybackState::Seeking || !pending.readyFrames),"seek never drains stale lookahead");
        Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==latest && s.state==VideoPlaybackState::Paused;}),
            "newest rapid backward/forward seek completes");
        const auto s=player.Snapshot();
        Expect(s.frame && s.frame->requestGeneration==latest && s.frame->pts100ns>=target,
            "rapid seek retains exact latest-generation pixels");
        Expect(s.lastRequestReadCount<=VideoPlayback::kSeekSampleLimit && s.lastRequestLatencyUs>0 &&
            s.lastRequestLatencyUs<=2'000'000,"request diagnostics measure first target publication within safety bound");
        Expect(s.lastRequestCopiedFrames==1,"discarded seek preroll produces no owned full-frame copies");
        mostSeekReads=std::max(mostSeekReads,s.lastRequestReadCount);
        seekTimes.push_back(s.lastRequestLatencyUs);
    }
    Expect(mostSeekReads>1,"the native fixture actually exercises discarded codec preroll");
    std::sort(seekTimes.begin(),seekTimes.end());
    std::cout<<(dci?"DCI":"UHD")<<" 4K native request-to-target latency us: p50="<<seekTimes[seekTimes.size()/2]
        <<" max="<<seekTimes.back()<<" (8 bursts; worker publication, not display latency)\n";
    {
    const auto fineStart=player.Seek100ns(1);
    Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==fineStart && s.state==VideoPlaybackState::Paused;}),
        "fine-scrub fixture selects the first frame after zero");
    const auto fineFrame=player.Snapshot().frame;
    std::vector<std::int64_t> fineTimes;
    for(const auto target:{100'000ll,500'000ll,1'000'000ll,1'500'000ll,200'000ll,1ll}) {
        const auto request=player.Seek100ns(target);
        Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==request && s.state==VideoPlaybackState::Paused;}),
            "same-frame fine scrub completes under newest authority");
        const auto s=player.Snapshot();
        Expect(s.lastRequestReusedFrame && !s.lastRequestReadCount && !s.lastRequestCopiedFrames,
            "same-frame seek reuses cached selection with zero recorded preroll reads or pixel copies");
        Expect(s.frame && fineFrame && s.frame->pts100ns==fineFrame->pts100ns &&
            s.frame->requestGeneration==request && s.frame->bgra==fineFrame->bgra && s.frame->gpu==fineFrame->gpu,
            "cached seek retickets exact immutable pixels without changing their source time");
        fineTimes.push_back(s.lastRequestLatencyUs);
    }
    std::sort(fineTimes.begin(),fineTimes.end());
    std::cout<<(dci?"DCI":"UHD")<<" 4K same-frame scrub latency us: p50="<<fineTimes[fineTimes.size()/2]
        <<" max="<<fineTimes.back()<<" (6 seeks; worker publication)\n";
    }
    {
        const auto heldFrame=player.Snapshot().frame;
        const auto failed=player.Seek100ns(40'000'000);
        Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==failed && s.state==VideoPlaybackState::Unavailable;}),
            "seek beyond native duration truthfully fails");
        Expect(player.Snapshot().frame==heldFrame,"failed seek preserves pointer-identical last selected pixels");
    }
    const auto pressureStart=player.Seek100ns(0);
    Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==pressureStart && s.state==VideoPlaybackState::Paused;}),
        "retained-lease pressure fixture selects start");
    std::vector<std::shared_ptr<const VideoPlaybackFrame>> leases;
    player.SetPlaying(true);
    Expect(Wait([&]{
        const auto s=player.Snapshot();
        if(s.frame && (leases.empty() || leases.back()->pts100ns!=s.frame->pts100ns))leases.push_back(s.frame);
        Expect(s.state!=VideoPlaybackState::Unavailable,"consumer hoarding applies backpressure without invalidating decoder");
        return s.memoryPressure && !s.readyFrames && leases.size()>=4;
    },5000),"retained external leases cannot grow the pool past its cap");
    const auto pressureBytes=player.Snapshot().decodedBytes;
    const auto cacheUnderPressure=player.Seek100ns(0);
    Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==cacheUnderPressure && s.state==VideoPlaybackState::Paused;}),
        "same-frame seek succeeds while consumers retain all available pixel credit");
    {const auto s=player.Snapshot();
     Expect(s.lastRequestReusedFrame && !s.lastRequestReadCount && !s.lastRequestCopiedFrames &&
         !leases.empty() && s.frame && s.frame->bgra==leases.front()->bgra && s.decodedBytes<=pressureBytes,
         "pressure reticket shares the exact charged allocation without growing the pixel pool");}
    auto pressured=player.Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Expect(player.Snapshot().state!=VideoPlaybackState::Unavailable,"waiting for consumer credit remains available");
    leases.clear();
    player.SetPlaying(true);
    Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==cacheUnderPressure && s.frame && pressured.frame &&
        s.frame->pts100ns>pressured.frame->pts100ns && s.state!=VideoPlaybackState::Unavailable;}),
        "releasing old leases resumes without a new seek or Resume");
    pressured={};player.SetPlaying(false);
    const auto sequenceStart=player.Seek100ns(0);
    Expect(Wait([&]{const auto s=player.Snapshot();return s.requestGeneration==sequenceStart && s.state==VideoPlaybackState::Paused;}),
        "ordered playback fixture selects start");
    player.SetPlaying(true);std::int64_t previousPts=-1;bool pausedOnce=false;
    Expect(Wait([&]{
        const auto s=player.Snapshot();
        Expect(s.state!=VideoPlaybackState::Unavailable,"ordered ready queue remains available");
        if(s.frame) {
            Expect(s.frame->pts100ns>=previousPts,"queued playback never presents a backward frame");previousPts=s.frame->pts100ns;
            if(!pausedOnce && previousPts>=4'000'000) {
                pausedOnce=true;player.SetPlaying(false);const auto paused=player.Snapshot().frame;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                Expect(player.Snapshot().frame==paused,"Pause holds exact frame while lookahead is retained");
                player.SetPlaying(true);
            }
        }
        return s.state==VideoPlaybackState::Ended;
    },5000) && previousPts==22'000'000,"EOF occurs only after the final queued frame is published");
    const auto end = player.JumpToEnd();
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == end && s.state == VideoPlaybackState::Ended; }),
        "4K endpoint retains its last frame through the final no-sample EOS read");
    player.Close();
    if (!embedded && failures == priorFailures) {
        std::error_code ec; std::filesystem::remove(path, ec);
        if (ec) std::wcout << L"Retained 4K fixture pending handle retirement: " << path.wstring() << L'\n';
    } else std::wcout << L"Retained failed 4K fixture: " << path.wstring() << L'\n';
}
void NativeGpuConversion() {
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !gpuAdapter ||
       FAILED(factory->EnumAdapterByLuid(*gpuAdapter,IID_PPV_ARGS(&adapter))) ||
       FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
           D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) {
        Expect(false,"GPU conversion fixture creates the exact video device");return;
    }
    for(const bool privateTarget:{false,true}) {
    Expect(Wait([]{return VideoGpuDecoder::RetainedBytes()==0;}),"prior converter retires before the next pool fixture");
    auto converter=VideoGpuDecoder::CreateFromDevice(device.Get(),privateTarget);
    if(!privateTarget) {
        // Negotiation follows the installed decoder's own preference order:
        // for a synthetic VP9 coded type the decoder lists its native 8-bit
        // output before the 10-bit type it merely tolerates. Forcing the
        // tolerated type first stalls the first native sample read.
        ComPtr<IMFMediaType> coded;MFCreateMediaType(&coded);
        coded->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);coded->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_VP90);
        MFSetAttributeSize(coded.Get(),MF_MT_FRAME_SIZE,640,360);
        MFSetAttributeRatio(coded.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        coded->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        const auto offered=converter->DecoderPreferredOutputs(coded.Get());
        if(!offered.empty()) {
            const auto nv12=std::find(offered.begin(),offered.end(),MFVideoFormat_NV12);
            const auto p010=std::find(offered.begin(),offered.end(),MFVideoFormat_P010);
            Expect(nv12!=offered.end() && (p010==offered.end() || nv12<p010),
                "decoder preference enumeration keeps native 8-bit VP9 output before tolerated 10-bit output");
        } else std::cout<<"SKIP-INFO no installed VP9 decoder accepted the synthetic coded type\n";
    }
    const UINT width=320,height=192;
    for(const bool tenBit:{false,true}) {
        ComPtr<IMFMediaType> type;MFCreateMediaType(&type);type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE,tenBit?MFVideoFormat_P010:MFVideoFormat_NV12);
        MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,width,height);MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);type->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709);
        type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235);type->SetUINT32(MF_MT_TRANSFER_FUNCTION,MFVideoTransFunc_709);
        converter->SetFormat(type.Get());
        std::vector<std::uint8_t> pixels(std::size_t(width)*height*3/2*(tenBit?2:1));
        if(tenBit) {
            auto* values=reinterpret_cast<std::uint16_t*>(pixels.data());
            std::fill(values,values+width*height,64u<<6);std::fill(values+width*height,values+pixels.size()/2,512u<<6);
        } else {std::fill(pixels.begin(),pixels.begin()+width*height,16);std::fill(pixels.begin()+width*height,pixels.end(),128);}
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=tenBit?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_DECODER;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(),width*(tenBit?2u:1u),0};ComPtr<ID3D11Texture2D> source;
        const auto textureResult=device->CreateTexture2D(&desc,&initial,&source);
        Expect(SUCCEEDED(textureResult),"synthetic NV12/P010 texture is admitted by the real device");
        if(FAILED(textureResult))continue;
        ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFSample> sample;MFCreateSample(&sample);
        Expect(SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),source.Get(),0,FALSE,&buffer)) &&
            SUCCEEDED(sample->AddBuffer(buffer.Get())),"synthetic sample retains its native surface");
        std::shared_ptr<const VideoPlaybackGpuImage> first;
        try {first=converter->Convert(sample.Get(),[]{return true;},Clock::now()+std::chrono::seconds(2));}
        catch(const std::exception& error) {std::cerr<<"GPU conversion diagnostic: "<<error.what()<<'\n';}
        Expect(bool(first),"native NV12 and 10-bit P010 both convert on the GPU");
        if(!first)continue;
        D3D11_TEXTURE2D_DESC output{};first->texture->GetDesc(&output);
        Expect(output.Format==DXGI_FORMAT_B8G8R8A8_UNORM && (output.MiscFlags&D3D11_RESOURCE_MISC_SHARED_NTHANDLE),
            "converted output is an owned shareable BGRA allocation");
        output.Usage=D3D11_USAGE_STAGING;output.BindFlags=output.MiscFlags=0;output.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Expect(SUCCEEDED(device->CreateTexture2D(&output,nullptr,&staging)),"fixture readback allocation succeeds");
        if(!staging)continue;
        context->CopyResource(staging.Get(),first->texture);D3D11_MAPPED_SUBRESOURCE mapped{};
        Expect(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"fixture-only GPU color readback completes");
        if(mapped.pData) {
            const auto* pixel=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(height/2)*mapped.RowPitch+width/2*4;
            Expect(pixel[0]<=3 && pixel[1]<=3 && pixel[2]<=3 && pixel[3]==255,
                "limited black is correctly converted to opaque full-range RGB for 8/10-bit input");
            context->Unmap(staging.Get(),0);
        }
        std::vector<std::shared_ptr<const VideoPlaybackGpuImage>> leases{first};
        for(unsigned i=1;i<VideoGpuDecoder::kTextureSlotLimit && converter->HasConversionCredit();++i)
            leases.push_back(converter->Convert(sample.Get(),[]{return true;},Clock::now()+std::chrono::seconds(2)));
        Expect(leases.size()>=2 && leases.size()<=VideoGpuDecoder::kTextureSlotLimit && !converter->HasConversionCredit(),
            "consumer-held GPU images stop conversion at the bounded slot count");
        const auto reused=leases.back()->texture;leases.back().reset();
        Expect(converter->HasConversionCredit(),"a released GPU lease makes its slot reusable without another allocation");
        const auto again=converter->Convert(sample.Get(),[]{return true;},Clock::now()+std::chrono::seconds(2));
        Expect(again && again->texture==reused && first->texture!=again->texture,"only the released GPU slot is reused");
        Expect(!converter->Convert(sample.Get(),[]{return false;},Clock::now()+std::chrono::seconds(2)),
            "canceled conversion publishes no image");
    }
    }
}
void NativeGpuExternal(const std::filesystem::path& path) {
    VideoPlayback player;ConfigureGpu(player);
    const auto opened=player.OpenFile(path.wstring());
    const bool ready=Wait([&] {const auto s=player.Snapshot();return s.frame || s.state==VideoPlaybackState::Unavailable;},10000);
    auto s=player.Snapshot();
    std::cout<<"External GPU format "<<path.filename().string()<<" resident="<<s.gpuResident<<" reason="<<s.backendReason<<" error="<<s.error<<'\n';
    Expect(ready && s.frame && s.requestGeneration==opened,"external native format opens with a current frame");
    if(!s.frame)return;
    RequireGpu(s);
    if(path.filename().string().find("main10")!=std::string::npos)
        Expect(s.gpuDecodedBitsPerComponent>=10,"10-bit native source keeps high-precision decoder output before GPU BGRA conversion");
    const auto seek=player.Seek100ns(10'000'000);
    Expect(Wait([&] {s=player.Snapshot();return s.requestGeneration==seek && s.frame && s.state==VideoPlaybackState::Paused;},10000),"external native format seeks without stale publication");
    RequireGpu(s);Expect(s.frame && s.frame->pts100ns>=10'000'000,"external format publishes requested PTS");
    for (const std::int64_t seconds : {3, 7, 13, 23}) {
        const auto target=seconds*10'000'000;
        if(target>=s.duration100ns)continue;
        s={}; // release the consumer lease before each independent seek
        const auto request=player.Seek100ns(target);
        const bool sought=Wait([&] {s=player.Snapshot();return s.requestGeneration==request &&
            (s.state==VideoPlaybackState::Paused || s.state==VideoPlaybackState::Unavailable);},10000);
        std::cout<<"External seek seconds="<<seconds<<" error="<<s.error<<'\n';
        Expect(sought && s.state==VideoPlaybackState::Paused && s.frame && s.frame->pts100ns>=target,
               "external format random access publishes the requested frame");
    }
    const auto end=player.JumpToEnd();
    Expect(Wait([&] {s=player.Snapshot();return s.requestGeneration==end && s.state==VideoPlaybackState::Ended;},10000),"external native format holds actual endpoint");
    RequireGpu(s);player.Close();
}
void SeekProgressLimits() {
    using Budget=detail::VideoPlaybackSeekBudget;
    const auto started=Clock::time_point{};
    Budget advancing(started);
    for(int second=1;second<60;++second) {
        Expect(advancing.Observe(second*10'000'000ll,started+std::chrono::seconds(second)),
            "strict media progress keeps a long GOP seek eligible before the total cap");
        Expect(!advancing.Expired(started+std::chrono::seconds(second)),
            "healthy preroll remains seeking beyond the short native progress window");
    }
    Expect(advancing.Expired(started+std::chrono::seconds(60)) &&
        !advancing.Observe(600'000'000,started+std::chrono::seconds(60)),
        "steady progress cannot extend the absolute sixty-second request limit");
    Budget stalled(started);
    Expect(stalled.Observe(10'000'000,started+std::chrono::seconds(1)),"first real sample establishes progress");
    Expect(!stalled.Observe(10'000'000,started+std::chrono::milliseconds(1900)) &&
        !stalled.Observe(9'000'000,started+std::chrono::milliseconds(2500)) &&
        !stalled.Observe(-1,started+std::chrono::milliseconds(2900)) &&
        !stalled.Observe(INT64_MAX,started+std::chrono::milliseconds(2900)) &&
        stalled.Expired(started+std::chrono::seconds(3)),
        "repeated, backward and absent samples cannot renew a stalled reader");
    Expect(!stalled.Observe(20'000'000,started+std::chrono::seconds(3)),
        "a native read returning after the progress deadline cannot revive its request");
}
void PacedAudioDelivery(VideoPlayback& player) {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"data"/"paced-audio-h264.mp4";
    const auto request=player.OpenFile(path.wstring());
    Expect(Wait([&] {const auto s=player.Snapshot();return s.requestGeneration==request &&
        (s.state==VideoPlaybackState::Paused || s.state==VideoPlaybackState::Unavailable);}),
        "committed audio pacing fixture settles without an encoder or output endpoint");
    auto s=player.Snapshot();
    Expect(s.state==VideoPlaybackState::Paused && s.frame && s.audioAvailable,"AAC pacing fixture exposes native PCM");
    if(s.state!=VideoPlaybackState::Paused || !s.frame)return;
    const auto firstPts=s.frame->pts100ns;
    player.SetMuted(false);player.SetPlaying(true);
    const auto started=Clock::now();
    std::int64_t audioEnd{},maximumLead{},duration{};
    bool havePcm{};std::int64_t largestPcmGap{};
    while(Clock::now()-started<std::chrono::seconds(2)) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count()/100;
        // Drain exactly as the UI does. The decode high-water must survive
        // TakeAudio; otherwise each video wait pumps another future packet.
        for(const auto& p:player.TakeAudio()) {
            const auto span=static_cast<std::int64_t>(p.pcm.size()*10'000'000ull/(p.sampleRate*p.channels*2));
            if(havePcm)largestPcmGap=std::max(largestPcmGap,std::abs(p.pts100ns-audioEnd));
            Expect(p.requestGeneration==request,"PCM packets retain the selected request generation");
            havePcm=true;
            duration+=span;audioEnd=p.pts100ns+span;
            maximumLead=std::max(maximumLead,audioEnd-firstPts-elapsed);
        }
        s=player.Snapshot();
        if(s.state==VideoPlaybackState::Unavailable || s.state==VideoPlaybackState::Ended)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout<<"Paced audio maximumLead100ns="<<maximumLead<<" PCM100ns="<<duration
        <<" largestPcmBoundary100ns="<<largestPcmGap<<" videoPts="<<(s.frame?s.frame->pts100ns:-1)<<" error="<<s.error<<'\n';
    Expect(largestPcmGap<=1,"PCM has no dropped, duplicated or discontinuous sample blocks beyond one timestamp tick");
    Expect(maximumLead<=4'000'000,"draining the decoder queue cannot push PCM beyond the output sink's half-second allowance");
    Expect(s.state==VideoPlaybackState::Playing && s.audioAvailable && duration>=15'000'000,
        "bounded audio continues while the silent native video reader advances");
    player.SetMuted(true);Expect(player.TakeAudio().empty(),"muting immediately revokes decoded audio");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto mutedFloor=player.Snapshot().frame->pts100ns;
    player.SetMuted(false);
    bool heard{};
    Expect(Wait([&] {
        for(const auto& packet:player.TakeAudio()) {
            Expect(packet.pts100ns>=mutedFloor,"unmuted PCM excludes the prior muted cursor");
            heard=true;
        }
        return heard || player.Snapshot().state==VideoPlaybackState::Unavailable;
    },1000) && heard,"unmute repositions the decoded audio high-water and resumes PCM");
    player.SetPlaying(false);Expect(player.TakeAudio().empty(),"paused pacing fixture revokes pending PCM");
    const auto pausedFloor=player.Snapshot().frame->pts100ns;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    player.SetPlaying(true);heard=false;
    Expect(Wait([&] {
        for(const auto& packet:player.TakeAudio()) {
            Expect(packet.pts100ns>=pausedFloor,"resumed PCM excludes preroll before the exact paused frame");
            heard=true;
        }
        return heard || player.Snapshot().state==VideoPlaybackState::Unavailable;
    },1000) && heard,"Resume resets the high-water at the paused cursor rather than remaining silent");
    player.Close();
}
// Private owner-media diagnostic: decode PCM into memory only. It never
// creates an audio endpoint, GUI, recorder or temporary copy of the input.
void NativeExternalPlayback(const std::filesystem::path& path) {
    VideoPlayback player; ConfigureGpu(player);
    auto request=player.OpenFile(path.wstring());
    VideoPlaybackSnapshot s;
    const auto settled=[&] {
        s=player.Snapshot();
        return s.requestGeneration==request &&
            (s.state==VideoPlaybackState::Paused || s.state==VideoPlaybackState::Unavailable);
    };
    Expect(Wait(settled,10000) && s.frame && s.state==VideoPlaybackState::Paused,
        "private playback input opens with an exact current frame");
    std::cout<<"Private playback open GPU="<<s.gpuResident<<" reason="<<s.backendReason
        <<" audio="<<s.audioAvailable<<" error="<<s.error<<'\n';
    if(!s.frame || s.state!=VideoPlaybackState::Paused)return;
    for(const std::int64_t seconds:{6,23}) {
        s={};request=player.Seek100ns(seconds*10'000'000);
        const bool ready=Wait(settled,65000);
        std::cout<<"Private playback seek="<<seconds<<" state="<<static_cast<int>(s.state)
            <<" reads="<<s.lastRequestReadCount<<" latencyUs="<<s.lastRequestLatencyUs
            <<" reason="<<s.backendReason<<" error="<<s.error<<'\n';
        Expect(ready && s.frame && s.state==VideoPlaybackState::Paused && s.frame->pts100ns>=seconds*10'000'000,
            "private playback long-GOP seek selects its requested frame");
        if(!ready || s.state!=VideoPlaybackState::Paused)return;
    }
    s={};player.Seek100ns(700'000'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    request=player.Seek100ns(60'000'000);
    Expect(Wait(settled,65000) && s.state==VideoPlaybackState::Paused && s.frame &&
        s.frame->requestGeneration==request && s.frame->pts100ns>=60'000'000 && s.frame->pts100ns<61'000'000,
        "a newer near seek supersedes in-progress long-GOP preroll without publishing the old selection");
    if(!s.frame || s.state!=VideoPlaybackState::Paused)return;
    const auto firstPts=s.frame->pts100ns;
    player.SetMuted(false);player.SetPlaying(true);
    const auto resumeStarted=Clock::now();
    const bool resumed=Wait([&] {s=player.Snapshot();return s.state==VideoPlaybackState::Unavailable ||
        (s.frame && s.frame->pts100ns>firstPts);},65000);
    std::cout<<"Private playback resume prerollMs="<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-resumeStarted).count()
        <<" state="<<static_cast<int>(s.state)<<" error="<<s.error<<'\n';
    Expect(resumed && s.state==VideoPlaybackState::Playing,"unmuted Resume survives healthy long-GOP preroll");
    if(!resumed || s.state!=VideoPlaybackState::Playing)return;
    const auto started=Clock::now();
    std::int64_t audioBytes{},audioEnd{},maximumLead{},largestPcmBoundary{};bool havePcm{};
    while(Clock::now()-started<std::chrono::seconds(3)) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count()/100;
        for(const auto& packet:player.TakeAudio()) {
            if(havePcm)largestPcmBoundary=std::max(largestPcmBoundary,std::abs(packet.pts100ns-audioEnd));
            Expect(packet.requestGeneration==request && packet.pts100ns>=firstPts,"private PCM retains exact current seek authority and floor");
            havePcm=true;
            audioBytes+=packet.pcm.size();
            audioEnd=packet.pts100ns+static_cast<std::int64_t>(packet.pcm.size()*10'000'000ull/(packet.sampleRate*packet.channels*2));
            maximumLead=std::max(maximumLead,audioEnd-firstPts-elapsed);
        }
        s=player.Snapshot();
        if(s.state==VideoPlaybackState::Unavailable || s.state==VideoPlaybackState::Ended)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout<<"Private playback state="<<static_cast<int>(s.state)<<" videoPts="<<(s.frame?s.frame->pts100ns:-1)
        <<" audioAvailable="<<s.audioAvailable<<" PCMBytes="<<audioBytes<<" audioEnd="<<audioEnd
        <<" maximumAudioLead100ns="<<maximumLead<<" largestPcmBoundary100ns="<<largestPcmBoundary<<" error="<<s.error<<'\n';
    Expect(largestPcmBoundary<=1,"private PCM packet boundaries remain contiguous within one timestamp tick");
    Expect(s.state==VideoPlaybackState::Playing && s.frame && s.frame->pts100ns>firstPts+20'000'000,
        "private source advances video without becoming unavailable");
    Expect(!s.hasAudio || (s.audioAvailable && audioBytes>0),"private source retains decoded audio throughout playback");
    Expect(maximumLead<=4'000'000,"PCM delivery stays within a bounded lead while callers drain the decoder queue");
    s={};request=player.JumpToEnd();
    const bool ended=Wait([&] {s=player.Snapshot();return s.requestGeneration==request &&
        (s.state==VideoPlaybackState::Ended || s.state==VideoPlaybackState::Unavailable);},65000);
    std::cout<<"Private playback end state="<<static_cast<int>(s.state)<<" reads="<<s.lastRequestReadCount
        <<" latencyUs="<<s.lastRequestLatencyUs<<" endpoint="<<s.seekableEnd100ns<<" error="<<s.error<<'\n';
    Expect(ended && s.state==VideoPlaybackState::Ended && s.frame && s.frame->pts100ns==s.seekableEnd100ns,
        "private single-GOP playback establishes its actual decoded endpoint under the absolute request cap");
    player.Close();
}
void EmbeddedAdmissionRefusals() {
    const auto longBoxes=TempPath();
    const auto badAudio=TempPath();
    {
        std::ofstream file(longBoxes,std::ios::binary);
        const char ftyp[]={0,0,0,16,'f','t','y','p','i','s','o','m',0,0,0,0};
        const char freeBox[]={0,0,0,8,'f','r','e','e'};
        const char fragmented[]={0,0,0,16,'m','o','o','v',0,0,0,8,'m','v','e','x'};
        file.write(ftyp,sizeof(ftyp));
        for(unsigned i=0;i<1024;++i)file.write(freeBox,sizeof(freeBox));
        file.write(fragmented,sizeof(fragmented));
    }
    {std::ofstream file(badAudio,std::ios::binary);file<<"unreadable audio cursor";}
    {
        ComPtr<IMFByteStream> bytes;VideoFileIdentity identity;VideoReadStreamClone clone;
        Expect(SUCCEEDED(OpenStableVideoReadStream(longBoxes.wstring(),&bytes,&identity,&clone)),
            "bounded container fixture opens its held stream");
        ComPtr<IMFSourceReader> refused;std::string reason;
        Expect(FAILED(detail::CreateFfmpegVideoReader(bytes.Get(),clone,[]{return true;},
            Clock::now()+std::chrono::seconds(2),&refused,reason)) && !refused &&
            reason.find("bounded box probe")!=std::string::npos,
            "top-level probe exhaustion cannot certify a later fragmented container as ordinary MP4");
    }
    {
        const auto source=std::filesystem::path(__FILE__).parent_path()/"data/paced-audio-h264.mp4";
        ComPtr<IMFByteStream> bytes;VideoFileIdentity identity;VideoReadStreamClone clone;
        Expect(SUCCEEDED(OpenStableVideoReadStream(source.wstring(),&bytes,&identity,&clone)),
            "audio refusal fixture holds the original video and audio metadata");
        const VideoReadStreamClone unreadable=[&](IMFByteStream** result) {
            VideoFileIdentity ignored;
            return OpenStableVideoReadStream(badAudio.wstring(),result,&ignored);
        };
        ComPtr<IMFSourceReader> refused;std::string reason;
        Expect(FAILED(detail::CreateFfmpegVideoReader(bytes.Get(),unreadable,[]{return true;},
            Clock::now()+std::chrono::seconds(2),&refused,reason)) && !refused,
            "failure of a known audio cursor cannot silently admit a video-only decoder");
    }
    std::error_code ignored;
    std::filesystem::remove(longBoxes,ignored);std::filesystem::remove(badAudio,ignored);
}
// Same production worker and immutable issuance/PCM contract, no native UI,
// audio endpoint, camera, external decoder process or model preparation.
void EmbeddedContract() {
    SetEnvironmentVariableW(L"OKUFLOW_VIDEO_BACKEND",L"ffmpeg");
    EmbeddedAdmissionRefusals();
    VideoPlayback player;
    LongGopSeeking(player);PausedSceneCutNeighbor(player);PacedAudioDelivery(player);
    player.Close();
    Expect(Wait([&]{return player.Snapshot().decodedBytes==0;}),"all unretained embedded output allocations retire after Close");
    const auto path=std::filesystem::path(__FILE__).parent_path()/"data/paced-audio-h264.mp4";
    auto generation=player.OpenFile(path.wstring());VideoPlaybackSnapshot snapshot;
    Expect(Wait([&]{snapshot=player.Snapshot();return snapshot.requestGeneration==generation &&
        snapshot.state==VideoPlaybackState::Paused;}),"embedded lease fixture opens");
    Expect(snapshot.decoderBackend=="ffmpeg-dav1d","opt-in contract actually used embedded decoding");
    auto held=snapshot.frame;snapshot={};
    {
        ComPtr<IMFByteStream> bytes;VideoFileIdentity identity;VideoReadStreamClone clone;
        Expect(SUCCEEDED(OpenStableVideoReadStream(path.wstring(),&bytes,&identity,&clone)),"embedded cancellation fixture holds a stable source");
        ComPtr<IMFSourceReader> refused;std::string reason;
        const auto started=Clock::now();
        Expect(FAILED(detail::CreateFfmpegVideoReader(bytes.Get(),clone,[]{return false;},Clock::now()+std::chrono::seconds(2),&refused,reason)) &&
            !refused && Clock::now()-started<std::chrono::milliseconds(200),"superseded custom I/O cannot publish or block on decoding");
        Expect(FAILED(detail::CreateFfmpegVideoReader(bytes.Get(),clone,[]{return true;},Clock::now()-std::chrono::milliseconds(1),&refused,reason)),
            "expired custom I/O deadline refuses admission");
    }
    if(held) {
        const auto bytes=held->bgra;
        const auto copied=std::make_shared<VideoPlaybackFrame>(*held);
        Expect(!player.PauseAtFrame(copied),"embedded sample metadata copies cannot forge issuance");
        generation=player.Seek100ns(10'000'000);
        Expect(Wait([&]{snapshot=player.Snapshot();return snapshot.requestGeneration==generation &&
            snapshot.state==VideoPlaybackState::Paused;}),"embedded successor publishes");
        Expect(!player.PauseAtFrame(held),"previous-generation embedded frames cannot regain authority");
        player.Close();snapshot={};
        Expect(Wait([&]{return player.Snapshot().decodedBytes==bytes->size();}),"only the exact consumer pixel lease stays charged after retirement");
    }
    held.reset();
    Expect(Wait([&]{return player.Snapshot().decodedBytes==0;}),"last embedded consumer releases its pool charge");
    // Archive selection always keeps the producer-fenced native path. Missing
    // SDK/open refusal is checked in a separate process before DLL loading.
    auto archive=std::make_shared<RecordingArchive>(path.wstring());archive->Finalized(true);
    generation=player.OpenArchive(archive);
    Wait([&]{snapshot=player.Snapshot();return snapshot.requestGeneration==generation &&
        snapshot.state!=VideoPlaybackState::Opening;});
    Expect(snapshot.decoderBackend=="media-foundation","recording archives never use the saved-file prototype");
    player.Close();
}
void EmbeddedMissingRuntime() {
    SetEnvironmentVariableW(L"OKUFLOW_VIDEO_BACKEND",L"ffmpeg");
    SetEnvironmentVariableW(L"OKUFLOW_FFMPEG_RUNTIME",TempPath().c_str());
    VideoPlayback player;const auto path=std::filesystem::path(__FILE__).parent_path()/"data/paced-audio-h264.mp4";
    const auto generation=player.OpenFile(path.wstring());VideoPlaybackSnapshot snapshot;
    Expect(Wait([&]{snapshot=player.Snapshot();return snapshot.requestGeneration==generation &&
        (snapshot.state==VideoPlaybackState::Paused || snapshot.state==VideoPlaybackState::Unavailable);}) &&
        snapshot.state==VideoPlaybackState::Paused && snapshot.frame && snapshot.audioAvailable,
        "missing embedded runtime falls back using the same held file, retaining video and PCM");
    Expect(snapshot.decoderBackend=="media-foundation" && !snapshot.backendReason.empty(),"native fallback retains the original SDK refusal reason");
    player.Close();
}
void CompareEmbedded(const std::filesystem::path& path) {
    std::vector<std::shared_ptr<const VideoPlaybackFrame>> native;
    VideoPlayback player;VideoPlaybackSnapshot snapshot;
    for(const bool embedded:{false,true}) {
        SetEnvironmentVariableW(L"OKUFLOW_VIDEO_BACKEND",embedded?L"ffmpeg":nullptr);
        auto generation=player.OpenFile(path.wstring());
        const auto settled=[&]{snapshot=player.Snapshot();return snapshot.requestGeneration==generation &&
            (snapshot.state==VideoPlaybackState::Paused || snapshot.state==VideoPlaybackState::Unavailable);};
        Expect(Wait(settled,10000) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused,"comparison opens the held source");
        if(!snapshot.frame || snapshot.state!=VideoPlaybackState::Paused)return;
        Expect(snapshot.decoderBackend==(embedded?"ffmpeg-dav1d":"media-foundation"),"comparison uses the required decoder, without fallback");
        std::cout<<"Compare backend="<<snapshot.decoderBackend<<" openUs="<<snapshot.lastRequestLatencyUs<<" reason="<<snapshot.backendReason<<'\n';
        unsigned index{};
        for(const std::int64_t second:{6,23}) {
            generation=player.Seek100ns(second*10'000'000);
            Expect(Wait(settled,65000) && snapshot.state==VideoPlaybackState::Paused,"comparison completes finite exact seek");
            if(snapshot.state!=VideoPlaybackState::Paused || !snapshot.frame)return;
            const auto f=snapshot.frame;
            std::cout<<"Compare backend="<<snapshot.decoderBackend<<" seek="<<second<<" pts="<<f->pts100ns
                <<" sample="<<f->sampleId<<" latencyUs="<<snapshot.lastRequestLatencyUs<<" reads="<<snapshot.lastRequestReadCount
                <<" copied="<<snapshot.lastRequestCopiedFrames<<" ownedPeak="<<snapshot.peakDecodedBytes<<'\n';
            if(!embedded)native.push_back(f);
            else {
                const auto& reference=native.at(index);
                Expect(f->pts100ns==reference->pts100ns && f->sampleId==reference->sampleId &&
                    f->width==reference->width && f->height==reference->height,"same-source backends select strict normalized frame/PTS identity");
                if(f->bgra && reference->bgra && f->bgra->size()==reference->bgra->size()) {
                    double absolute{},squared{};unsigned maximum{};
                    for(std::size_t p=0;p<f->bgra->size();++p)if(p%4!=3) {
                        const auto difference=std::abs(int((*f->bgra)[p])-int((*reference->bgra)[p]));
                        absolute+=difference;squared+=difference*difference;maximum=std::max(maximum,unsigned(difference));
                    }
                    const auto count=double(f->width)*f->height*3;
                    std::cout<<"Compare RGB meanAbsolute="<<absolute/count<<" maximum="<<maximum
                        <<" PSNR="<<(squared?10*std::log10(255.*255.*count/squared):999.)<<" noHdrToneMap=true\n";
                }
            }
            ++index;
        }
        player.Close();snapshot={};
        // Keep only the explicitly retained comparison images. Native permit
        // retirement is verified by successful admission of the next open.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    native.clear();
    Expect(Wait([&]{return player.Snapshot().decodedBytes==0;}),"same-source retained images retire completely");
}
}
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf); // retain each diagnostic stage before a slow native call
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    if (FAILED(MFStartup(MF_VERSION))) return 2;
    const std::string mode = argc > 1 ? argv[1] : "";
    if (!mode.empty()) {
        if (!GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC", nullptr, 0)) {
            std::cout << "SKIP opt-in native codec run requires OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1\n";
            MFShutdown(); CoUninitialize(); return 77;
        }
        if(mode=="--native-gpu-avc" || mode=="--native-gpu-av1" || mode=="--native-gpu-4k" || mode=="--native-gpu-dci-4k" || mode=="--gpu-conversion" || mode=="--native-gpu-external" || mode=="--native-gpu-playback-external") {
            ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
            if(!GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_GPU",nullptr,0) ||
               FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
               FAILED(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))) ||
               FAILED(adapter->GetDesc1(&desc)) || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
                std::cout<<"SKIP opt-in native GPU decoder requires a hardware adapter and OKUFLOW_TEST_VIDEO_GPU=1\n";
                MFShutdown();CoUninitialize();return 77;
            }
            gpuAdapter=desc.AdapterLuid;
        }
        if(mode=="--embedded-contract")EmbeddedContract();
        else if(mode=="--embedded-missing-runtime")EmbeddedMissingRuntime();
        else if(mode=="--compare-embedded") {
            if(argc!=3)Expect(false,"comparison requires one exact private source path");
            else CompareEmbedded(std::filesystem::path(argv[2]));
        }
        else if(mode=="--native-playback-external" || mode=="--native-gpu-playback-external") {
            if(argc!=3)Expect(false,"private playback fixture requires exactly one file path");
            else NativeExternalPlayback(std::filesystem::path(argv[2]));
        }
        else if(mode=="--native-gpu-external") {
            if(argc!=3)Expect(false,"external GPU fixture requires exactly one file path");
            else NativeGpuExternal(std::filesystem::path(argv[2]));
        }
        else if(mode=="--gpu-conversion")NativeGpuConversion();
        else if (mode == "--native-4k" || mode == "--native-dci-4k" || mode=="--native-gpu-4k" || mode=="--native-gpu-dci-4k")
            NativeFourKPool(mode=="--native-dci-4k" || mode=="--native-gpu-dci-4k");
        else {
            const auto codec = mode == "--native-av1" || mode=="--native-gpu-av1" ? VideoRecorder::Codec::Av1 : VideoRecorder::Codec::H264;
            NativeAvcAac(codec, true);
            if (!failures && !nativeSkipped) NativeSparseVideoAudio(codec);
        }
    } else { SeekProgressLimits(); LayoutAndIdentity(); MutationObservation(); UnsupportedAndUnknownWrites(); FailedOpenSupersession(); VideoPlayback player; LongGopSeeking(player); InitialSparseSavedFileNeighbour(player); PausedSceneCutNeighbor(player); PacedAudioDelivery(player); }
    // Workers own their MFStartup/MFShutdown pairs; these local scopes own
    // only the fixture's startup. No camera/microphone or native GUI opens.
    MFShutdown(); CoUninitialize();
    std::cout << (failures ? "FAIL" : "PASS") << " video playback contracts\n"; return failures ? 1 : (nativeSkipped ? 77 : 0);
}
#endif
