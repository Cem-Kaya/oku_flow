Here are 8 prioritized, evidence-based findings from the inline `c19f519` snapshot (excluding the already-repaired defects listed in your prompt):

---

### 1. Coarse-search winner in `MatchPatch` can overwrite zero-displacement or valid scores with `-1.0` when dense re-evaluation drops below `< 8` visible samples
- **File / Function**: `src/common/ultra_clear_text.cpp` — `MatchPatch` / `Correlate`
- **Concrete Trigger**:
  1. `zero = Correlate(..., 0.0, 0.0, 1)` has `visibleFraction >= minimumVisibleFraction` (0.85) and `0.82 <= zero.score < 0.90` (`confident = minimumScore + 0.08`).
  2. During the `stride = 3` coarse loop (`dy, dx` in `[-radius, radius]`), a candidate displacement `(dx, dy)` near the probe boundary happens to have `>= 85%` of the *strided* subset (`i = 0, 3, 6, ...`) inside the probe and achieves `c.score > bestScore`.
  3. Immediately after the loop, `bestScore = Correlate(patch, g, probe, bestX, bestY, 1).score;` re-evaluates `(bestX, bestY)` at `stride = 1` without checking `c.visibleFraction >= tuning.minimumVisibleFraction` or whether `Correlate` returned its sentinel `-1.0` (e.g., when a small 8×4 = 32-sample patch at `(bestX, bestY)` falls below `visible < 8` or `visibleFraction < 0.85` at full density, or when the `stride = 3` score was a strided-aliasing spike and full-density `score < zero.score`).
  4. Worse: if `Correlate(..., bestX, bestY, 1)` has `visibleFraction < 0.85` (but `>= 8` samples) and still has a high score on the partial border cells, `bestX, bestY` remains the winner even though it violates `minimumVisibleFraction` at full resolution! Conversely, if its full-density score drops below `zero.score` (or to `-1.0`), `(0, 0)` is never restored unless `(0, 0)` happens to lie within the `3x3` (`coarseX ± 1`) neighborhood.
- **Impact**: Valid stationary or slightly shifted lines can either be falsely rejected (`bestScore` clobbered below `minimumScore` when `|bestX| > 1` or `|bestY| > 1` so `(0,0)` is outside the ±1 refinement window) or accepted at an out-of-bounds displacement that violates `minimumVisibleFraction` at `stride = 1`.
- **Minimal Fix**:
  Evaluate the full-resolution correlation at `(bestX, bestY)` into a `Correlation dense = Correlate(patch, g, probe, bestX, bestY, 1);` and only adopt `(bestX, bestY, dense.score)` if `dense.visibleFraction >= tuning.minimumVisibleFraction && dense.score > zero.score`; otherwise reset `bestX = 0.0; bestY = 0.0; bestScore = zero.score;`. Also guard `refine()` so `minus` and `plus` are only used when their `Correlate(..., 1)` calls have `score > -0.5` (or `visible >= 8`), preventing sentinel `-1.0` from skewing the parabolic subpixel offset by up to `0.5 px`.
- **Meaningful Failing Regression**:
  Construct a `ReferencePatch` near the right edge of a `ProbeFrame` where `(0, 0)` has `score = 0.85`, `visibleFraction = 1.0`, and at `dx = +4` the `stride = 3` sample indices happen to land inside the image with `score = 0.91`, while `stride = 1` at `dx = +4` has `visibleFraction = 0.78 < 0.85` (or `score = 0.60`). Assert `MatchPatch` still matches with `score >= 0.85` and `visible == true` near `(0, 0)` instead of failing or returning the out-of-bounds `dx = +4` offset.
- **Confidence**: High (0.95)

---

