# UCT Argon 4b/5 — deterministic cross-decimation scratch fixtures (UNEXECUTED)

Review-subagent deliverable. Nothing here was compiled or run; the parent
compiles/runs and decides. No live-checkout file was edited. Checkout inspected
read-only: `C:\Users\USER\AppData\Local\OkuFlow\worktrees\test-opt`.

## What is being measured (static facts, re-verified in current source)

- `MakeProbeFrame` (`src/common/ultra_clear_text.cpp:421`): `ow = width / decimation`
  truncates; the rightmost `rem = width - ow*decimation` source columns are unsampled.
- Mapping treats the probe as spanning the full viewport: `ProjectPatch` (:89) and
  `SceneSimilarity` (:692) call `SceneToViewPixel(t, luma.width(), …)`, and
  `PixelsPerSceneX` (:69) uses `luma.width()`. So model position `p = u*ow` actually
  samples source `u*ow*d = u*(W-rem)`: a scale (not translation) error, zero at the
  left edge, maximal at the right edge.
- Predicted search offset when a patch built at `dR` is matched in a probe at `dP`,
  for a cell at normalized viewport position `u` (identity transform):

  `offsetProbePx(u) = u * (remP - remR) / dP`  (+ kernel-centroid term, below)

- Kernel (claim 5, :432–445): `step=max(1,d/2)`, samples `dx ∈ {0, step, …}`.
  d2 `{0,1}` centroid 0.5 = block centre; d3 `{0,1,2}` centroid 1.0 = centre;
  d4 `{0,2}` centroid 1.0 vs centre 1.5 → −0.5 source px common-mode term
  (position-independent; cancels in the right-minus-left double difference).
- Fast path (`MatchPatch` :527–532): if zero-offset score ≥ `minimumScore+0.08`
  (= 0.90 at design tuning) the search is skipped and `sceneOffset` stays exactly 0.
  An unshifted score > 0.9 therefore *hides* any sub-probe-pixel bias; offset 0 from
  the default-tuning call is not evidence of zero displacement. The snippets below
  run every measurement twice: design tuning (contract: matched, score ≥ 0.82) and a
  search-forced tuning (`minimumScore = 1.01`, so the confidence gate is unreachable
  and the bounded search always runs; `matched` is then false by construction and
  only `score`/`sceneOffset` are read). No production change is needed to measure.

## Production pairing reality check (do not over-claim the stress fixture)

Reference is always d2 (`RecognitionScheduler(…, timing_.probeDecimation=2)`,
controller :146). Probe is `max(2, ceil(W/960))` (controller :431, cap 960 at :46):

| viewport W | probe d | pairing |
|---|---|---|
| ≤1920 | 2 | ref2/probe2 (no cross-decimation) |
| 1921–2880 | 3 | ref2/probe3 (2560 fixture below is production-true) |
| 2881–3840 | 4 | ref2/probe4 |

The brief's 1003-wide d2-vs-d4 case is therefore a *stress* pairing (maximizes
`remP-remR = 2`), not a production pairing at that width. W=2883 ref2/probe4 has the
identical remainder pair (1 vs 3) and is production-true; it is included as an
optional data row only.

## Predicted magnitudes (formula above; identity transform, so scene px = source px)

| fixture | u (patch centre) | predicted offset |
|---|---|---|
| 1003 ref2/probe4, right line | ≈0.86 | +0.43 probe px ≈ +1.73 src px, plus −0.125 probe px kernel term |
| 1003 ref2/probe4, left line | ≈0.13 | +0.065 probe px, plus same kernel term |
| 1003 double difference (R−L) | — | ≈0.365 probe px (kernel cancels) |
| 2560 ref2/probe3, right line | ≈0.86 | +0.287 probe px ≈ +0.86 src px, no kernel term (d2/d3 centroids exact) |
| any, extreme right edge u=1 | 1.0 | 1003 case: 0.5 probe px ≈ 2.0 src px |

