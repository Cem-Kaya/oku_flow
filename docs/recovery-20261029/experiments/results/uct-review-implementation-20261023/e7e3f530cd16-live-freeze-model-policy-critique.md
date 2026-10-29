# Live fast / Freeze accurate recognizer policy: design critique

Status: **IDEA ONLY. Not authorized for implementation.** Read-only review by
Opus 5.5 on 2026-10-23 against `fix/uct-integrated-latency` HEAD
`16cb3b6d11166ea5c285e9dae18d7528056d8f0c` (clean). During the review, root
committed `0d0b932` (private small/tiny BF16 experiment seam, N8 fixed1024 only).
That commit has no diff in `src/app`, `ultra_clear_text.cpp` or `uct_paddle.cpp`,
so the source findings below still hold at `0d0b932`. No repo edits, builds, tests,
model/GPU/preparation/cache, GUI, browser or device actions were performed.
The small-BF16 and tiny-BF16 benchmarks are separate, root-owned work; nothing
here predicts their results.

Labels: **[M]** measured (existing evidence or read-only computation over it),
**[S]** source/doc finding at this HEAD, **[I]** inference by this reviewer.

## 1. Evidence used

### 1.1 Worker pilot (same executable, medium FP32 detector in every arm, N8 fixed1024, 32 steady samples per arm/source, shared PC) [M]

| Source | Medium FP32 p50/p95 | Medium BF16 p50/p95 | Small FP32 p50/p95 |
|---|---:|---:|---:|
| Book | 120.17 / 130.99 | 78.40 / 89.37 | 47.60 / 55.54 |
| Dense frame 55 | 450.34 / 505.91 | 274.98 / 320.93 | 193.81 / 226.26 |
| Menu | 41.80 / 47.09 | 34.37 / 37.02 | 28.80 / 32.17 |

- Submit-to-callback equals worker time plus about 0.02 ms in every cell, so the
  isolated scheduler has no hidden queue. It ran without decoder/presenter load.
- Cached construction to first real result (one startup per visit, n=2 per arm):
  medium FP32 3488 / 3560 ms, medium BF16 3948 / 3964 ms, small FP32 2309 / 2300 ms.
  Source: `visit-0N-*-raw/**/summary.json` `startupFirstResultMs`.

### 1.2 Book ground truth (550 words, independent transcript, one 1120x800 image) [M]

All words, case/punctuation-insensitive, includes detection coverage and within-page order:

| Model | Errors / 550 | Bag correct (upper bound) | Wrong returned (lower bound) |
|---|---:|---:|---:|
| Medium FP32 | 263 (47.8%) | 328 | 92 |
| Medium BF16 | 262 (47.6%) | 331 | 88 |
| Small FP32 | 291 (52.9%) | 295 | 117 |

Candidate-disposition words only (the words that can become glyphs before the
size/geometry/protection/paint gates), alignment-based:

| Model | Candidate words | Correct | Substituted |
|---|---:|---:|---:|
| Medium FP32 | 382 | 286 | 95 |
| Medium BF16 | 385 | 288 | 96 |
| Small FP32 | 348 | 256 | 91 |

About a quarter of candidate words are wrong for every model on this image.
Spot-checked medium FP32 candidate errors include real misreads (`well`->`wel`,
`that`->`tht`, `itself`->`itelf`, `possible`->`possile`, `words`->`werds`) and
some alignment artifacts. **All three models read `not` as `no` at the same
position with candidate disposition.**

Per-position cross-tab (candidate-only, reference positions; computed read-only
from `book-ground-truth-score.json` for this review):

| Transition | correct->wrong | wrong->correct | correct->missing | missing->correct |
|---|---:|---:|---:|---:|
| Medium FP32 -> small | 26 | 12 | 18 | 2 |
| Medium FP32 -> medium BF16 | 1 | 3 | 1 | 1 |

On this image BF16 is equivalent to FP32 within noise. Small trades many correct
candidates for wrong ones or omissions.

### 1.3 Dictionary audit (read-only parse of pinned `inference.yml`; config SHA-256 matched `uct_model_pins.hpp`) [M]

