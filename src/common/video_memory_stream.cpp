#ifdef _WIN32

#include "okuflow/common/video_memory_stream.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace okuflow {
namespace {

// Reject an over-allocation before it happens, even if a vector implementation
// requests more than reserve's argument. Reserve once at the first growth:
// there are never two compressed-byte allocations during vector expansion.
template<class T> struct BoundedAllocator {
    using value_type = T;
    std::size_t limit{};
    explicit BoundedAllocator(std::size_t bound = 0) noexcept : limit(bound) {}
    template<class U> BoundedAllocator(const BoundedAllocator<U>& other) noexcept
        : limit(other.limit) {}
    T* allocate(std::size_t count) {
        if (count > max_size()) throw std::bad_alloc();
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(data, count);
    }
    std::size_t max_size() const noexcept {
        // MSVC debug iterators rebind the allocator for a small container
        // proxy. That metadata is not compressed-byte capacity.
        if constexpr (std::is_same_v<T, BYTE>) return limit;
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }
    template<class U> bool operator==(const BoundedAllocator<U>& other) const noexcept {
        return limit == other.limit;
    }
};

} // namespace

struct VideoMemoryBacking {
    explicit VideoMemoryBacking(std::size_t maximum)
        : limit(std::min(maximum, static_cast<std::size_t>(
              std::numeric_limits<std::int64_t>::max()))),
          bytes(BoundedAllocator<BYTE>(limit)) {}
    mutable std::mutex mutex;
    const std::size_t limit;
    std::vector<BYTE, BoundedAllocator<BYTE>> bytes;
    std::size_t writableCursors{};
    bool sealed{}, recorderAttached{}, recordingActive{}, finalized{};
    HRESULT writeError{S_OK};
};

namespace {
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

HRESULT Resize(VideoMemoryBacking& backing, std::size_t size) {
    if (size > backing.limit) return STG_E_MEDIUMFULL;
    try {
        if (size > backing.bytes.capacity()) backing.bytes.reserve(backing.limit);
        backing.bytes.resize(size); // zero-fill holes and newly extended bytes
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (const std::length_error&) { return STG_E_MEDIUMFULL; }
}

class MemoryStream final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IStream> {
public:
    // Caller holds the shared mutex during creation and cursor accounting.
    MemoryStream(std::shared_ptr<const VideoMemoryBacking> backing,
                 std::shared_ptr<VideoMemoryBacking> writable = {},
                 std::size_t position = 0)
        : backing_(std::move(backing)), writable_(std::move(writable)),
          position_(position) {
        if (writable_) ++writable_->writableCursors;
    }
    ~MemoryStream() {
        if (writable_) {
            std::lock_guard lock(writable_->mutex);
            --writable_->writableCursors;
        }
    }
    STDMETHODIMP Read(void* data, ULONG bytes, ULONG* read) override {
        if (read) *read = 0;
        if (!data && bytes) return STG_E_INVALIDPOINTER;
        std::lock_guard lock(backing_->mutex);
        const auto size = backing_->bytes.size();
        const auto count = std::min<std::size_t>(bytes,
            position_ < size ? size - position_ : 0);
        if (count) std::memcpy(data, backing_->bytes.data() + position_, count);
        position_ += count;
        if (read) *read = static_cast<ULONG>(count);
        return count == bytes ? S_OK : S_FALSE;
    }
    STDMETHODIMP Write(const void* data, ULONG bytes, ULONG* written) override {
        if (written) *written = 0;
        if (!writable_) return STG_E_ACCESSDENIED;
        if (!data && bytes) return STG_E_INVALIDPOINTER;
        std::lock_guard lock(writable_->mutex);
        if (writable_->sealed ||
            (writable_->recorderAttached && !writable_->recordingActive))
            return STG_E_ACCESSDENIED;
        HRESULT hr = S_OK;
        if (position_ > writable_->limit || bytes > writable_->limit - position_)
            hr = STG_E_MEDIUMFULL;
        else if (bytes && position_ + bytes > writable_->bytes.size())
            hr = Resize(*writable_, position_ + bytes);
        if (FAILED(hr)) {
            if (SUCCEEDED(writable_->writeError)) writable_->writeError = hr;
            return hr;
        }
        if (bytes) std::memcpy(writable_->bytes.data() + position_, data, bytes);
        position_ += bytes;
        if (written) *written = bytes;
        return S_OK;
    }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        std::lock_guard lock(backing_->mutex);
        std::size_t base{};
        if (origin == STREAM_SEEK_CUR) base = position_;
        else if (origin == STREAM_SEEK_END) base = backing_->bytes.size();
        else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        // Avoid negating INT64_MIN or overflowing either signed/unsigned sum.
        const auto negative = move.QuadPart < 0;
        const auto distance = negative
            ? static_cast<std::uint64_t>(-(move.QuadPart + 1)) + 1
            : static_cast<std::uint64_t>(move.QuadPart);
        const auto bound = writable_ ? backing_->limit : backing_->bytes.size();
        if ((negative && distance > base) ||
            (!negative && (base > bound || distance > bound - base)))
            return STG_E_INVALIDFUNCTION;
        const auto next = negative ? base - distance : base + distance;
        if (next > bound) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<std::size_t>(next);
        if (result) result->QuadPart = position_;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER size) override {
        if (!writable_) return STG_E_ACCESSDENIED;
        std::lock_guard lock(writable_->mutex);
        if (writable_->sealed ||
            (writable_->recorderAttached && !writable_->recordingActive))
            return STG_E_ACCESSDENIED;
        const auto hr = size.QuadPart > writable_->limit ? STG_E_MEDIUMFULL
            : Resize(*writable_, static_cast<std::size_t>(size.QuadPart));
        if (FAILED(hr) && SUCCEEDED(writable_->writeError)) writable_->writeError = hr;
        return hr;
    }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER* read,
                        ULARGE_INTEGER* written) override {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::lock_guard lock(backing_->mutex);
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = backing_->bytes.size();
        stat->grfMode = writable_ ? STGM_READWRITE : STGM_READ;
        return S_OK; // no fabricated pathname or allocated name
    }
    STDMETHODIMP Clone(IStream** stream) override {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        // Unlock before CopyTo/local COM destruction (destructor takes mutex).
        ComPtr<MemoryStream> copy;
        {
            std::lock_guard lock(backing_->mutex);
            if (writable_ && (writable_->sealed ||
                (writable_->recorderAttached && !writable_->recordingActive)))
                return STG_E_ACCESSDENIED;
            copy = Make<MemoryStream>(backing_, writable_, position_);
        }
        return copy ? copy.CopyTo(stream) : E_OUTOFMEMORY;
    }
private:
    std::shared_ptr<const VideoMemoryBacking> backing_;
    std::shared_ptr<VideoMemoryBacking> writable_;
    std::size_t position_{}; // serialized by backing mutex, independent per clone
};

