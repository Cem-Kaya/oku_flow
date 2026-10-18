#pragma once

#ifdef _WIN32
#include <mfidl.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace okuflow {

// A held Windows file identity. Paths are labels, never identity evidence.
struct VideoFileIdentity {
    std::uint64_t volumeSerial{};
    std::array<std::uint8_t, 16> fileId{};
    std::uint64_t sizeBytes{}, lastWriteTime{};
    bool valid{};
    bool SameFile(const VideoFileIdentity& other) const;
};

enum class ActiveRewindAvailability {
    WaitingForMedia, Available, SharingDenied, UnsupportedContainer,
    Invalidated, ResourceLimit, Finalized, Failed
};

struct RecordingArchiveSnapshot {
    std::wstring path;
    std::string attemptId;
    std::uint64_t codecConfigurationId{};
    std::uint32_t videoTrackId{};
    VideoFileIdentity file;
    std::uint64_t generation{1}, mutationSequence{}, committedBytes{};
    std::uint64_t verifiedMediaBytes{}, videoSamples{};
    std::uint64_t parserBoxAttempts{}; // bounded-work diagnostic, monotonic
    std::int64_t latestWriterAccepted100ns{-1};
    std::int64_t seekableEnd100ns{-1}; // actual last verified video PTS
    std::int64_t duration100ns{};
    ActiveRewindAvailability availability{ActiveRewindAvailability::WaitingForMedia};
    bool finalized{}, complete{}, hasAudio{};
    std::string error;
};

// Producer mutation fencing is synchronous and bounded. Parsing runs on its
// own worker; no decoder/parser call is made while the producer lock is held.
class RecordingArchive {
public:
    struct State;
    explicit RecordingArchive(std::wstring path);
    ~RecordingArchive();
    RecordingArchive(const RecordingArchive&) = delete;
    RecordingArchive& operator=(const RecordingArchive&) = delete;
    RecordingArchiveSnapshot Snapshot() const;
    void WriterAccepted(std::int64_t pts100ns);
    void Finalized(bool complete);
    void Abandoned();
    // Reader copies must pass both checks. A generation change revokes all
    // previously issued active metadata; private decoded pixels stay owned.
    bool ValidateRange(std::uint64_t generation, std::uint64_t begin,
                       std::uint64_t bytes) const;
    HRESULT OpenReadStream(IMFByteStream** stream,
                           RecordingArchiveSnapshot* admitted = nullptr) const;
    static HRESULT Observe(IMFByteStream* underlying,
                           const std::shared_ptr<RecordingArchive>& archive,
                           IMFByteStream** observed);
private:
    std::shared_ptr<State> state_;
};

// Used by the file player too: the exact handle behind the returned byte
// stream supplies identity and denies writes/deletes for saved media.
HRESULT OpenStableVideoReadStream(const std::wstring& path,
                                 IMFByteStream** stream,
                                 VideoFileIdentity* identity);
// CREATE_NEW preserves exclusive output ownership while FILE_SHARE_READ
// permits the producer-fenced active parser. Never truncates an existing file.
HRESULT CreateRecordingArchiveOutputStream(const std::wstring& path,
                                          IMFByteStream** stream);

} // namespace okuflow
#endif
