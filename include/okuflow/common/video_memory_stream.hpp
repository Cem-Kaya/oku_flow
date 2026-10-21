#pragma once

#ifdef _WIN32

#include "okuflow/common/video_archive.hpp"
#include <objidl.h>
#include <cstddef>
#include <memory>

namespace okuflow {

struct VideoMemoryBacking;
class VideoMemorySource;

// A single bounded heap allocation, with no file, mapping, spill or cache.
// Capacity reports allocated compressed-byte storage, not just logical size.
class VideoMemoryStream {
public:
    explicit VideoMemoryStream(std::size_t limit = 8u * 1024u * 1024u);
    ~VideoMemoryStream();
    VideoMemoryStream(const VideoMemoryStream&) = delete;
    VideoMemoryStream& operator=(const VideoMemoryStream&) = delete;

    HRESULT OpenWriteStream(IMFByteStream** stream);
    // Returns null while any writable IStream/MF adapter survives, or a
    // recorder has not completed Finalize successfully. Close alone does not
    // release an adapter: release every writable COM reference before sealing.
    // Repeated successful calls return the same source object/pointer identity.
    std::shared_ptr<const VideoMemorySource> Seal();
    std::size_t ByteSize() const;
    std::size_t Capacity() const;

private:
    friend class VideoRecorder;
    friend struct VideoMemoryStreamTestAccess;
    HRESULT OpenWriteCursor(IStream** stream);
    HRESULT BeginRecording();
    HRESULT ResetForRetry();
    void FinishRecording(bool completed);
    HRESULT WriteError() const;
    std::shared_ptr<VideoMemoryBacking> backing_;
    std::shared_ptr<const VideoMemorySource> sealed_;
};

// Immutable shared owned bytes. Use the shared source pointer as opaque
// identity, never a fabricated VideoFileIdentity or a filesystem label.
// Streams and clone callbacks retain the backing even after ring eviction.
class VideoMemorySource {
public:
    ~VideoMemorySource() = default;
    VideoMemorySource(const VideoMemorySource&) = delete;
    VideoMemorySource& operator=(const VideoMemorySource&) = delete;
    HRESULT OpenReadStream(IMFByteStream** stream,
                          VideoReadStreamClone* clone = nullptr) const;
    std::size_t ByteSize() const;
    std::size_t Capacity() const;

private:
    friend class VideoMemoryStream;
    friend struct VideoMemoryStreamTestAccess;
    explicit VideoMemorySource(std::shared_ptr<const VideoMemoryBacking> backing);
    std::shared_ptr<const VideoMemoryBacking> backing_;
};

} // namespace okuflow

#endif // _WIN32
