# UCT / accessible-video review disposition (2026-10-18)

Opus and Muse Wonderful reviewed the initial architecture. Opus implemented the
positioned accessibility model; its final review was unavailable because of an
API rate limit. Muse Wonderful continued the implementation and independently
reviewed media ownership/performance. A separate Sol review checked the UIA
adapter seams. Grok 4.7 High supplied an independent code review. The coordinator
checked each reported path before authorizing the repairs below. Reviewer
reports were read-only unless explicitly assigned implementation; their reports
are not independent test passes.

| Confirmed issue | Repair / regression |
| --- | --- |
| Repeated still resampling/upload on every 16 ms poll | Exact successful viewport stamp reuses unchanged pixels; admission-count test covers 120 polls, dirty view and retry. |
| 4K staging plus retained neighbors exceeded decoded quota | Initial 41eb8cd repair dropped optional decoder leases before new samples and retained through no-sample EOS. Follow-up review below replaces the fragile fixed 96 MiB policy. |
| IStream cursor raced validation/actual file offset | Per-stream cursor lock encloses validation, seek, I/O and advancement before backing lock. |
| Playing video continually restarted immutable OCR | Paced advancing-media receipts share OCR/probe workers; separate real PTS and wall deadline; held-worker/change/pause/authority cases. |
| Finalization collapsed previously proven part duration | Retain proven duration while revoking old byte-range leases. |
| Unsupported fragments reparsed forever | Latch unsupported generation; parser-attempt counter regression. |
| Late canceled Original readback reached viewport consumers | Bounded exact retired-ID discard before any observation/analysis consumer. |
| Active-tail wait mislabeled Playing | Explicit native Waiting with preserved forward intent and silenced output. |
| Unknown-position write received offset-zero credit | Dedicated unknown-write loss path grants no verified prefix. |
| Waiting clock fast-forwarded arriving fragments | Rebase to held PTS on growth and invalidate stale audio clock; automatic no-Resume pacing test. |
| Encoder startup failure discarded saved playback | First accepted Original acknowledgement gates source transition; failed encoder/first-frame and stale callback tests. |
| Invalid Save click reused prior receipt identity | Receipt starts with owned payload admission; service rejects unchanged revision; previous-committed regression. |
| Duplicate live pixels established pending words | One bounded pending-evidence ticket requires distinct digest/frame and same scene/authority; established valid text survives refresh. |
| Selected presenter failure left UCT exposure live | Media-only failure latch immediately clears glyph/pane/UIA; view/toggle/late callbacks cannot restore without a successful receipt. |
| Paint queries ran before offscreen expose delivery | Bounded real-paint fixture delivery; existing geometry assertions retained. |
| Fit fixtures claimed zoom/pan coverage | Fill performs real crop changes; 300 projections retain interface identity and current ranges. |
| Lazy character query used a changed font over old pixels | Paint plan retains its shaping font; query-before-repaint regression. |
| RTL original-ink cells and omission gaps ran backward | Visual caret normalization and direction-independent gaps; Hebrew/Arabic hit tests. |
| Hidden endpoints created a nonempty native range | Shared hidden-run anchor plus exact Qt endpoint-aggregation regression. |
| Export/grab paint became on-screen geometry authority | Redirected paints use a scratch plan; later real paint alone promotes authority. |
| Source-scoped undo skipped a view change across its own temporary camera restore | Exact-owner restore preserves the inverse only for the same selected media identity/generation; native selection and field changes still win. Actual-app fixture plus both synthetic selection rows cover it. |

## Seek/buffer follow-up

Muse Wonderful compared the fixed decoded allowance with modern bounded
playback queues and reviewed the adaptive implementation. The coordinator
confirmed that UHD leaves almost no spare capacity at 96 MiB and DCI 4K cannot
fit the same roles. The replacement accounts for staging, replacement,
presentation/seek retention and small ready queues under a 256 MiB ceiling;
arbitrarily retained external leases cause backpressure. Muse also caught
credit waits holding their own optional predecessor and a due-head deadline
discard; those paths were repaired before the native stress runs.

Grok 4.7 High independently found an End-scan cache interval incorrectly
covering earlier real frames. End now proves only its actual endpoint. The
coordinator also verified predecessor loss across sample-less EOS, copied
same-frame scrubs, audible Resume preroll copies and stale request diagnostics.
Repairs retain optional predecessors when credit permits, share immutable pixel
allocations across reticketed frames, filter Resume preroll and clear counters
on Submit. Native 30 fps cases test End followed by an earlier seek and Resume;
UHD/DCI cases test allocation identity under full consumer-held credit.

