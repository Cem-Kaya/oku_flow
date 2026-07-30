# Plan 24 — Test and Build Gates

Status: **IMPLEMENTED / VERIFY COMMIT CONTENTS.** Proposed 2026-07-24,
re-audited and expanded 2026-07-28, then implemented and validated on Windows
the same day. The CPU matrix passed 4/4 tests, the CUDA matrix passed 6/6
tests, and the release-bundle path passed the same six tests before publishing
a hash-verified `dist/OpenZoom` bundle.

The remaining gate is repository hygiene rather than code: several test
sources and the optional fixture generator are still untracked in the owner's
dirty working tree and must be included in the eventual commit/PR. Generated
Y4M and MP4 media must remain ignored; the owner confirmed it is regeneratable
and must not be pushed.

Effort: **one day** for A0-C. Section D is a backlog, not a day.

Everything else in plans 20-23 is unverifiable without this. Today the build
can report success while running zero tests, the tests that matter most cannot
run under any preset, and — new — most of the test suite exists only on the
owner's disk.

## Inventory — what actually exists today (2026-07-28)

Read this before touching anything; the original plan predates half of it.

### Test targets (`tests/CMakeLists.txt`)

| Target | CTest name | Guard | Skip semantics | In git? |
|---|---|---|---|---|
| `settings_store_tests` | `settings_store_round_trip` (`:19`) | none | n/a | source tracked (modified) |
| `view_transform_tests` | `viewport_transform_geometry` (`:42`) | none | n/a | tracked |
| `annotation_model_tests` | `annotation_model_geometry` (`:64`) | none | n/a | **untracked** |
| `annotation_overlay_tests` | `annotation_overlay_interactions` (`:91`) | `WIN32` (`:70`) | needs a real desktop session — it drags across a frameless-window boundary | **untracked** |
| `stabilization_cuda_tests` | `stabilization_similarity_cuda` (`:112`) | `WIN32 AND OPENZOOM_ENABLE_CUDA` (`:98`) | `SKIP_RETURN_CODE 77` (`:116`) — clean skip without a CUDA device | **untracked** |
| `stabilization_replay_cuda` | `stabilization_replay_regression` | `WIN32 AND OPENZOOM_ENABLE_CUDA` | `SKIP_RETURN_CODE 77`; deterministic clamp-bump input synthesized in memory | **untracked** |

`stabilization_replay_cuda` is the offline replay harness (CLI:
`--input/--output/--trace/--width/--height/--zoom/--strength`,
`tests/stabilization_replay_cuda.cu:25-33`) that replays a saved camera
recording through the production kernels. It is the tool that caught the
2026-07-26 real-clip regression. It has no `add_test` — even a CUDA-enabled
test preset would not run it.

Fixtures: `tests/fixtures/virtual_camera/` contains the optional visual
generator, README, CSV evidence, and generated Y4M/MP4 previews. The executable
regression does not depend on those files: it synthesizes the deterministic
clamp-bump sequence in memory. `.gitignore` deliberately excludes every Y4M
and MP4 below `tests/fixtures/`; `git check-ignore -v` was verified for all
three generated videos.

### Configure/build/test presets (`cmake/CMakePresets.json`; root
`CMakePresets.json` just includes it)

| Preset | CUDA | TEXT_SR | TESTS | binaryDir |
|---|---|---|---|---|
| `msvc-debug` | ON | ON | **OFF** | `build/msvc-debug` |
| `msvc-release` | ON | ON | **OFF** | `build/msvc-release` |
| `msvc-cpu` | OFF | OFF | ON | `build/msvc-cpu` |
| `msvc-cuda-tests` | ON | OFF | ON | `build/msvc-cuda-tests` |

The false-green debug/release test presets have been deleted. The only test
presets are `msvc-cpu-tests` and `msvc-cuda-tests`; both use
`execution.noTestsAction: error` and show failed output. All configure presets
inherit the hidden `msvc-base`, which owns the one preset-side Qt default.

The earlier claim that this project had no `option()` declarations was
incorrect: `cmake/ProjectOptions.cmake` declares the build switches. The
release script now explicitly sets tests ON regardless, builds the default
target set, and runs CTest against the exact release tree it packages.

### Scripts

- `scripts/agent_build.bat` — repo-relative release compile, CPU test, and
  CUDA test gates. It discovers Visual Studio through `vswhere`, falls back to
  the documented Community path, prints one accessible PASS/FAIL line per
  leg, and exits non-zero unless all three legs pass.
