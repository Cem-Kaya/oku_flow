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
| 4K staging plus retained neighbors exceeded decoded quota | Drop optional decoder leases before new samples, retain through no-sample EOS; native twelve-frame 4K playback/seek stays within 96 MiB. |
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

## Scope and remaining acceptance

Qt's Windows adapter does not map structure-change events. The implementation
uses passive live navigation/hit-testing queries instead of fabricated property
or speech events. Clients that cache the tree without re-querying need native
acceptance; this is a documented integration limitation, not a claim that every
screen reader fails or works. Partial/disjoint character-range clipping and
mixed-monitor scaling also retain Qt adapter limits.

Current selected-media viewing supports CPU zoom, rotation, reading colors and
UCT. Advanced GPU effects are explicitly unavailable. Exact live-recording Pause
refuses an actually displayed composite/warp/neural/unsupported recipe instead
of substituting different pixels. Saved recordings always remain clean Original.

Offscreen, synthetic native codec and fake-provider checks do not establish
physical A/V sync, camera optics, long soak, native Narrator/NVDA/Magnifier,
installer or public release acceptance. The owner reserved the desktop for this
integration run; native GUI cases remain pending. Test evidence and original
failures stay in the tracked LocalAppData ledger. See
[implementation and validation](accessible_video_implementation.md).