- Small and medium share an **identical** 18,708-entry dictionary (18,710 classes
  with blank and space). Tiny has 6,904 entries (6,906 classes) and is a strict
  subset of small. No character exists only in tiny.
- 11,804 small/medium characters are absent from tiny: 9,734 CJK ideographs; all
  239 kana (152 katakana, 87 hiragana), so tiny has **no Japanese kana at all**;
  314 extended Latin letters (Latin Extended-B and beyond, including Vietnamese
  `ơ` U+01A1 and `ư` U+01B0); 12 Greek symbol forms (`ϐ ϑ ϒ ϖ ϰ ϱ ϴ` and archaic
  letters); 27 dingbat circled digits; about 1,477 other symbols (arrows, shapes,
  pictographs, for example `▪` U+25AA).
- Present in tiny (and small/medium): all tested Turkish letters `ç Ç ğ Ğ ı İ ö Ö
  ş Ş ü Ü`; common Western/Central European accents; `€ £ ¥ ° ± × ÷ ≤ ≥ ≠ µ § ©
  ® ™ • … – — ‘ ’ “ ” « » ½ ² √ ∑ ∞ ≈ π Ω ₺ ₹`; `→ ← ↑ ↓ ⇒ ✓ ✔ ① ② ⑩ ■ ● ○ □ ☐ ☑`;
  ASCII punctuation; all ASCII letters and digits.
- No tier contains Cyrillic, Hangul, Arabic, Hebrew, Devanagari or Thai letters.
  Even small/medium hold only 2 of the 90 precomposed Vietnamese letters
  (U+1EA0..U+1EF9). `¡` is absent from all tiers.
- [S] The app's own character filter accepts Unicode letters but rejects many
  symbols. Tiny's symbol gaps therefore matter less for display than its CJK/kana
  gaps, which are display-relevant.
- [I] Tiny cannot emit a missing class at any confidence. On text with those
  characters it substitutes or drops deterministically, and two-read agreement
  then confirms the substitution. Part of any tiny speedup may come from the
  2.7x smaller class layer and CTC output, so that part of the speed is bought by
  removing capability. Tiny's accuracy on shared characters is unmeasured.

## 2. Source findings that constrain the idea [S]

1. **Model switching today is destructive.** `ReplaceRecognizerFactory` calls
   `SetEnabled(false)` then `SetEnabled(true)`. `ApplyEnabled(false)` runs
   `Invalidate({})`, which resets the model and clears glyphs, the reading pane
   and all UIA elements, then retires the scheduler. Re-enable builds a fresh
   scheduler and `AutomaticRecognizer` (TensorRT, CPU, Windows), and the Windows
   OCR bridge serves while TensorRT prepares. One `AutomaticRecognizer` fixes its
   GPU detector and recognizer tiers at construction.
2. **One worker, one latest-only pending slot, cooperative cancellation**
   between recognition groups, with no native-call preemption. A Freeze job
   replaces a queued live job but waits for the in-flight one to drain.
3. **Freeze/Pause does not keep live text today.** A new selection in
   `SetImmutableScene` calls `Invalidate("scene")`. The held image then gets one
   Standalone read. The standalone policy forbids reusing eligibility from one
   mode as evidence in the other.
4. **An atomic-replacement precedent exists.** In Standalone mode
   `immutableFullPreparing_` marks a provisional bridge artifact. A complete
   preferred full read of the identical input (frame, PTS, digest, scheduler
   authority, transform, smoothing and hold metadata) is admitted into a fresh
   `OverlayModel` copy. `Reset` preserves monotonic line IDs. The copy is swapped
   in only on `kAccepted`. Failed or empty reads keep the provisional artifact,
   crops cannot consume the handoff, and no references or geometry are
   transferred. This is keyed on `backendPreparing`, which also drives
   `CreditCompletion` refusal, 500 ms preparing retries and detail gating.
