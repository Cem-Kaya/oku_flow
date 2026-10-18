# UCT uncertainty and protected source ink

Development change, 18 October 2026. This note records common/UI contracts for
the coordinating change to fold into `ultra_clear_text.md`, `code_reference.md`,
the test guide and the changelog. No new dependency, model, catalog key or
third-party implementation is introduced.

`PaddleDecodeTokens` now retains below-0.7 word candidates in their original
order. `TokenDisposition` records candidate, low-confidence, undersized,
unsupported-notation, invalid-geometry and unrecognized reasons. A rejected
candidate is evidence only: its spelling never becomes recognized reading
text or a glyph, and the 0.7 threshold is unchanged. `RecognizedWord` adds
`disposition`, `confidence` and immutable `protectedBoundsPx`; ink fitting can
refine replacement geometry without shrinking rejected-source protection.

`PrepareLines` retains every known span as a `PreparedWord`, including its
`disposition`, `confidence` and `protectedSceneRect`. Size, notation, confidence
and geometric filters produce ordered omissions. Malformed geometry prevents
registration of the containing line. This preserves only known OCR spans;
regions the detector never found remain outside the coverage claim.

`OverlayModel::Admit` retains omission-only lines, collapses consecutive empty
reading tokens to the existing ellipsis, and retires prior accepted words
covered by a new successful result even when every new span was rejected or
the result is empty. A partially covered cached line is retired conservatively
when any of its words/protected spans intersect the new coverage.

`AcceptedLine::protectedSceneRects` stores rejected and pending source spans.
`SceneWord::protectedSceneRects` carries their current offset into every
drawable word, including protection for source text with no safe replacement.
The Qt vectors share storage. `UltraClearTextOverlay::SceneWord` carries the
same field; its controller adapter must copy and compare it when caching
overlay words. Both renderer passes exclude projected protected rectangles.
If original/glyph geometry intersects protection, the whole replacement is
suppressed and its source is protected before any plates paint. This prevents
partial-word glyphs and protects original ink from neighboring plate padding.
Overlap propagation is capped at eight passes; a still-growing pathological
chain keeps all original pixels for that paint rather than doing quadratic work.

`OverlayModel::IsReadable` separates source-fresh reading evidence from
`IsShown`, which additionally requires a valid residual reference and an
eligible replacement. A line that cannot register can expose agreed text with
a leading ellipsis while drawing no glyphs. Such a line is excluded from
`LineReferences`, so probes cannot renew its source clock. It expires when a
new frame exceeds freshness and can remain readable for the unchanged exact
frame. Existing presentation/source suppression still belongs to the controller.

`OverlayModel::AdmitImmutable(result, selectedFrame, nowMs, corroboration)` is
an explicit immutable-frame seam. Selected source epoch, geometry generation
and frame sequence must match; the result reference must retain that stamp.
The old live age gate is bypassed only by this method. A single selected
analysis exposes safe recognized text with an uncertainty marker and keeps
original pixels. `AcceptedWord::glyphEligible` records this distinction.
An optional neighbor can enable a word only when all of the following hold:

- Both successful results have the same scheduler authority and source/geometry
  identity, with genuinely different frame sequence values.
- Their 32-byte `RecognitionResult::sourcePixelDigest` values differ. The worker
  helper `SourcePixelDigest` hashes full visible BGRA rows, excluding allocation
  padding. Different job IDs/timestamps or changed padding are insufficient.
- The neighbor reference carries its own matching source/frame stamp, the
  selected line's residual patch matches those pixels, and safe neighbor word
  text/geometry agrees within the existing IoU policy and admission work budget.

All glyph geometry, colors and provenance continue to come from the selected
result. The caller owns exact selected-media/model authority, job deadlines,
neighbor acquisition, revocation and the bounded scheduling policy. This method
does not schedule work or pretend that repeated identical pixels corroborate
each other. Switching live/immutable admission resets prior evidence.