// Keep the mutable cursor through the adapter's final COM Release, including
// after Close. MF is allowed to release its own IStream reference on Close;
// retaining it here makes the explicit writable-adapter Seal fence reliable.
// Begin/End methods use MF's own adapter, with no new I/O queue or worker.
class WriteAdapter final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFByteStream> {
public:
    WriteAdapter(ComPtr<IStream> cursor, ComPtr<IMFByteStream> adapter)
        : cursor_(std::move(cursor)), adapter_(std::move(adapter)) {}
    STDMETHODIMP GetCapabilities(DWORD* value) override { return adapter_->GetCapabilities(value); }
    STDMETHODIMP GetLength(QWORD* value) override { return adapter_->GetLength(value); }
    STDMETHODIMP SetLength(QWORD value) override { return adapter_->SetLength(value); }
    STDMETHODIMP GetCurrentPosition(QWORD* value) override { return adapter_->GetCurrentPosition(value); }
    STDMETHODIMP SetCurrentPosition(QWORD value) override { return adapter_->SetCurrentPosition(value); }
    STDMETHODIMP IsEndOfStream(BOOL* value) override { return adapter_->IsEndOfStream(value); }
    STDMETHODIMP Read(BYTE* data, ULONG count, ULONG* read) override {
        return adapter_->Read(data, count, read);
    }
    STDMETHODIMP BeginRead(BYTE* data, ULONG count, IMFAsyncCallback* callback,
                           IUnknown* state) override {
        return adapter_->BeginRead(data, count, callback, state);
    }
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override {
        return adapter_->EndRead(result, read);
    }
    STDMETHODIMP Write(const BYTE* data, ULONG count, ULONG* written) override {
        return adapter_->Write(data, count, written);
    }
    STDMETHODIMP BeginWrite(const BYTE* data, ULONG count, IMFAsyncCallback* callback,
                            IUnknown* state) override {
        return adapter_->BeginWrite(data, count, callback, state);
    }
    STDMETHODIMP EndWrite(IMFAsyncResult* result, ULONG* written) override {
        return adapter_->EndWrite(result, written);
    }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset,
                      DWORD flags, QWORD* position) override {
        return adapter_->Seek(origin, offset, flags, position);
    }
    STDMETHODIMP Flush() override { return adapter_->Flush(); }
    STDMETHODIMP Close() override { return adapter_->Close(); }
private:
    ComPtr<IStream> cursor_;
    ComPtr<IMFByteStream> adapter_;
};