All predictions are **below the brief's 1-probe-pixel tolerance**; the open question
the run must answer is whether NCC score stays ≥ 0.82 and whether the measured
displacement is statistically distinguishable from the same-decimation controls.
The sign convention must be calibrated in-run by the known +3 src px shift control
(asserted first); the snippet asserts magnitudes and QINFOs signs.

## Snippet — paste into `tests/ultra_clear_text_tests.cpp`

Uses only existing fixture helpers (`ProbeOf` with its decimation argument,
`PageFont`, `RegisterFixtureFonts`, `ComputeViewTransform`). Actual rendered images
only: patches come from `BuildReferencePatch`, probes from `MakeProbeFrame`; no
hand-filled `ReferencePatch` values, no internal-resolution equality, no >0.99 score
assertions. Heights are chosen divisible by every decimation used, so Y has no
remainder term and serves as an internal zero control.

```cpp
// --- Cross-decimation scratch helpers (Argon 4b/5) -------------------------
// Identity transform throughout: scene size == viewport size, zoom 1, Fit.
// Then PixelsPerSceneX(probe) == probe.luma.width() and one normalized scene
// unit == W source pixels, which makes the conversions below exact.
struct EdgePage {
    QImage image;
    QRectF leftLine;  // normalized scene rects
    QRectF rightLine;
};

EdgePage MakeEdgePage(int w, int h, int pixelSize, int shiftX, const QString& text)
{
    RegisterFixtureFonts();
    EdgePage page;
    page.image = QImage(w, h, QImage::Format_ARGB32);
    page.image.fill(QColor(246, 244, 236));
    QPainter painter(&page.image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QFont font = PageFont(pixelSize);
    painter.setFont(font);
    painter.setPen(QColor(18, 18, 22));
    const QFontMetricsF m(font);
    const double adv = m.horizontalAdvance(text);
    const double baseline = h * 0.45;
    auto draw = [&](double x) {
        painter.drawText(QPointF(x, baseline), text);
        return QRectF(x / w, (baseline - m.ascent()) / h, adv / w,
                      (m.ascent() + m.descent()) / h);
    };
    page.leftLine = draw(12.0 + shiftX);
    page.rightLine = draw(w - 12.0 - adv + shiftX);
    painter.end();
    return page;
}

ViewTransform IdentityTransform(int w, int h)
{
    return ComputeViewTransform(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
}

double OffProbePxX(const PatchMatch& match, const ProbeFrame& probe)
{
    return match.sceneOffset.x() * probe.luma.width();
}
double OffSourcePxX(const PatchMatch& match, int w) { return match.sceneOffset.x() * w; }

// Design-tuning call measures the contract; search-forced call measures the
// offset the fast path may hide (minimumScore 1.01 => confidence gate 1.09
// can never short-circuit; read score/sceneOffset only, never `matched`).
struct Measured {
    PatchMatch design;
    PatchMatch forced;
    bool fastPathMasked{};
};
Measured Measure(const ReferencePatch& patch, const ProbeFrame& probe)
{
    Measured out;
    out.design = MatchPatch(patch, probe);
    MatchTuning forced;
    forced.minimumScore = 1.01;
    out.forced = MatchPatch(patch, probe, forced);
    out.fastPathMasked = out.design.score >= 0.90 && out.design.sceneOffset == QPointF(0, 0);
    return out;
}
```

Test slots (add declarations next to the other validation-primitive slots):

```cpp
void crossDecimationRightEdgeKeepsDesignScoreAndOffset_data();
void crossDecimationRightEdgeKeepsDesignScoreAndOffset();
void crossDecimationRemainderDisplacementIsMeasurable();
void crossDecimationSmallTextAtHighDpi_data();
void crossDecimationSmallTextAtHighDpi();
```