The first audio regression run caught the new copy filter leaving the Resume
loop after a discarded sample and later replacing the same held PTS. The
worker now carries the cursor floor across passes and continues past discarded
samples. Both AVC/AAC and AV1/AAC failed attempts remain archived; the repaired
focused cases passed. Performance output describes worker publication on short
synthetic clips, not measured screen latency or a GPU decoder benchmark.

## Scope and remaining acceptance

Qt's Windows adapter does not map structure-change events. The implementation
uses passive live navigation/hit-testing queries instead of fabricated property
or speech events. Clients that cache the tree without re-querying need native
acceptance; this is a documented integration limitation, not a claim that every
screen reader fails or works. Partial/disjoint character-range clipping and
mixed-monitor scaling also retain Qt adapter limits.

Selected-media viewing supports zoom, rotation, reading colors and UCT through
the CPU compatibility or preferred GPU scene. The later full-effects GPU
integration uses shared camera/video settings with independent media history;
CPU-only rendering retains its restricted subset. Exact live-recording Pause
refuses an actually displayed composite/warp/neural/unsupported recipe instead
of substituting different pixels. Saved recordings always remain clean Original.

Offscreen, synthetic native codec and fake-provider checks do not establish
physical A/V sync, camera optics, long soak, native Narrator/NVDA/Magnifier,
installer or public release acceptance. The owner reserved the desktop for this
integration run; native GUI cases remain pending. Test evidence and original
failures stay in the tracked LocalAppData ledger. See
[implementation and validation](accessible_video_implementation.md).

## GPU integration review follow-up

Muse Wonderful implemented and reviewed the independent CUDA scene, device UCT
inputs and analysis-gap continuity. Grok 4.7 High separately reviewed decoder
ownership and app integration. Opus reviewed transport usability and low-level
app wiring, then received separate implementation assignments for each scope.
The coordinator verified the reported paths before applying or assigning fixes.
Review is evidence of code inspection, not native desktop or device acceptance.

| Verified issue | Repair |
| --- | --- |
| Failed or timed-out video-processor work could release the decoder sample/reader or poll a query never ended | Capture the entire native owner before Blt; never poll an unended query; quarantine unknown completion. |
| A fallback reader could share a quarantined reader's byte-stream cursor | Clone independent cursors over the same held file backing and range policy. |
| Temporary pressure or an ordinary stale/seek failure could latch CPU compatibility | Separate typed GPU incompatibility from allocation pressure and request validity. |
| Shared video-processor targets are rejected by some drivers | Capability fallback uses a private processor target and GPU copy, under the same allocation budget. |
| Eight-bit negotiation could silently discard a higher-precision native source | Prefer appropriate high-precision output and verify actual decoder precision on 10-bit fixtures. |
| A queued private-fence wait could outlive its unretained native fence | Retain the fence before Wait, after admission checks; retire only behind proven global-queue completion. |
| Presenter lifetime holds accumulated every prior source | Bounded worker retirement follows each source's actual global read fence. |
| Observe paired new pixels with the previous displayed sample's sequence/PTS | Build media observation authority from the exact submitted candidate. |
| Read/Explain and image attachments could capture a decoder successor | Pin the displayed sample and submit its copy during the click turn. |
| Ink capture rejected a replaced scene before attempting recovery | Regenerate the actual selected frame before reading its annotation authority. |

Native conversion, CUDA scene and window-free presenter fixtures exercise real
GPU operations. App fixtures use the real target under Qt offscreen. Their
precise final results and any remaining gaps belong in the validation record;
none of these fixtures claims visible UIA, screen-reader, HDR or A/V acceptance.

The full-effects Muse follow-up found additional app seams: multi-part observation
PTS needed the document offset; first GPU candidates needed explicit backend
provenance; view retries could discard a valid OCR lease or drain its attempt
budget; resize-dropped annotation readbacks lacked media expiry; and paused
keystone/SR status lacked refresh. Repairs address those paths separately from
GPU decoding. The coordinator also revoked prior-document display receipts on
new-file adoption and preserved unknown GPU diagnostic state until proven.


The final Muse regression round added hardware-free real-app checks for unknown
first-GPU diagnostics, retained exact UCT leases and bounded retries, and the
one-second annotation expiry while media is selected. The coordinator closed its
remaining admission concern by validating incoming OCR identity before storage,
rather than waiting for another replay. Multi-part PTS remains code-reviewed;
a real multi-segment recording is still separate acceptance.

The pre-update hosted CPU check failed two layout cases because their shared
assertion inventoried five actions after Open video became the sixth. The
assertion now includes the real sixth button; all geometry requirements remain.
Native execution of those two cases is deferred to the free hosted desktop.
The first local GPU matrix exposed a fixture still expecting three queued CPU
uploads. The fixture now verifies the intentional one-in-flight bound, unchanged
scene/fence/history on repeated busy admission, nonblocking return and exact
owned queued pixels after releasing the private graphics gate. The failure and
original evidence are retained.
