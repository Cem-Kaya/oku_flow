# Engineering audit closure — 2026-10-17

This record maps all 36 findings in the supplied *OkuFlow Agent Engineering
Brief* to code changes and evidence. “Repaired” describes an implemented boundary
with targeted regressions; it does not imply a published release or complete
hardware/provider/installer acceptance. Original failed attempts remain in the
private evidence archive, not in Git or release payloads.

The work is on `fix/report-critical-20261017`; browser controller changes are in
the sibling website's `fix/report-transcription-cancel-20261017` branch. Public
v0.2.5 downloads have not been replaced by this work.

| Finding | Disposition | Implementation / evidence |
| --- | --- | --- |
| NP-001 | Repaired | Camera open/reopen/reconnect driver startup is worker-owned; real-target held-open/cancellation regressions. |
| NP-002 | Repaired | Microphone open, flush and final shutdown are worker-owned; late session completions cannot affect successors. |
| NP-003 | Repaired | Recording remains nonterminal until old-session cleanup completes; barrier regressions. |
| NP-004 | Repaired | Annotation PNGs use exclusive temporary files and atomic no-clobber publication. |
| NP-005 | Repaired | Glare-only Text Clarity no longer enables unchecked background flattening; CPU/CUDA fixtures. |
| NP-006 | Reproduced and repaired | Native AAC compacted a three-second gap. Bounded silence restores the marker to 3.999333 s versus video 3.999967 s, without changing normal PCM accounting. |
| NP-007 | Repaired | Every failed encoder attempt keeps its bytes; retries use exclusive GUID paths and notes use the selected finalized pair. |
| NP-008 | Repaired | Annotation intent retains displayed source/frame/processing revision, actual GPU output signature and transform; camera retirement revokes pending copies. |
| NP-009 | Repaired | Saved microphone None is distinct from actual audio capture; metadata follows real capture state. |
| NP-010 | Implemented; native performance acceptance open | Independent Maxine producer owns its stream, private snapshots/output and one in-flight job. Model load, inference, completion and teardown stay off UI; publication requires the exact current scene and graphics fence, with immediate same-frame capture fallback. GPU fixtures cover stale/cancel/consumer lifetime and retry; native pan/quality/flicker acceptance remains separate. Historical host-tail measurements predate the implementation. |
| PW-001 | Repaired | One serial Android session owner fences restart, stop and disconnect with immutable host/session tokens. |
| PW-002 | Repaired, sibling website | Cancellation generation rechecked after every await and before worker/result publication. |
| PW-003 | Repaired | Completed segments are frozen before awaits; sidecar writes serialize by media identity. |
| PW-004 | Repaired across native/browser/website | Immutable UUID/content markers plus bounded fresh byte verification bind notes, File handoff, result cache and UUID-named transcript sidecars. |
| PW-005 | Repaired, sibling website | Audio decoding and worker results preserve the original media timeline origin. |
| PW-006 | Repaired, sibling website | Decoder/parser acquisition, feeding and flush use finally cleanup while preserving the primary error. |
| PW-007 | Repaired | Native and Android share one bounded startup deadline; candidate retries reserve useful time and CSD alone does not start HEVC output timing. |
| PW-008 | Repaired | Serialized field patches replace stale whole-settings writes; concurrency/barrier JVM fixtures. |
| PW-009 | Repaired | Zoom intent waits for its exact capture session and survives same-generation fallback, without modifying display settings. |
| AS-001 | Repaired at child launch boundary | Non-coding Codex launches use a constrained capability profile and replace the child after permission changes. Live provider/OS sandbox acceptance remains separate. |
| AS-002 | Repaired | Credential IDs bind immutable provider/endpoint/key tuples; failed settings commit and backup recovery preserve matching old generations. |
| AS-003 | Repaired | History commits before pruning; bounded durable overflow preserves previous conversations on write/cleanup failure. |
| AS-004 | Repaired | Authorized image observations retain native provenance and grant epochs through final HTTP/Codex/MCP egress checks. |
| AS-005 | Repaired | Exact approvals report pending until actual operation outcome; final receipt retains the original chat-turn identity. All 48 new translated entries reviewed. |
| AS-006 | Repaired | Wait compares the same enriched operation projection used by queries, avoiding unchanged-state false transitions. |
| UI-001 | Repaired | Stable message/block/UTF-16 anchors preserve duplicate occurrence, reverse selection and reading position; deleted messages cannot transfer selection. |
| ST-001 | Repaired | Quick Mode Save commits immediately and rolls back only the staged mode on failure. |
| TB-001 | Repaired | Fresh exact-byte hashes bracket every evidence attempt, including resume; metadata-preserving mutation invalidates reuse. |
| TB-002 | Repaired | Strict release-capability profiles reject missing/skipped required evidence; optional development skips stay explicit. |
| TB-003 | Repaired | Imported Android host probe participates in CI and scoped impact selection. |
| BR-001 | Repaired | CMake minimum is 3.25 for schema-6 presets; official 3.25.3 root/preset parsing checked. |
| RB-001 | Repaired | WiX executes only the exact pinned archive apphost/tree; ambient feeds and cached installation shims cannot substitute tools. |
| RB-002 | Repaired | Effective CMake target facts and successful-build facts/binary seals govern CUDA/compiler/runtime/license provenance; reconfigure-without-build is refused. |
| RB-003 | Repaired | Verified bundle generations exchange by same-parent rename with rollback. Locked/user-owned/unknown bundles remain intact; previous complete generations are retained. |
| RB-004 | Repaired | Bundle builds use Ninja with VS2022 x64 and compilation databases; unsupported generators/stale caches fail before mutation. |
| PF-001 | Measured and optimized | Eligible streaming tails update only the final root paragraph; stale resize callbacks no longer rebuild unchanged documents. Same-workload queued p99 improved from 36.062 to 10.610 ms with zero selection/anchor changes. |

