#ifdef _WIN32
#include "okuflow/common/video_recap.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <thread>

namespace okuflow {
std::optional<VideoRecapPolicy> VideoRecapPolicy::ForSize(std::uint32_t w, std::uint32_t h) {
    if (!w || !h || (w & 1) || (h & 1) || std::max(w,h) > 3840 ||
        std::min(w,h) > 2160) return {};
    return std::uint64_t(w)*h <= 1920u*1080u ? VideoRecapPolicy{30,8'000'000}
                                            : VideoRecapPolicy{30,16'000'000};
}
struct VideoRecapRing::Pins { std::atomic<unsigned> count{}; };
VideoRecapRing::VideoRecapRing(std::size_t bytes, std::int64_t window)
    : pins_(std::make_shared<Pins>()), byteLimit_(bytes), window100ns_(std::max<std::int64_t>(1,window)) {}
void VideoRecapRing::Reset(std::uint64_t epoch) {
    std::lock_guard lock(mutex_); segments_.clear(); bytes_=0; epoch_=epoch;
}
void VideoRecapRing::SetWindow(std::int64_t window) {
    std::lock_guard lock(mutex_);window100ns_=std::clamp<std::int64_t>(window,60LL*10'000'000,VideoRecapPolicy::kWindow100ns);
    while(!segments_.empty() && segments_.front()->end100ns<segments_.back()->end100ns-window100ns_) {
        bytes_-=segments_.front()->source->Capacity();segments_.pop_front();
    }
}
bool VideoRecapRing::Append(std::shared_ptr<const VideoRecapSegment> segment) {
    if (!segment || !segment->source || segment->admissions.empty() || segment->admissions.size()>64 ||
        segment->begin100ns < 0 || segment->end100ns < segment->begin100ns || segment->end100ns>INT64_MAX/100 ||
        segment->end100ns-segment->begin100ns>VideoRecapPolicy::kSegment100ns ||
        segment->source->ByteSize()==0 || segment->source->Capacity()==0 ||
        segment->source->ByteSize()>segment->source->Capacity() ||
        segment->source->Capacity()>VideoRecapPolicy::kSegmentBytes || segment->source->Capacity() > byteLimit_) return false;
    std::int64_t pts=-1,capture=-1;std::uint64_t sequence{};
    for(const auto& a:segment->admissions) {
        if(a.pts100ns<0 || a.pts100ns<=pts || a.capture100ns<segment->captureOrigin100ns ||
            a.capture100ns<=capture || a.sequence<=sequence ||
            a.pts100ns!=a.capture100ns-segment->captureOrigin100ns)return false;
        pts=a.pts100ns;capture=a.capture100ns;sequence=a.sequence;
    }
    if(segment->admissions.front().pts100ns!=0 || pts!=segment->end100ns-segment->begin100ns)return false;
    segment=std::make_shared<const VideoRecapSegment>(*segment);
    std::lock_guard lock(mutex_);
    if (segment->epoch != epoch_ || (!segments_.empty() &&
        (segment->id <= segments_.back()->id || segment->begin100ns <= segments_.back()->end100ns))) return false;
    const auto bytes=segment->source->Capacity();
    while (!segments_.empty() && (segments_.size()>=VideoRecapPolicy::kSegments || bytes_ > byteLimit_-bytes ||
        segments_.front()->end100ns < segment->end100ns-window100ns_)) {
        bytes_-=segments_.front()->source->Capacity(); segments_.pop_front();
    }
    segments_.push_back(std::move(segment)); bytes_+=bytes; return true;
}
std::shared_ptr<const VideoRecapSegment> VideoRecapRing::Acquire(std::int64_t target) const {
    std::lock_guard lock(mutex_);
    if (segments_.empty()) return {};
    auto count=pins_->count.load();
    do { if (count>=VideoRecapPolicy::kPinnedSegments) return {}; }
    while (!pins_->count.compare_exchange_weak(count,count+1));
    target=std::clamp(target,std::max(segments_.front()->begin100ns,
        segments_.back()->end100ns-window100ns_),segments_.back()->end100ns);
    auto found=std::find_if(segments_.begin(),segments_.end(),
        [target](const auto& s){return s->end100ns>=target;});
    const auto segment=*found;
    // Aliasing the holder makes all copies of this lease one pin. The ring
    // cannot reclaim its immutable bytes while decoder cursors still own it.
    struct Holder { std::shared_ptr<const VideoRecapSegment> segment; std::shared_ptr<Pins> pins;
        ~Holder(){pins->count.fetch_sub(1);} };
    std::shared_ptr<Holder> holder;
    try {
        holder=std::make_shared<Holder>();holder->segment=segment;holder->pins=pins_;
    } catch (const std::bad_alloc&) { pins_->count.fetch_sub(1);return {}; }
    std::shared_ptr<VideoRecapSegment> leased;
    try{leased=std::make_shared<VideoRecapSegment>(*segment);}catch(const std::bad_alloc&){return {};}
    // A lease-specific immutable wrapper propagates this pin into read cursors
    // and clone factories too. A shared_ptr alias alone would lose accounting
    // if a consumer opened a cursor and then dropped its source/segment.
    try{leased->source=segment->source->WithLifetime(std::move(holder));}catch(const std::bad_alloc&){return {};}
    return leased;
}
VideoRecapSnapshot VideoRecapRing::Snapshot() const {
    std::lock_guard lock(mutex_); VideoRecapSnapshot r;r.epoch=epoch_;r.compressedBytes=bytes_;
    r.pinnedSegments=pins_->count.load();
    if (!segments_.empty()) {r.end100ns=segments_.back()->end100ns;
        r.first100ns=std::max(segments_.front()->begin100ns,r.end100ns-window100ns_);
        r.available=true;r.warming=r.end100ns-r.first100ns<window100ns_;}
    return r;
}
struct RamVideoRecap::State {
    mutable std::mutex mutex; std::condition_variable changed;
    std::atomic<bool> stop{};
    std::uint64_t generation{1}, cameraEpoch{}, nextSegment{};
    int rotation{-1};std::uint32_t width{},height{};
    std::deque<VideoRecapFrame> queue;
    std::size_t queuedBytes{};
    std::int64_t lastQueued{-1}, origin{-1}, latestCapture{-1}, lastPressureClock{-1};
    std::uint64_t dropped{}, admitted{}, completedFrames{}, completed{};
    std::int64_t maximumNativeCallUs{};
    std::atomic<std::int64_t> nativeStartedNs{};
    std::string error; bool pressure{}, terminal{};
    VideoRecapRing ring;
};
namespace {
using Clock=std::chrono::steady_clock;
std::int64_t NowNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
std::size_t FrameBytes(const VideoRecapFrame& f) {return std::size_t(f.width)*f.height*4;}
void RecapWorker(std::shared_ptr<RamVideoRecap::State> s) {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const auto mf=SUCCEEDED(com)?MFStartup(MF_VERSION):E_FAIL;
    if (FAILED(mf)) {std::lock_guard lock(s->mutex);s->error="Live recap Media Foundation initialization failed.";s->terminal=true;}
    std::unique_ptr<VideoRecorder> writer;
    std::shared_ptr<VideoMemoryStream> bytes;
    std::shared_ptr<VideoRecapSegment> segment;
    std::uint64_t generation{};
    const auto native=[&](auto&& operation) {
        const auto start=NowNs();s->nativeStartedNs.store(start);
        struct Progress {RamVideoRecap::State& state;~Progress(){state.nativeStartedNs.store(0);}} progress{*s};
        auto result=operation();s->nativeStartedNs.store(0);
        std::lock_guard lock(s->mutex);s->maximumNativeCallUs=std::max(s->maximumNativeCallUs,(NowNs()-start)/1000);
        return result;
    };
    const auto finish=[&](bool publish) {
        if (!writer) return;
        const auto result=native([&]{return writer->Stop();});
        writer.reset(); // native ownership is confined to this worker
        if (publish && result.HasPlayableVideo() && bytes && segment && !segment->admissions.empty()) {
            segment->source=bytes->Seal();
            std::lock_guard lock(s->mutex);
            if (generation==s->generation && !s->stop.load() && segment->source && s->ring.Append(segment)) {
                ++s->completed;s->completedFrames+=segment->admissions.size();
            }
            else if(generation==s->generation && !s->stop.load()) s->error="Live recap segment could not be published.";
        }
        bytes.reset();segment.reset();
    };
    try {
        while (SUCCEEDED(mf) && !s->stop.load()) {
            VideoRecapFrame frame;std::uint64_t epoch{};std::int64_t origin{};
            {std::unique_lock lock(s->mutex);
                s->changed.wait_for(lock,std::chrono::milliseconds(10),[&]{return s->stop.load() || !s->queue.empty() || generation!=s->generation;});
                if(s->stop.load())break;
                epoch=s->generation;origin=s->origin;
                if(!s->queue.empty()){frame=std::move(s->queue.front());s->queue.pop_front();s->queuedBytes-=FrameBytes(frame);}
            }
            if(generation!=epoch){finish(false);generation=epoch;}
            if (!frame.width) {if(writer && writer->HasPendingGpuReads()) native([&]{return writer->PollGpuInputReads(true);});continue;}
            if (segment && frame.identity.captureClock100ns-segment->captureOrigin100ns>=VideoRecapPolicy::kSegment100ns)
                finish(true);
            if (!writer) {
                const auto policy=VideoRecapPolicy::ForSize(frame.width,frame.height);
                if(!policy)continue;
                bytes=std::make_shared<VideoMemoryStream>(VideoRecapPolicy::kSegmentBytes);
                writer=std::make_unique<VideoRecorder>();
                if (!native([&]{return writer->StartMemory(bytes,frame.width,frame.height,policy->fps,1,policy->bitrate,
                    frame.gpu.IsValid()?&frame.gpu:nullptr);})) {
                    {std::lock_guard lock(s->mutex);if(generation==s->generation)s->error=writer->LastError();++s->dropped;}
                    // Startup failure latches until source reset, so capture
                    // never repeatedly initializes an unsupported codec.
                    writer.reset();bytes.reset();continue;
                }
                if(frame.gpu.IsValid() && !writer->UsesGpuInput() && !writer->UsesWorkerGpuReadback() && !frame.pixels) {
                    {std::lock_guard lock(s->mutex);if(generation==s->generation)s->error="Live recap cannot open this Original GPU allocation.";++s->dropped;}
                    finish(false);continue;
                }
                segment=std::make_shared<VideoRecapSegment>();segment->epoch=generation;
                {std::lock_guard lock(s->mutex);segment->id=++s->nextSegment;}
                segment->captureOrigin100ns=frame.identity.captureClock100ns;
                segment->begin100ns=segment->end100ns=frame.identity.captureClock100ns-origin;
            }
            bool current{};{std::lock_guard lock(s->mutex);current=generation==s->generation;}
            if(!current)continue;
            auto identity=frame.identity;
            // Explicitly use delivery time for the recap. Driver exposure
            // timestamps may restart; no inferred or duplicated frame is made.
            identity.captureTimestamp100ns=identity.captureClock100ns;
            const bool accepted=native([&]{return writer->CanAcceptVideoFrame() &&
                (frame.gpu.IsValid()?writer->AddGpuFrame(frame.gpu,identity):
                    frame.pixels && writer->AddFrame(frame.pixels->data(),frame.width*4,identity));});
            {std::lock_guard lock(s->mutex);
                if(generation==s->generation){if(accepted)++s->admitted;else {++s->dropped;s->pressure=true;}}
            }
            if (accepted) {
                if(segment->admissions.empty()) {
                    segment->captureOrigin100ns=frame.identity.captureClock100ns;
                    segment->begin100ns=frame.identity.captureClock100ns-origin;
                }
                segment->end100ns=frame.identity.captureClock100ns-origin;
                segment->admissions.push_back({identity.sequenceNumber,identity.captureClock100ns,writer->LastAdmittedVideoTimestamp100ns()});
            } else if (!writer->IsRecording()) {
                {std::lock_guard lock(s->mutex);if(generation==s->generation)s->error=writer->LastError();}
                finish(false);
            }
        }
        finish(false);
    } catch (const std::exception& e) {std::lock_guard lock(s->mutex);s->error=e.what();s->terminal=true;}
      catch (...) {std::lock_guard lock(s->mutex);s->error="Live recap worker failed.";s->terminal=true;}
    // A wedged native call keeps this entire closure and MF ownership alive;
    // cancellation never concurrently releases its inputs or calls MFShutdown.
    writer.reset();bytes.reset();segment.reset();
    std::deque<VideoRecapFrame> retired;
    {std::lock_guard lock(s->mutex);retired.swap(s->queue);s->queuedBytes=0;}
    if(SUCCEEDED(mf))MFShutdown();if(SUCCEEDED(com))CoUninitialize();
}
}
RamVideoRecap::RamVideoRecap():state_(std::make_shared<State>()){std::thread(RecapWorker,state_).detach();}
RamVideoRecap::~RamVideoRecap(){state_->stop.store(true);state_->changed.notify_one();}
void RamVideoRecap::SetDurationMinutes(int minutes) {
    state_->ring.SetWindow(std::clamp(minutes,1,5)*60LL*10'000'000);
}
void RamVideoRecap::Reset(std::uint64_t camera,int rotation) {
    std::deque<VideoRecapFrame> retired;
    std::lock_guard lock(state_->mutex);
    if(camera==state_->cameraEpoch && rotation==state_->rotation)return;
    state_->cameraEpoch=camera;state_->rotation=rotation;++state_->generation;
    retired.swap(state_->queue);state_->queuedBytes=0;state_->lastQueued=state_->origin=state_->latestCapture=-1;
    state_->width=state_->height=0;if(!state_->terminal)state_->error.clear();state_->pressure=false;
    state_->dropped=state_->admitted=state_->completedFrames=state_->completed=0;state_->lastPressureClock=-1;
    state_->ring.Reset(state_->generation);state_->changed.notify_one();
}
bool RamVideoRecap::WantsFrame(std::uint32_t w,std::uint32_t h,std::int64_t clock) const {
    const auto policy=VideoRecapPolicy::ForSize(w,h);if(!policy || clock<0 || clock>INT64_MAX/100)return false;
    std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock || state_->stop.load() || !state_->error.empty())return false;
    const bool due=state_->lastQueued<0 || clock<state_->lastQueued || w!=state_->width || h!=state_->height ||
        clock-state_->lastQueued>=10'000'000/policy->fps;
    const bool space=state_->queue.size()<VideoRecapPolicy::kQueueFrames &&
        FrameBytes({{}, {},w,h,{}})<=VideoRecapPolicy::kQueueBytes-state_->queuedBytes;
    if(due && !space && state_->lastPressureClock!=clock){state_->lastPressureClock=clock;++state_->dropped;state_->pressure=true;}
    return due && space;
}
bool RamVideoRecap::Submit(VideoRecapFrame frame) {
    const auto policy=VideoRecapPolicy::ForSize(frame.width,frame.height);
    const auto clock=frame.identity.captureClock100ns;
    if(!policy || clock<0 || clock>INT64_MAX/100 ||
        (!frame.gpu.IsValid() && (!frame.pixels || frame.pixels->size()!=FrameBytes(frame))))return false;
    std::deque<VideoRecapFrame> retired;
    std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock)return false;
    auto& s=*state_;if(s.stop.load() || !s.error.empty())return false;
    if((s.width && (s.width!=frame.width || s.height!=frame.height)) || (s.lastQueued>=0 && clock<s.lastQueued)) {
        ++s.generation;s.ring.Reset(s.generation);retired.swap(s.queue);s.queuedBytes=0;s.origin=s.lastQueued=-1;
        s.dropped=s.admitted=s.completedFrames=s.completed=0;s.lastPressureClock=-1;s.pressure=false;
    }
    s.latestCapture=clock;
    if(s.lastQueued>=0 && clock-s.lastQueued<10'000'000/policy->fps)return false;
    const auto bytes=FrameBytes(frame);
    if(s.queue.size()>=VideoRecapPolicy::kQueueFrames || bytes>VideoRecapPolicy::kQueueBytes-s.queuedBytes){++s.dropped;s.pressure=true;return false;}
    if(s.origin<0)s.origin=clock;
    const auto width=frame.width,height=frame.height;
    try{s.queue.push_back(std::move(frame));}catch(const std::bad_alloc&){++s.dropped;s.pressure=true;return false;}
    s.width=width;s.height=height;s.lastQueued=clock;s.queuedBytes+=bytes;
    s.changed.notify_one();return true;
}
std::shared_ptr<const VideoRecapSegment> RamVideoRecap::Acquire(std::int64_t target) const {return state_->ring.Acquire(target);}
VideoRecapSnapshot RamVideoRecap::Snapshot() const {
    std::lock_guard lock(state_->mutex);auto result=state_->ring.Snapshot();
    result.queuedBytes=state_->queuedBytes;result.queuedFrames=static_cast<unsigned>(state_->queue.size());
    result.droppedFrames=state_->dropped;result.admittedFrames=state_->admitted;
    result.completedFrames=state_->completedFrames;result.completedSegments=state_->completed;
    result.latestCapture100ns=state_->latestCapture<0 || state_->origin<0 ? -1:state_->latestCapture-state_->origin;
    result.captureOrigin100ns=state_->origin;
    result.maximumNativeCallUs=state_->maximumNativeCallUs;result.pressure=state_->pressure;result.error=state_->error;
    const auto start=state_->nativeStartedNs.load();
    if(start && NowNs()-start>3'000'000'000)result.error="Live recap native encoder is still busy; capture continues.";
    return result;
}
} // namespace okuflow
#endif