5. **Resume carries proof forward, but advancing admission has no model identity.**
   `ResumeImmutableMedia` keeps accepted words and drops pending ones. In
   `AdmitImpl` a word is shown only if the same **text** at IoU >= agreementIou
   exists among earlier accepted glyph-eligible words or independent pending
   words. With a medium Freeze artifact and small live reads:
   - Small agrees: the word is confirmed immediately. Cross-model agreement is
     silently treated as independent confirmation.
   - Small disagrees (`heart` vs `heat`): the first covering read retires the
     medium word to original ink, and a second distinct small read then confirms
     `heat`. **A correct word degrades on unchanged pixels within two advancing
     reads.**
6. **Pacing.** For advancing media `mediaRecognitionIntervalMs` = 350 ms between
   full submissions, probes run every 80 ms and refresh is 1500 ms. For live
   camera, submission needs >= 1 stable probe (similarity >= 0.92) and an idle
   worker, with no 350 ms floor. New words need a second distinct acquisition
   with an unequal digest (advancing) or a later live capture.
7. **UIA.** Identical-text lines rebind silently at IoU >= 0.5. A changed line
   revokes its old interface synchronously, so a reader holding that range gets
   `UIA_E_ELEMENTNOTAVAILABLE`. The first content after a revocation publishes at
   once.
8. **Precision scope.** The local BF16 selector qualifies only medium/medium, N8,
   fixed1024, with one GPU candidate. The public/precision schemas bind 18,710
   classes ("No smaller dictionary, sorting or new output ABI is enabled"), and
   the public corpus catalog is empty. Tiny's production default TensorRT capacity
   is **N1** (`DefaultTensorRtBatchCapacity`), so an N8 tiny arm is a capacity
   that has never been quality-accepted.
9. **Probe verifier limits.** The character baseline found high false-accept
   rates for single-character edits (for example tracking/line: 108/108 edited,
   no jitter). Covering full reads are the backstop for real content changes, so
   a design must not stop fast full reads from retiring stale words.

## 3. Latency model [I]

These are inferences from measured worker p50 plus source pacing. They ignore
probe, admission, paint and UIA costs and must be replaced by trace measurements.

- **Advancing, new text:** first glyph is about `max(350, W) + W`.
  - Book: 470 / 428 / 398 ms (FP32 / BF16 / small).
  - Dense: 900 / 625 / 544 ms.
  - BF16 already captures 77% (dense) and 58% (book) of the FP32-to-small gain.
    Small saves only about 81 / 30 ms beyond BF16.
  - BF16 dense p95 (321 ms) is still under the 350 ms floor.
- **Live camera, stable:** the chain contains W twice, plus gating. Small saves
  about 62 ms (book) or 162 ms (dense) versus BF16, before motion resets the
  stability gate. This is the only case with a material gain, and live camera
  frames are also the hardest text for a weaker model.
- **Freeze/Pause:** one read plus in-flight drain. BF16 p50 is 78 / 275 ms
  (p95 89 / 321). The fast model would save 31 to 81 ms on an action the user
  makes in order to read for seconds.

## 4. Risk assessment for low-vision use

- A crisp wrong glyph is worse than original ink. The user cannot cheaply check
  it against the blurred source. The 0.7 confidence threshold is not calibrated
  [S]. Small's book errors carried high raw confidence (`heat` .968, `thre` .999,
  `prosection` .956) with candidate disposition, so confidence routing cannot
  catch them [M].
- Two-read agreement is consistency, not truth [S]. Each model is
  byte-deterministic on repeated input [M]. Near-identical frames will likely
  repeat systematic errors [I]. This must be measured (E3).
- **Mode-transition flips:** the same static word can be spelled differently live
  and frozen, and after Resume it can degrade (finding 5). That looks like the
  content changed, and it erodes trust in both modes.
- **Primary-use risk:** if live reading (lecture playback, menus or labels while
  holding the camera) is a primary mode for the owner, a weaker live model
  degrades a core use, not just navigation [I]. Owner question.
- **Tiny:** a capability-restricted model, not a cheaper copy. With tiny live
  and medium Freeze, Japanese and most CJK lines would flip from wrong/absent to
  readable on Freeze.

