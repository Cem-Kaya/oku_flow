#include "okuflow/common/uct_timing_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace okuflow::uct::timing {
namespace {
const char* EventName(Event event) noexcept
{
    switch (event) {
    case Event::Submit: return "submit"; case Event::Dequeue: return "dequeue";
    case Event::Done: return "done"; case Event::UiReceipt: return "ui_receipt";
    case Event::Admit: return "admit"; case Event::BackendState: return "backend_state";
    case Event::Cancel: return "cancel"; case Event::Reset: return "reset";
    case Event::Fault: return "fault"; case Event::OverlayAssigned: return "overlay_assigned";
    case Event::OverlayPainted: return "overlay_painted";
    case Event::PaneAssigned: return "pane_assigned";
    case Event::AccessibilityScheduled: return "accessibility_scheduled";
    case Event::AccessibilityPublished: return "accessibility_published";
    case Event::PresentationReceipt: return "presentation_receipt";
    case Event::WindowClosed: return "window_closed";
    }
    return "unknown";
}
Recorder* ProcessRecorder() noexcept
{
    // Deliberately process-lifetime: a detached native job may finish after
    // normal teardown. It can safely observe closed=true; trace storage never
    // changes the existing worker/input ownership or teardown deadlines.
    static Recorder* recorder = []() noexcept -> Recorder* {
        if (!Enabled()) return nullptr;
        try {
            auto* value = new Recorder(true);
            std::atexit([] { FlushAtNormalTeardown(); });
            return value;
        } catch (...) { return nullptr; } // diagnostics cannot fail recognition
    }();
    return recorder;
}
}

