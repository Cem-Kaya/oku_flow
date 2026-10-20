#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/common/media_writer.hpp"
#include <mfapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <thread>

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
    snapshot={};request=player.Seek100ns(30'000'000);
    Expect(Wait(settled) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused && snapshot.frame->pts100ns>=30'000'000,
           "long-GOP seek reaches three seconds beyond 120 preroll reads");
    std::cout<<"Long-GOP regression reads="<<snapshot.lastRequestReadCount<<" latencyUs="<<snapshot.lastRequestLatencyUs<<" error="<<snapshot.error<<'\n';
    player.Seek100ns(10'000'000);request=player.Seek100ns(40'000'000);snapshot={};
    Expect(Wait(settled) && snapshot.frame && snapshot.state==VideoPlaybackState::Paused && snapshot.frame->pts100ns>=40'000'000,
           "new long-GOP seek supersedes a waiting target");
    player.Close();
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
    Expect(player.PauseAtFrame(selected),"pause accepts its exact issued displayed frame without a decoder seek");
    Expect(player.TakeAudio().empty(),"paused corroboration emits no audio");
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
        "idle/repeated pause neither rereads the selected frame nor consumes its following playback image");

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
        "EOF cannot invent a following frame or turn paused proof into hidden playback");
    player.SetPlaying(true);
    Expect(player.Snapshot().state==VideoPlaybackState::Ended,"proof preparation preserves explicit Resume-at-EOF refusal");
    player.Close();
}

void NativeAvcAac(VideoRecorder::Codec codec = VideoRecorder::Codec::H264, bool strictActive = false) {
    if (!GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC", nullptr, 0)) {
        std::cout << "SKIP opt-in AVC/AAC and active archive: set OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1\n"; return;
    }
    const int priorFailures = failures;
    const auto path = TempPath(); VideoRecorder recorder; VideoRecorder::AudioFormat audioFormat;
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
        const auto tail = player.JumpToEnd();
        Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == tail && s.state == VideoPlaybackState::Ended; }),
            "active prefix end selects its actual last decoded frame");
        const auto tailPts = player.Snapshot().frame ? player.Snapshot().frame->pts100ns : -1;
        player.SetPlaying(true);
        Expect(Wait([&] { return player.Snapshot().state == VideoPlaybackState::Waiting; }),
            "active-tail Resume explicitly waits for a complete fragment");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        Expect(player.Snapshot().state == VideoPlaybackState::Waiting && player.Snapshot().frame &&
            player.Snapshot().frame->pts100ns == tailPts,
            "active wait holds exact media PTS while wall time advances");
        for (int frame = 112; frame < 124 && recorder.IsRecording(); ++frame)
            if (!submitFrame(frame)) break;
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
    player.SetPlaying(false); Expect(player.TakeAudio().empty(), "Pause discards sparse-file PCM");
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
    const auto path = TempPath(); const int priorFailures = failures;
    std::wcout << L"4K decoded-pool fixture: " << path.wstring() << L'\n';
    VideoRecorder recorder;
    const std::uint32_t width=dci?4096u:3840u,height=2160;
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
    VideoPlayback player;ConfigureGpu(player); const auto open = player.OpenFile(path.wstring());
    Expect(Wait([&] { const auto s = player.Snapshot(); return s.requestGeneration == open && s.state == VideoPlaybackState::Paused; }),
        "4K first frame opens with an admitted complete working set");
    auto held=player.Snapshot(); // Actual app retains a displayed pair across decoder work.
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
            "same-frame seek performs no native seek/preroll reads or pixel copies");
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
    if (failures == priorFailures) {
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
}
int main(int argc, char** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    if (FAILED(MFStartup(MF_VERSION))) return 2;
    const std::string mode = argc > 1 ? argv[1] : "";
    if (!mode.empty()) {
        if (!GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC", nullptr, 0)) {
            std::cout << "SKIP opt-in native codec run requires OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1\n";
            MFShutdown(); CoUninitialize(); return 77;
        }
        if(mode=="--native-gpu-avc" || mode=="--native-gpu-av1" || mode=="--native-gpu-4k" || mode=="--native-gpu-dci-4k" || mode=="--gpu-conversion" || mode=="--native-gpu-external") {
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
        if(mode=="--native-gpu-external") {
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
    } else { LayoutAndIdentity(); MutationObservation(); UnsupportedAndUnknownWrites(); FailedOpenSupersession(); VideoPlayback player; LongGopSeeking(player); PausedSceneCutNeighbor(player); }
    // Workers own their MFStartup/MFShutdown pairs; these local scopes own
    // only the fixture's startup. No camera/microphone or native GUI opens.
    MFShutdown(); CoUninitialize();
    std::cout << (failures ? "FAIL" : "PASS") << " video playback contracts\n"; return failures ? 1 : (nativeSkipped ? 77 : 0);
}
#endif
