# Shared brief for read-only UCT source specialists (2026-10-23)

Parent: Opus end-to-end latency + shadow tracker audit. You are one of two
disjoint read-only source specialists. The parent reconciles and ranks.

## Hard limits
- READ-ONLY. No repo edits, commits, stashes, configure, compile, test,
  benchmark, GPU, inference, model prep, download, browser, GUI, device, runner
  lease. Owner desktop reserved; all compute held. No messages to anyone except
  your final answer.
- Write only NEW files under
  `Z:/Projects/OkuFlow/local-work-data/uct-review-implementation-20261023/opus/end-to-end-shadow-audit/specialists/`.
- Frozen source: `Z:/Projects/T3-worktrees/oku_flow/t3-f93349e5` at commit
  `9bdd5d13f2352342860ba146ad164c7e46645d05`. The worktree HEAD (a7d4757) only
  changes a test fixture/docs; production files on disk equal 9bdd. Cite
  `path:line` from that tree. Use `git -C <tree> show 9bdd5d1:<path>` if in doubt.
- Do not claim whole-file coverage if you only grepped. Record in a coverage
  table which line ranges you actually read vs grepped.

## Known facts (do NOT rediscover or take credit)
- Two-slot recognition look-ahead (299.51 -> 252.92 ms dense FP32 worker) and
  GPU integral (~0.60 -> 0.07 ms) are integrated. Four slots rejected.
- Host merge (PaddleMergeRecognition) was 49-53 ms of ~95 ms BF16 book worker;
  an exact optimized merge exists on another branch (CPU replay only; not a
  worker/FPS measurement). Batched rectify/CTC/3 bulk copies integrated, no
  neural pipeline perf yet. Mixed FP16 detector slower (don't recommend).
- Prior Opus report already covers inside-worker GPU items: per-group timeline
  (E2), graph replay (E3), next-region detector prefetch (E5), width buckets,
  small recognizer, tail padding, overview/tile dedup, analysis-pool refusal
  counts, staged per-region publication (research). Mention only if you have
  NEW source evidence that changes them.
- Viewport/Freeze/Resume fix (root 618, NOT in 9bdd): retains accepted OCR on
  same-file Resume; held acquisition marked already-probed; only distinct
  advancing acquisition refreshes. Repeated held pixels/old probe completion must
  never renew current-frame authority. Don't re-report that closed finding;
  but DO report if 9bdd itself has an unfixed variant on a path 618 did not touch.
- Don't recommend: relaxing freshness/thresholds/capacity, skipping OCR
  silently, resident fence removal, image egress, more OCR workers.

## Reporting
- Findings P1/P2/P3 with concrete source counterexample (exact path:line chain
  caller -> callee), confidence (high/medium/low), and a falsifier (smallest
  test or trace that would disprove it). Don't force findings; "no issue found,
  checked X" is valuable.
- Optimization ideas: affected path, mechanism, ceiling (measured/unknown; never
  invent ms), location, risk, exactness (exact vs approximate), falsifier.
- Timing: say which clock domain each timestamp uses (steady wall, media PTS,
  100-ns sample time, presentation sequence) and whether intervals overlap.
- Keep your file <= 2000 words plus a coverage table. Final answer: <= 400 word
  summary + the file path.
