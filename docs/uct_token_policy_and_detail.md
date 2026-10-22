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

The character filter still rejects empty text, non-ASCII digits (including
fullwidth, subscript and superscript forms), modifier letters and symbols
outside its existing punctuation set. `|`, `x_2`, `a_b`, `a^2`, `√2` and `∑`
remain unsupported. The conventional file-token separator exception is
unchanged. Existing accepted punctuation/simple arithmetic is unchanged;
this is not general mathematical-notation support.

Confidence must still be finite and in the existing accepted range, starting
at 0.7. That engine score is not a calibrated 70% probability of correctness.
Explicit backend rejection, seven-source-pixel minimum height, valid finite
geometry, source/acquisition identity, independent observations and pixel
registration all retain their existing rules. Paused replacement still needs
the real selected/neighbour pair. Repeated held pixels do not supply evidence.
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
`highRiskTokensFailClosed` fixture now explicitly accepts the newly permitted
ASCII forms. Reading-omission and jitter fixtures use genuinely unsupported
superscripts in place of the formerly rejected ordinary ASCII tokens, retaining
their omission/protection assertions. An explicitly backend-rejected `1O`
fixture remains rejected; changing the character filter cannot override a
backend disposition.

`ordinaryLabelsUseIndependentProofAndPaint` covers real preparation, CPU
reference registration, immutable admission and production offscreen painting
for every listed ordinary-label/isolated-lookalike row. It checks that no
neighbour, duplicate source pixels and changed source ink cannot enable glyphs.
`ordinaryLabelsKeepPreparationGates` covers low/nonfinite/out-of-range score,
sub-seven-pixel height, nonfinite geometry and explicit backend rejection.
These use synthetic known text, not a claim about an OCR engine's accuracy.

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
