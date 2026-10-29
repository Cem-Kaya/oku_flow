# Future documentation recommendations (NOT included in patch)

Update after root reviews/applies the experiment; preserve current repair docs.
No default change or performance/quality acceptance follows from this proposal.

- docs/code_reference.md: recognizer_width scalar helpers, checked bucket profile,
  Network cached-only/preparation separation, owned groupWidths/hostGroupWidths,
  RecognitionGroupWidths API, profile fields and five registered fixture slots.
  Explicitly retain current frozen-upload/RetireOwnedState contracts unchanged.
- docs/hardcoded_paths.md: exact width-v1 cache tag/profile dimensions/N and
  GPU/model/driver/TRT identities; TRT_WIDTH/PREPARE_WIDTH selectors and strict
  refusal semantics; separate reviewed detector/tier/N plan preparation list.
  Existing fixed1024 keys/plans/default preparation behavior remain unchanged.
- docs/uct_gpu_batching.md: opt-in contiguous-group maximum bucket policy, no
  crop reorder, common canvas within queued-N1 group, high-water memory bounds,
  compact metadata transfer and actual-profile/canvas-bin observability.
- docs/uct_resident_baseline.md: RESIDENT_WIDTH old/new selector, benchmark
  builder refusal/inference-only SDK, exact same sources/settings/frozen crop
  hashes and separate complete-path run, warm-only timing and retained output
  differences. Per-run N1/wider agreement is not cross-width acceptance.
- tests/README.md: three policy/reference/profile slots and two CUDA slots in
  their existing explicit selectors; no-model CUDA coverage versus outstanding
  prepared-engine transition/CTC geometry/omission/quality acceptance. Track
  failure/skip limits and do not claim execution from this private package.
- docs/uct_performance_roadmap_2026_10_20.md/CHANGELOG.md: only if recording the
  integrated experimental option. State disabled-by-default and measured results
  only after reviewed plans/quality gate.120FPS/8.3ms and3050 remain goals,
  not measurements here; useful-padding fraction is not measured speedup.

No new UI strings, dependencies, model pins or gradient representation are added.
No third-party code was introduced. Keep source-impact/equivalence guard inputs
and tracked validation selectors aligned when integrating this new private header.
Root owns appropriate scoped/release/CPU/CUDA gates and resulting evidence.
