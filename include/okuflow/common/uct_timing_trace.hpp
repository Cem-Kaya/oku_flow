#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <type_traits>

namespace okuflow::uct::timing {

// Software-event metadata only. No strings, pointers, image ownership or
// recognition content can enter an event. Source time is retained verbatim;
// it is NOT subtracted from steadyNs (saved-media PTS has another clock).
enum class Channel : std::uint8_t { Recognition, DeviceValidation, Probe, Controller };
enum class Event : std::uint8_t {
    Submit, Dequeue, Done, UiReceipt, Admit, BackendState, Cancel, Reset, Fault,
    OverlayAssigned, OverlayPainted, PaneAssigned, AccessibilityScheduled, AccessibilityPublished,
    PresentationReceipt, WindowClosed
};
enum Flag : std::uint32_t {
    Cancelled = 1u << 0, InputFault = 1u << 1, Preparing = 1u << 2,
    Composite = 1u << 3, Failed = 1u << 4, Stale = 1u << 5,
    DeviceInput = 1u << 6, Current = 1u << 7, LiveCamera = 1u << 8,
    AcquisitionKnown = 1u << 9, Coalesced = 1u << 10, Shutdown = 1u << 11,
    Corroboration = 1u << 12, RetainedArtifact = 1u << 13,
    GpuBackend = 1u << 14, BackendKnown = 1u << 15, InferenceAttempted = 1u << 16
};
struct Key {
    std::uint64_t producer{}, sourceEpoch{}, geometryGeneration{}, frameSeq{};
    std::uint64_t acquisition{}, jobId{}, authority{};
    std::int64_t sourceTimestampMs{};
    Channel channel{Channel::Controller};
};
struct Values {
    std::uint32_t code{}, count{}, flags{};
    std::uint64_t relatedJobId{}, publicationRevision{};
};
struct PublicationTicket {
    Key admission;
    std::uint64_t revision{};
    std::uint32_t flags{};
};
struct Record {
    std::int64_t steadyNs{};
    Key key;
    Values values;
    Event event{Event::Reset};
};
inline constexpr std::size_t kCapacity = 4096;
static_assert(std::is_trivially_copyable_v<Record>);
static_assert(sizeof(Record) * kCapacity <= 512 * 1024);

// Allocate snapshots on the heap: the bounded record array is deliberately
// large enough that two stack copies could exceed a Windows fixture's stack.
struct Snapshot {
    std::array<Record, kCapacity> records{};
    std::size_t count{};
    std::uint64_t acceptedAtSnapshot{}, capacityDrops{}, contentionDrops{}, outsideWindowDrops{};
    std::int64_t closedAtNs{};
    bool enabled{}, unavailableDueToContention{}, closeOverlappedAttempts{};
};
struct ChainSummary {
    std::uint64_t jobsObserved{}, fullyObserved{}, incomplete{}, cancelled{}, faulted{}, ticketMismatches{};
    bool coverageIncomplete{}; // drops/contended snapshot can hide entire jobs
};

struct RecorderTestAccess;
class Recorder {
public:
    explicit Recorder(bool enabled) noexcept : enabled_(enabled) {}
    bool TryRecord(Event event, const Key& key, Values values = {}, std::int64_t atNs = 0) noexcept;
    // Stops acceptance immediately, never waits for a trace lock/worker, and
    // reports snapshot contention explicitly. Repeated snapshots are safe.
    void CloseInto(Snapshot& snapshot) noexcept;
private:
    friend struct RecorderTestAccess;
    struct ActiveAttempt {
        explicit ActiveAttempt(std::atomic<std::uint64_t>& value) noexcept : value_(value) { ++value_; }
        ActiveAttempt(const ActiveAttempt&) = delete;
        ActiveAttempt& operator=(const ActiveAttempt&) = delete;
        ~ActiveAttempt() { --value_; }
        std::atomic<std::uint64_t>& value_;
    };
    const bool enabled_;
    std::atomic<bool> closed_{};
    std::atomic<bool> closeOverlappedAttempts_{};
    std::atomic<std::uint64_t> activeAttempts_{};
    std::atomic<std::int64_t> closedAtNs_{};
    std::mutex mutex_;
    std::array<Record, kCapacity> records_{};
    std::size_t count_{};
    std::atomic<std::uint64_t> accepted_{}, capacityDrops_{}, contentionDrops_{}, outsideWindowDrops_{};
};

std::int64_t SteadyNanoseconds() noexcept;
ChainSummary Summarize(const Snapshot& snapshot);
void WriteJsonLines(const Snapshot& snapshot, std::ostream& output);

// Cached once per process: set OKUFLOW_UCT_TIMING_TRACE=1 before launch.
// Disabled callers need only this cached branch; make Keys inside that branch.
bool Enabled() noexcept;
std::uint64_t NewProducerId() noexcept;
void RecordEvent(Event event, const Key& key, Values values = {}) noexcept;
// Diagnostic/test seam. Closes the recording window, without file I/O.
std::unique_ptr<Snapshot> CloseForDiagnostics() noexcept;
// Once only, normally after controller-owned workers/callbacks are retired.
// A normal-exit fallback also handles standalone workers. No joins/waits are
// added: unfinished/detached work is reported as an incomplete closed window.
// Windows output: %LOCALAPPDATA%/OkuFlow/uct-timing/ unique CREATE_NEW JSONL.
bool FlushAtNormalTeardown() noexcept;

} // namespace okuflow::uct::timing