## Ultra Clear Text

The experimental opt-in uses local Windows OCR on one latest-only worker, an
independent bounded probe worker and cached glyph reprojection on presentation.
Slow recognition never blocks presentation. Shared line type sizes/baselines
avoid independently stretching each word's height; horizontal width fitting
remains. A live synthetic-camera review accepted all 44 printed
fixture words and exercised Show original. A held-OCR fixture measured presentation
hook p99 0.006 ms and 1080p Qt painting p99 3.115 ms; these exclude real camera,
GPU scanout and OCR accuracy beyond the fixture. High-risk/moving/composite text
fails closed. Curved pages, handwriting, reliable math/diagram replacement,
multimonitor/NVDA acceptance and localization of new UCT controls remain open.
See [UCT architecture and limits](ultra_clear_text.md).

## Validation evidence

Private archive: `%LOCALAPPDATA%/OkuFlow/audit-closure-20261017`.

- Earlier complete tracked gate at `fe3cbd4`: shipping compile, 144 CPU/native
  cases (one explicitly optional dev-full Qt row skipped), and seven CUDA delta
  cases passed. Later changes require the final gate below.
- Remaining boundary SCOPED gate: recording manager, native H.264/AAC, phone
  protocol, notes HTML and source equivalence: five passed, zero skipped.
- Policy SCOPED gate: recording integrity, deadline worker and complete headless
  Assistant panel fixture passed. Final chat parity/fallback slot also passed.
- Android tracked debug assembly, JVM tests and lint passed; 11 Python host
  protocol tests and eight new pure JVM budget cases passed. No APK was installed.
- Bridge Node contracts: 11 passed; sibling website check: 23 passed plus asset,
  14-page and language checks. Native Chrome folder-grant acceptance is separate.
- Private real WiX synthetic MSI compile/extract and provenance mutation/refusal
  fixtures passed; no installer was installed on the owner's PC.
- Native target fixture verifies annotation source/geometry/actual-output refusal,
  camera/microphone worker ownership, operation waiting and stop/undo. Refreshed native fixture passed after the actual SR/focus-output signature
  regressions. At `0c8c853`, before the continuation below, the complete tracked
  Windows gate passed: shipping compile, all 146
  CPU/native cases (one optional dev-full Qt row skipped), and seven CUDA delta
  cases with no skips or unresolved failures. No saved results matched; all
  selected cases ran. PCH-OFF compilation plus chat, recording-integrity and
  source-equivalence checks also passed (three cases).

No phone display resolution, density, aspect ratio, rotation, scaling or font
setting was changed. No owner process or foreign adb server was stopped. There
is no new public installer, Play rollout, V1 catalog acceptance, or physical GTX
1070/microphone/camera/Maxine quality claim from these fixtures.

## Follow-up measurement

