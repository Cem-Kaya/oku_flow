#include "okuflow/app/cuda_surface_retry.hpp"
#include "okuflow/app/capture_handoff_policy.hpp"
#include "okuflow/cuda/timing_event_pair.hpp"

#include <chrono>
#include <iostream>
#include <set>

int main() {
    using namespace std::chrono;
    using okuflow::CudaSurfaceRetry;
    int device = 0;
    int replacementDevice = 0;
    int fence = 0;
    int replacementFence = 0;
    const okuflow::CudaSurfaceConfiguration configuration{
        &device, &fence, 1, 1280, 720, 1280, 720};
    CudaSurfaceRetry retry;
    auto now = CudaSurfaceRetry::Clock::time_point{};
    bool passed = true;
    auto check = [&](bool condition, const char* scenario) {
        if (!condition) {
            std::cerr << scenario << '\n';
            passed = false;
        }
    };

    // A second timing-event allocation can fail under memory pressure. The
    // caller must never observe half of the new pair, leak the first event,
    // or record a later frame against a null stop handle.
    int startEvent = 0;
    int stopEvent = 0;
    int allocation = 0;
    int failAt = 2;
    int duplicateDestroy = 0;
    std::set<int> liveEvents;
    const auto createEvent = [&](int* event) {
        ++allocation;
        if (allocation == failAt) return false;
        *event = allocation;
        liveEvents.insert(*event);
        return true;
    };
    const auto destroyEvent = [&](int event) {
        if (liveEvents.erase(event) != 1) ++duplicateDestroy;
    };
    check(!okuflow::EnsureTimingEventPair(startEvent, stopEvent,
                createEvent, destroyEvent), "second event failure was accepted");
    check(startEvent == 0 && stopEvent == 0 && liveEvents.empty(),
          "failed pair escaped or leaked its first event");
    failAt = 0;
    check(okuflow::EnsureTimingEventPair(startEvent, stopEvent,
                createEvent, destroyEvent), "event pair could not recover after failure");
    check(startEvent != 0 && stopEvent != 0 && startEvent != stopEvent &&
          liveEvents.size() == 2, "recovered timing pair is incomplete");
    const int completeAllocationCount = allocation;
    check(okuflow::EnsureTimingEventPair(startEvent, stopEvent,
                createEvent, destroyEvent) && allocation == completeAllocationCount,
          "valid timing pair was needlessly reallocated");
    // Also repair legacy partial state transactionally: failure retains old
    // ownership, success replaces it and retires each old/new handle once.
    destroyEvent(stopEvent);
    stopEvent = 0;
    const int retainedStart = startEvent;
    failAt = allocation + 2;
    check(!okuflow::EnsureTimingEventPair(startEvent, stopEvent,
                createEvent, destroyEvent) && startEvent == retainedStart &&
          stopEvent == 0 && liveEvents.size() == 1,
          "failed repair lost the existing timing event");
    failAt = 0;
    check(okuflow::EnsureTimingEventPair(startEvent, stopEvent,
                createEvent, destroyEvent) && startEvent != retainedStart &&
          liveEvents.size() == 2 && !liveEvents.contains(retainedStart),
          "partial pair was not replaced atomically");
    destroyEvent(startEvent);
    destroyEvent(stopEvent);
    check(liveEvents.empty() && duplicateDestroy == 0,
          "timing pair leaked or destroyed an event twice");

    // Model both raw and CPU-converted requests for every 60-Hz frame. A
    // persistent failure must perform no new work until the retry deadline.
    for (const int delay : {1, 2, 4, 8, 16, 30, 30, 30}) {
        check(retry.ShouldAttempt(configuration, now), "retry should be due");
        retry.RecordFailure(now);
        const auto deadline = now + seconds(delay);
        for (auto frame = now; frame < deadline; frame += milliseconds(16)) {
            check(!retry.ShouldAttempt(configuration, frame),
                  "raw frame retried before deadline");
            check(!retry.ShouldAttempt(configuration, frame),
                  "converted fallback bypassed negative cache");
        }
        check(!retry.ShouldAttempt(configuration, deadline - milliseconds(1)),
              "retry started before deadline");
        now = deadline;
    }
    retry.RecordSuccess();
    check(retry.ShouldAttempt(configuration, now), "success must clear backoff");
    retry.RecordFailure(now);
    check(retry.ShouldAttempt(configuration, now + seconds(1)),
          "failure after success must restart at one second");

    // Every relevant config boundary makes an immediate attempt possible,
    // including returning to a previously failed configuration.
    for (int field = 0; field < 7; ++field) {
        retry.ShouldAttempt(configuration, now);
        retry.RecordFailure(now);
        auto changed = configuration;
        switch (field) {
        case 0: changed.device = &replacementDevice; break;
        case 1: changed.fence = &replacementFence; break;
        case 2: ++changed.cameraSession; break;
        case 3: changed.width = 1920; break;
        case 4: changed.height = 1080; break;
        case 5: changed.superResWidth = 2560; break;
        case 6: changed.superResHeight = 1440; break;
        }
        check(retry.ShouldAttempt(changed, now),
              "changed configuration should retry immediately");
        retry.RecordFailure(now);
        check(!retry.ShouldAttempt(changed, now),
              "changed configuration must cache its own failure");
        check(retry.ShouldAttempt(changed, now + seconds(1)),
              "changed configuration must restart one-second delay");
        check(retry.ShouldAttempt(configuration, now),
              "restoring configuration should retry immediately");
    }
    // Two slow conversions followed by a healthy one must not demote the
    // camera. CUDA ownership waits cannot contribute to a conversion streak.
    okuflow::CaptureHandoffPolicy handoff;
    using okuflow::CaptureGpuPending;
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "first conversion deadline demoted");
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "second conversion deadline demoted");
    for (int tick = 0; tick < 100; ++tick)
        check(!handoff.RecordDeadline(CaptureGpuPending::CudaCopy), "CUDA ownership wait selected safe-copy rung");
    handoff.RecordReady();
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "success did not clear deadline streak");
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "recovered camera demoted too early");
    check(handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "persistent slow conversion did not demote");
    check(handoff.Demoted(), "session demotion not latched");
    handoff.RecordReady(); // A late completion cannot re-enable a demoted rung.
    check(handoff.Demoted(), "late completion undid session demotion");
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "demotion notification repeated");
    handoff.Reset();
    check(!handoff.Demoted(), "new camera session retained old demotion");
    check(!handoff.RecordDeadline(CaptureGpuPending::D3D11Query), "new session did not get fresh retry budget");
    return passed ? 0 : 1;
}