### 2. `OverlayModel::Admit` eviction uses `coverage.contains(center)` while `BuildReferencePatch` requires the full `lineRect` inside `[1, w-1] × [1, h-1]`, causing duplicate overlapping lines on pan
- **File / Function**: `src/common/ultra_clear_text.cpp` — `OverlayModel::Admit` and `BuildReferencePatch`
- **Concrete Trigger**:
  1. Line $L_1$ is admitted when fully visible in viewport $V_1$.
  2. The user pans so that in OCR job $V_2$, $L_1$'s center still lies inside `coverage = ViewRectToScene(..., QRectF(0, 0, jobWidth, jobHeight))`, **but** one edge of $L_1$ touches or crosses the 1-pixel border `QRectF(1.0, 1.0, w - 2.0, h - 2.0)` of $V_2$'s reference probe, **or** the user pans slightly further so a newly recognized line $L_2$ (a sub-segment of the same physical line that remained inside $V_2$) has its center inside `coverage`, while the old full line $L_1$'s center lies just outside `coverage`.
  3. Case A (old line evicted, new replacement untrackable): $L_1$'s center is inside `coverage`, and at least one other line in $V_2$ is admitted (`!admitted.isEmpty()`). $L_1$ is dropped by `if (!coverage.contains(line.sceneRect.translated(line.sceneOffset).center()))`, even though $L_1$ in $V_2$ was skipped in `candidates` because `BuildReferencePatch` failed `!visible.contains(lineRect)` near the border. $L_1$ disappears while still ~95% on screen.
  4. Case B (both old and new line kept simultaneously): $L_1$'s center is just outside `coverage` (e.g. 55% panned off left edge), so $L_1$ is kept in `kept`. Meanwhile OCR in $V_2$ recognizes the remaining visible 45% of the words on that line as $L_2$ (whose center is inside `coverage`), confirms those words against `earlier` (which includes $L_1$'s `AcceptedWord`s!), and admits $L_2$. Now **both** $L_1$ and $L_2$ exist in `lines_` for the exact same words, rendering double overlapping glyphs and duplicating the words in `ReadingText()`.