NP-010 now has actual host-tail evidence from one off/on synthetic-camera pair
using the installed NVIDIA runtime. Across 1,253 successful frame calls, p95/p99
caller wait was 7.583/12.469 ms and maximum 80.766 ms; model load took 201.780 ms.
There were no timeouts, rejected calls or SDK failures. Off/on camera-tick p95
was 1.889/6.411 ms and UI heartbeat-delay p95 was 1.914/5.010 ms. These are one
development-PC pair with a virtual source, not pan-to-present or real optics
acceptance. The earlier read-only review identified GPU lifetime, capture pairing,
exact presentation provenance and sharpness-flicker policy requirements. After
the owner's implementation request, the independent producer now implements the
[exact-current-frame policy](maxine_async_producer_design.md), with native
performance/optical acceptance still separate. The earlier bounded
patch admitted no new synchronous SDK work during actual navigation, fixed
reload/double teardown and allocated timing events transactionally; generic photo/readback requests do not
activate that navigation gate. The [performance diagnostic record](performance_diagnostics.md)
keeps the unsuccessful intermediate chat measurement as well as the final
improvement; controlled sleeps are not vendor SDK performance measurements.

## Navigation, lifecycle and CI continuation

Private evidence for this continuation is under
`%LOCALAPPDATA%/OkuFlow/audit-continuation-20261017`.

- The actual Maxine adapter now reloads after explicit teardown, dispatches no
  redundant teardown after retirement, and preserves stage/status diagnostics
  without changing timeout quarantine or device ownership. The private
  `maxine_lifecycle_cuda` fixture passed, including controlled partial failure,
  stage short-circuit/recovery and late completion after a timeout.
- `maxine_navigation_cuda` passed through the actual CUDA surface and private
  D3D12 textures. Known pattern readbacks verify no SDK admission during
  navigation, current-frame spatial/base pixels after previous neural output,
  same-model idle resumption and balanced retirement. It uses fake SDK code,
  not a proprietary model, native window, camera or presenter fence.
- The CPU navigation/timing-allocation cases passed. They distinguish actual
  input from repeated photo/readback-style redraws and inject partial event-pair
  allocation/repair without a GPU.
- Remote run `39043882433` timed out in `bundle_toolchain_contract` at 60.15 s;
  that failed log is retained. Configuration refusals now precede VS setup,
  retaining one real initializer rather than three. The fixture additionally
  refuses forged VS environment claims and fingerprints cmd/vswhere helpers.
  It passed locally in 5.17 s alone and 9.06 s in the parallel background gate,
  preserving its 60 s deadline and behavior assertions. Those local results do
  not establish a passing replacement remote run.
- The tracked background matrix passed: shipping Release compilation, all 83
  selected CPU cases (one optional development MCP Qt row skipped), and all
  eight CUDA delta cases with no skips or unresolved failures. No saved results
  matched. GUI was explicitly excluded; the prior complete native gate does
  not certify these newer changes. This is partial validation, not release
  acceptance. No phone, native foreground window or public release was touched.

The independent Maxine producer is now implemented; see its
[ownership/capture policy and fixtures](maxine_async_producer_design.md).
Native performance, UCT/hardware/provider/installer acceptance remain open with
the limits recorded above. Earlier gates above predate this continuation.

## Hosted relocated-SDK repair

Run `39051051673` on `08e877d` confirms that VS discovery/initialization no longer
times out: the preflight finishes in 3.47 seconds but fails because it searches
the local Qt default instead of the runner's `QT_ROOT_DIR`. The bundle now
recognizes that prefix after explicit overrides. Hosted Qt setup also installs
only the matching Multimedia source archive for the required FFmpeg notice.
The regression receives CTest's configured SDK/source root, uses a private real
SDK junction with spaces in its path, and fingerprints its notice inputs. It
failed against the previous batch before the fix (2.87 seconds), then passed;
the final tracked SCOPED gate passed 1 of 146 cases in 3.74 seconds with no
skips or unresolved results. CPU compilation, YAML parsing and 24-catalog
translation parity passed. The infrastructure selector correctly requests the
full pre-merge gate; this focused local result is not release acceptance.
Original hosted/local failures and final scoped evidence remain under
`%LOCALAPPDATA%/OkuFlow/ci-39051051673`. No foreground UI or device was touched.

## Focused publication fixture follow-up