## 5. Better use of a second model to evaluate: verifier on Freeze

[M] This was computed read-only for this review: one image, alignment-based,
pre-admission, case-insensitive. Medium BF16 is authoritative and small is the
verifier; only agreeing candidates are painted.

| Rule | Correct kept | Wrong kept | Wrong share | Correct dropped | Wrong removed |
|---|---:|---:|---:|---:|---:|
| Baseline (BF16 only) | 288 | 96 | 25.0% | 0 | 0 |
| Strict: both candidates, same text | 241 | 23 | 8.7% | 47 | 73 |
| Lenient: small missing = no vote | 260 | 54 | 17.2% | 28 | 42 |

Shared errors such as `not`->`no` survive any agreement rule. The verifier pass
could reuse the same crops on the same exact image. The 2026-10-20 frozen-crop
recognizer-only small N8 dense time was 71.51 ms, from a different run; do not
combine numbers across runs.

This option needs an owner decision on the comparison policy. The Standalone
`Will`/`will` handoff deliberately stopped fallback/preferred disagreement from
creating conflict masks, and in-app agreement is currently exact text.

## 6. Minimal reviewable design (proposal only)

**D0 (no switching).** Use medium BF16 as the single GPU recognizer under the
existing owner-approved local opt-in. Measure end-to-end latency with
`OKUFLOW_UCT_TIMING_TRACE` (see E4). Stop if the owner finds it adequate.

**D1 (only if section 7 gates pass): "never-downgrade dual-role".**

- **Resources:** one scheduler, worker, TensorRT State and stream. Share the FP32
  medium detector, plus two resident recognizer contexts: `Fast` and
  `Authoritative`. Do not use two schedulers (two authority domains, and it breaks
  the "no second OCR worker" invariant). Do not use `ReplaceRecognizerFactory`.
- **Role per job, chosen by mode:**
  - live camera, advancing and regional reads use `Fast`;
  - the Freeze/Pause Standalone read and paused detail use `Authoritative`.
  - User model selectors are never silently rewritten.
- **Result identity:** each result carries a numeric role and artifact identity.
  A `Fast` result can never be Standalone authority. Whether cross-role agreement
  counts as two-read confirmation is an explicit, tested rule, not today's silent
  behaviour. Do not overload `backendPreparing`.
- **Freeze:** keep today's clean cut. `Invalidate` leaves original ink until the
  `Authoritative` standalone read paints. No provisional fast text in v1. This
  avoids a visible swap and lost NVDA positions.
- **Optional later Freeze change:** a provisional display hold, only for an
  exact-held-frame probe-validated overlay. Reuse the `immutableFullPreparing_`
  swap shape with an explicit provisional-role flag.
- **Resume:** if a `Fast` read agrees, the `Authoritative` glyph stays. If it
  disagrees, the word goes to original ink and is never replaced by the fast
  spelling. A fast spelling becomes eligible only after the word has been gone
  (pixel change or expiry). Covering full reads must still retire words: probes
  miss single-character edits (finding 9).
- **Failure:** a fault in either context quarantines the shared State and
  follows normal failover. A fast result is never relabelled as authoritative.
  The status names the model per mode.
- **Approval:** each `Fast` artifact is separately owner-approved and opt-in. It
  stays separate from the public BF16 catalog and golden path, and nothing is
  enabled silently.

## 7. Approval gates (owner sets thresholds)

- **G1 Vocabulary:** the `Fast` dictionary covers every script declared for live
  use. Tiny is excluded for Japanese and most CJK by construction.
- **G2 Painted safety:** for each content class, painted-wrong per 1000 painted
  words must be <= medium BF16's rate + an owner margin. Use production
  admission/paint replay, not raw WER.
- **G3 Critical tokens:** zero additional sensitive-token painted errors versus
  BF16. The set covers negation, digits, decimals, signs, inequalities, O/0,
  I/l/1 and Turkish `İ I ı i`. This is the same go/no-go that blocked small for
  `O0`->`00`.