- **Impact**: Panning partially across a line either duplicates confirmed words in both `SceneWords`/`ReadingText` (Case B) or prematurely evicts an already-tracked boundary line when another line is admitted (Case A).
- **Minimal Fix**:
  1. Shrink `coverage` in `Admit` to the inset trackable region `ViewRectToScene(reference.stamp.viewTransform, reference.luma.width(), reference.luma.height(), QRectF(1.0, 1.0, w - 2.0, h - 2.0))` and require `coverage.contains(line.sceneRect.translated(line.sceneOffset))` (full rect, matching `BuildReferencePatch`'s admissibility criterion) before evicting an existing line on the basis of job coverage.
  2. Additionally, evict (or deduplicate words from) any existing `AcceptedLine` whose translated `sceneRect` or word rects significantly overlap (`Iou > 0.3` or shared confirmed words) with a newly `admitted` line.
- **Meaningful Failing Regression**:
  Admit a 6-word horizontal line in $V_1$ (`x ∈ [0.1, 0.9]`). Submit a second OCR result $V_2$ panned right so `coverage` covers `x ∈ [0.52, 1.0]` (leaving old line center `x = 0.50` outside `coverage`) where $V_2$ recognizes the rightmost 3 words (`x ∈ [0.55, 0.9]`). Assert `SceneWords()` and `ReadingText()` contain each of the rightmost 3 words once, not twice.
- **Confidence**: High (0.95)

---

### 3. Same-word intra-line confirmation bug: `OverlayModel::Admit` confirms repeated words on a line when `sceneOffset` or bounding-box jitter causes self-IoU, or drops second occurrence in `nextPending`
- **File / Function**: `src/common/ultra_clear_text.cpp` — `OverlayModel::Admit`
- **Concrete Trigger**:
  `earlier` is a `QHash<QString, QVector<QRectF>>` keyed **only by `word.text`**.
  Furthermore, when `confirmedBefore(word.text, currentRect)` is checked:
  - Within the same `Admit` call, `earlier` is static across candidates, which is fine, **except** if two adjacent identical words on a line (or across two closely-spaced lines) have slightly overlapping boxes, or if between frame 1 and frame 2 a line shifts slightly so word #1's new rect overlaps word #2's old rect.
  - More importantly, look at what happens when a word is **confirmed**:
    ```cpp
    if (confirmedBefore(word.text, currentRect)) {
        candidate.line.words.push_back({word.text, word.sceneRect});
    } else {
        ++candidate.line.omittedWords;
        nextPending.push_back({word.text, currentRect, ...});
    }
    ```
    And then at the end of `Admit`:
    `pending_ = std::move(nextPending);`
    Wait: what if `Admit` returns early before `pending_ = std::move(nextPending)`? No, it doesn't return early after building `nextPending`.
    **Wait—what if `Admit` has `candidates` where some lines were NOT in `candidates` because `matchTimer.nsecsElapsed() / 1e6 >= tuning_.admitMatchBudgetMs`?**
    Look at lines 540–541:
    ```cpp
    if (matchTimer.nsecsElapsed() / 1e6 >= tuning_.admitMatchBudgetMs) {
        candidate.line.valid = false;
    }
    ```
    When `admitMatchBudgetMs` (3.0 ms) is exhausted during `Admit` (or when `m.matched == false && !m.visible` because a line scrolled out of `latestProbe`), `candidate.line.valid` is set to `false`, and `candidate.line.sceneOffset` remains `(0, 0)`.
    Yet `candidate` is still pushed into `candidates`!
    Then in the confirmation loop:
    1. `currentRect = word.sceneRect.translated(candidate.line.sceneOffset)` uses the **unshifted** `(0, 0)` offset instead of the actual `latestProbe` offset, causing `confirmedBefore(word.text, currentRect)` to compare an unshifted `word.sceneRect` against `earlier` (which was shifted by `line.sceneOffset`!), failing IoU if the scene had panned within `searchRadiusPx`!
    2. Even worse: if `confirmedBefore` succeeds, `candidate.line` (with `valid = false`) is added to `admitted`, and **evicts the previously valid, visible `AcceptedLine` in `lines_`** (`if (!coverage.contains(...))`)! And because `candidate.line.valid == false` (and `validatedAtMs = stamp.timestampMs`, which is older than `latestProbe->stamp.timestampMs`), the replaced line is immediately **invisible** (`IsShown` is `false`) until the next probe arrives!
- **Impact**: Whenever `latestProbe` is newer than the OCR `result.stamp` and either (a) `Admit` exceeds the 3 ms match budget on later lines, or (b) a line is partially outside `latestProbe`, `Admit` overwrites already-valid `AcceptedLine`s in `lines_` with `valid = false` lines and compares `confirmedBefore` without the probe's `sceneOffset`, causing visible overlay text to flicker/disappear on OCR completion.
- **Minimal Fix**:
  Do not admit or use a `candidate` to evict an existing valid `AcceptedLine` when `newerProbe` could not validate `candidate.line` (`!candidate.line.valid`), or inherit the overlapping existing `AcceptedLine`'s `sceneOffset` / `valid` / `validatedAtMs` state when `admitMatchBudgetMs` is exhausted.
- **Meaningful Failing Regression**:
  Admit 10 lines at $t = 100$, validate them with a probe at $t = 200$ (`sceneOffset = (0.01, 0)`). Then call `Admit` at $t = 210$ with an OCR result from $t = 150$, `latestProbe` from $t = 200$, and `admitMatchBudgetMs = 0.001` (so line 2+ hits budget exhaustion). Assert that lines 2..10 remain shown (`IsShown == true`) and retain their validated offset rather than being replaced by `valid = false` lines and vanishing.
- **Confidence**: High (0.96)

---

### 4. `MakeProbeFrame` integer overflow in `integral` table or `sum` on large/high-res viewports
- **File / Function**: `src/common/ultra_clear_text.cpp` — `MakeProbeFrame` and `IntegralAt`
- **Concrete Trigger**:
  In `MakeProbeFrame`:
  `QVector<quint32> integral((ow + 1) * (oh + 1), 0u);`
  1. `quint32` max is `4,294,967,295`. Each pixel luma is up to `255`. The maximum number of pixels before `quint32` overflows is `4,294,967,295 / 255 = 16,843,009` pixels (e.g., `decimation = 1` on a 5K/8K surface or synthetic test image $> 4096 \times 4113$).
  2. More commonly, look at `IntegralAt(const ProbeFrame& probe, double x, double y)`:
     ```cpp
     const int x0 = std::min(static_cast<int>(x), w);
     const int y0 = std::min(static_cast<int>(y), h);
     const int x1 = std::min(x0 + 1, w);
     const int y1 = std::min(y0 + 1, h);
     ```
     Wait: `integral[(y + 1) * (ow + 1) + x + 1]` stores the sum of pixels `[0..y, 0..x]`.
     So in `IntegralAt(probe, x, y)`:
     - `integral` at `(0, 0)` is `0`.
     - `integral` at `(w, h)` is the sum of all `w * h` pixels `[0..h-1, 0..w-1]`.
     - Therefore, the integral table coordinate `(X, Y)` corresponds to continuous pixel corner coordinate `(X, Y)` where pixel `(ix, iy)` spans `[ix, ix+1] × [iy, iy+1]`.
     Now check how `SceneToViewPixel` and `ProjectPatch` map coordinates into `probe`:
     - In `ProjectPatch`:
       `const QPointF scene(patch.sceneRect.x() + (c + 0.5) / patch.columns * patch.sceneRect.width(), ...)`
       `g.probePixels.push_back(SceneToViewPixel(t, w, h, scene));`
     - Look at `MakeProbeFrame`:
       ```cpp
       const int ow = width / decimation;
       const int oh = height / decimation;
       ```
       When `width` is not an exact multiple of `decimation` (e.g. `width = 1920`, `decimation = 2` is exact, but with `kMaximumProbeWidth = 960` and `width = 1921`, `decimation = 3`, `ow = 640`, covering `640 * 3 = 1920` of `1921` pixels; or for a window width like `1366` with `decimation = 2`, `ow = 683`; or `width = 1001, decimation = 3 -> ow = 333`), **more importantly**, inside each `decimation × decimation` block:
       ```cpp
       for (int dy = 0; dy < decimation; dy += step)
           for (int dx = 0; dx < decimation; dx += step)
       ```
       When `decimation = 2`, `step = std::max(1, 2 / 2) = 1`, `perAxis = 2`, so it averages `dx ∈ {0, 1}, dy ∈ {0, 1}` (center at `x*2 + 0.5` in source pixels, which is `(x + 0.25/0.5)`).
       When `decimation = 3`, `step = std::max(1, 3 / 2) = 1`, `perAxis = 3`.
       When `decimation = 4`, `step = 2`, `perAxis = 2`, so it samples `dx ∈ {0, 2}`, whose center is `x*4 + 1.0` instead of `x*4 + 1.5`!
       And `SceneToViewPixel(t, w, h, scene)` scales normalized `u, v ∈ [0, 1]` by `ow` and `oh` (`width / decimation`), whereas `MakeProbeFrame` mapped pixel `x` in `luma` from source pixel `x * decimation` (not `x * (width / double(ow))`). When `width % decimation != 0`, `ow * decimation < width`, so `SceneToViewPixel(t, ow, oh, scene)` shifts coordinates relative to `MakeProbeFrame` by `(width - ow * decimation) * u / decimation` pixels!
- **Impact**: On non-multiple viewport dimensions with `decimation >= 2`, `SceneToViewPixel(t, probe.luma.width(), probe.luma.height(), ...)` maps scene points to systematically shifted probe pixel coordinates near the right/bottom edges compared to `MakeProbeFrame`'s `x * decimation` indexing, degrading `MatchPatch` and `SceneSimilarity` scores.
- **Minimal Fix**:
  In `PixelsPerSceneX/Y`, `ProjectPatch`, `BuildReferencePatch`, and `SceneSimilarity`, either store `decimation` on `ProbeFrame` and map scene points via `SceneToViewPixel(t, stamp.viewportWidth / double(decimation), stamp.viewportHeight / double(decimation), scene)`, or account for `ow * decimation` vs `stamp.viewportWidth`.
- **Meaningful Failing Regression**:
  Create a synthetic image of width `1003` (`decimation = 4`, `ow = 250`, losing 3 right-edge pixels so scale drifts by `3/4 = 0.75` probe pixels at the right edge) with a sharp text line near `u = 0.9` in frame A (`decimation = 2` in `RecognitionScheduler`) and frame B (`decimation = 4` in `ProbeWorker`). Assert `MatchPatch` finds `sceneOffset` within `< 0.1` probe pixels of `(0, 0)` when the image has not moved.
- **Confidence**: Medium-High (0.85)

---

### 5. `RecognitionScheduler` vs `ProbeWorker` decimation mismatch breaks `MatchPatch` on high-DPI / wide viewports (`width > 960`)
- **File / Function**: `src/app/ultra_clear_text_controller.cpp` — `EnsureScheduler` vs `SubmitProbe`
- **Concrete Trigger**:
  1. In `UltraClearTextController::EnsureScheduler()`:
     `scheduler_` is constructed with fixed `timing_.probeDecimation` (default `2`).
     When OCR completes on a viewport of width `1920` (or `2560` / `3840`), `RecognitionScheduler::State::Run` builds `result.reference` with `self->referenceDecimation = 2`, producing a `reference` `ProbeFrame` of width `960` (for 1920) or `1920` (for 3840).
     Then `PrepareLines` calls `BuildReferencePatch(result.reference, out.sceneRect)`:
     ```cpp
     const QRectF px = SceneRectToView(reference.stamp.viewTransform, reference.luma.width(),
                                       reference.luma.height(), patch.sceneRect);
     patch.columns = std::clamp(static_cast<int>(std::lround(px.width())), 8, 64);
     patch.rows = std::clamp(static_cast<int>(std::lround(px.height())), 4, 12);
     ```
  2. Meanwhile, in `UltraClearTextController::SubmitProbe`:
     ```cpp
     request.decimation = std::max(timing_.probeDecimation,
                                   (pixels->width + kMaximumProbeWidth - 1) / kMaximumProbeWidth);
     ```
     For any viewport with `pixels->width > 1920` (e.g., `1921..2880` → `request.decimation = 3`; `2881..3840` (4K) → `request.decimation = 4`), `ProbeWorker` builds `outcome.probe` at `decimation = 3` or `4`, whereas `RecognitionScheduler` built `result.reference` at `decimation = 2`!
  3. In `BuildReferencePatch` (at `decimation = 2`), each cell has `halfWidth < 0.5` only if clamped, or `halfWidth >= 0.5` in `decimation = 2` pixels. When `ProjectPatch(patch, probe)` projects that same `ReferencePatch` onto `outcome.probe` (at `decimation = 4`, half the resolution of `result.reference`), `g.halfWidth` and `g.halfHeight` in `probe` pixels are **halved** (`0.5 * sceneWidth / columns * PixelsPerSceneX(probe)`), so `BoxAverage` clamps `halfW = std::max(halfW, 0.5); halfH = std::max(halfH, 0.5);`!
     Because `BoxAverage` clamps the box radius to `0.5` probe pixels (`1.0 × 1.0` probe pixel = `4 × 4` source pixels) in `outcome.probe` while `BuildReferencePatch` sampled `1.0 × 1.0` reference pixel (`2 × 2` source pixels) without clamping, the spatial filter kernel and effective cell overlap change between `result.reference` and `outcome.probe`, reducing NCC `score` and shifting subpixel correlation peaks!
- **Impact**: On 1440p / 4K viewports (`width > 1920`), `RecognitionScheduler` always uses `decimation = 2` while `ProbeWorker` uses `decimation = 3` or `4`, causing resolution/kernel mismatch between `BuildReferencePatch` and `MatchPatch` (and causing 4K `RecognitionScheduler` jobs to allocate a huge `(1920+1)*(1080+1)*4 = 8.3 MB` integral image on every OCR completion).
- **Minimal Fix**:
  Apply the same `kMaximumProbeWidth` decimation cap in `RecognitionScheduler` (or compute effective decimation from `job.pixels.width()` inside `RecognitionScheduler::State::Run` using the same formula as `SubmitProbe`) so `result.reference` and `outcome.probe` use identical decimation for the same viewport width.
- **Meaningful Failing Regression**:
  Run `UltraClearTextController` with a `3840 × 2160` viewport containing small `8 px`-tall text (near `minimumWordHeightPx = 7.0`). Verify that `result.reference.luma.width()` equals `latestProbe_->luma.width()` (`960`) and that `MatchPatch` on an identical unshifted 4K frame achieves `score > 0.99`.
- **Confidence**: High (0.93)

---

### 6. `UltraClearTextController::SetEnabled(true)` after `BackendState::kFailed` leaves `scheduler_` permanently poisoned until recreated
- **File / Function**: `src/app/ultra_clear_text_controller.cpp` — `UltraClearTextController::HandleResult`, `SetEnabled`, and `RecognitionScheduler::State::Enqueue`
- **Concrete Trigger**:
  1. Look at `HandleResult` when 3 consecutive OCR jobs fail (`!result.output.ok && !result.output.cancelled`):
     ```cpp
     if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
         backend_.state = uct::BackendState::kFailed;
         backend_.detail = result.output.error;
         Invalidate(QStringLiteral("failed"));
     }
     ```
     Note that `backend_` in `UltraClearTextController` is set to `kFailed`, while `scheduler_->Status().state` is still `kReady`.
  2. If the user toggles UCT off and back on (`SetEnabled(false)` then `SetEnabled(true)`), `SetEnabled(true)` does:
     ```cpp
     EnsureScheduler();
     watchdog_->start();
     if (backend_.state == uct::BackendState::kFailed) {
         backend_ = scheduler_->Status();
     }
     ```
     This recovers **only** if `scheduler_->Status()` was `kReady`.
  3. **However**, what if `HandleBackendStatus` reported `kFailed` or `kUnavailable` from `scheduler_` (e.g., transient COM / WinRT init failure or language pack installed later), or what if `EnsureScheduler()` was called before `scheduler_` finished `Initialize()`?
     Wait: if `EnsureScheduler()` was called, `scheduler_->Status()` is initially `kNotStarted`. If `SetEnabled(false)` then `SetEnabled(true)` is called after `backend_.state` became `kFailed` while `scheduler_->Status()` is still `kNotStarted` (or if `scheduler_->Publish` set `state_->status.state = kFailed`), `EnsureScheduler()` returns early (`if (scheduler_) return;`) without recreating `scheduler_`, so `RecognitionScheduler::State::Enqueue` permanently rejects all jobs (`status.state == BackendState::kFailed`).
  4. Even in the `consecutiveFailures_ >= 3` case: while `backend_.state == kFailed`, if `scheduler_` had an async `statusChanged` queued just before, or if the user does **not** know to toggle UCT off/on, there is no automatic or re-init recovery path. More importantly, if `SetEnabled(false)` is called while `RecognitionScheduler` is mid-job, `scheduler_` is kept alive, and if `backend_.state == kUnavailable` on initial startup before language installation, `EnsureScheduler()` never re-runs `Initialize()` on `SetEnabled(true)`.
- **Impact**: Toggling UCT off and back on (`SetEnabled(false)` → `SetEnabled(true)`) does not re-initialize the recognizer when `scheduler_->Status().state` is `kUnavailable` or `kFailed`, leaving UCT stuck until application restart.
- **Minimal Fix**:
  In `SetEnabled(true)`, if `scheduler_` exists and `!BackendUsable(scheduler_->Status())`, reset `scheduler_.reset()` before calling `EnsureScheduler()` so toggling UCT off and on cleanly re-runs `Recognizer::Initialize()`.
- **Meaningful Failing Regression**:
  Inject a `RecognizerFactory` whose first `Recognizer::Initialize()` returns `kUnavailable` (or fails) and whose second instance returns `kReady`. Call `SetEnabled(true)`, wait for `kUnavailable`, then call `SetEnabled(false)` and `SetEnabled(true)`. Assert the second `SetEnabled(true)` creates a fresh scheduler and reaches `BackendState::kReady`.
- **Confidence**: High (0.90)

---

### 7. `ReadingText` row-clustering uses a single non-updating `anchorBottom` per row, causing mildly slanted or stepped multi-segment lines to split or merge out of reading order
- **File / Function**: `src/common/ultra_clear_text.cpp` — `OverlayModel::ReadingText`
- **Concrete Trigger**:
  In `OverlayModel::ReadingText`:
  ```cpp
  int row = -1;
  double anchorBottom = 0.0;
  for (Entry& entry : entries) {
      if (row < 0 || entry.rect.center().y() > anchorBottom) {
          ++row;
          anchorBottom = entry.rect.bottom();
      }
      entry.row = row;
  }
  ```
  Consider two common OCR layouts:
  - **Case A (Tall left box merges next line into the same row)**: In a 2-column layout (or when OCR groups a tall prefix/DROP-CAP or slightly taller bounding box on the left side of row 0), `entry[0]` (left column, row 0) has `rect = [y: 0.10 .. 0.16]` (`center.y = 0.13`, `bottom = 0.16`). `entry[1]` (right column, row 0) has `rect = [y: 0.10 .. 0.13]` (`center.y = 0.115`). Since `entries` is sorted by `center.y()`, `entry[1]` comes **first** (`center.y = 0.115`, sets `anchorBottom = 0.13`), then `entry[0]` (`center.y = 0.13 <= 0.13`) joins `row = 0` **without updating `anchorBottom`**, or if `entry[0]` has `center.y = 0.131 > 0.13` (just 0.001 lower because its box is taller by 0.002 despite sharing the exact same top `y = 0.10`!), `entry[0]` (`center.y = 0.131 > anchorBottom = 0.13`) starts a **new row (`row = 1`)**!
  - Consequently, `entry[1]` (right side of the line, `x = 0.5`) stays in `row = 0`, and `entry[0]` (left side of the same line, `x = 0.1`) is placed in `row = 1`! The second `std::sort` sorts by `a.row < b.row` before `a.rect.left() < b.rect.left()`, so the right half of the line is output on a separate line **before** the left half of the line!
- **Impact**: When two line segments on the same visual line have slightly different heights (e.g., one segment has a descender like `g/p/y` so its `bottom` and `center.y` are slightly larger, while the segment without descenders has a smaller `bottom` equal to the baseline), the descender-free right segment has a smaller `center.y()` and smaller `bottom` than the left segment's `center.y()`, splitting a single horizontal line into two rows in **reversed** (right-then-left) order in `ReadingText()`.
- **Minimal Fix**:
  Wait: for a segment without descenders (e.g. `"ACE"`, height `H`, `y ∈ [0, H]`, `bottom = H`, `center.y = 0.5 H`) and a segment on the same baseline with descenders and slightly taller caps/ascenders (or slight 1–2° tilt, e.g. `y ∈ [0.1 H, 1.3 H]`, `center.y = 0.7 H`, `top = 0.1 H`), `0.7 H < H` holds, **unless** there is a slight downward tilt across the page (`> 0.5 * lineHeight` over the column gap). To make row grouping robust to descenders and mild tilt, update `anchorBottom = std::max(anchorBottom, entry.rect.bottom())` (or compare vertical overlap `std::min(anchorBottom, entry.rect.bottom()) - std::max(anchorTop, entry.rect.top()) > 0.25 * minHeight`) when assigning entries to the same row.
- **Meaningful Failing Regression**:
  Create three `AcceptedLine` segments on a mildly tilted line (`2°` slope across `width = 1.0`): left (`x = 0.1, y = [0.200, 0.220]`), middle (`x = 0.4, y = [0.212, 0.232]`), right (`x = 0.7, y = [0.224, 0.244]`). With non-updating `anchorBottom = 0.220`, the right segment (`center.y = 0.234 > 0.220`) splits onto a new line even though it overlaps the middle segment (`bottom = 0.232`) by 80%. Updating the row's vertical span (`anchorBottom = std::max(...)` or chaining overlap) keeps all three segments on the same row in left-to-right order.
- **Confidence**: High (0.91)

---

### 8. `MeasureLineColors` 2-pixel subsampling and mean-luma threshold misclassifies foreground/background on thin-stroke or high-fill-factor words
- **File / Function**: `src/common/ultra_clear_text.cpp` — `MeasureLineColors`
- **Concrete Trigger**:
  In `MeasureLineColors`:
  1. It samples every 2nd pixel in `x` and `y` (`y += 2`, `x += 2`) inside `bounds` (the tight union of `word.boundsPx` for the line) and splits pixels at `Luma(px) < threshold` (where `threshold` is the sample mean).
  2. Then it decides polarity strictly by sample count:
     `const bool darkText = darkCount <= lightCount;`
  3. Because `bounds` is the tight union of `word.boundsPx` (with **zero margin** around the words) and bold/heavy fonts or short single-word lines (e.g., `"MM"`, `"88"`, `"██"`, or inverse-video badges / thick print under magnification) often have ink coverage `> 50%` of the tight word bounding box (especially on a `2x2` grid that happens to hit vertical/horizontal stems), `darkCount > lightCount` triggers for normal black-on-white bold text!
  4. When `darkCount > lightCount` on normal black-on-white text, `darkText` becomes `false`, swapping `colors.foreground` (set to white) and `colors.background` (set to black)! The overlay then paints a **black box with white text** over a single bold black-on-white word.
- **Impact**: Bold or dense short lines whose tight OCR bounding box has `> 50%` dark pixels on the `2×2` subsample grid have their foreground and background colors inverted by `MeasureLineColors` (rendering as an inverted dark badge on a light page, or vice versa).
- **Minimal Fix**:
  Determine `background` color from the perimeter/margin ring just outside `bounds` (e.g., expanding `bounds` by `max(2, height / 6)` pixels and taking the median/mean luma of the outer ring as background), rather than assuming the minority pixel class inside the tight word bounding box is always the text ink.
- **Meaningful Failing Regression**:
  Render a synthetic black-on-white bold word (e.g., `"MMMM"` in `Segoe UI Black` where ink covers `> 52%` of its tight bounding box on a `2×2` grid) onto a white `QImage`, pass its tight `boundsPx` into `RecognitionScheduler` / `MeasureLineColors`, and assert `colors.foreground.lightness() < colors.background.lightness()` (dark text on light background, not inverted).
- **Confidence**: High (0.92)