HRESULT OpenRead(const std::shared_ptr<const VideoMemoryBacking>& backing,
                 IMFByteStream** stream) {
    if (!stream) return E_POINTER;
    *stream = nullptr;
    auto cursor = Make<MemoryStream>(backing);
    if (!cursor) return E_OUTOFMEMORY;
    return MFCreateMFByteStreamOnStream(cursor.Get(), stream);
}
} // namespace

VideoMemoryStream::VideoMemoryStream(std::size_t limit)
    : backing_(std::make_shared<VideoMemoryBacking>(limit)) {}
VideoMemoryStream::~VideoMemoryStream() = default;

HRESULT VideoMemoryStream::OpenWriteCursor(IStream** stream) {
    if (!stream) return E_POINTER;
    *stream = nullptr;
    ComPtr<MemoryStream> cursor;
    {
        std::lock_guard lock(backing_->mutex);
        if (backing_->sealed || (backing_->recorderAttached && !backing_->recordingActive))
            return STG_E_ACCESSDENIED;
        cursor = Make<MemoryStream>(backing_, backing_);
    }
    return cursor ? cursor.CopyTo(stream) : E_OUTOFMEMORY;
}

HRESULT VideoMemoryStream::OpenWriteStream(IMFByteStream** stream) {
    if (!stream) return E_POINTER;
    *stream = nullptr;
    ComPtr<IStream> cursor;
    const auto hr = OpenWriteCursor(&cursor);
    if (FAILED(hr)) return hr;
    ComPtr<IMFByteStream> adapter;
    const auto adapterHr = MFCreateMFByteStreamOnStream(cursor.Get(), &adapter);
    if (FAILED(adapterHr)) return adapterHr;
    auto tracked = Make<WriteAdapter>(std::move(cursor), std::move(adapter));
    return tracked ? tracked.CopyTo(stream) : E_OUTOFMEMORY;
}

std::shared_ptr<const VideoMemorySource> VideoMemoryStream::Seal() {
    std::lock_guard lock(backing_->mutex);
    if (sealed_) return sealed_;
    if (backing_->writableCursors ||
        (backing_->recorderAttached && !backing_->finalized)) return {};
    try {
        sealed_ = std::shared_ptr<const VideoMemorySource>(new VideoMemorySource(backing_));
    } catch (const std::bad_alloc&) { return {}; }
    backing_->sealed = true;
    return sealed_;
}

std::size_t VideoMemoryStream::ByteSize() const {
    std::lock_guard lock(backing_->mutex);
    return backing_->bytes.size();
}
std::size_t VideoMemoryStream::Capacity() const {
    std::lock_guard lock(backing_->mutex);
    return backing_->bytes.capacity();
}
HRESULT VideoMemoryStream::BeginRecording() {
    std::lock_guard lock(backing_->mutex);
    if (backing_->sealed || backing_->recorderAttached ||
        backing_->writableCursors || !backing_->bytes.empty()) return MF_E_INVALIDREQUEST;
    backing_->recorderAttached = backing_->recordingActive = true;
    backing_->writeError = S_OK;
    return S_OK;
}
HRESULT VideoMemoryStream::ResetForRetry() {
    std::lock_guard lock(backing_->mutex);
    if (backing_->sealed || !backing_->recordingActive || backing_->writableCursors)
        return MF_E_INVALIDREQUEST;
    backing_->bytes.clear(); // retains and charges the same allocation
    backing_->writeError = S_OK;
    return S_OK;
}
void VideoMemoryStream::FinishRecording(bool completed) {
    std::lock_guard lock(backing_->mutex);
    backing_->recordingActive = false;
    backing_->finalized = completed && SUCCEEDED(backing_->writeError);
}
HRESULT VideoMemoryStream::WriteError() const {
    std::lock_guard lock(backing_->mutex);
    return backing_->writeError;
}

VideoMemorySource::VideoMemorySource(std::shared_ptr<const VideoMemoryBacking> backing)
    : backing_(std::move(backing)) {}
std::size_t VideoMemorySource::ByteSize() const { return backing_->bytes.size(); }
std::size_t VideoMemorySource::Capacity() const { return backing_->bytes.capacity(); }

HRESULT VideoMemorySource::OpenReadStream(IMFByteStream** stream,
                                         VideoReadStreamClone* clone) const {
    if (!stream) return E_POINTER;
    *stream = nullptr;
    if (clone) *clone = {};
    try {
        // Build the callback before publishing either output, so allocation
        // failure cannot return success with a missing independent cursor.
        VideoReadStreamClone factory;
        if (clone) factory = [backing = backing_](IMFByteStream** result) {
            return OpenRead(backing, result);
        };
        const auto hr = OpenRead(backing_, stream);
        if (SUCCEEDED(hr) && clone) *clone = std::move(factory);
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

} // namespace okuflow
#endif // _WIN32
