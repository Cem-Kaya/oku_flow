#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <objidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace okuflow {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
namespace {
constexpr std::uint64_t kReadLimit = 64ull * 1024 * 1024;
constexpr std::uint64_t kMetadataLimit = 8ull * 1024 * 1024;
struct Range { std::uint64_t begin{}, end{}, token{}; };
bool Intersects(Range a, Range b) { return a.begin < b.end && b.begin < a.end; }
VideoFileIdentity Identity(HANDLE h) {
    VideoFileIdentity result;
    FILE_ID_INFO id{}; BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandle(h, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.volumeSerial = id.VolumeSerialNumber;
        std::memcpy(result.fileId.data(), id.FileId.Identifier, 16);
        result.sizeBytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        result.lastWriteTime = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime;
        result.valid = true;
    }
    return result;
}
struct FileBacking {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::mutex mutex;
    std::uint64_t length{};
    bool writable{};
    std::function<bool(std::uint64_t, std::uint64_t, std::uint64_t)> validate;
    std::uint64_t generation{};
    ~FileBacking() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

// The Source Reader decodes the same held handle whose identity was admitted.
// MF's IStream adapter supplies asynchronous byte-stream requests; no native
// codec or file operation is dispatched by a public player method.
class FileStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IStream> {
public:
    explicit FileStream(std::shared_ptr<FileBacking> b) : backing_(std::move(b)) {}
    STDMETHODIMP Read(void* dst, ULONG bytes, ULONG* read) override {
        std::lock_guard positionLock(positionMutex_);
        if (read) *read = 0;
        if ((!dst && bytes) || bytes > kReadLimit) return E_INVALIDARG;
        std::uint64_t length{};
        { std::lock_guard lock(backing_->mutex); length = backing_->length; }
        const auto amount = static_cast<DWORD>(std::min<std::uint64_t>(bytes,
            position_ < length ? length - position_ : 0));
        const auto checkedPosition = std::min(position_, length);
        if (backing_->validate && !backing_->validate(
                backing_->generation, checkedPosition, amount)) return E_ABORT;
        std::lock_guard lock(backing_->mutex);
        LARGE_INTEGER offset{}; offset.QuadPart = position_;
        DWORD got{};
        if (!SetFilePointerEx(backing_->handle, offset, nullptr, FILE_BEGIN) ||
            !ReadFile(backing_->handle, dst, amount, &got, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if (backing_->validate && !backing_->validate(
                backing_->generation, checkedPosition, got)) return E_ABORT;
        position_ += got;
        if (read) *read = got;
        return got == bytes ? S_OK : S_FALSE;
    }
    STDMETHODIMP Write(const void* src, ULONG bytes, ULONG* written) override {
        std::lock_guard positionLock(positionMutex_);
        if (written) *written = 0;
        if (!backing_->writable) return STG_E_ACCESSDENIED;
        if ((!src && bytes) || position_ > std::uint64_t(INT64_MAX) - bytes) return E_INVALIDARG;
        std::lock_guard lock(backing_->mutex);
        LARGE_INTEGER offset{}; offset.QuadPart = position_; DWORD got{};
        if (!SetFilePointerEx(backing_->handle, offset, nullptr, FILE_BEGIN) ||
            !WriteFile(backing_->handle, src, bytes, &got, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        position_ += got; backing_->length = std::max(backing_->length, position_);
        if (written) *written = got; return S_OK;
    }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        std::lock_guard positionLock(positionMutex_);
        std::uint64_t base{};
        if (origin == STREAM_SEEK_CUR) base = position_;
        else if (origin == STREAM_SEEK_END) { std::lock_guard lock(backing_->mutex); base = backing_->length; }
        else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        if ((move.QuadPart < 0 && std::uint64_t(-(move.QuadPart + 1)) + 1 > base) ||
            (move.QuadPart > 0 && base > std::uint64_t(INT64_MAX) - move.QuadPart))
            return STG_E_INVALIDFUNCTION;
        position_ = move.QuadPart < 0 ? base - (std::uint64_t(-(move.QuadPart + 1)) + 1)
                                    : base + move.QuadPart;
        if (result) result->QuadPart = position_;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER size) override {
        if (!backing_->writable) return STG_E_ACCESSDENIED;
        if (size.QuadPart > INT64_MAX) return E_INVALIDARG;
        std::lock_guard lock(backing_->mutex); LARGE_INTEGER end{}; end.QuadPart = size.QuadPart;
        if (!SetFilePointerEx(backing_->handle, end, nullptr, FILE_BEGIN) || !SetEndOfFile(backing_->handle))
            return HRESULT_FROM_WIN32(GetLastError());
        backing_->length = size.QuadPart; return S_OK;
    }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override {
        if (!backing_->writable) return S_OK;
        return FlushFileBuffers(backing_->handle) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    STDMETHODIMP Revert() override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::lock_guard lock(backing_->mutex);
        *stat = {}; stat->type = STGTY_STREAM; stat->cbSize.QuadPart = backing_->length;
        stat->grfMode = backing_->writable ? STGM_READWRITE : STGM_READ; return S_OK;
    }
    STDMETHODIMP Clone(IStream** stream) override {
        std::lock_guard positionLock(positionMutex_);
        if (!stream) return E_POINTER;
        auto copy = Make<FileStream>(backing_); copy->position_ = position_;
        return copy.CopyTo(stream);
    }
private:
    std::shared_ptr<FileBacking> backing_;
    // MF's byte-stream adapter may issue asynchronous IStream calls. Always
    // acquire position before the shared handle lock, including Clone/Seek.
    std::mutex positionMutex_;
    std::uint64_t position_{};
};
HRESULT MakeStream(std::shared_ptr<FileBacking> backing, IMFByteStream** stream) {
    auto input = Make<FileStream>(std::move(backing));
    if (!input) return E_OUTOFMEMORY;
    return MFCreateMFByteStreamOnStream(input.Get(), stream);
}
std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
        std::uint32_t(p[2]) << 8 | p[3];
}
std::uint64_t U64(const std::uint8_t* p) { return std::uint64_t(U32(p)) << 32 | U32(p + 4); }
constexpr std::uint32_t Tag(char a, char b, char c, char d) {
    return std::uint32_t(a) << 24 | std::uint32_t(b) << 16 | std::uint32_t(c) << 8 | d;
}
struct Box { std::uint32_t type{}; std::size_t payload{}, end{}; };
bool NextBox(const std::vector<std::uint8_t>& v, std::size_t& p, std::size_t end, Box& b) {
    if (p > end || end - p < 8) return false;
    std::uint64_t size = U32(v.data() + p); std::size_t header = 8;
    if (size == 1) { if (end - p < 16) return false; size = U64(v.data() + p + 8); header = 16; }
    if (size < header || size > end - p) return false;
    b = {U32(v.data() + p + 4), p + header, p + static_cast<std::size_t>(size)};
    p = b.end; return true;
}
struct Track {
    std::uint32_t id{}, timescale{}, duration{}, size{}, flags{};
    bool video{}, audio{}, config{};
    std::uint64_t configId{};
    std::uint64_t nextDecode{};
};
struct ParsedInit { std::vector<Track> tracks; bool ftyp{}, moov{}, syncVideo{}; std::uint32_t sequence{}; };
bool ScanTrack(const std::vector<std::uint8_t>& v, std::size_t start,
               std::size_t end, Track& t, unsigned depth = 0) {
    if (depth > 6) return false;
    for (auto p = start; p < end;) {
        Box b; if (!NextBox(v, p, end, b)) return false;
        const auto n = b.end - b.payload; const auto* d = v.data() + b.payload;
        if (b.type == Tag('t','k','h','d')) {
            const std::size_t o = n && d[0] == 1 ? 20 : 12;
            if (n < o + 4) return false; t.id = U32(d + o);
        } else if (b.type == Tag('m','d','h','d')) {
            const std::size_t o = n && d[0] == 1 ? 20 : 12;
            if (n < o + 4) return false; t.timescale = U32(d + o);
        } else if (b.type == Tag('h','d','l','r')) {
            if (n < 12) return false;
            t.video = U32(d + 8) == Tag('v','i','d','e');
            t.audio = U32(d + 8) == Tag('s','o','u','n');
        } else if (b.type == Tag('e','d','t','s')) {
            // Active timeline mapping for arbitrary edit lists is not proven.
            return false;
        } else if (b.type == Tag('s','t','t','s')) {
            if (n < 8 || U32(d + 4) != 0) return false;
        } else if (b.type == Tag('s','t','s','d')) {
            if (n < 16 || U32(d + 4) != 1) return false;
            const auto entry = U32(d + 12);
            const bool supported = entry == Tag('a','v','c','1') || entry == Tag('a','v','c','3') ||
                entry == Tag('a','v','0','1') || entry == Tag('h','v','c','1') ||
                entry == Tag('h','e','v','1') || entry == Tag('m','p','4','a');
            // Require the complete sample entry, including decoder config.
            if (U32(d + 8) < 16 || U32(d + 8) > n - 8) return false;
            const auto entryStart = b.payload + 8;
            const auto entryEnd = entryStart + U32(d + 8);
            const bool audio = entry == Tag('m','p','4','a');
            auto child = entryStart + (audio ? 36 : 86);
            if (!supported || child >= entryEnd) return false;
            for (; child < entryEnd;) {
                Box config; if (!NextBox(v, child, entryEnd, config)) return false;
                if (config.type == Tag('a','v','c','C') || config.type == Tag('h','v','c','C') ||
                    config.type == Tag('a','v','1','C') || config.type == Tag('e','s','d','s')) {
                    if (config.end - config.payload < 4) return false;
                    t.config = true;
                    std::uint64_t hash = 1469598103934665603ull;
                    for (auto i = config.payload; i < config.end; ++i) { hash ^= v[i]; hash *= 1099511628211ull; }
                    t.configId = hash;
                }
            }
        } else if (b.type == Tag('m','d','i','a') || b.type == Tag('m','i','n','f') ||
                   b.type == Tag('s','t','b','l')) {
            if (!ScanTrack(v, b.payload, b.end, t, depth + 1)) return false;
        }
    }
    return true;
}
bool ParseMoov(const std::vector<std::uint8_t>& v, ParsedInit& init) {
    Box root; std::size_t p{}; if (!NextBox(v, p, v.size(), root)) return false;
    for (p = root.payload; p < root.end;) {
        Box b; if (!NextBox(v, p, root.end, b)) return false;
        if (b.type == Tag('t','r','a','k')) {
            if (init.tracks.size() >= 8) return false;
            Track t; if (!ScanTrack(v, b.payload, b.end, t) || !t.id ||
                !t.timescale || !t.config || (!t.video && !t.audio)) return false;
            init.tracks.push_back(t);
        }
    }
    for (p = root.payload; p < root.end;) {
        Box b; if (!NextBox(v, p, root.end, b)) return false;
        if (b.type != Tag('m','v','e','x')) continue;
        for (auto q = b.payload; q < b.end;) {
            Box x; if (!NextBox(v, q, b.end, x)) return false;
            if (x.type != Tag('t','r','e','x')) continue;
            if (x.end - x.payload < 24) return false;
            const auto* d = v.data() + x.payload;
            for (auto& t : init.tracks) if (t.id == U32(d + 4)) {
                if (U32(d + 8) != 1) return false;
                t.duration = U32(d + 12); t.size = U32(d + 16); t.flags = U32(d + 20);
            }
        }
    }
    return std::any_of(init.tracks.begin(), init.tracks.end(), [](auto& t) { return t.video; });
}
struct Fragment {
    std::vector<Range> data;
    std::int64_t lastPts{-1}, duration{};
    std::uint64_t videos{};
    bool syncVideo{};
    std::uint64_t samples{};
    std::vector<std::pair<std::uint32_t, std::uint64_t>> decodeEnds;
    std::uint32_t sequence{};
};
bool MediaTime(std::uint64_t time, std::uint32_t scale, std::int64_t& result) {
    if (!scale || time / scale > std::uint64_t(INT64_MAX / 10'000'000)) return false;
    result = static_cast<std::int64_t>(time / scale * 10'000'000 +
        time % scale * 10'000'000 / scale); return true;
}
bool ParseMoof(const std::vector<std::uint8_t>& v, std::uint64_t fileOffset,
               const ParsedInit& init, Fragment& fragment) {
    Box root; std::size_t p{}; if (!NextBox(v, p, v.size(), root)) return false;
    bool firstTraf = true;
    for (p = root.payload; p < root.end;) {
        Box b; if (!NextBox(v, p, root.end, b)) return false;
        if (b.type == Tag('m','f','h','d')) {
            if (b.end - b.payload < 8) return false;
            fragment.sequence = U32(v.data() + b.payload + 4);
        }
    }
    if (!fragment.sequence || fragment.sequence != init.sequence + 1) return false;
    for (p = root.payload; p < root.end;) {
        Box traf; if (!NextBox(v, p, root.end, traf)) return false;
        if (traf.type != Tag('t','r','a','f')) continue;
        Track track; bool found{}, decodeFound{}, explicitBase{}; std::uint64_t base = fileOffset, decode{};
        for (auto q = traf.payload; q < traf.end;) {
            Box b; if (!NextBox(v, q, traf.end, b)) return false;
            const auto n = b.end - b.payload; const auto* d = v.data() + b.payload;
            if (b.type == Tag('t','f','h','d')) {
                if (n < 8) return false; const auto flags = U32(d) & 0xffffff;
                if (!(flags & 1) && !(flags & 0x020000) && !firstTraf) return false;
                const auto it = std::find_if(init.tracks.begin(), init.tracks.end(),
                    [&](auto& t) { return t.id == U32(d + 4); });
                if (it == init.tracks.end()) return false; track = *it; found = true;
                // The native Windows sink omits tfdt. In an unbroken
                // sequence with empty initial sample tables, the next decode
                // time is exactly the verified preceding track duration.
                if (!decodeFound) decode = track.nextDecode;
                std::size_t o = 8;
                auto get = [&](std::uint32_t& value) { if (o + 4 > n) return false; value = U32(d + o); o += 4; return true; };
                if (flags & 1) { if (o + 8 > n) return false; base = U64(d + o); o += 8; explicitBase = true; }
                if (flags & 2) { std::uint32_t desc{}; if (!get(desc) || desc != 1) return false; }
                if ((flags & 8) && !get(track.duration)) return false;
                if ((flags & 16) && !get(track.size)) return false;
                if ((flags & 32) && !get(track.flags)) return false;
            } else if (b.type == Tag('t','f','d','t')) {
                if (n < 8 || d[0] > 1 || (d[0] == 1 && n < 12)) return false;
                decode = d[0] == 1 ? U64(d + 4) : U32(d + 4); decodeFound = true;
            }
        }
        if (!found) return false;
        firstTraf = false;
        std::uint64_t cursor = base; bool cursorKnown = explicitBase;
        for (auto q = traf.payload; q < traf.end;) {
            Box b; if (!NextBox(v, q, traf.end, b)) return false;
            if (b.type != Tag('t','r','u','n')) continue;
            const auto n = b.end - b.payload; const auto* d = v.data() + b.payload;
            if (n < 8) return false;
            const auto flags = U32(d) & 0xffffff; const auto count = U32(d + 4);
            if (d[0] > 1 || ((flags & 4) && (flags & 0x400))) return false;
            if (count > 65536 || fragment.samples > 65536 - count || fragment.data.size() >= 64) return false;
            fragment.samples += count;
            std::size_t o = 8; std::uint32_t firstFlags = track.flags;
            auto get = [&](std::uint32_t& x) { if (o + 4 > n) return false; x = U32(d + o); o += 4; return true; };
            if (flags & 1) {
                std::uint32_t raw{}; if (!get(raw)) return false;
                const auto delta = static_cast<std::int32_t>(raw);
                if (delta < 0 && std::uint64_t(-std::int64_t(delta)) > base) return false;
                if (delta > 0 && base > UINT64_MAX - delta) return false;
                cursor = delta < 0 ? base - std::uint64_t(-std::int64_t(delta)) : base + delta;
                cursorKnown = true;
            }
            if (!cursorKnown || ((flags & 4) && !get(firstFlags))) return false;
            const auto begin = cursor;
            for (std::uint32_t i = 0; i < count; ++i) {
                auto duration = track.duration, size = track.size;
                auto sampleFlags = i == 0 ? firstFlags : track.flags; std::uint32_t cto{};
                if ((flags & 0x100) && !get(duration)) return false;
                if ((flags & 0x200) && !get(size)) return false;
                if ((flags & 0x400) && !get(sampleFlags)) return false;
                if ((flags & 0x800) && !get(cto)) return false;
                if (!duration || !size || cursor > UINT64_MAX - size || decode > UINT64_MAX - duration) return false;
                const std::int64_t composition = d[0] == 1 ? static_cast<std::int32_t>(cto) : std::int64_t(cto);
                if (decode > std::uint64_t(INT64_MAX) ||
                    (composition < 0 && std::uint64_t(-composition) > decode) ||
                    (composition > 0 && decode > std::uint64_t(INT64_MAX - composition))) return false;
                if (track.video) {
                    std::int64_t pts{}, end{};
                    if (!MediaTime(static_cast<std::uint64_t>(std::int64_t(decode) + composition), track.timescale, pts) ||
                        !MediaTime(static_cast<std::uint64_t>(std::int64_t(decode) + composition) + duration, track.timescale, end)) return false;
                    fragment.lastPts = std::max(fragment.lastPts, pts);
                    fragment.duration = std::max(fragment.duration, end);
                    fragment.syncVideo |= (sampleFlags & 0x10000) == 0;
                    ++fragment.videos;
                }
                cursor += size; decode += duration;
            }
            fragment.data.push_back({begin, cursor, 0});
        }
        fragment.decodeEnds.push_back({track.id, decode});
    }
    return fragment.videos && (fragment.syncVideo || init.syncVideo);
}
} // namespace

struct RecordingArchive::State {
    std::mutex mutex; std::condition_variable changed;
    std::atomic<bool> stop{};
    RecordingArchiveSnapshot snapshot;
    std::array<Range, 64> pending{};
    std::array<Range, 128> completed{};
    std::size_t pendingCount{}, completedCount{};
    std::uint64_t token{}, highWater{};
    bool lost{};
    std::uint64_t UnknownWrite() {
        std::lock_guard lock(mutex);
        ++snapshot.mutationSequence; ++snapshot.generation; lost = true;
        snapshot.verifiedMediaBytes = 0; snapshot.seekableEnd100ns = -1;
        snapshot.availability = ActiveRewindAvailability::Invalidated;
        snapshot.error = "Recording write position could not be observed; no byte credit was granted.";
        changed.notify_one(); return 0;
    }
    std::uint64_t Begin(std::uint64_t begin, std::uint64_t bytes) {
        std::lock_guard lock(mutex);
        ++snapshot.mutationSequence;
        if (lost) return 0;
        if (bytes > UINT64_MAX - begin || pendingCount == pending.size() || pendingCount != 0) {
            lost = true; ++snapshot.generation; snapshot.availability = ActiveRewindAvailability::Invalidated;
            snapshot.error = "Recording write observation exceeded its bounded mutation ledger.";
            return 0;
        }
        const Range range{begin, begin + bytes, ++token};
        if (bytes && begin < highWater) { // synchronous BEFORE the underlying mutation
            ++snapshot.generation; snapshot.verifiedMediaBytes = 0; snapshot.seekableEnd100ns = -1;
            snapshot.duration100ns = 0; snapshot.videoSamples = 0; snapshot.codecConfigurationId = 0;
            snapshot.availability = ActiveRewindAvailability::WaitingForMedia;
        }
        pending[pendingCount++] = range;
        changed.notify_one(); return range.token;
    }
    void End(std::uint64_t t, std::uint64_t bytes, HRESULT hr) {
        std::lock_guard lock(mutex);
        auto it = std::find_if(pending.begin(), pending.begin() + pendingCount,
            [&](auto& r) { return r.token == t; });
        if (it == pending.begin() + pendingCount) return;
        Range r = *it; *it = pending[--pendingCount];
        if (lost) { changed.notify_one(); return; }
        if (r.begin == r.end && SUCCEEDED(hr) && bytes == 0) { changed.notify_one(); return; }
        const bool lengthMutation = r.begin == 0 && r.end == UINT64_MAX;
        if (FAILED(hr) || (!lengthMutation && bytes != r.end - r.begin)) {
            ++snapshot.generation; snapshot.verifiedMediaBytes = 0;
            snapshot.seekableEnd100ns = -1; snapshot.availability = ActiveRewindAvailability::Failed;
            snapshot.error = "A recording byte-stream mutation did not complete successfully.";
            lost = true; changed.notify_one(); return;
        }
        r.end = r.begin + bytes; highWater = std::max(highWater, r.end);
        // Merge successful intervals; keep memory independent of duration.
        for (std::size_t i = 0; i < completedCount;) {
            if (r.begin <= completed[i].end && completed[i].begin <= r.end) {
                r.begin = std::min(r.begin, completed[i].begin);
                r.end = std::max(r.end, completed[i].end);
                completed[i] = completed[--completedCount];
            } else ++i;
        }
        if (completedCount == completed.size()) { lost = true; ++snapshot.generation; }
        else completed[completedCount++] = r;
        snapshot.committedBytes = 0;
        for (std::size_t i = 0; i < completedCount; ++i)
            if (completed[i].begin == 0) snapshot.committedBytes = completed[i].end;
        changed.notify_one();
    }
    bool Valid(std::uint64_t gen, std::uint64_t begin, std::uint64_t bytes,
               bool verified) {
        std::lock_guard lock(mutex);
        if (lost || gen != snapshot.generation || bytes > UINT64_MAX - begin ||
            begin + bytes > (verified ? snapshot.verifiedMediaBytes : snapshot.committedBytes)) return false;
        const Range range{begin, begin + bytes, 0};
        for (std::size_t i = 0; i < pendingCount; ++i) if (Intersects(range, pending[i])) return false;
        return true;
    }
};

namespace {
bool ReadParser(HANDLE file, const std::shared_ptr<RecordingArchive::State>& state,
                std::uint64_t generation, std::uint64_t offset, std::size_t size,
                std::vector<std::uint8_t>& bytes) {
    if (size > kMetadataLimit || !state->Valid(generation, offset, size, false)) return false;
    bytes.resize(size); LARGE_INTEGER pos{}; pos.QuadPart = offset; DWORD got{};
    return SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) &&
        ReadFile(file, bytes.data(), static_cast<DWORD>(size), &got, nullptr) &&
        got == size && state->Valid(generation, offset, size, false);
}
void ParseArchive(std::shared_ptr<RecordingArchive::State> state) {
    struct ParserFile {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~ParserFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } ownedFile;
    auto& file = ownedFile.value;
    bool finalizedHandle{};
    ParsedInit init; Fragment fragment; std::uint64_t offset{}, gen{}, moofEnd{}, unsupportedGeneration{};
    std::vector<std::uint8_t> bytes;
    while (!state->stop.load()) {
        RecordingArchiveSnapshot snap;
        { std::unique_lock lock(state->mutex);
          state->changed.wait_for(lock, std::chrono::milliseconds(100));
          snap = state->snapshot;
          if (state->lost) return;
        }
        // More appends cannot repair a complete unsupported box. A producer
        // rewrite/finalization advances generation and permits one new proof.
        if (snap.generation == unsupportedGeneration) {
            if (snap.finalized) return;
            continue;
        }
        if (snap.finalized && !finalizedHandle && file != INVALID_HANDLE_VALUE) {
            CloseHandle(file); file = INVALID_HANDLE_VALUE;
        }
        if (file == INVALID_HANDLE_VALUE) {
            file = CreateFileW(snap.path.c_str(), GENERIC_READ,
                snap.finalized ? FILE_SHARE_READ | FILE_SHARE_DELETE : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                std::lock_guard lock(state->mutex);
                state->snapshot.availability = error == ERROR_SHARING_VIOLATION
                    ? ActiveRewindAvailability::SharingDenied : ActiveRewindAvailability::Failed;
                state->snapshot.error = "The native recording stream does not currently permit a shared read handle.";
                if (snap.finalized) return;
                continue;
            }
            const auto openedIdentity = Identity(file);
            std::lock_guard lock(state->mutex);
            if (snap.file.valid && !openedIdentity.SameFile(snap.file)) {
                state->lost = true; ++state->snapshot.generation;
                state->snapshot.availability = ActiveRewindAvailability::Invalidated;
                state->snapshot.error = "The finalized recording path no longer names the recorded file.";
                continue;
            }
            state->snapshot.file = openedIdentity; finalizedHandle = snap.finalized;
        }
        const auto currentIdentity = Identity(file);
        {
            std::lock_guard lock(state->mutex);
            if (!currentIdentity.SameFile(state->snapshot.file)) {
                state->lost = true; ++state->snapshot.generation;
                state->snapshot.availability = ActiveRewindAvailability::Invalidated;
                state->snapshot.error = "The recording file identity could no longer be verified.";
                continue;
            }
            state->snapshot.file = currentIdentity;
        }
        if (gen != snap.generation) { gen = snap.generation; offset = 0; init = {}; fragment = {}; moofEnd = 0; }
        const auto beforeOffset = offset;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        unsigned boxes{};
        while (!state->stop.load() && boxes++ < 128 && std::chrono::steady_clock::now() < deadline) {
            if (offset > snap.committedBytes || snap.committedBytes - offset < 8 ||
                !ReadParser(file, state, gen, offset, 8, bytes)) break;
            { std::lock_guard lock(state->mutex); ++state->snapshot.parserBoxAttempts; }
            auto length = std::uint64_t(U32(bytes.data())); const auto type = U32(bytes.data() + 4);
            std::uint64_t header = 8;
            if (length == 1) {
                if (!ReadParser(file, state, gen, offset, 16, bytes)) break;
                length = U64(bytes.data() + 8); header = 16;
            }
            if (!length || length < header || length > UINT64_MAX - offset) {
                unsupportedGeneration = gen;
                std::lock_guard lock(state->mutex);
                state->snapshot.availability = ActiveRewindAvailability::UnsupportedContainer;
                state->snapshot.error = "Recording container has open-ended or invalid box boundaries."; break;
            }
            if (length > snap.committedBytes - offset ||
                !state->Valid(gen, offset, length, false)) break;
            bool ok = true;
            if (type == Tag('f','t','y','p')) init.ftyp = true;
            else if (type == Tag('m','o','o','v')) {
                ok = !init.moov && ReadParser(file, state, gen, offset, static_cast<std::size_t>(length), bytes) && ParseMoov(bytes, init);
                init.moov = ok;
            } else if (type == Tag('m','o','o','f')) {
                fragment = {};
                ok = init.ftyp && init.moov && length <= 1024 * 1024 &&
                    ReadParser(file, state, gen, offset, static_cast<std::size_t>(length), bytes) &&
                    ParseMoof(bytes, offset, init, fragment);
                moofEnd = offset + length;
            } else if (type == Tag('m','d','a','t') && moofEnd) {
                std::sort(fragment.data.begin(), fragment.data.end(), [](auto& a, auto& b) { return a.begin < b.begin; });
                for (std::size_t i = 1; i < fragment.data.size(); ++i)
                    ok &= fragment.data[i - 1].end <= fragment.data[i].begin;
                for (const auto& range : fragment.data)
                    ok &= range.begin >= offset + header && range.end <= offset + length && range.begin < range.end;
                if (ok) {
                    // Decode continuity advances only AFTER all referenced
                    // payload bytes are complete and generation-stable.
                    for (const auto& entry : fragment.decodeEnds)
                        for (auto& t : init.tracks) if (t.id == entry.first) t.nextDecode = entry.second;
                    init.sequence = fragment.sequence;
                    init.syncVideo |= fragment.syncVideo;
                    std::lock_guard lock(state->mutex);
                    if (gen == state->snapshot.generation && !state->lost) {
                        state->snapshot.verifiedMediaBytes = offset + length;
                        state->snapshot.seekableEnd100ns = std::max(state->snapshot.seekableEnd100ns, fragment.lastPts);
                        state->snapshot.duration100ns = std::max(state->snapshot.duration100ns, fragment.duration);
                        state->snapshot.videoSamples += fragment.videos;
                        state->snapshot.hasAudio = std::any_of(init.tracks.begin(), init.tracks.end(), [](auto& t) { return t.audio; });
                        for (const auto& t : init.tracks) if (t.video) {
                            state->snapshot.videoTrackId = t.id; state->snapshot.codecConfigurationId = t.configId; break;
                        }
                        state->snapshot.availability = state->snapshot.finalized
                            ? ActiveRewindAvailability::Finalized : ActiveRewindAvailability::Available;
                        state->snapshot.error.clear();
                    }
                }
                moofEnd = 0;
            }
            if (!ok) {
                unsupportedGeneration = gen;
                const std::string detail = type == Tag('m','o','o','v') ? "initialization/configuration" :
                    (type == Tag('m','o','o','f') ? "fragment sequence/timestamps/sample tables" : "fragment payload boundaries");
                const auto message = "Active rewind cannot prove native MP4 " + detail + " at byte " + std::to_string(offset) + ".";
                std::lock_guard lock(state->mutex);
                state->snapshot.availability = ActiveRewindAvailability::UnsupportedContainer;
                state->snapshot.error = message;
                break;
            }
            offset += length;
        }
        // A finalized file cannot acquire another producer extent. Finish
        // bounded batches while progressing, then retire both the metadata
        // scratch and read handle even if a receipt retains this facade.
        if (snap.finalized && (offset == beforeOffset || offset >= snap.committedBytes)) {
            std::lock_guard lock(state->mutex);
            if (gen == state->snapshot.generation &&
                snap.committedBytes == state->snapshot.committedBytes) return;
        }
    }
}

class WriteResult final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IUnknown> {
public: ULONG bytes{}; HRESULT result{E_PENDING};
};
class WriteCallback final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFAsyncCallback> {
public:
    WriteCallback(IMFByteStream* stream, std::shared_ptr<RecordingArchive::State> state,
                  std::uint64_t token, IMFAsyncCallback* callback, IUnknown* userState)
        : stream_(stream), state_(std::move(state)), token_(token), callback_(callback), userState_(userState) {}
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        auto completed = Make<WriteResult>();
        completed->result = stream_->EndWrite(result, &completed->bytes);
        state_->End(token_, completed->bytes, completed->result);
        ComPtr<IMFAsyncResult> outgoing;
        const auto hr = MFCreateAsyncResult(completed.Get(), callback_.Get(), userState_.Get(), &outgoing);
        if (FAILED(hr)) return hr;
        outgoing->SetStatus(completed->result); return MFInvokeCallback(outgoing.Get());
    }
private:
    ComPtr<IMFByteStream> stream_; std::shared_ptr<RecordingArchive::State> state_;
    std::uint64_t token_; ComPtr<IMFAsyncCallback> callback_; ComPtr<IUnknown> userState_;
};
class ObservedStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFByteStream> {
public:
    ObservedStream(IMFByteStream* s, std::shared_ptr<RecordingArchive::State> state) : stream_(s), state_(std::move(state)) {}
    STDMETHODIMP GetCapabilities(DWORD* x) override { return stream_->GetCapabilities(x); }
    STDMETHODIMP GetLength(QWORD* x) override { return stream_->GetLength(x); }
    STDMETHODIMP SetLength(QWORD x) override {
        const auto t = state_->Begin(0, UINT64_MAX);
        const auto hr = stream_->SetLength(x);
        if (SUCCEEDED(hr)) {
            std::lock_guard lock(state_->mutex);
            for (std::size_t i = 0; i < state_->completedCount;) {
                auto& r = state_->completed[i];
                if (r.begin >= x) r = state_->completed[--state_->completedCount];
                else { r.end = std::min(r.end, x); ++i; }
            }
            state_->highWater = std::min(state_->highWater, x);
            state_->snapshot.verifiedMediaBytes = 0;
            state_->snapshot.seekableEnd100ns = -1;
            state_->snapshot.duration100ns = 0;
            state_->snapshot.videoSamples = 0;
            state_->snapshot.availability = ActiveRewindAvailability::WaitingForMedia;
        }
        // Zero-filled extension is never successful media-write credit.
        // Shrink/rewrites reparse under the new producer generation.
        state_->End(t, 0, hr);
        return hr;
    }
    STDMETHODIMP GetCurrentPosition(QWORD* x) override { return stream_->GetCurrentPosition(x); }
    STDMETHODIMP SetCurrentPosition(QWORD x) override { return stream_->SetCurrentPosition(x); }
    STDMETHODIMP IsEndOfStream(BOOL* x) override { return stream_->IsEndOfStream(x); }
    STDMETHODIMP Read(BYTE* x, ULONG n, ULONG* got) override { return stream_->Read(x, n, got); }
    STDMETHODIMP BeginRead(BYTE* x, ULONG n, IMFAsyncCallback* c, IUnknown* s) override { return stream_->BeginRead(x, n, c, s); }
    STDMETHODIMP EndRead(IMFAsyncResult* r, ULONG* n) override { return stream_->EndRead(r, n); }
    STDMETHODIMP Write(const BYTE* data, ULONG bytes, ULONG* written) override {
        QWORD position{}; const auto positionHr = stream_->GetCurrentPosition(&position);
        const auto t = FAILED(positionHr) ? state_->UnknownWrite() : state_->Begin(position, bytes);
        ULONG got{}; const auto hr = stream_->Write(data, bytes, &got);
        state_->End(t, got, hr); if (written) *written = got; return hr;
    }
    STDMETHODIMP BeginWrite(const BYTE* data, ULONG bytes, IMFAsyncCallback* callback, IUnknown* userState) override {
        if (!callback) return E_INVALIDARG;
        QWORD position{}; const auto positionHr = stream_->GetCurrentPosition(&position);
        const auto t = FAILED(positionHr) ? state_->UnknownWrite() : state_->Begin(position, bytes);
        auto proxy = Make<WriteCallback>(stream_.Get(), state_, t, callback, userState);
        const auto hr = stream_->BeginWrite(data, bytes, proxy.Get(), nullptr);
        if (FAILED(hr)) state_->End(t, 0, hr); return hr;
    }
    STDMETHODIMP EndWrite(IMFAsyncResult* r, ULONG* written) override {
        if (!r || !written) return E_POINTER;
        ComPtr<IUnknown> object; const auto hr = r->GetObject(&object);
        if (FAILED(hr)) return hr;
        // Only results created by our callback are passed to this method.
        auto* result = static_cast<WriteResult*>(object.Get());
        *written = result->bytes; return result->result;
    }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD flags, QWORD* position) override {
        return stream_->Seek(origin, offset, flags, position);
    }
    STDMETHODIMP Flush() override { return stream_->Flush(); }
    STDMETHODIMP Close() override { return stream_->Close(); }
private: ComPtr<IMFByteStream> stream_; std::shared_ptr<RecordingArchive::State> state_;
};
} // namespace