- **G4 Benefit:** trace-measured first-glyph improvement in the target mode at
  both p50 and p95 under representative decode/present load, versus BF16 (not
  FP32).
- **G5 Stability:** zero authoritative-correct painted words that become fast
  spelling on unchanged pixels across Freeze and Resume.
- **G6 Residency:** both contexts resident, with no cold load at Freeze. Measure
  VRAM and startup deltas on every target GPU, plus first-Freeze latency after a
  long live period.
- **G7 Identity:** explicit approval of graph, plan, dictionary hash and output
  ABI (6,906 vs 18,710), and a visible per-mode status.

## 8. Minimum experiments

- **E1 (already authorized separately).** Run the small-BF16 and tiny-BF16 worker
  pilots with the same executable, detector and protocol.
  - The `0d0b932` seam requires N8 for tiny, but tiny's production default is
    N1. Label tiny N8 "never quality-accepted capacity". Before tiny N8 informs
    any product decision, it needs the same N8-vs-N1 tolerance acceptance that
    small and medium passed.
  - Record startup-first-result and dictionary/output ABI.
  - Do not extrapolate from E1 to quality.
- **E2 Independent truth beyond one photo.**
  - Use dense frame 55, the menu, and the existing symbols/accents fixture.
  - Add owner-representative live frames: handheld camera, lecture video, a label
    or menu.
  - Transcripts must be made without seeing any OCR output.
  - Add a Japanese/CJK sample only if tiny is ever proposed outside Latin text.
- **E3 Score what is painted.**
  - Replay outputs through production admission and paint: Standalone for
    Freeze, consecutive distinct frames for advancing.
  - Classify outcomes as painted-correct, painted-wrong, original ink and
    sensitive-token outcome.
  - Measure the rate of wrong words confirmed after two reads for each fast
    model.
  - Recompute the section 5 verifier table on E2.
- **E4 End-to-end timing.** Use the integrated-app trace, n >= 20, p50/p95,
  recording:
  - Freeze -> first `OverlayPainted`;
  - advancing new text -> first painted;
  - live -> first painted.
- **E5 Residency.** Measure VRAM and startup with both contexts. Record
  first-Freeze-after-idle latency.
- **E6 Owner review artifact.** Make it low-vision accessible: enlarged source
  crop per word, each model's text, painted vs original classification and
  sensitive flags, keyboard and screen-reader navigable. Extend the existing
  `book-model-word-review.html`.

## 9. Open owner questions

1. Is reading in live/advancing mode a primary use, or is Freeze the reading act?
2. During the authoritative Freeze read, original ink, or provisional fast text
   followed by a swap?
3. Which scripts must live mode support (tiny excludes Japanese)?
4. Is a verifier rule (section 5) acceptable, given its coverage loss and the
   `Will`/`will` precedent?

## Evidence paths

- Pilot: `Z:/Projects/OkuFlow/local-work-data/uct-private-model-pilot-20261023/pilot-f80/C2-20261023-172541-eea502ed5c1042b08b0e29c9b9545014/`
  (`pilot-summary.json`, `book-ground-truth-score.json`,
  `book-spatial-word-comparison.json`, `visit-*-raw/`).
- Source image: `C:/Users/USER/AppData/Local/OkuFlow/uct-paused-geometry-20261021/frames/book_001.png`.
- Pinned dictionaries: `%LOCALAPPDATA%/OkuFlow/ocr-runtime/rec{,/medium,/tiny}/inference.yml`.
- Code: `src/app/ultra_clear_text_controller.cpp` (`ReplaceRecognizerFactory`,
  `ApplyEnabled`, `SetImmutableScene`, `Invalidate`, `ResumeImmutableMedia`,
  `HandleProbeOutcome`, `HandleImmutableResult`), `src/common/ultra_clear_text.cpp`
  (`AdmitImmutable` preferred handoff, `AdmitImpl` temporal agreement),
  `src/common/uct_paddle.cpp` (`AutomaticRecognizer`),
  `include/okuflow/common/uct_model_settings.hpp`.
