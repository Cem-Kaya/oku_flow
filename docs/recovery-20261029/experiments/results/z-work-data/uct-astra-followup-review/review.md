# Read-only follow-up review

Live repository untouched. No builds, tests or GUI run. Proposed fixture bodies
are unexecuted; compile and capture red evidence before applying any repair.
Only a MatchPatch repair is packaged as a patch; no color/retry edits.

## 1. Dense-match incumbent loss and unchecked final interpolation

Confirmed from code at ultra_clear_text.cpp:530-604. The coarse stride-3 search
compares its score against the full-density zero score, overwrites its offsets,
and line 557 replaces bestScore with the dense score without keeping the better
zero hypothesis or checking dense visibility. Local refinement may never revisit
zero when the coarse winner is distant. The proposed 40x32 pattern yields zero
NCC .8575, versus a distractor six rows away with sparse NCC 1 and dense NCC -1/3.
No OCR/font dependence in this fixture. Current code should incorrectly reject.

The final parabolic offset also keeps the pre-interpolation score and visibility.
The patch evaluates the combined final offset at full density, retaining the
integer candidate unless interpolation is at least as good and sufficiently
visible. Insufficiently visible/sentinel neighbors do not drive interpolation.

The patch preserves the zero hypothesis, separates sparse ranking from dense
acceptance, and changes no thresholds or search radii. Parent should capture
additional border-visibility/combined-subpixel fixtures before claiming those
branches empirically verified; only the incumbent-loss fixture is constructed.

## 2. Partial-line duplicate after return pan

Confirmed lifetime path at Admit coverage retention (1331 onward), ApplyChecks
(1360 onward), IsShown (1420 onward): an old full line whose center is outside a
cropped result is retained. Probe validation hides it while cropped, but does
not remove it. The cropped OCR result admits the visible suffix using agreement
from the retained line. After panning back, a full-view probe validates BOTH
reference patches and IsShown shows both copies of the suffix word. Thus current
revalidation does not permanently prevent the duplicate; it restores it.

Proposed actual-image fixture: Alpha/Beta/Gamma in one row at x100/500/1200,
y350, 64px font; recognize full view twice, validate right-half crop to hide old
line, admit cropped Gamma, return full probe and count exactly one Gamma.
Check premises before final assertion, especially crop word count and old line
center outside coverage. Fixture uses ResultForView to exclude offscreen words.

A bounded conservative repair is to retire an old line when a newly admitted
line contains a spatially overlapping accepted word (IoU >= agreementIou), even
when the old line center lies outside coverage. This can lose cached offscreen
words; subsequent OCR restores them. A word-level merge retaining old offscreen
words needs more careful readingTokens/ref-patch handling and is not proposed as
a rushed fix. The separate boundary-line blanking claim is not elevated: losing
untrackable text is consistent with the fail-closed contract.

## 3. Refresh discards independently valid identical reference

Confirmed conditional availability defect at 1241-1245 and 1331-1347. With a zero
admission budget, a refresh creates invalid candidate lines and replaces valid
old lines even when the old exact reference already passed the latest probe.
Current tests cover absence of prior validity, but not this refresh case.
The proposed fixture validates old accepted lines on probe timestamp500, then
admits identical OCR reference timestamp300 at now520 with budget0. Before text
is shown; afterwards all lines are hidden until another probe.

Safe narrow repair: only for budget-exhausted candidates, reuse validation of an
old line when exact reference rectangle/grid/values, accepted word text/boxes,
and scene identity match, and old validation is valid and at least as recent as
the latest probe (matching frame for equal timestamp). Transfer validation state
only; do not transfer it to different pixels, geometry, words or a newer probe.
Keep the existing unchecked-text rejection fixture and add negative variants.
General unconditional retention would be unsafe. No admission patch supplied
before those positive/negative tests are established.