```cpp
void UltraClearTextTests::crossDecimationRightEdgeKeepsDesignScoreAndOffset_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("refD");
    QTest::addColumn<int>("probeD");
    QTest::newRow("1003-stress-ref2-probe4") << 1003 << 2 << 4;   // rem 1 vs 3
    QTest::newRow("1003-same-dec-ref2-probe2") << 1003 << 2 << 2; // control
    QTest::newRow("1003-same-dec-ref4-probe4") << 1003 << 4 << 4; // control
    QTest::newRow("1000-divisible-ref2-probe4") << 1000 << 2 << 4; // rem 0 vs 0
    // Optional, production-true twin of the stress row (probe d4 because W>2880):
    // QTest::newRow("2883-prod-ref2-probe4") << 2883 << 2 << 4;  // needs H % 4 == 0 row height below
}

void UltraClearTextTests::crossDecimationRightEdgeKeepsDesignScoreAndOffset()
{
    QFETCH(int, width);
    QFETCH(int, refD);
    QFETCH(int, probeD);
    constexpr int kH = 564; // divisible by 2 and 4: no Y remainder term
    const ViewTransform t = IdentityTransform(width, kH);
    const QString text = QStringLiteral("Reading printed pages");
    const EdgePage page = MakeEdgePage(width, kH, 28, 0, text);
    const EdgePage shifted = MakeEdgePage(width, kH, 28, 3, text); // +3 source px
    const EdgePage changed = MakeEdgePage(width, kH, 28, 0,
                                          QStringLiteral("Totally other words"));
    const ProbeFrame reference = ProbeOf(page.image, t, 1, 1, 1, 0, refD);
    QVERIFY(reference.IsValid());
    const ReferencePatch patch = BuildReferencePatch(reference, page.rightLine);
    QVERIFY2(patch.valid, "right-edge patch must be trackable at reference decimation");
    // Blank-region negative control (below the text band).
    QVERIFY(!BuildReferencePatch(reference, QRectF(0.40, 0.80, 0.20, 0.05)).valid);

    const ProbeFrame same = ProbeOf(page.image, t, 1, 1, 2, 10, probeD);
    const ProbeFrame moved = ProbeOf(shifted.image, t, 1, 1, 3, 20, probeD);
    const ProbeFrame other = ProbeOf(changed.image, t, 1, 1, 4, 30, probeD);
    QVERIFY(same.IsValid() && moved.IsValid() && other.IsValid());

    // Changed content must never validate, at any decimation pairing.
    QVERIFY(!MatchPatch(patch, other).matched);

    const Measured identical = Measure(patch, same);
    qInfo("W=%d refD=%d probeD=%d identical: design score %.4f off %.3f probePx "
          "(fastPathMasked=%d); forced score %.4f off %.3f probePx / %.2f srcPx",
          width, refD, probeD, identical.design.score,
          OffProbePxX(identical.design, same), identical.fastPathMasked,
          identical.forced.score, OffProbePxX(identical.forced, same),
          OffSourcePxX(identical.forced, width));
    QVERIFY(identical.design.visible);
    QVERIFY2(identical.design.matched && identical.design.score >= 0.82,
             "design contract: identical content across decimation must validate");
    QVERIFY2(std::abs(OffProbePxX(identical.forced, same)) <= 1.0,
             "forced-search offset must stay within one probe pixel");
    QVERIFY(std::abs(identical.forced.sceneOffset.y() * same.luma.height()) <= 0.5);

    const Measured shiftedM = Measure(patch, moved);
    QVERIFY2(shiftedM.design.matched && shiftedM.design.score >= 0.82,
             "+3 src px shift must still validate at design tuning");
    // Sign/magnitude calibration: differencing cancels any constant bias, so
    // the recovered shift must be +3 source px within one probe pixel.
    const double recoveredSrcPx =
        OffSourcePxX(shiftedM.forced, width) - OffSourcePxX(identical.forced, width);
    QVERIFY2(std::abs(recoveredSrcPx - 3.0) <= probeD,
             qPrintable(QStringLiteral("recovered shift %1 src px").arg(recoveredSrcPx)));
}

// The regression that may claim a real 4b displacement: right-minus-left
// double difference between cross- and same-decimation forced offsets.
// Kernel centroid and any renderer common-mode cancel; only the remainder
// scale term survives. Expected magnitude is computed from the actual rects.
void UltraClearTextTests::crossDecimationRemainderDisplacementIsMeasurable()
{
    constexpr int kW = 1003, kH = 564;
    const ViewTransform t = IdentityTransform(kW, kH);
    const EdgePage page = MakeEdgePage(kW, kH, 28, 0, QStringLiteral("Reading printed pages"));
    const ProbeFrame reference = ProbeOf(page.image, t, 1, 1, 1, 0, 2);
    const ProbeFrame probe4 = ProbeOf(page.image, t, 1, 1, 2, 10, 4);
    const ProbeFrame probe2 = ProbeOf(page.image, t, 1, 1, 3, 20, 2);
    auto forcedOff = [&](const QRectF& line, const ProbeFrame& probe,
                         const ProbeFrame& ref) {
        const ReferencePatch patch = BuildReferencePatch(ref, line);
        Q_ASSERT(patch.valid);
        MatchTuning forced;
        forced.minimumScore = 1.01;
        const PatchMatch m = MatchPatch(patch, probe, forced);
        // Normalize to d4 probe pixels so both pairings share one unit:
        // source px / 4.
        return m.sceneOffset.x() * kW / 4.0;
    };
    const double crossRL = forcedOff(page.rightLine, probe4, reference) -
                           forcedOff(page.leftLine, probe4, reference);
    const double sameRL = forcedOff(page.rightLine, probe2, reference) -
                          forcedOff(page.leftLine, probe2, reference);
    const double uR = page.rightLine.center().x();
    const double uL = page.leftLine.center().x();
    const double expected = (uR - uL) * (3 - 1) / 4.0; // (remP-remR)/dP, in d4 probe px
    const double dd = crossRL - sameRL;
    qInfo("4b double difference: measured %.3f d4-probePx (crossRL %.3f, sameRL %.3f), "
          "predicted magnitude %.3f", dd, crossRL, sameRL, expected);
    // Measurable-effect bar only: at least half the predicted scale term and
    // bounded by prediction + noise. If this fails low, 4b is runtime-
    // immaterial at this geometry; do not loosen after seeing the result.
    QVERIFY(std::abs(dd) >= 0.5 * expected);
    QVERIFY(std::abs(dd) <= expected + 0.25);
}

void UltraClearTextTests::crossDecimationSmallTextAtHighDpi_data()
{
    QTest::addColumn<int>("pixelSize");
    QTest::newRow("8px") << 8;
    QTest::newRow("12px") << 12;
    QTest::newRow("16px") << 16;
}

// Production-true pairing at 2560 wide: reference d2, probe d3. Small text is
// where kernel/aliasing (claim 5) would bite; d2/d3 centroids are both exact,
// so any offset here is remainder scale (+0.33 probePx max) plus aliasing.
void UltraClearTextTests::crossDecimationSmallTextAtHighDpi()
{
    QFETCH(int, pixelSize);
    constexpr int kW = 2560, kH = 1440; // H divisible by 2 and 3
    const ViewTransform t = IdentityTransform(kW, kH);
    const QString text = QStringLiteral("Reading printed pages 123");
    const EdgePage page = MakeEdgePage(kW, kH, pixelSize, 0, text);
    const EdgePage shifted = MakeEdgePage(kW, kH, pixelSize, 3, text);
    const ProbeFrame reference = ProbeOf(page.image, t, 1, 1, 1, 0, 2);
    QVERIFY(reference.IsValid());
    const ReferencePatch patch = BuildReferencePatch(reference, page.rightLine);
    QVERIFY2(patch.valid, "small-text patch must be trackable at d2; "
                          "invalid here is a finding, not a decimation result");
    const ProbeFrame same = ProbeOf(page.image, t, 1, 1, 2, 10, 3);
    const ProbeFrame moved = ProbeOf(shifted.image, t, 1, 1, 3, 20, 3);
    const Measured identical = Measure(patch, same);
    const Measured shiftedM = Measure(patch, moved);
    qInfo("%dpx @2560 ref2/probe3: identical design %.4f forced off %.3f probePx; "
          "shifted design %.4f", pixelSize, identical.design.score,
          OffProbePxX(identical.forced, same), shiftedM.design.score);
    QVERIFY2(identical.design.matched && identical.design.score >= 0.82,
             "design contract at production pairing");
    QVERIFY(std::abs(OffProbePxX(identical.forced, same)) <= 1.0);
    QVERIFY2(shiftedM.design.matched && shiftedM.design.score >= 0.82,
             "+3 src px shift must validate at production pairing");
    const double recoveredSrcPx =
        OffSourcePxX(shiftedM.forced, kW) - OffSourcePxX(identical.forced, kW);
    QVERIFY(std::abs(recoveredSrcPx - 3.0) <= 3.0); // one d3 probe pixel
}
```