bool VideoFileIdentity::SameFile(const VideoFileIdentity& b) const {
    return valid && b.valid && volumeSerial == b.volumeSerial && fileId == b.fileId;
}
RecordingArchive::RecordingArchive(std::wstring path) : state_(std::make_shared<State>()) {
    state_->snapshot.path = std::move(path);
    GUID id{}; wchar_t text[40]{};
    if (SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, text, 40)) {
        for (auto p = text; *p; ++p) state_->snapshot.attemptId.push_back(static_cast<char>(*p));
    }
    std::thread([state = state_] {
        try { ParseArchive(state); }
        catch (...) {
            std::lock_guard lock(state->mutex); state->lost = true;
            ++state->snapshot.generation;
            state->snapshot.availability = ActiveRewindAvailability::Failed;
            // No allocation in an out-of-memory recovery path.
            state->snapshot.error.clear();
        }
    }).detach();
}
RecordingArchive::~RecordingArchive() { state_->stop.store(true); state_->changed.notify_one(); }
RecordingArchiveSnapshot RecordingArchive::Snapshot() const { std::lock_guard lock(state_->mutex); return state_->snapshot; }
void RecordingArchive::WriterAccepted(std::int64_t pts) {
    std::lock_guard lock(state_->mutex); state_->snapshot.latestWriterAccepted100ns = pts;
}
void RecordingArchive::Finalized(bool complete) {
    std::lock_guard lock(state_->mutex); state_->snapshot.finalized = true; state_->snapshot.complete = complete;
    // Retire active read handles' write-sharing policy. New finalized leases
    // deny writers; old decoded pixels remain private and usable.
    ++state_->snapshot.generation;
    state_->snapshot.videoSamples = 0; state_->snapshot.verifiedMediaBytes = 0;
    state_->snapshot.seekableEnd100ns = -1;
    // Duration is previously proven mapping evidence for retained multipart
    // receipts. Revoking read ranges must not temporarily collapse offsets.
    if (!state_->lost) state_->snapshot.availability = ActiveRewindAvailability::WaitingForMedia;
    state_->changed.notify_one();
}
void RecordingArchive::Abandoned() {
    std::lock_guard lock(state_->mutex); state_->lost = true; ++state_->snapshot.generation;
    state_->snapshot.availability = ActiveRewindAvailability::Failed;
    state_->snapshot.error = "Recording worker was abandoned; no new active archive leases are admitted.";
}
bool RecordingArchive::ValidateRange(std::uint64_t gen, std::uint64_t begin, std::uint64_t bytes) const {
    return state_->Valid(gen, begin, bytes, true);
}
HRESULT RecordingArchive::Observe(IMFByteStream* stream, const std::shared_ptr<RecordingArchive>& archive, IMFByteStream** result) {
    if (!stream || !archive || !result) return E_POINTER;
    auto observed = Make<ObservedStream>(stream, archive->state_);
    return observed ? observed.CopyTo(result) : E_OUTOFMEMORY;
}
HRESULT OpenStableVideoReadStream(const std::wstring& path, IMFByteStream** stream, VideoFileIdentity* identity) {
    if (!stream || !identity) return E_POINTER; *stream = nullptr;
    auto backing = std::make_shared<FileBacking>();
    backing->handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (backing->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    *identity = Identity(backing->handle);
    if (!identity->valid) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    backing->length = identity->sizeBytes;
    return MakeStream(std::move(backing), stream);
}
HRESULT CreateRecordingArchiveOutputStream(const std::wstring& path, IMFByteStream** stream) {
    if (!stream) return E_POINTER; *stream = nullptr;
    auto backing = std::make_shared<FileBacking>();
    backing->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (backing->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    backing->writable = true;
    return MakeStream(std::move(backing), stream);
}
HRESULT RecordingArchive::OpenReadStream(IMFByteStream** stream, RecordingArchiveSnapshot* admitted) const {
    if (!stream) return E_POINTER; *stream = nullptr;
    const auto snap = Snapshot();
    if (snap.verifiedMediaBytes == 0 || snap.seekableEnd100ns < 0 ||
        !ValidateRange(snap.generation, 0, snap.verifiedMediaBytes)) return MF_E_NOT_AVAILABLE;
    auto backing = std::make_shared<FileBacking>();
    backing->handle = CreateFileW(snap.path.c_str(), GENERIC_READ,
        snap.finalized ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (backing->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const auto heldIdentity = Identity(backing->handle);
    if (!heldIdentity.SameFile(snap.file) ||
        (snap.finalized && (heldIdentity.sizeBytes != snap.file.sizeBytes ||
                           heldIdentity.lastWriteTime != snap.file.lastWriteTime)) ||
        !ValidateRange(snap.generation, 0, snap.verifiedMediaBytes)) return E_ABORT;
    backing->validate = [state = state_](auto generation, auto begin, auto bytes) {
        return state->Valid(generation, begin, bytes, true);
    };
    backing->generation = snap.generation; backing->length = snap.verifiedMediaBytes;
    if (admitted) *admitted = snap;
    return MakeStream(std::move(backing), stream);
}
} // namespace okuflow
#endif