std::int64_t SteadyNanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool Recorder::TryRecord(Event event, const Key& key, Values values, std::int64_t atNs) noexcept
{
    if (!enabled_) return false;
    ActiveAttempt attempt(activeAttempts_);
    if (closed_.load()) { ++outsideWindowDrops_; return false; }
    try {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { ++contentionDrops_; return false; }
        if (closed_.load()) { ++outsideWindowDrops_; return false; }
        if (count_ == kCapacity) { ++capacityDrops_; return false; }
        records_[count_++] = {atNs ? atNs : SteadyNanoseconds(), key, values, event};
        accepted_.store(count_);
        return true;
    } catch (...) { ++contentionDrops_; return false; }
}
void Recorder::CloseInto(Snapshot& snapshot) noexcept
{
    snapshot.enabled = enabled_;
    snapshot.count = 0;
    snapshot.acceptedAtSnapshot = snapshot.capacityDrops = snapshot.contentionDrops = snapshot.outsideWindowDrops = 0;
    snapshot.closedAtNs = 0;
    snapshot.unavailableDueToContention = false;
    snapshot.closeOverlappedAttempts = false;
    if (!enabled_) return;
    closed_.store(true);
    // A producer can have failed try_lock but not yet published its drop.
    // Never wait for it: conservatively latch incomplete coverage instead.
    if (activeAttempts_.load() != 0) closeOverlappedAttempts_.store(true);
    std::int64_t unset = 0;
    closedAtNs_.compare_exchange_strong(unset, SteadyNanoseconds());
    try {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            snapshot.count = count_;
            std::copy_n(records_.begin(), count_, snapshot.records.begin());
            // An already accepted writer may have passed its last closed
            // check before closure. Bound the window by every copied receipt.
            auto bound = closedAtNs_.load();
            for (std::size_t i = 0; i < count_; ++i) bound = std::max(bound, records_[i].steadyNs);
            auto previous = closedAtNs_.load();
            while (previous < bound && !closedAtNs_.compare_exchange_weak(previous, bound)) {}
        } else snapshot.unavailableDueToContention = true;
    } catch (...) { snapshot.unavailableDueToContention = true; }
    snapshot.acceptedAtSnapshot = accepted_.load();
    snapshot.capacityDrops = capacityDrops_.load();
    snapshot.contentionDrops = contentionDrops_.load();
    snapshot.outsideWindowDrops = outsideWindowDrops_.load();
    snapshot.closeOverlappedAttempts = closeOverlappedAttempts_.load();
    snapshot.closedAtNs = closedAtNs_.load();
}
ChainSummary Summarize(const Snapshot& snapshot)
{
    struct Chain {
        Key first;
        std::uint64_t knownAcquisition{};
        unsigned phases{};
        bool initialized{}, acquisitionKnown{}, recognition{}, corroboration{}, cancelled{}, faulted{}, mismatch{};
    };
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, Chain> chains;
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const auto& record = snapshot.records[i];
        if (!record.key.jobId) continue; // backend/session events name no job
        auto& chain = chains[{record.key.producer, record.key.jobId, record.key.authority}];
        if (!chain.initialized) { chain.first = record.key; chain.initialized = true; }
        const auto& first = chain.first;
        chain.mismatch |= first.sourceEpoch != record.key.sourceEpoch ||
            first.geometryGeneration != record.key.geometryGeneration || first.frameSeq != record.key.frameSeq ||
            first.sourceTimestampMs != record.key.sourceTimestampMs || first.channel != record.key.channel;
        if (record.values.flags & AcquisitionKnown) {
            if (chain.acquisitionKnown) chain.mismatch |= chain.knownAcquisition != record.key.acquisition;
            chain.acquisitionKnown = true;
            chain.knownAcquisition = record.key.acquisition;
        }
        chain.recognition = record.key.channel == Channel::Recognition;
        chain.corroboration |= bool(record.values.flags & Corroboration);
        switch (record.event) {
        case Event::Submit: chain.phases |= 1; break;
        case Event::Dequeue: chain.phases |= 2; break;
        case Event::Done: chain.phases |= 4; break;
        case Event::UiReceipt: chain.phases |= 8; break;
        case Event::Admit: chain.phases |= 16; break;
        default: break;
        }
        chain.cancelled |= record.event == Event::Cancel || (record.values.flags & Cancelled);
        chain.faulted |= record.event == Event::Fault || (record.values.flags & (Failed | InputFault));
    }
    ChainSummary result;
    result.jobsObserved = chains.size();
    for (const auto& [key, chain] : chains) {
        const unsigned required = chain.recognition && !chain.corroboration ? 31u : 15u;
        if ((chain.phases & required) == required && !chain.mismatch) ++result.fullyObserved;
        else ++result.incomplete;
        if (chain.mismatch) ++result.ticketMismatches;
        if (chain.cancelled) ++result.cancelled;
        if (chain.faulted) ++result.faulted;
    }
    result.coverageIncomplete = snapshot.unavailableDueToContention || snapshot.closeOverlappedAttempts ||
        snapshot.capacityDrops || snapshot.contentionDrops || snapshot.acceptedAtSnapshot != snapshot.count || result.ticketMismatches;
    return result;
}
void WriteJsonLines(const Snapshot& snapshot, std::ostream& output)
{
    output.imbue(std::locale::classic());
    const auto summary = Summarize(snapshot);
    output << "{\"schema\":1,\"clock\":\"steady_nanoseconds\",\"scope\":\"software_events_only\","
              "\"photon_timing\":false,\"enabled\":" << (snapshot.enabled ? "true" : "false")
           << ",\"capacity\":" << kCapacity
           << ",\"records\":" << snapshot.count << ",\"accepted_at_snapshot\":" << snapshot.acceptedAtSnapshot
           << ",\"capacity_drops\":" << snapshot.capacityDrops << ",\"contention_drops\":" << snapshot.contentionDrops
           << ",\"outside_window_drops_at_snapshot\":" << snapshot.outsideWindowDrops
           << ",\"late_events_outside_window\":true,\"closed_at_ns\":" << snapshot.closedAtNs
           << ",\"snapshot_unavailable\":" << (snapshot.unavailableDueToContention ? "true" : "false")
           << ",\"close_overlapped_attempts\":" << (snapshot.closeOverlappedAttempts ? "true" : "false") << "}\n";
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const auto& r = snapshot.records[i];
        output << "{\"event\":\"" << EventName(r.event) << "\",\"steady_ns\":" << r.steadyNs
               << ",\"producer\":" << r.key.producer << ",\"channel\":" << unsigned(r.key.channel)
               << ",\"source_epoch\":" << r.key.sourceEpoch << ",\"geometry_generation\":" << r.key.geometryGeneration
               << ",\"frame\":" << r.key.frameSeq << ",\"acquisition\":" << r.key.acquisition
               << ",\"job\":" << r.key.jobId << ",\"authority\":" << r.key.authority
               << ",\"source_timestamp_ms\":" << r.key.sourceTimestampMs
               << ",\"code\":" << r.values.code << ",\"count\":" << r.values.count
               << ",\"flags\":" << r.values.flags << ",\"related_job\":" << r.values.relatedJobId
               << ",\"publication_revision\":" << r.values.publicationRevision << "}\n";
    }
    output << "{\"summary\":true,\"jobs_observed\":" << summary.jobsObserved
           << ",\"fully_observed_chains\":" << summary.fullyObserved << ",\"incomplete_chains_known\":" << summary.incomplete
           << ",\"cancelled_chains\":" << summary.cancelled << ",\"faulted_chains\":" << summary.faulted
           << ",\"ticket_mismatches\":" << summary.ticketMismatches
           << ",\"coverage_incomplete\":" << (summary.coverageIncomplete ? "true" : "false")
           << ",\"eligible_for_complete_chain_latency\":" << (summary.coverageIncomplete || summary.incomplete ? "false" : "true")
           << ",\"summary_covers_worker_controller_stages_only\":true"
              ",\"fully_observed_is_not_admission_success\":true,\"workers_may_outlive_window\":true}\n";
}
bool Enabled() noexcept
{
    static const bool enabled = [] {
        const char* value = std::getenv("OKUFLOW_UCT_TIMING_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
std::uint64_t NewProducerId() noexcept
{
    if (!Enabled()) return 0;
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1);
}
void RecordEvent(Event event, const Key& key, Values values) noexcept
{
    if (!Enabled()) return;
    if (auto* recorder = ProcessRecorder()) recorder->TryRecord(event, key, values);
}
std::unique_ptr<Snapshot> CloseForDiagnostics() noexcept
{
    try {
        auto snapshot = std::make_unique<Snapshot>();
        if (auto* recorder = ProcessRecorder()) recorder->CloseInto(*snapshot);
        return snapshot;
    } catch (...) { return {}; }
}
bool FlushAtNormalTeardown() noexcept
{
    if (!Enabled()) return false;
    static std::atomic<bool> attempted{};
    if (attempted.exchange(true)) return false;
    try {
        const auto snapshot = CloseForDiagnostics();
        if (!snapshot) return false;
#if defined(_WIN32)
        const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (!needed || needed > 32768) return false;
        std::wstring local(needed, L'\0');
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), needed);
        if (!length || length >= needed) return false;
        local.resize(length);
        const auto directory = std::filesystem::path(local) / L"OkuFlow" / L"uct-timing";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        std::ostringstream output;
        WriteJsonLines(*snapshot, output);
        const auto bytes = output.str(); // bounded, flush-only allocation; never a pipeline queue
        if (bytes.size() > 4 * 1024 * 1024) return false;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto name = L"uct-timing-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(snapshot->closedAtNs) + L"-" + std::to_wstring(attempt) + L".jsonl";
            const auto path = directory / name;
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                return false;
            }
            DWORD written{};
            const bool ok = bytes.size() <= MAXDWORD && WriteFile(file, bytes.data(),
                static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
            CloseHandle(file);
            return ok;
        }
#endif
    } catch (...) {} // diagnostic allocation/filesystem failure never escapes teardown
    return false;
}

} // namespace okuflow::uct::timing