Negative controls summary (all inside the snippets): same-decimation rows
(ref2/probe2, ref4/probe4), divisible-width row (1000, rem 0/0), changed-text
probe rejected, blank region untrackable, Y offset ≈ 0 (no Y remainder),
known-shift sign calibration. Existing cross-zoom/freshness coverage
(`patchMatchesUnchangedTextAcrossPanAndZoom`, the 66 Qt rows) is untouched;
these snippets add rows, never modify production or tuning.

## Decision rules for the parent run (pre-registered; do not move after seeing output)

1. All design asserts pass **and** |double difference| < 0.5×predicted →
   4b/5 statically real but runtime-immaterial; close as bounded, **no fix**.
2. Design asserts pass, displacement measurable, but every |offset| ≤ 1 probe px →
   real but within design tolerance; fix is optional geometry tightening only.
3. Any production-true row (2560 ref2/probe3; optional 2883 ref2/probe4) fails
   score ≥ 0.82 or |offset| > 1 probe px → real defect; narrow fix below is
   justified, with these snippets retained as the regression.
4. Only the 1003 stress row fails → no production defect; document the bound,
   no code change. A fast-path-masked identical result (design offset exactly 0
   with score ≥ 0.90) must be reported as "offset unmeasured by design call",
   never as zero displacement.

