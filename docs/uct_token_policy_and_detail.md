# UCT ordinary labels and paused 8x detail (development)

At base `e2d433fcd184d669d79ef40096a2a03180b1c28e`, `IsHighRiskToken`
rejects any token containing both a digit and one of its ASCII lookalike
letters, including `G`. That makes `GPT-6` ineligible before pixel proof;
`Astra High` does not meet that condition. Increasing detector magnification
cannot bypass this veto, and removing it does not guarantee any particular
recognition or replacement.

The owner requested ordinary letter/digit acceptance instead of a model-name
whitelist or identifier grammar. The blanket mixture veto and isolated
`l I 1 0 O o` veto are removed. Labels such as `GPT-6`, `GPT6`, `RTX4090`,
`OCRv6`, `R2D2`, `COVID-19`, `B2`, `5S` and even the literal `l0O` may qualify.
No string is spell-corrected, and an engine's repeatable misreading can remain
a misreading. There is no brand, language-model or dictionary decision.

The 26 October equation-scene investigation found that correctly recognized
`W=△K` was rejected solely by this character filter. It now permits Unicode
mathematical operators, Number_Other glyphs (including precomposed fractions,
superscripts and subscripts), the triangular delta `△` and degree sign `°`.
It renders the literal recognized characters, without interpreting or rewriting
an expression. Empty text, other decimal digit scripts (such as fullwidth
digits), modifier letters, controls and unsupported symbols still fail closed.
`|`, `x_2`, `a_b` and `a^2` remain outside the reviewed alphabet. The conventional
file-token exception is unchanged. Font, geometry and source proof still apply;
this is not a two-dimensional handwritten-math recognition engine.

[Source-ink outlines](uct_ink_outlines.md) add a separate visual outcome for
some rejected spans. They trace current source shapes, including a written
1 over 2 and raised 2, without admitting or rewriting recognition output.
The text alphabet, confidence and word-proof requirements below are unchanged.

Confidence must still be finite and in the existing accepted range, starting
at 0.7. That engine score is not a calibrated 70% probability of correctness.
Windows.Media.Ocr provides no per-word score: its adapter's default `1.0` is
nominal metadata, not measured certainty, so this numeric gate cannot filter
Windows misreads. The owner's removal of ASCII vetoes also applies to that
fallback; ordinary labels still require its independent-pair/pixel proof.
Neither that proof nor a high Paddle score can exclude a repeatable spelling
error. No score is invented from agreement, and no ASCII veto is reinstated.
Explicit backend rejection, five-source-pixel minimum height, valid finite
geometry, source/acquisition identity, independent observations and pixel
registration all retain their existing rules. Paused/Freeze replacement uses
selected-frame standalone admission; live and explicitly corroborated protocols
retain their independent-observation rules. Repeated held pixels do not supply
independent evidence.
The character change does not alter glyph rendering, color, startup or memory
ownership.

Paused visible-detail maximum and default increase from 6x to 8x through
`kMaximumDetailMagnification`. The controller already uses that shared default;
there is no separate persisted magnification preference to migrate. The
960-pixel detector cap, 300 ms settling, 25% scale-improvement requirement and
32 attempted-region bound remain. A tiny visible region expands around its
centre to `ceil(960/8) = 120` source pixels on its long edge, limited by image
bounds. Enlarged detector axes floor to multiples of 32. The existing 32-pixel
minimum can exceed 8x on a tiny short axis; it is not an isotropic 8x guarantee.
Nonfinite magnification falls back to 1x. Live supplemental source/detector
limits remain 480/704, with the existing reciprocal-rounding guard.

The earlier six-times private-image measurements remain historical evidence;
they do not establish 8x quality. No model, engine profile, precision or batch
default changes, and no new dependencies or redistributed assets are added.

## Fixtures and evidence

The old lexical expectations are retained in Git at the named base. The
`highRiskTokensFailClosed` fixture explicitly accepts the permitted ordinary
and mathematical forms. Reading-omission and jitter fixtures use an unsupported
modifier letter in place of now-supported superscripts, retaining
their omission/protection assertions. An explicitly backend-rejected `1O`
fixture remains rejected; changing the character filter cannot override a
backend disposition.

