# Root repair record

Opus reviewed clean `ea66aa0d` read-only. Root applied the repairs in
`a3854495` and `8af64642`; no extra delegate was used for these findings.

- P2 smoothed admission: removed `originalInkOnly`. Shared
  `CurrentInputRegistrationBounds` supplies candidate or rejected protected
  bounds to the CPU and resident EMA/current matching paths. Selection does not
  depend on contour enablement or outcome. Added actual CPU scheduler and GPU
  paired-input matching/mismatching checks. The model regression covers keeping
  independently current text on mismatch and reading equality on/off.
- P2 ink emphasis: tiny positive islands require a strict-threshold survivor.
  Their original pixels remain when omitted. Holes of painted components remain
  holes; orphan negative paths are never emitted. Faint resolved components need
  resolved central support across their extents (max 2 px / 15%). Added partly
  faint bar, weak/strong tiny speck and existing pinhole fixtures. Updated the
  agent policy to prioritize not amplifying weak texture, not hiding source ink.
- P3 nested content: smallest containing owners prevent outer rings counting
  enclosed independent marks twice. Added nested-ring/inner-mark coverage.
- P3 lifetime: resident exceptions quarantine retained work; cancellation is
  classified cancelled rather than input-fault. Other resident failures continue
  the existing fail-closed whole-job policy, documented as a conservative limit.
- P3 transfers/tests: any over-cap count skips all contour copies for that
  region. Documented 524,332 B/region and 4,194,656 B/job bounds. Expanded CUDA
  parity to noisy and many-loop inputs and exercised the production resident
  wrapper with two regions, an oversize skip, counters and cancellation.
- Docs: mid-contrast edges can look thinner; glyph stroke weight does not apply.
  Planner skips, inward regions and whole-region glyph collisions remain explicit
  conservative limits. No native/GUI/release acceptance is claimed.
- GitHub P2 immutable merge (`discussion_r4381348105`): outline-only detail
  artifacts retain their own source reference/lifetime as empty-reading carriers.
  Matching carriers are reused, old risk/original masks survive, and retained
  geometry is capped. Pixel freshness is independent of reading tokens. The test
  covers disjoint and containing mask owners, unchanged prior glyph proof and
  reading output, and 12 repeat passes with stable storage.

Recorded results before the repaired full matrix:

- `repair-cpu-r3-gate.log`: scoped 2/339 pass; primitive/model contracts.
- `cpu-paired-gate.log`: scoped 1/339 pass, including actual CPU scheduler proof.
- `detail-merge-gate.log`: scoped 1/339 pass, including immutable merge regression.
- `repair-cuda-r2-gate.log`: scoped 3/345 pass; parity, resident host/cancel and
  existing paired capture/curve contracts.
- `repair-frozen-cpu-r2.out`: both source regions drawn, nine resolved components,
  1,414/1,086 vertices. Direct logging attempts without forced Qt stderr retained
  empty logs; explicit bounded Start-Process plus Qt logging produced receipts.
- `repair-abba-analysis.json`: 40 timed jobs, clean measured head `a3854495`,
  archived executable hash in `repair-measured-identity.json`, production warm
  BF16, zero image egress, unchanged cache and completed retirement. Whole-worker
  median differences +2.55/+3.25 ms; extraction medians 2.21/1.22 ms with notable
  visit drift. This is not native paint or sensor latency.
- `repair-production-correctness.log`: actual production worker still draws one
  contour region and eight OCR words on each input. Exact directory:
  `uct-resident-20261026-192254-7949c707-b8a3-44ea-9a28-30bb04409614`.

`repaired-background-matrix.log` and `repaired-nopch-gate.log` are separate final
rechecks; identity and GUI exclusion are captured in
`repaired-validation-identity.json`. Their results must be read after completion.
Earlier failed assertions remain, including incomplete probe setup in the new
eviction fixture; the repaired test supplies a real current probe and preserves
the original expected text assertion.

Final repaired-head background results (8af64642, desktop excluded):
- Release compilation PASS; CPU 263 passed, including 4 with skipped Qt rows,
  9 skipped, 0 unresolved; CUDA delta 16 passed, 5 codec skips, 0 unresolved.
- PCH OFF scoped contracts 5/339 passed, no skips or unresolved tests.
- These results are partial validation. Native GUI checks remain pending.
- Exact identities, binary hashes, fresh patch and copies of each gate ledger
  are archived under repaired-validation-results.json, repaired-final-review.patch
  and repaired-{cpu,cuda,nopch}-ledger-v2.json.
- GitHub thread PRRT_kwDOP8zSJs6waWgH confirmed resolved through API.