- `scripts/build_release_bundle.bat` — tested staging/publishing pipeline;
  details in C.
- `scripts/run_minimal_test.bat` — optional DX12/CUDA smoke; missing sandbox
  is a clean, explicitly-worded skip (`:29-35`). Acceptable as-is.
- Stale litter, ignored but confusing: a Visual Studio solution + cache at
  `build/` root, ad-hoc `build-cpu/`, `build-cpu-msvc/`, `build-cuda-tests/`
  dirs, `maxine_*.obj` at repo root. Delete opportunistically; nothing
  depends on them.

## A0. Commit the implemented test suite (remaining owner/PR hygiene)

The tracked `tests/CMakeLists.txt` references four sources that are still
untracked in the current dirty working tree:
`annotation_model_tests.cpp`, `annotation_overlay_tests.cpp`,
`stabilization_cuda_tests.cu`, and `stabilization_replay_cuda.cu`.
`view_transform_tests.cpp` is already tracked. A fresh clone cannot reproduce
the green matrix until those source files and `scripts/agent_build.bat` enter
the eventual commit/PR.

Generated fixture policy is now final: do **not** add `*.y4m` or `*.mp4`
files. They are regeneratable visual evidence and remain ignored. The optional
fixture generator/README may be committed as developer tooling, but the
executable regression gate has no Python, NumPy, ffmpeg, codec-version, or
checked-in-media dependency.

## A. CUDA regression suite — implemented and reachable

The original defect was wiring rather than test authorship: the only
tests-ON preset disabled CUDA. `msvc-cuda-tests` now configures, builds, and
runs the CUDA block, and `scripts/agent_build.bat` includes it in the standard
agent matrix.

### A1. Fourth preset pair — implemented

The hidden base and CUDA test preset now use this shape
(`cmake/CMakePresets.json`):

```json
{
  "name": "msvc-base",
  "hidden": true,
  "generator": "Ninja",
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "C:/Qt/6.9.3/msvc2022_64"
  }
}
```

All three existing configure presets switch to `"inherits": "msvc-base"` and
drop their private copies of generator + Qt path. Then add:

```json
{
  "name": "msvc-cuda-tests",
  "displayName": "MSVC Release (CUDA + tests)",
  "inherits": "msvc-base",
  "binaryDir": "${sourceDir}/build/msvc-cuda-tests",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Release",
    "OPENZOOM_ENABLE_CUDA": "ON",
    "OPENZOOM_ENABLE_TEXT_SR": "OFF",
    "OPENZOOM_ENABLE_TESTS": "ON",
    "CMAKE_CUDA_ARCHITECTURES": "75;86;89"
  }
}
```