## Narrow fix sketch — ONLY if rule 3 fires (not proposed on static evidence alone)

Store the decimation in `ProbeFrame` (one int, set in `MakeProbeFrame`) and make
the three mapping sites use the *effective* sampled extent
`viewportWidth / decimation` (fractional) instead of `luma.width()`:
`ProjectPatch` (:92–101), `PixelsPerSceneX/Y` (:69–79), `BuildReferencePatch`
visible rect (:475–478), `SceneSimilarity` (:692–718). Probe size, kernel, OCR
input resolution, search radius and async budget are unchanged, so cross-zoom
matching and UI-thread cost are preserved; this is a mapping correction, not a
resolution alignment. The alternative (ceil-division probe width with a partial
edge block) changes probe dimensions and the 960-px cap accounting and is not
recommended without separate evidence. The d4 kernel-centroid term (−0.125 probe
px) is below tolerance on its own; do not change sampling offsets unless the
left-edge/divisible controls isolate it above 0.25 probe px.

## Honest limits

Unexecuted: QPainter text at 8 px could make the d2 patch itself invalid
(deviation < 6 at `BuildReferencePatch` :504) — the snippet reports that as its
own finding rather than a decimation failure. Parabolic sub-pixel refinement
noise on real glyphs is unknown until run; thresholds above (½×predicted,
+0.25 probe px) are pre-registered guesses, flagged as such. No runtime failure
of current code is claimed anywhere in this file.
