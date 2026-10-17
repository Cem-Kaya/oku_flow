# Bounded performance diagnostics (NP-010 / PF-001)

The audit's Maxine host-call tails were measured before the independent producer
implementation; post-change native pan latency remains unmeasured. An offline
chat baseline confirmed CPU cost in a specific long-conversation workload; its
optimization below uses the same compiled benchmark. Synthetic CPU timings do
not establish native input responsiveness or Maxine acceptance.

## Chat baseline and bounded update

The 2026-10-17 local baseline used 200 deltas after ten warmups, 40 messages ×
1024 initial characters, two views and eight synthetic previews. Synchronous
snapshot p95/p99 was 29.761/31.771 ms; snapshot plus offscreen CPU paint was
33.880/36.008 ms and queued callback delay was 33.981/36.062 ms. Exact selection
failures and maximum reading-anchor displacement were both zero. Preserve the
original `chat-performance-before.json` with the final comparison; these are
one machine/workload's observations, not a general application benchmark.

The common append-only update now reparses only the final assistant body when
its heading and body have already reached maximum bubble width. It requires
unchanged prefix messages, snapshot metadata, header/status/actions/images and
font/palette/locale/layout/viewport-width context. It preserves the root
paragraph format/identity and semantic selection/reading anchors. Empty or
short unsaturated bodies, first-word/status transitions, receipts, changed
headers, changed images, reflow and other mutations keep full reconstruction.
Each incremental edit invalidates cached full HTML so a later fallback renders
the current snapshot. Text replacement batches document layout until the final
body/format exists. A queued viewport resize rebuilds only if its final render
context still differs, avoiding stale callbacks that reconstruct current text.
No deltas are throttled or discarded.

An intermediate measurement reduced synchronous render cost while queued-probe
tails worsened. The opt-in event trace identified two deferred `QEvent::MetaCall`
callbacks to `AssistantChatView`, about 14 ms each per delta. That failed result
is retained separately; synchronous render time alone was insufficient evidence
of improved event-loop responsiveness.

The final untraced comparison used the same workload described above:

| CPU timing, p95 / p99 in ms | Before | Intermediate, deferred work unresolved | Final |
| --- | ---: | ---: | ---: |
| Synchronous snapshot | 29.761 / 31.771 | 6.297 / 6.526 | 6.175 / 6.292 |
| Snapshot plus offscreen paint | 33.880 / 36.008 | 10.180 / 10.449 | 9.757 / 9.911 |
| Queued callback delay | 33.981 / 36.062 | 39.066 / 39.639 | 10.170 / 10.610 |

All three runs had zero selection failures and zero maximum reading-anchor
displacement. Raw evidence is retained as `chat-performance-before.json`,
`chat-performance-fixed.json` (the unsuccessful intermediate),
`chat-event-trace.json` and the final comparison report in the local audit
evidence directory. The targeted headless geometry/fallback fixture passed.
These results establish lower CPU occupancy and queued-callback tails for this
synthetic offline workload. They do not establish native pan-to-present,
screen-reader behavior, live-provider burst handling or actual Maxine tails;
the full tracked build/test matrix is separate validation.

`streamingTailMatchesFullRenderAndFallback` compares the optimized result to a
fresh full render in narrow/wide LTR/RTL views, including escaped markup,
whitespace/newlines, mixed scripts, root paragraph geometry, exact reverse
selection/reading anchors, prefix mutation signals, fallback transitions,
resize and Qt accessible character geometry. Native screen-reader acceptance
remains separate.

## Offline chat and controlled host waits

Build the excluded development target explicitly from a VS2022 environment:

```powershell
cmake --build --preset msvc-cpu-build --target assistant_chat_benchmark deadline_worker_tests
$env:PATH = 'C:\Qt\6.12.0\msvc2022_64\bin;' + $env:PATH
$evidence = Join-Path $env:LOCALAPPDATA 'OkuFlow/performance-diagnostics'
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
& ./build/msvc-cpu/tests/assistant_chat_benchmark.exe --output "$evidence/chat-40-previews.json" --messages 40 --characters 1024 --samples 200 --previews
& ./build/msvc-cpu/tests/assistant_chat_benchmark.exe --output "$evidence/chat-120-previews.json" --messages 120 --characters 1024 --samples 200 --previews
```