Live admission applies the same duplicate-source rule when a pending word
would first become confirmed. Its earlier observation must share source,
geometry and scheduler authority, have a different frame sequence, and carry
an unequal valid 32-byte source-pixel digest. Changed job IDs or timestamps,
missing digests, changed allocation padding and repeatedly recognized identical
pixels do not establish new words. Because `pending_` is replaced as a batch,
one bounded pending-evidence ticket covers it; there is no per-word digest or
unbounded image/hash history. Reset and switching admission modes clear that
ticket. Already-confirmed words remain eligible during duplicate refreshes,
subject to the existing source, residual, geometry and freshness rules; newly
recognized spans in the same refresh still need independent confirmation.

All four model selectors now use the existing `WheelSafeComboBox`. Wheel and
trackpad events remain available to the surrounding scrolling surface; keyboard
selection and blocked programmatic settings restoration remain functional.

Targeted regression coverage added:

- `PaddleTests::decoderPreservesUncertainNegationThroughReading`: production
  CTC probability decoding, preparation, two source observations, admission and
  readout for `Do not proceed` with 0.60 confidence on `not`; requires
  `Do … proceed`, two eligible words and a retained protected middle span.
- Existing CTC boundary coverage now checks preservation/confidence/protection
  of a below-threshold trailing candidate and neighbor ink-search boundaries.
- `rejectedTinyAndDenseSpansKeepOrderedOmissions`: undersized decimal, unsupported
  exponent and 180 low-confidence spans collapse to one ordered omission.
- `untrackableTextStaysReadableWithoutReplacement`: agreed reading text remains
  marked, glyphs stay absent and unrelated probes cannot renew its clock.
- `rejectedRefreshCannotRestoreOldAcceptedText`: all-rejected and empty successful
  results cannot revive prior accepted words.
- `immutableEvidenceRequiresDistinctPixelsAndSafeRegistration`: old immutable
  source, repeated input, identical bytes, same frame, authority mismatch,
  low-confidence neighbor, content mismatch, compatible distinct pixels and
  live/immutable isolation.
- `livePendingEvidenceRejectsDuplicatePixels`: literal identical source
  evidence stays pending under changed job IDs, timestamps and frame IDs;
  different pixels with the same frame, missing digests and different scheduler
  authority also stay pending. A distinct compatible frame can confirm text.
- `liveDuplicateRefreshPreservesEstablishedWords`: an already-confirmed word
  stays drawable across duplicate refreshes while a newly recognized word
  remains an ordered omission until a distinct image corroborates it.
- `pendingEvidenceCannotCrossResetOrAdmissionMode`: Reset and live/immutable
  transitions prevent old pending batches from authorizing new words.
- `schedulerPixelEvidenceIgnoresAllocationPadding`: exact worker SHA-256
  output stays equal for identical visible rows with different padding bytes.
- `protectedAdjacentInkSurvivesCompositing`: minus, exponent, decimal and comma
  at 1x/2x. It first demonstrates unprotected plate coverage, then checks actual
  overlay alpha is zero and composited source pixels are identical throughout
  protected geometry while the accepted replacement still paints.
- `modelSelectorsIgnoreWheelAndKeepDeliberateKeyboardSelection`: all four real
  panel controls, focus/no focus, both wheel directions, pixel-scroll events,
  unchanged model settings/signals, explicit keyboard edits and settings restore.

The coordinating agent owns CMake registration/linking of the new decoder test
and central validation. No build, test, native GUI, browser, phone, camera or
owner-application action was executed by this subtask. Offscreen pixel fixtures
do not establish native/DPI, model accuracy, real-camera or release acceptance.

Synthetic live admission fixtures now use `IndependentFramePixels` to change
eight non-text pixels at the bottom-right edge of each observation and
`SourcePixelsDigest` to hash the exact visible BGRA rows. References are built
from those same pixels. `ResultFor`/`ResultForView`, controller readbacks, the
stable device-scene fixture, Paddle's uncertain-negation regression and the
screen-text accessibility model seeds use real distinct observations. Their
observation indices supply synthetic frame IDs when a fixture did not specify
one. Intentional duplicate-evidence cases copy the original pixels/digest;
they never gain independence from IDs or timestamps alone. Existing color,
reading-order, omission, coverage and residual assertions remain in place.
