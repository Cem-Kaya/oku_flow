# Character-sensitive UCT verification baseline

The revised performance roadmap requires character-change evidence before region
reuse can skip recognition. `tests/uct_character_diagnostic.hpp` measures the
existing residual verifier independently of neural recognition. It does not
authorize reuse or claim OCR accuracy.

The two unregistered, opt-in `uct_paddle_tests` slots are
`optionalCharacterChangeCpu` and `optionalCharacterChangeGpu`. Set
`OKUFLOW_UCT_CHARACTER_DIAGNOSTIC=1`, select just the desired slot, and retain
its output and process exit. The GPU variant requires a CUDA build and actual
resident references; it refuses pixel/feature downloads. It is serial with other
GPU experiments. Neither slot needs models, a camera, native windows or downloads.

The finite corpus has 12 substitutions: signs, decimal removal, comma/dot,
comparison/equality operators, O/0, l/I, 1/7, 8/9, rn/m and c/e. It renders the
unchanged surrounding text at fixed positions with the installed Consolas font,
records that font's SHA-256, and tests pixel sizes 14/24/44 and probe decimation
1/2/4. Each has changed/unchanged controls with zero or (2,1)-pixel image
translation. This is synthetic raster jitter, **not physical-camera jitter**.
The source is 960 x 128 and both implementations assert their actual probe
dimensions before comparison; the resident 960-pixel bound cannot change the
requested decimation silently.

Both whole-line and edited-token reference extents are evaluated with the actual
tracking, exact-current and paused-word policies. Each policy/extent has 216
changed and 216 unchanged cases, including small/untrackable references. Raw
rows retain reference validity, scores, visibility, offsets, expected change
and current active-row pixel hashes. A false accept means changed text matched;
a false hide means unchanged text did not match. Invalid references stay visible
in the evidence rather than being removed to improve the scores.

Evidence is written once, after the finite run, to a unique
`%LOCALAPPDATA%/OkuFlow/uct-character-<timestamp>-<uuid>/character-baseline.json`.
A diagnostic PASS means its operations and report completed; it is **not** a
character-sensitivity quality PASS. There is no threshold relaxation, production
per-frame hashing, extra runtime readback or new display eligibility in this
fixture. The GPU branch starts with synthetic uploads solely to construct the
independently owned analysis images; its reference/match samples remain resident.

## Resolution-verified CPU/resident observation, 2026-10-20

Both finite runs completed with normal exit. All 2,592 decisions and scores
matched exactly, with identical font and source hashes and zero invalid references.
Each table cell below has 108 cases:

| Policy / extent | Edited, no jitter: false accepts | Unchanged, no jitter: false hides | Unchanged, jitter: false hides |
| --- | ---: | ---: | ---: |
| Tracking / line | 108 | 0 | 12 |
| Tracking / token | 92 | 0 | 12 |
| Exact-current / line | 100 | 0 | 79 |
| Exact-current / token | 67 | 0 | 108 |
| Paused-word / line | 100 | 0 | 36 |
| Paused-word / token | 67 | 0 | 32 |

The exact-current policy deliberately disallows translation, so its jitter hides
must not be presented as a registration bug. Even without jitter, NCC and a
smaller extent alone miss edits. These are verifier observations, not whole-UCT
error rates: OCR text agreement, high-risk token rejection and other admission
requirements still apply. They are not representative population rates or
physical-camera acceptance.

Validated raw reports are under
`uct-character-20261020-173529-e4b642e7-51e3-493b-acd9-4b3a58036385` (CPU) and
`uct-character-20261020-173535-62602bf0-168c-4324-8a39-5655f12af93c` (resident).
The comparison is in
`uct-paused-scrolling-20261020/character-verifier-comparison.json`.
Earlier reports remain preserved: the first 1280-wide fixture requested D1 on
GPU, whose production size bound actually used D2. That invalid CPU/GPU pairing
was diagnosed and repaired by the 960-wide fixture plus actual-size assertions;
it is not reported as a production kernel fix or a passing parity comparison.

Future verifiers must retain these baselines and add independent held-out
documents, actual camera motion/noise and source/seek lifecycle tests.
Region scheduling may be studied in shadow mode while full OCR remains
authoritative; skipping recognition remains blocked by this safety result.