(TEXT_SR OFF: the tests never touch it and it only adds configure surface.
The CUDA arch list is also hard-set per test target at
`tests/CMakeLists.txt:110`/`:130` — leave that, the property wins; note the
duplication in a comment so they don't drift silently.)

Plus `msvc-cuda-tests-build` and a `msvc-cuda-tests` test preset (see B for
the mandatory `output` block). `SKIP_RETURN_CODE 77` already makes this preset
safe on CUDA-less machines — configure/build still need the CUDA toolkit,
which every CUDA-ON preset already needs.

### A2. Replay harness CTest gate — implemented

`stabilization_replay_cuda` is registered as
`stabilization_replay_regression`. `--synthetic-clamp-bump` constructs the
120-frame 160x90 deterministic fixture directly in the CUDA test process,
writes optional BGRA/CSV diagnostics into the build tree, and exercises the
production stabilization kernels. It has a 120-second timeout, GPU label, and
return-code-77 skip semantics.

The remaining Phase 2 enhancement is to make the harness compute a
trace-vs-ground-truth attenuation threshold internally. The current replay
gate catches crashes, IO failures, kernel failures, and gross output failures;
`stabilization_similarity_cuda` owns the quantitative attenuation gate.

### A3. Repo-relative agent build script — implemented

`scripts/agent_build.bat` derives the repo root from `%~dp0..` and locates
VsDevCmd through `vswhere` with the documented Community path as fallback. It
runs three gates and then prints the fourth required concern, the explicit
per-leg summary:

1. `msvc-release` configure + build (compile gate for the shipping binary),
2. `msvc-cpu` configure + build + `ctest --preset msvc-cpu-tests`,
3. `msvc-cuda-tests` configure + build + `ctest --preset msvc-cuda-tests`
   (CUDA tests skip cleanly off-hardware but must *compile*),
4. explicit per-leg PASS/FAIL/SKIP summary — the owner reads this
   output through a screen reader; one line per leg, outcome word first.

`tests/README.md` names `msvc-cuda-tests`, and `agents.md` points at the
repo-relative script.

## B. False-green test presets — fixed

The original debug/release test presets targeted tests-OFF configures and
could exit zero after running nothing. They are now deleted, and both honest
test presets make an empty suite an error in the preset itself.

**Fix — final `testPresets` block:**

- **Delete** `msvc-debug-tests` and `msvc-release-tests`. Their configure
  presets build no tests by design; a repointed name would still mislead
  ("release-tests" that doesn't test the release configure). Two honest test
  presets beat four aspirational ones.
- Both survivors get:

```json
"output": { "outputOnFailure": true },
"execution": { "noTestsAction": "error" }
```

- `msvc-cpu-tests` keeps its name; add the new `msvc-cuda-tests` test preset
  from A1.

Anyone's muscle-memory `ctest --preset msvc-release-tests` now fails with
"no such preset" — the correct outcome; it never tested anything.

## C. Packaging must fail when it did not package — implemented

`scripts/build_release_bundle.bat` now configures tests ON, runs the exact
release tree's suite, assembles a disposable staging directory, validates its
runtime inventory and executable hash, and only then publishes. The original
defects and their implemented remedies are retained below for provenance:

**C1. It never runs a test.** The build step compiles only the `open_zoom`
target (`:116-121`); `OPENZOOM_ENABLE_TESTS` is never passed (`:107-113`), so
the bundle tree contains no tests to run even by hand. Fix: add
`-DOPENZOOM_ENABLE_TESTS=ON` to the configure at `:113`, build the default
target set, and insert between `:121` and the EXE probe at `:123`:

```bat
ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure --no-tests=error
if errorlevel 1 goto :fail
```

One tree, and the gate runs against the exact bits being shipped — CUDA suite
included on the owner's machine (CUDA is ON by default here, `:108`), clean
77-skips elsewhere. Escape hatch for emergency rebuilds:
`OPENZOOM_SKIP_BUNDLE_TESTS=1` prints a loud `WARNING: UNTESTED BUNDLE` and
skips the ctest leg only — default is always test.

**C2. Missing windeployqt is a warning, then success.** `:166-168` prints
"Warning: windeployqt.exe not found; Qt DLLs were not copied" and falls
through to the success epilogue (`:188-197`). A bundle without Qt runtime
DLLs is dead on arrival on every machine except a Qt developer's. Fix:
`goto :fail` with a message naming the three discovery mechanisms
(`QT_PREFIX`, `Qt6_DIR`, the `:22` default).

**C3 (new). windeployqt's exit code is ignored.** `:165` runs the deploy and
checks nothing; a failed deployment (locked DLL, wrong-bitness Qt, disk full)
still reports the bundle ready. Fix: `if errorlevel 1 goto :fail` on the next
line.

**C4 (new). No deployment sanity check.** windeployqt can exit 0 and still
produce an unlaunchable directory (e.g. pointed at the wrong exe earlier in a
refactor). Before the success epilogue, assert the two files whose absence has
actually bitten this project:

```bat
if not exist "%OUTPUT_DIR%\Qt6Core.dll" goto :deploy_incomplete
if not exist "%OUTPUT_DIR%\platforms\qwindows.dll" goto :deploy_incomplete
```

**C5. Qt location single-sourcing.** After A1's hidden base preset, exactly
two places name the Qt default: `msvc-base` and `:22` of this script. Leave
it at two — a shared file read by both CMake presets and batch is more
machinery than it saves — but make the failure obvious: in
`cmake/CMakeLists.txt`, wrap the Qt find with a check that prints "Qt6 not
found — set QT_PREFIX or edit msvc-base in cmake/CMakePresets.json" instead
of the raw CMake missing-package spew. A clean machine must fail with
instructions, not with a stack of `Qt6Config.cmake` paths.

`run_minimal_test.bat` needs no change — its optional-harness skip is already
explicit and honestly worded (`:29-35`).

## D. The coverage that is missing

Current suite (updated 2026-07-28): settings round-trip incl. annotation
style/shape/text-size migration; canonical Fill/Fit viewport geometry;
annotation model geometry — creation, dashed halo/core match, shape-outline
hit tests, undo/redo incl. one-record-per-scale-gesture, zoom-scaled
tolerance; overlay interaction (cross-window drag, click-then-type text);
CUDA stabilization similarity/outlier/clamp/recovery suite with the
97.33%/0.36 px gate; and the registered in-memory clamp-bump replay harness. The
geometry, annotation, and settings layers are genuinely well covered. The
gaps sit exactly where plans 20-23 operate.

Priority order for new tests, highest first — unchanged, with owners:

1. **Capture/recording** (plan 20): timestamp propagation at 15/30/60 FPS;
   recorded duration vs wall clock; drop accounting under induced
   starvation; original/processed sequence alignment; window resize during
   recording; disk-full finalize; negative stride; mid-stream format change;
   reconnect activation lifetime; camera-mode negotiation reporting (plan 27
   Phase 4 shipped the negotiation — it has no test).
2. **User data paths** (plan 21): root resolution, migration copy-not-move,
   unwritable-root rejection, install-internal-root rejection.
3. **Robustness** (plan 23): corrupt settings file, future schema version,
   credential storage round-trip/migration, assistant turn timeout and
   interrupt recovery, `model/list` schema error surfaced.
4. **Multi-adapter** (plan 20 G): D3D12/CUDA LUID matching; the no-match
   path must fail loudly, not fall back to device 0.
5. **Performance** (plan 22): readback-ring saturation; 4K30/1080p60/1080p120
   budget checks driven by Phase 1 telemetry.
6. **Soak**: 30-minute and multi-hour runs asserting a memory plateau and no
   drop growth.
7. **Accessibility automation**: keyboard-only traversal of both modes,
   accessible-name presence on every focusable control, and a scripted
   contrast audit of the stylesheet constants against their declared
   backgrounds (plan 18 R2.7 defined the thresholds; a 50-line QtTest can
   enforce them forever).
8. **Release smoke**: launch the built bundle on a clean Windows machine.

1-4 are ordinary unit/integration tests; 5-8 need hardware and should be a
documented manual matrix first, automated later.

**CI note (plan 06 B3 owns CI itself):** when CI arrives,
`annotation_overlay_interactions` needs a desktop session — it moves real
windows. Label it now (`set_tests_properties(... PROPERTIES LABELS "gui")`)
so a headless runner can `ctest -LE gui` instead of discovering the hang.
The CUDA tests already self-skip via return code 77.

## Execution order and effort

| Step | What | Effort |
|---|---|---|
| A0 | Include untracked test source in the eventual commit; keep generated media ignored | owner/PR |
| B | Delete two presets; make empty suites fail | **done** |
| A1 | `msvc-base` + `msvc-cuda-tests` preset trio | **done** |
| A2 | In-memory replay `add_test` (phase 1) | **done** |
| A3 | Repo-relative agent build script and docs | **done** |
| C1-C5 | Tested, validated, hash-checked staging/publishing | **done** |
| A2 phase 2 | Trace-vs-ground-truth attenuation gate | half day, later |
| D | Per owning plan | ongoing |

Implementation and the owner-hardware acceptance run are complete. A0 is now
only a future commit-content check.

## Acceptance — the exact commands and their required outcomes

Validated on Windows 2026-07-28:

1. `scripts\agent_build.bat` passed the release compile gate, CPU 4/4, and
   CUDA 6/6. The CUDA run included both stabilization tests.
2. `ctest --preset msvc-release-tests` fails with "No such test preset".
3. `scripts\build_release_bundle.bat` rebuilt the tested Release tree, passed
   all six tests, ran `windeployqt`, verified required Qt/platform/license
   files, matched the staged and built executable hashes, and published
   `dist/OpenZoom`.
4. The published executable SHA-256 was
   `6995ea8d897240ae37e9310d03c182544f4c5462db33105969f1f8def013953b`.
5. A deliberately invalid Qt prefix failed non-zero with an instructional
   error and did not declare or publish an incomplete bundle.
6. `git check-ignore -v` confirms the generated Y4M and both MP4 previews are
   ignored.

A fresh-clone check becomes meaningful only after the untracked test sources
are included in the eventual commit/PR; do not mark that repository-hygiene
gate complete before then.