The executable unconditionally selects Qt offscreen before QApplication, loads
the installed Segoe UI font and constructs production `AssistantChatView`
widgets only. It has no app runtime, provider, camera, microphone, adb or stored
settings. Two differently sized views are updated by default; `--views 1`
isolates one view. Synthetic previews obey the production eight-thumbnail bound.
The generated English fixture text is test data, not new interface copy.

After ten unmeasured deltas, JSON retains every measured delta and nearest-rank
p50/p95/p99/max microseconds for synchronous `SetChatSnapshot`, snapshot plus
offscreen CPU painting, and a queued callback posted immediately before that
work. One callback is posted per delta and the event queue is drained between
deltas. This measures event-loop occupancy under that explicit workload, not a
provider arrival rate, navigation input, native compositor or sensor-to-photon
latency. No unbounded delta backlog is simulated. Comparisons require the same
executable configuration, Qt scale/font, workload, machine and desktop load;
retain executable/source hashes and raw JSON with any comparison.

The fixture keeps a reverse selection away from the streaming message and
checks exact endpoints and reading-anchor pixel displacement after every delta.
Selection loss or displacement above one pixel makes the result invalid and
returns 1. Argument/font failure returns 2; missing queued callback returns 3;
evidence publication failure returns 4. There is no machine-dependent timing
threshold and the target belongs to neither the default build nor CTest.

`--trace-events` adds up to 32 events of at least 500 microseconds per delta,
including event type, receiver class/name, snapshot/paint/process-events phase
and inclusive duration. Nested event durations can overlap. `process_events_us`
also measures the complete event-drain call. Traced runs have diagnostic
overhead and are a separate workload; use untraced JSON for the final A/B.

The same report contains up to 40 completed calls per 0/5/20/50 ms controlled
sleep using production `DeadlineWorker`. These are synthetic caller-wait tails,
not NVIDIA SDK/GPU measurements. `deadline_worker` additionally verifies that
an overlapping call is rejected without executing or queueing it, and that a
timeout retains task ownership. This is the helper's one-call admission contract,
not an observed camera queue-depth measurement.

## Actual Maxine host-call distributions

For an independently authorized native/camera session, set
`OKUFLOW_MAXINE_PROFILE=1` before starting the existing tracked startup profiler:

```powershell
$env:OKUFLOW_MAXINE_PROFILE = '1'
try {
    ./scripts/profile_startup.ps1 -Executable 'C:\path\to\oku_flow.exe' -SettingsPath 'C:\path\to\isolated-reviewed-settings.json' -OutputDirectory "$evidence/maxine-on" -Runs 2 -DurationSeconds 60
} finally {
    Remove-Item Env:OKUFLOW_MAXINE_PROFILE
}
Get-ChildItem "$evidence/maxine-on" -Filter stderr.log -Recurse |
    Select-String 'OKUFLOW_MAXINE_PROFILE'
```

The settings fixture must select a supported camera/Maxine runtime and the
intended enabled profile, zoom, source resolution and strength. Prepare a
matching Maxine-off fixture and repeat into a different output directory,
alternating order. The profiler refuses an existing OkuFlow instance, isolates
settings, disables AI requests and exits normally. Run only when the desktop
and camera are explicitly available; this document does not authorize a live
run during other work. Preserve startup reports and stderr, including failures.