`ordinaryLabelsUseIndependentProofAndPaint` covers real preparation, CPU
reference registration, immutable admission and production offscreen painting
for every listed ordinary-label/isolated-lookalike row. It checks that no
neighbour, duplicate source pixels and changed source ink cannot enable glyphs.
`ordinaryLabelsKeepPreparationGates` covers low/nonfinite/out-of-range score,
sub-five-pixel height, nonfinite geometry and explicit backend rejection.
These use synthetic known text, not a claim about an OCR engine's accuracy.

## Equation-scene diagnosis (26 October 2026)

Actual warm BF16 OCR on the existing generated website board found a separate
recognition/layout limitation: the kinetic-energy crop returned `K=mv2`, losing
the stacked fraction, and a work formula returned `W=Fdcost` for a theta ending.
Disabling curved sampling admitted the same incomplete kinetic-energy text;
that counterfactual is not a repair. Invalid curved geometry and ambiguous
portrait glyphs retain their existing original-ink protection. The character
change recovers literal supported symbols such as `△` when all other gates
pass; it does not insert missing numbers or correct a model's spelling.
The source, warm-plan receipts, counterfactuals and failed detail-crop diagnostic
are retained under the local `website-uct-gallery-20261026` evidence folder.

After the character repair, the same full-resolution source admitted and painted
eight words instead of seven: `W=△K` now reaches both the painter and reading
text. The actual 1280x720 decoded demo-video frame also painted eight words.
Both still returned the incomplete `K=mv2` with invalid geometry; neither
reconstructed the missing stacked fraction or the superscript's position.
Consequently this is a character-policy repair, not a claim that the owner's
specific `½ mv²` example is fully recognized. Lowering the confidence threshold
would not repair that missing structure. The original equation remains visible.

The production-factory run used existing warm BF16 plans for both networks,
retired its exact worker successfully and left the cache inventory unchanged.
An earlier attempt inherited the owner's legacy local precision selector and
was correctly refused as a both-role BF16 diagnostic; its failed evidence is
preserved separately. The corrected child alone cleared that selector. No
owner setting, model tier, plan or persistent environment value was changed.

`pausedDetailGeometry` covers 8x default/clamps, fractional/ceil sizing,
portrait and border crops, source limits, alignment and the tiny-axis minimum.
The CPU Paddle normalization fixture checks actual enlarged tensor contents;
the serial GPU fixture compares an actual strided 8x crop with the CPU output.
Existing controller coverage checks the shared cap on both original
acquisitions. The live regional rows retain their exact 480/704 expectations.

The first fresh Windows CPU/PCH-off configure and build of
`ultra_clear_text_tests`, `uct_region_scheduler_tests` and `uct_paddle_tests`
succeeded with at most two compiler jobs. The first tracked CPU gate passed
all six selected CTest cases, with no skipped Qt rows, in 38.97 seconds:
`ultra_clear_text_contract`, `uct_paddle_processing`, and the scheduler's
`regionalDetectorBudgetKeepsPausedDefaults`, `pausedDetailGeometry`,
`regionalRealizedInputDimensions` and `regionalMalformedInputsAreRefused`.
It ran with `Parallel=1` and GUI excluded: **SCOPED PASS, 6 of 232 tests**.
The original build and gate receipts are retained under the task's private
Z: output directory; no failed attempt or pre-change red proof was invented.

CUDA compilation/execution, the separately edited accessibility omission
fixture and the full tracked Windows matrix remain for the parent's combined
validation. The CPU result establishes neither GPU normalization execution,
8x real-image recognition quality, native desktop nor release acceptance.

For the 26 October character repair, the focused CPU gate passed all seven
selected CTest cases, including the model/painter contract, Paddle/ribbon
contracts, accessibility at 100/150/200% and shared-source equivalence. The
tracked background matrix compiled the shipping application, passed 262 CPU
cases (nine optional whole-case skips) and 15 CUDA-delta cases (five optional
whole-case skips), with no unresolved failures. GUI was explicitly excluded;
this does not establish native desktop or full release acceptance. First failed
test attempts are retained: they exposed a vertical-bar category exception and
one remaining obsolete superscript-refusal fixture, both corrected before the
passing run. The existing omission and protection assertions remain intact.
The separate PCH-off build and four-case model/Paddle/accessibility/equivalence
gate also passed. The real-image diagnostic is correctness evidence only;
no added-latency claim is made from these visits.
