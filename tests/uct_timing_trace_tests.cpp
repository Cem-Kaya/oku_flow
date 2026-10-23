#include "okuflow/common/uct_timing_trace.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>
#include <vector>

namespace okuflow::uct::timing {
struct RecorderTestAccess {
    static std::unique_lock<std::mutex> Hold(Recorder& recorder) { return std::unique_lock(recorder.mutex_); }
    static auto UnresolvedAttempt(Recorder& recorder) {
        return std::make_unique<Recorder::ActiveAttempt>(recorder.activeAttempts_);
    }
    static void FinishContendedAttempt(Recorder& recorder) { ++recorder.contentionDrops_; }
};
}
namespace {
using namespace okuflow::uct::timing;
int failures{};
void Expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
Key Ticket(std::uint64_t job = 1) { return {9, 41, 7, 10, 10, job, 3, 1, Channel::Recognition}; }
std::unique_ptr<Snapshot> Close(Recorder& recorder) {
    auto snapshot = std::make_unique<Snapshot>(); recorder.CloseInto(*snapshot); return snapshot;
}
void Disabled() {
    auto recorder = std::make_unique<Recorder>(false);
    Expect(!recorder->TryRecord(Event::Submit, Ticket()), "a disabled recorder accepts no event");
    const auto snapshot = Close(*recorder);
    Expect(!snapshot->enabled && !snapshot->count && !snapshot->acceptedAtSnapshot &&
        !snapshot->capacityDrops && !snapshot->contentionDrops, "disabled recording has no storage/counter activity");
}
void Capacity() {
    auto recorder = std::make_unique<Recorder>(true);
    for (std::size_t i = 0; i < kCapacity; ++i)
        Expect(recorder->TryRecord(Event::Submit, Ticket(i + 1), {}, i + 1), "every slot in the fixed capacity is usable");
    Expect(!recorder->TryRecord(Event::Submit, Ticket(kCapacity + 1)), "overflow drops instead of growing storage");
    const auto snapshot = Close(*recorder);
    Expect(snapshot->count == kCapacity && snapshot->capacityDrops == 1 &&
        snapshot->records.front().key.jobId == 1 && snapshot->records.back().key.jobId == kCapacity,
        "overflow preserves existing receipts and exposes its exact dropped count");
    const auto summary = Summarize(*snapshot);
    Expect(summary.coverageIncomplete && summary.fullyObserved == 0 && summary.incomplete == kCapacity,
        "a full trace cannot claim complete chains after overflow");
}
void ContentionAndClose() {
    auto recorder = std::make_unique<Recorder>(true);
    recorder->TryRecord(Event::Submit, Ticket());
    auto snapshot = std::make_unique<Snapshot>();
    {
        auto held = RecorderTestAccess::Hold(*recorder);
        Expect(!recorder->TryRecord(Event::Dequeue, Ticket()), "trace contention drops immediately rather than waiting");
        recorder->CloseInto(*snapshot);
        Expect(snapshot->unavailableDueToContention && !snapshot->count && snapshot->acceptedAtSnapshot == 1 &&
            snapshot->contentionDrops == 1, "a contended close reports unavailable receipts and known drops explicitly");
    }
    const auto closedAt = snapshot->closedAtNs;
    Expect(!recorder->TryRecord(Event::Done, Ticket()), "a late worker cannot append after the recording window closes");
    recorder->CloseInto(*snapshot);
    Expect(snapshot->count == 1 && snapshot->closedAtNs == closedAt && snapshot->outsideWindowDrops == 1 &&
        !snapshot->unavailableDueToContention, "a later snapshot preserves the original close time and labels outside-window events");
}
void ConcurrentAccounting() {
    auto recorder = std::make_unique<Recorder>(true);
    constexpr unsigned workers = 8, calls = 1500;
    std::atomic<std::uint64_t> accepted{};
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < workers; ++thread) threads.emplace_back([&, thread] {
        for (unsigned i = 0; i < calls; ++i) {
            auto key = Ticket(i + 1); key.producer = thread + 1; key.frameSeq = key.acquisition = i;
            if (recorder->TryRecord(Event::Submit, key, {thread, i, AcquisitionKnown})) ++accepted;
        }
    });
    for (auto& thread : threads) thread.join();
    const auto snapshot = Close(*recorder);
    Expect(snapshot->count == accepted && snapshot->count <= kCapacity &&
        snapshot->count + snapshot->capacityDrops + snapshot->contentionDrops == workers * calls,
        "parallel producers account for every receipt or drop within the same fixed bound");
    for (std::size_t i = 0; i < snapshot->count; ++i) {
        const auto& r = snapshot->records[i];
        Expect(r.key.producer == r.values.code + 1 && r.key.frameSeq == r.values.count &&
            r.key.jobId == r.key.frameSeq + 1 && r.key.sourceEpoch == 41 && r.key.authority == 3,
            "concurrent metadata never mixes fields from separate source/job tickets");
    }
}
void CloseOverlappingUnpublishedDrop() {
    auto recorder = std::make_unique<Recorder>(true);
    // Inject the precise preemption point after failed try_lock and before
    // its drop counter increment, using the same attempt guard as TryRecord.
    auto unresolved = RecorderTestAccess::UnresolvedAttempt(*recorder);
    const auto during = Close(*recorder);
    Expect(!during->contentionDrops && during->closeOverlappedAttempts &&
        Summarize(*during).coverageIncomplete,
        "an unresolved pre-close attempt disqualifies complete coverage even before its drop becomes visible");
    RecorderTestAccess::FinishContendedAttempt(*recorder);
    unresolved.reset();
    const auto later = Close(*recorder);
    Expect(later->contentionDrops == 1 && later->closeOverlappedAttempts &&
        Summarize(*later).coverageIncomplete,
        "later snapshots retain the close-overlap latch and eventual drop");
    auto timestamped = std::make_unique<Recorder>(true);
    const auto delayedReceipt = SteadyNanoseconds() + 1'000'000'000;
    timestamped->TryRecord(Event::Done, Ticket(), {}, delayedReceipt);
    const auto bounded = Close(*timestamped);
    Expect(bounded->closedAtNs >= delayedReceipt,
        "the copied receipt window never ends before an accepted event timestamp");
}
void ChainsAndSchema() {
    auto recorder = std::make_unique<Recorder>(true);
    const auto first = Ticket(1), failed = Ticket(2), cancelled = Ticket(3);
    for (auto event : {Event::Submit, Event::Dequeue, Event::Done, Event::UiReceipt, Event::Admit})
        recorder->TryRecord(event, first, {0, 2, AcquisitionKnown});
    for (auto event : {Event::Submit, Event::Dequeue, Event::Done, Event::UiReceipt, Event::Fault})
        recorder->TryRecord(event, failed, {4, 0, Failed});
    recorder->TryRecord(Event::Submit, cancelled);
    recorder->TryRecord(Event::Cancel, cancelled, {0, 0, Cancelled});
    recorder->TryRecord(Event::Reset, {});
    recorder->TryRecord(Event::BackendState, {}, {2});
    const auto snapshot = Close(*recorder);
    const auto summary = Summarize(*snapshot);
    Expect(summary.jobsObserved == 3 && summary.fullyObserved == 1 && summary.incomplete == 2 &&
        summary.cancelled == 1 && summary.faulted == 1, "only a fully observed source/job chain is complete; faults/cancel/reset remain distinct");
    std::ostringstream text; WriteJsonLines(*snapshot, text); const auto json = text.str();
    Expect(json.find("\"scope\":\"software_events_only\"") != std::string::npos &&
        json.find("\"photon_timing\":false") != std::string::npos &&
        json.find("\"eligible_for_complete_chain_latency\":false") != std::string::npos,
        "serialized incomplete chains cannot advertise complete or photon timing");
    for (const auto* forbidden : {"\"text\":", "\"pixels\":", "\"path\":", "\"hash\":", "\"backend_name\":"})
        Expect(json.find(forbidden) == std::string::npos, "the output schema contains no recognition content or private asset fields");
    Expect(std::count(json.begin(), json.end(), '\n') == snapshot->count + 2,
        "one metadata header, one line per receipt and one explicit coverage summary are written");
}
void MismatchedTicketIsIncomplete() {
    auto recorder = std::make_unique<Recorder>(true);
    auto key = Ticket(); recorder->TryRecord(Event::Submit, key, {0, 0, AcquisitionKnown});
    ++key.frameSeq; ++key.acquisition; // mutable current-frame attribution would be wrong
    for (auto event : {Event::Dequeue, Event::Done, Event::UiReceipt, Event::Admit})
        recorder->TryRecord(event, key, {0, 0, AcquisitionKnown});
    const auto snapshot = Close(*recorder); const auto summary = Summarize(*snapshot);
    Expect(summary.fullyObserved == 0 && summary.incomplete == 1 && summary.ticketMismatches == 1 &&
        summary.coverageIncomplete, "matching job numbers with different frame/acquisition tickets never form a complete chain");
}
void ResolvedAcquisitionAndCorroboration() {
    auto recorder = std::make_unique<Recorder>(true);
    auto key = Ticket(); key.acquisition = 0;
    recorder->TryRecord(Event::Submit, key, {0, 0, Corroboration, 7});
    recorder->TryRecord(Event::Dequeue, key, {0, 0, Corroboration, 7});
    key.acquisition = 6; // capture-owned held identity resolves on the worker
    recorder->TryRecord(Event::Done, key, {0, 0, Corroboration | AcquisitionKnown | DeviceInput, 7});
    recorder->TryRecord(Event::UiReceipt, key, {0, 0, Corroboration | AcquisitionKnown, 7});
    const auto snapshot = Close(*recorder); const auto summary = Summarize(*snapshot);
    Expect(summary.fullyObserved == 1 && !summary.incomplete && !summary.ticketMismatches && !summary.coverageIncomplete,
        "a truthful resolved held identity and independent corroborator need no fabricated self-admission");
    Expect(!(snapshot->records[2].values.flags & GpuBackend),
        "resident input alone never reports GPU OCR execution");
    struct GroupedNumbers final : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    std::ostringstream output;
    output.imbue(std::locale(std::locale::classic(), new GroupedNumbers));
    WriteJsonLines(*snapshot, output);
    Expect(output.str().find("\"capacity\":4096") != std::string::npos,
        "a process numeric locale cannot introduce invalid grouped JSON numbers");
}
}
int main() {
    Disabled(); Capacity(); ContentionAndClose(); ConcurrentAccounting(); CloseOverlappingUnpublishedDrop();
    ChainsAndSchema(); MismatchedTicketIsIncomplete();
    ResolvedAcquisitionAndCorroboration();
    std::cout << (failures ? "FAIL" : "PASS") << " bounded UCT timing trace contracts\n";
    return failures ? 1 : 0;
}