Each `MaxineSuperRes::Worker` retains only its last 16,384 host-call samples in
memory and writes one `OKUFLOW_MAXINE_PROFILE` JSON summary on destruction.
No per-frame file I/O occurs. The report states total and omitted-earlier calls
and separates `model load`, `frame processing` and `teardown`. For each stage it
reports completed/timeout/rejected/exception/SDK-success counts and nearest-rank
p50/p95/p99/max caller-wait and worker-dispatch microseconds. Completed SDK
host-call duration excludes CUDA device selection; incomplete/throwing SDK
calls are censored rather than included as successful operation durations.
Adapter-caller wait includes scheduling/device selection and timeout. Production
now makes those calls from the private producer, so it is not UI-thread wait.
These are host
timings, not CUDA-event GPU completion. More than 16,384 calls means the
percentiles cover the retained tail window only. A fault can omit later stages;
zero calls does not establish runtime availability.

Existing startup JSON reports heartbeat p95/max, capture-to-present p95,
camera-tick p95, arrival/processed/presented counts, ingress drops and GPU retry
expiration. Pipeline diagnostics retain a separate 240-sample p50/p95/p99
window; startup JSON does not export all of those percentiles. The heartbeat
retains its first 3000 samples. Capture-to-present ends at submission and begins
after capture returns, not at physical sensor exposure.

Pan-input-to-present p95/p99 and camera/processing queue-depth traces remain
uninstrumented. The helper's rejection count is not camera queue depth, and
ingress drops are not stale SR results. A controlled sleep demonstrates the
helper's synchronous wait but cannot establish a vendor-call tail. The later
independent producer exports bounded job/stale/replacement counters and
accepted-result age through `SuperResStatistics`; these have not yet been
collected as native traces. Its [policy and fixtures](maxine_async_producer_design.md)
are separate evidence from the historical host measurements.
Actual pan-to-submission distributions and temporal/optical quality still need
native measurement; historical host waits are not post-change UI timings.


## Actual Maxine host run (2026-10-17)

One sequential 60-second off/on pair used CUDA 12.9.2, the installed NVIDIA
runtime, RTX 4090 and an original looped synthetic 1280×720/30 FPS virtual
camera. The private settings used 2× Fill zoom with preferred 2× SR, disabled AI
and recording, and exited normally. The owned OBS harness was stopped afterward.
The source was not a real camera/phone and its observed viewport rate was about
21 FPS in both runs; this is not a 60-FPS or optical-quality claim.

The on run retained 1,256 calls: 1,253 successful frame calls, one model load
and two teardown calls, with zero rejection, timeout or exception. Frame host
caller-wait p50/p95/p99/max was 1.632/7.583/12.469/80.766 ms. The single model
load took 201.780 ms; teardown maximum was 55.328 ms. Off/on camera-tick p95 was
1.889/6.411 ms, UI heartbeat-delay p95 1.914/5.010 ms and the final rolling
capture-to-submission p95 14.689/21.340 ms. Startup is included in heartbeat
maximum (about 1.1 s in both); that value cannot isolate SR latency.

Raw settings, reports and stderr are under the private audit archive's
`maxine-profile/{off,on}/worker-1`. These observations confirm nonzero synchronous
host tails. Pan-input, stale-SR frame age and camera queue identities remain
unmeasured; no async-producer speedup follows from this pair. The [independent producer review](maxine_async_producer_design.md) records the
implemented ownership/provenance/capture policy and its separate acceptance limits.

## Navigation admission and lifecycle continuation

The Maxine caller now defers new model loads and frame calls while the viewport
scheduler reports keyboard/joystick/mouse navigation or its 150 ms tail. Actual
pan/zoom changes explicitly stamp navigation; generic photo/readback redraws
cannot keep inference paused. The current camera frame uses the normal
spatial/base fallback, and the performance guard/model state survives the pause.
This prevents admission during observed input; it cannot preempt an earlier SDK
call, remove idle-call waits, or prove a native pan latency improvement.

Explicit adapter teardown can load again, and repeated teardown/destruction does
not dispatch another worker teardown. The two teardown calls in the original run
above are retained historical evidence, not the expected post-repair call count.
Private fake-SDK lifecycle tests and transactional timing-event fault tests check
these ownership boundaries without measuring proprietary SR pixels or speed.
