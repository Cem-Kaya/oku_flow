# Root source clarification, 2026-10-23

The Opus audit is preserved unchanged. Its follow-up correction attempt hit the
Claude API rate limit and provided no additional review coverage.

The P2 expiry mechanism differs between live camera and advancing saved video.
In `src/app/ultra_clear_text_controller.cpp`, the 200 ms watchdog refreshes a
live overlay only after `PresentationAvailable` becomes false. That predicate
uses the separate 600 ms presentation timeout (`kPresentationWatchdogMs`), not
the model's 350 ms per-line freshness limit. A live receipt that stops just
before a line's freshness expiry can therefore retain the cached overlay until
the presentation timeout and the next watchdog tick. The audit's roughly
200 ms overrun bound applies to the advancing-media wall-freshness branch,
which is checked on each watchdog tick; it is not a general live-camera bound.
Event-loop delay can extend either delay. These are source-derived paths,
not measured timing results.

The implementation assignment covers the earliest applicable line, advancing
wall-validation, and live-presentation deadlines while preserving same-frame,
paused-scene, proof and clock-domain rules. No freshness threshold is changed.

References at root `618ee9a57acb14a601d1fa60e518c0465808968f`:

- `src/app/ultra_clear_text_controller.cpp`: constant at line 125, watchdog at
  lines 150 onward, `PresentationAvailable`, `AdvancingValidationFresh`, and
  `RefreshOverlay`.
- `src/common/ultra_clear_text.cpp`: `OverlayModel::IsReadable`.

The other important implementation constraint is that completion-triggered
probe admission must coordinate both 80 ms gates: the controller probe gate
and the CUDA analysis-capture gate. Bypassing one alone does not establish the
proposed latency benefit. A retry must preserve exact acquisition identity,
media pacing, independent confirmation and the two-slot lease bound.

No test, build, native API, model execution or desktop action was performed for
this clarification. The new implementation is a separate, unvalidated task.