Run `39055193846` on `642ba49` repeats the Qt-prefix failure already addressed
by `9171e03` and also times out the cumulative publication fixture at 60.20 s.
Its buffered log does not isolate a blocked operation. Production publication
logic is unchanged. All 13 existing scenarios now run in three serial CTest
groups: Exchange (3), Refusals (7), Rollback (3), each with the same 60-second
deadline, shared process-identity lock, named scenario progress and completed-
coverage count. A single slow operation can still fail its focused group.
The tracked SCOPED CPU gate passed all three groups (3 of 148 CTest cases) in
2.24/2.01/1.34 seconds, without skips or unresolved results; CPU compilation
and translation parity also passed. These local timings do not establish the
hosted cause or replacement hosted pass. Original failed evidence and focused
results are under `%LOCALAPPDATA%/OkuFlow/ci-39055193846`. No GUI or phone was
touched, and no public release was produced.

## Archive SDK license-layout repair

Run `39056398406` on `0c9051b` passes all three publication groups, but CTest
refuses to start the toolchain regression because the archive-installed SDK
lacks `Qt/Licenses/LICENSE`. Preflight now accepts the unchanged LGPL notice
from the matching Multimedia source archive after the two existing SDK/installer
locations. CTest resolves and fingerprints that actual source file; no notice
requirement or staging-byte check is removed. Hosted setup already installs
this source archive, so no additional action or dependency is introduced.

The relocated real-SDK fixture now omits only its optional `LICENSES` directory
and verifies selection of the source notice. It reproduces the original failure
before the fix (3.10 seconds). With the fix, the tracked CPU build and SCOPED
gate pass 1 of 148 cases in 3.89 seconds, without skips or unresolved results;
24-catalog translation parity also passes. One real VS initialization and the
60-second deadline remain intact. Local source inputs come from the official
Qt Multimedia `v6.12.0` archive; its LGPL file is byte-identical to the installed
SDK's copy. Download provenance, hashes and both attempts are retained under
`%LOCALAPPDATA%/OkuFlow/ci-39056398406`. Hosted confirmation remains pending.
No native GUI, phone or release acceptance was performed.

## Independent UCT review and regression repairs

Private evidence for this continuation is under
`%LOCALAPPDATA%/OkuFlow/uct-review-20261017`.

Astra and Muse source reviews identified four independently reproduced defects:

- Failed or stopped presentation could leave reading text available, and a
  toggle or queued completion could restore cached glyphs without a successful
  presentation. Both surfaces now require a recent successful presenter
  submission receipt; attempted submission cannot refresh that clock.
- Omitted high-risk/unconfirmed tokens appeared only at a line's end, changing
  the apparent order. Accepted reading tokens now retain their original slots
  and collapse only adjacent omission markers.
- Plates and replacement glyphs had different size/clip predicates. Both now
  consume one eligibility decision, leaving original pixels when either cannot
  be drawn. Reference font, ink metrics and readable colors are cached for
  presentation-rate repaint.
- Recognition refresh reset reading-pane selection/caret. Common unchanged
  prefix/suffix offsets preserve reading position and both scrollbars. Parent
  regression tests additionally caught a live-`QTextCursor` bug in the proposed
  patch; integer endpoints are now saved before document replacement and mapped
  away from surrogate-pair interiors.

The first focused UCT contract passed all 52 Qt rows, including the new
regressions. Real Windows OCR recognized all six known words in each of four
synthetic dark/light fixtures, including opposite-brightness surrounds. The
suggested global-inversion failure did not reproduce, so no extra OCR pass was
added. Native control geometry and overlay Fill-mode pan/zoom, resize and owner
hiding checks passed with composed screenshots. The first native movement
attempts used Fit mode, which deliberately ignores zoom; those original fixture
failures are retained alongside the corrected Fill-mode check.

A recorded held-recognizer fixture measured callback p95/p99 0.005/0.006 ms and
1080p Qt paint p95/p99 2.912/3.078 ms. Dense 4K probe validation p95 was 64.15 ms
on its separate worker. These are development-PC synthetic observations, not
end-to-end FPS or physical camera accuracy. Cached pan/zoom reprojection follows
the presentation clock; content motion remains translation-only and probe-rate
(at most once per 80 ms), not high-FPS OCR or high-FPS content tracking.

Aspect-preserving horizontal glyph geometry, stroke-control placement and UCT
localization remain follow-up work. Background inference remains a heuristic,
with the dense-word failure repaired below.
Curves, handwriting, reliable math replacement, physical-camera quality and
screen-reader/multi-monitor acceptance are not established by these fixtures.

Argon's independent inline-source review completed after its first filesystem
attempt was refused by the provider. Astra, Muse and the parent verified its
claims separately rather than treating confidence scores as evidence:

| Item | Disposition and actual evidence |
| --- | --- |
| 1, dense-match incumbent | Reproduced and repaired. Sparse ranking cannot replace the better dense zero score; combined subpixel output is rechecked. Astra then found a proposal-neighborhood regression in its patch, reproduced separately and repaired while retaining both positive cases. |
| 2, cropped-line duplication | Reproduced: a return pan showed Gamma twice. Spatially replaced cached lines are retired, including those whose centers lie outside the current OCR coverage. Their unrecognized offscreen remainder is reacquired by later OCR. |
| 3, delayed refresh validity | Reproduced: all ten words disappeared after a budget-deferred identical refresh. Only exact references/text/geometry already validated against the latest probe may reuse that proof. Seven negative rows reject altered samples, text, geometry, older proof and scene changes. |
| 4, overflow and sampling geometry | Normal 8K runtime paths do not overflow the integral table. Actual-image diagnostics measured bounded cross-decimation bias: up to 3.35 source pixels (0.84 probe pixels) in the tested production pairing. This remains geometry precision follow-up, within the one-probe-pixel diagnostic tolerance. |
| 5, differing probe decimation | No threshold failure reproduced in the nine measured pairings. At production width 2560, 8/12/16/28 px print scored 0.92–0.96 on identical content and 0.92–0.98 after a three-source-pixel shift, above 0.82. Changed-text controls were rejected. No blind resolution alignment or extra OCR pass was added. |
| 6, initialization retry | Both failed/unavailable initialization rows reproduced. Explicit off/on now creates a fresh recognizer and generation-fences predecessor callbacks. |
| 7, row chaining | Rejected. Entries remain OCR lines; the suggested observable order is already correct. Expanding the anchor would break existing mixed-height ordering. |
| 8, dense text polarity | Both polarities reproduced through the real scheduler's color path with controlled 75%-fill strokes. A bounded surrounding margin identifies paper/board before the minority-ink fallback. This is not an OCR accuracy claim. |

The focused contract now passes 67 Qt rows. All original failure logs and the
counterexample to the first matching patch are retained. An initial full tracked
run passed shipping compilation, 148 of 149 CPU cases (all 64 native GUI cases),
and all ten CUDA delta cases. Its sole failure was the old held-OCR fixture's
implicit startup ordering under parallel CPU load: it had only 110 presentation
ticks after the worker started. The fixture now establishes the held job before
the fixed 300-tick loop; progress/time assertions and deadlines are unchanged.

The final tracked Windows run then passed shipping CUDA 12.9 Release
compilation, all 149 CPU CTest cases (including 64 serial native GUI cases),
and all ten CUDA delta cases. One explicitly optional development MCP Qt row
skipped; no selected CTest case skipped or remained unresolved. No saved result
matched, so every selected case ran. The final UCT contract's 67 Qt rows passed
under parallel CPU load. Its held-OCR observations were callback p95/p99
0.006/0.008 ms and 1080p Qt paint p95/p99 3.204/4.078 ms; separate dense 4K
validation p95 was 62.54 ms. These are fixture observations, not camera FPS.

A separate one-off Qt-offscreen probe compiled the unchanged production core
under LocalAppData and measured Muse's cross-decimation hypotheses on actual
rendered images. Raw JSON, stderr, source and CMake inputs are retained under
`sampling-probe/`; it is not an added release runner or a CER/WER measurement.
The 1003-wide ref2/probe4 pair is a stress case, not the controller's pairing at
that width. Production-true width 2883 ref2/probe4 also stayed above threshold
(0.92 identical, 0.91 shifted); its forced-search offset was the 3.35 source
pixels recorded above. Same-decimation/divisible-width controls and both page
sides distinguish remainder bias from kernel/renderer effects.

The rebuilt CUDA application also completed one isolated 120-second session
with the existing synthetic page through the private OBS/DroidCam feed. UCT
paused while the fixture's Temporal Smooth was on, then displayed four accepted
words and matching reading-pane content in the cropped 1.6x view after that mode
was disabled. It received 3558 camera frames and presented 2538; aggregate idle
viewport rate was 21.4 FPS, not a claim of high-FPS camera or overlay tracking.
The profiling child exited normally and the private camera helper stopped its
owned OBS. Its optional PNG export was refused because the parent supplied a
90000 ms delay beyond the hook's 60000 ms limit; that failed export is retained,
not represented as a saved screenshot. Actual Computer Use images were inspected;
the complete native fixture captures are retained separately. Show-original's
attempt at the session deadline found an already-closed window, so it adds no
live comparison acceptance beyond the passing controller fixture.
