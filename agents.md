# Agent Operation Guide

OkuFlow evolves as an **AI-assisted, GPU-accelerated magnifier** for people
with low vision. Agents help us deliver that mission by building features,
keeping the pipeline fast, and ensuring the docs stay accurate. Read this
guide end-to-end before you touch the repo.

This project expects any autonomous or semi-autonomous agent (LLM, scripting
bot, CI assistant) to work within the following guardrails. Keep this document
open while contributing and update it whenever the workflow evolves.

## Core Principles
1. **Read the docs first** – Always consult `README.md`, `docs/developer_guide.md`,
   `docs/README.md`, `docs/hardcoded_paths.md`, and
   `docs/THIRD_PARTY_LICENSES.md` before making changes. They describe the architecture, build matrix, and licensing rules.
2. **Keep documentation current** – Any code or script change that affects
   usage, outputs, dependencies, or licensing must update the relevant doc.
   Never commit features or fixes without doc updates.
   - Changes to classes, functions, or public structs require updating
     `docs/code_reference.md` in the same PR/commit so the reference stays
     authoritative.
3. **Respect the dual license** – All contributions are accepted under GPL-3.0
   plus the commercial license. Do not add third-party code unless the license
   is compatible and you note it in `docs/THIRD_PARTY_LICENSES.md`.

## Naming
- The product is **OkuFlow**, formerly OpenZoom. Website:
  <https://okuflow.com>; repository: <https://github.com/Cem-Kaya/oku_flow>.
- Code identifiers use `okuflow` (namespace, include directory, file
  prefixes), `oku_flow` (executable, repository), and `OKUFLOW_` (build
  options, environment variables).
- The one rename record that intentionally keeps the old name is
  `improvement_ideas/17-project-rename-plan.md`, along with the
  `CHANGELOG.md` rename entry.

## Module Map
- `src/app/` – Qt entry point, application wiring, settings persistence.
- `src/cuda/` – CUDA interop surface and the GPU effect pipeline (stabilization,
  keystone, color conversion, blur/sharpen, display color grading).
- `src/d3d12/` – pipelined D3D12 presenter with async readback ring.
- `src/capture/` – Media Foundation camera enumeration/capture with retry and
  device-loss recovery.
- `src/ui/` – Simple/Advanced main window, floating chrome, AI settings dialog.
- `src/common/` – CPU frame prep, assistive runtime (vision reading/TTS/notes), Codex
  app-server client, fragmented-MP4 video recorder.
- Public headers mirror the source tree under `include/okuflow/`.
- `android/` – OkuFlow Camera, the Android companion app (Kotlin, Compose,
  Camera2, MediaCodec) that streams the phone camera over USB debugging. Its
  own Gradle project; see `android/README.md` and plan 51.

## Workflow Expectations
- Align every change with the mission: produce a responsive magnifier that
  helps visually impaired users read content with AI assistance (temporal
  smoothing, upcoming VLM overlays, adaptive sharpening).
- Maintain coding style and structure; mirror the source layout in
  `include/okuflow/…` and keep module READMEs updated as you populate them.
- Update `CHANGELOG.md` and licensing notices when shipping user-visible or
  legal-impacting changes.
- Ensure build scripts (`scripts/build_and_run.bat` and
  `scripts/build_release_bundle.bat`) remain functional on Windows 10/11 with
  the documented toolchain.
- From the WSL/Linux agent shell, launch Windows-side commands with PowerShell
  7 via `pwsh.exe -NoProfile -Command '...'`, for example
  `pwsh.exe -NoProfile -Command 'Get-Date'`. Use this PowerShell 7 bridge for
  Windows build tooling and batch scripts when a native Linux command is not
  enough; do not use the legacy `powershell.exe` bridge.
- CUDA from the WSL shell needs the WSL driver stub on the library path. A
  working `nvidia-smi` is not sufficient: without it `torch.cuda.is_available()`
  returns False, cupy fails to initialise, and CUDA code silently falls back to
  the CPU. Prefix the command:
  `LD_LIBRARY_PATH=/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-} python your_script.py`
  (same prefix for `jupyter lab`).
- Create Python virtualenvs on local ext4 (`/home/...`), never under the repo.
  The working copy lives on a 9p Google Drive mount: large installs are slow
  there and would also be synced to the cloud. Point the local interpreter at
  the project instead, e.g. `/home/<user>/.venvs/<name>/bin/python /mnt/.../x.py`.
- Run the appropriate build or test command locally before submitting
  automated changes; note the result in your summary.
- While editing, use `scripts/agent_check.ps1` for scoped CPU checks of the
  files you changed; report its SCOPED result as such. Use
  `scripts/agent_build.bat` for the tracked Windows release/CPU/CUDA test
  matrix before merging shared behavior and before releases. Do not use or
  recreate machine-specific test runners under the gitignored `build/` directory.
- Keep `translations/okuflow_tr.ts`, `translations/okuflow_de.ts`, and
  `src/ui/translation_catalog.cpp` in exact source-key parity. Run
  `scripts/check_translations.ps1`; use its `-UpdateManifest` switch after
  intentionally adding or removing catalog entries.

## Locked Release Bundle Fallback
- Never terminate or relaunch a running OkuFlow instance merely to replace
  `dist\OkuFlow\oku_flow.exe`.
- When Windows reports that the primary release executable is in use after a
  successful build, create a complete sibling bundle at `dist\OkuFlow2\`.
  Copy the deployed runtime files from `dist\OkuFlow\`, exclude its
  user-owned `output\` captures, and replace `OkuFlow2\oku_flow.exe` with the
  newly built executable.
- Verify that the SHA-256 of the new `OkuFlow2\oku_flow.exe` exactly matches
  the release-build executable. Report that `dist\OkuFlow\` remains the
  running/older bundle and give the new bundle path.
- Use exactly `OkuFlow2`; do not create ambiguous alternatives such as
  `OkuFlow-next`.

## Useful References
- Qt moc and object model: <https://doc.qt.io/qt-6/moc.html>
- NVIDIA CUDA interop (runtime API): <https://docs.nvidia.com/cuda/>
- Media Foundation capture overview: <https://learn.microsoft.com/windows/win32/medfound/>
- Direct3D 12 best practices: <https://learn.microsoft.com/windows/win32/direct3d12/>
- Accessible magnification guidance: <https://www.w3.org/WAI/standards-guidelines/>
- VLM-assisted magnification ideas: <https://arxiv.org/abs/2102.09576>

Agents that modify the workflow must append to this guide so future runs remain
aligned with the project goals.

## Lecture Test Camera Workflow
- Use `scripts/start_lecture_camera.bat` and `scripts/stop_lecture_camera.bat`
  for manual recorded-lecture capture testing. The harness owns a private
  portable OBS copy under `%LOCALAPPDATA%\OkuFlow\lecture-camera\`; never
  replace the user's ordinary OBS scenes or stop an unrelated OBS instance.
- Native OBS Virtual Camera is DirectShow-only. On the setup machine the
  existing modern DroidCam Video driver receives the same scene through a
  pinned private OBS output plugin, making it visible to Media Foundation.
  Validate actual lecture frames on that device, not its idle placeholder.
- Downloaded lectures and OBS copies are local test assets, excluded from git
  and release bundles. Keep sample provenance/license in
  `scripts/lecture_camera_sample.json` and `docs/lecture_camera.md`. This
  virtual feed does not validate camera optics, autofocus, or microphone audio.

## Qt Toolchain Baseline
- Windows presets and scripts now default to Qt 6.12.0 at
  `C:\Qt\6.12.0\msvc2022_64`. Use that SDK for translation validation,
  compilation, profiling, and deployment; do not install Qt 6.9.3 to satisfy
  old cache paths. Regenerate stale Qt CMake caches during an SDK migration.
- Keep Multimedia, TextToSpeech, SVG, Image Formats, and matching source
  license material installed. PDF is not an OkuFlow dependency; its exact
  SPDX document remains mandatory whenever PDF is deployed.
- `OKUFLOW_BUNDLE_BUILD_DIR` selects a local build directory for the tracked
  bundle script when the synced build tree has stale paths or file locks.
  This does not skip its compilation, CTest, staging, or publishing gates.

## Validation Script Behavior
- `scripts/agent_build.bat` compiles the shipping release configuration, then
  runs the complete CPU CTest preset and the CUDA delta: tests labelled `gpu`
  or `cuda-variant` plus names absent from the CPU inventory. Set
  `OKUFLOW_FULL_CUDA_SUITE=1` for the complete CUDA suite. Its PASS/FAIL summary
  is the normal pre-merge gate for code changes that touch shared behavior.
  `--scoped PRESET LABEL-OR-TEST...` builds one preset and runs a SCOPED gate;
  that result never substitutes for release acceptance.
- `scripts/agent_check.ps1` selects tests from changed files through the Ninja
  graph and `tests/test_impact.json`, builds only those targets and leaves GUI
  cases pending unless `-IncludeGui` is given. Infrastructure changes, unknown
  inputs and stale dependency data select the full gate.
  Automatic Visual Studio discovery is limited to 17.x (2022), preventing
  newer installed headers from being mixed with the cached 2022 compiler.
- `scripts/build_release_bundle.bat` runs CTest by default and publishes only
  from a validated staging directory. `OKUFLOW_SKIP_BUNDLE_TESTS=1` is an
  explicit emergency escape hatch and must remain visibly marked untested.
- `scripts/run_minimal_test.bat` always requires the main application build to
  succeed. It runs `dx12_cuda_minimal` when
  `sandbox/dx12_cuda_minimal/CMakeLists.txt` exists and otherwise reports the
  optional harness as skipped without failing the validation run.

## Optional Startup Performance Validation
- Manual cable-recovery profiles may run for 1–600 seconds while retaining
  sibling fixture settings and normal automatic exit. Tell the owner the
  actual deadline before requesting a physical unplug and start the live
  session first. The default remains 15 seconds; ordinary app launches are
  untimed. Heartbeat evidence retains at most its first 3000 samples.
- Use tracked `scripts/profile_startup.ps1` for real-camera startup and UI
  latency measurements. It refuses an existing OkuFlow instance and launches
  only normally exiting profiling children with explicit isolated settings.
  Do not terminate a user's instance to obtain a measurement.
- `-CompareLegacy` compares camera-opening sequences in the same executable;
  it is not a benchmark of every change against an old release. Report actual
  arrival/presentation rates separately from negotiated camera FPS, and treat
  first-present timing as submission timing rather than sensor-to-photon latency.

## UI Regression Validation
- `main_window_interactions` exercises the production Qt window without
  opening a camera or submitting AI requests. Run it through the tracked CPU
  and CUDA CTest presets in an interactive Windows desktop session.
- Set `OKUFLOW_UI_TEST_SCREENSHOTS` to an evidence directory when reviewing
  layouts. Captures include the native corner-control windows; inspect them
  alongside the assertions. Use separate output directories for each
  `QT_SCALE_FACTOR` run and avoid concurrent GUI test runs.
- Preserve the three-tab ownership contract in `docs/ui_modes_design.md` and
  keep keyboard order, collapsed-section persistence, and English/Turkish/
  German regression coverage aligned when moving controls.
- `ai_settings_dialog_interactions` validates provider switching, credential-id
  retention, workspace validation, keyboard order, and narrow dialog layouts
  using fixture settings only. It never submits AI requests or changes stored
  secrets. Keep it serial with other GUI tests.
- Keep `src/ui/translation_catalog.cpp` as LF through its `.gitattributes`
  rule, matching the translation validator's generated-manifest contract.

## Native Promotional Captures
- Follow `docs/ui_capture.md` for opt-in captures during an isolated startup
  profiling session. Use separate fixture settings and normally exiting children;
  never replace user settings or terminate an existing instance for screenshots.
- Inspect the raw composed capture for complete native controls, genuine camera
  frames, and foreign overlays. Reset computer-use automation before capture to
  remove its pointer overlay; retake unsuitable images without repairing pixels.
- Keep public media provenance separate from real-camera quality and performance
  claims. An original generated scene through the private OBS feed validates the
  demonstrated UI only.

## Desktop-Restricted Build Validation
- The owner objects to GUI fixtures taking foreground control while using the
  PC. Run them only during an explicitly free desktop session for that run;
  later owner input/desktop use revokes that availability. Keep background
  checks under `OKUFLOW_AGENT_EXCLUDE_GUI=1`, preserve interrupted/failed
  evidence, and never treat that partial pass as full release validation.
- When the owner reserves the desktop, set `OKUFLOW_AGENT_EXCLUDE_GUI=1`
  before running `scripts/agent_build.bat`. The script still compiles release,
  CPU and CUDA configurations and runs all tests without the `gui` label.
  Its summary explicitly marks partial validation. The default remains the
  complete test matrix; a filtered pass never substitutes for the full release
  gate or permits claiming that desktop interactions were verified.

## Private Android Signing and Serial Desktop Validation
- Use `scripts/build_android_signed.ps1` for owner-controlled upload signing.
  The owner enters a new key password in a masked local terminal; keys and the
  Windows DPAPI profile stay under LocalAppData, outside Drive/Git/release ZIPs.
  Persistent `OKUFLOW_ANDROID_SIGNING_*` values are safe pointers only. The four
  secret-bearing `UPLOAD_*` inputs are temporary, all-or-none process values.
- Never overlap Windows GUI regression tests with browser automation or other
  desktop interaction. No owner clicks are needed to make fixtures pass. An
  interrupted focus/idle test must be rerun in a serial desktop session.
- A signed build, accepted Alpha draft and live Digital Asset Links do not
  establish closed-test rollout, policy approval or Play-signed device behavior.

- Set `OKUFLOW_AGENT_RELEASE_BUILD_DIR` to an isolated local build directory
  when the development executable is running. `scripts/agent_build.bat` applies
  the same shipping release preset there and still runs the tracked CPU/CUDA
  test presets. Never terminate the user's process to unlock build output.

## Android Companion Validation
- Run `pwsh -NoProfile -File scripts/build_android.ps1 -Lint` for phone code,
  and `python -B -m unittest discover -s android/tests -v` for host protocol
  changes. `-B` keeps Python bytecode caches off the synced checkout.
- Run the tracked Windows matrix for receiver/facade changes; the
  `phone_camera_contract` fixture checks the system decoder without using a
  real camera or adb server. Preserve saved physical-camera selection when
  adding phone entries or changing discovery.
- Follow `android/PROTOCOL.md`; never replace/kill a foreign adb server to test
  the companion. Query structured USB transport type and use its transport ID;
  unknown/Wi-Fi transports must not become cable-camera sources. The probe's
  recordings belong in local app data, not release bundles or the synced tree.
- Report local fixtures separately from real-phone quality, disconnect and
  long-soak acceptance. A debug APK is not a signed public release.

- Keep Android outputs, Gradle project caches and Kotlin persistent data outside
  Drive; see `android/BUILDING.md`. `-Install` requires one authorized device.
- Android 17 redacts developer/debugging settings. Preserve unknown versus off,
  retain older-version checks and verify with current USB/session evidence.
- Keep real-device validation in `android/DEVICE_VALIDATION.md`; do not infer
  older-phone or long-soak acceptance from the connected development phone.
- Inspect actual phone screenshots after Android UI changes. A UI hierarchy
  can report labels that render invisibly; text presence alone does not verify
  contrast, layout or visible switch/slider labels. Report offline-device
  installation or visual-validation limits explicitly.
- If Android screenshots intermittently omit system chrome as well as app text,
  compare an independent native screen recording before changing UI code. Keep
  the original evidence and report capture limitations separately from app bugs.

## USB Companion Discovery Validation
- Keep real phone discovery/startup off the UI thread. Use non-claiming probes
  and retain saved camera intent through delayed arrival; never encode a phone
  serial/model as a default. Explicit webcam selection must survive sessions.
- Fake-adb tests must set `PhoneAdbTestHooks` to a private local port with server
  launch disabled. Never probe the owner's port 5037 from unit tests or kill an
  unrelated adb server. Runtime bootstrap uses a private direct loopback bind
  only with phone-use intent and no listener/endpoint override, with a
  30-second throttle. Explicit webcam intent releases only an idle owned child.
- The pinned adb staging helper retains exact notices. Before public release,
  resolve any open source/relinking obligations in `THIRD_PARTY_LICENSES.md`;
  a local successful staging test does not clear a redistribution gate.

## Source-built adb and unattended GUI validation
- Build adb with `scripts/build_adb_from_source.ps1`; retain its complete runtime,
  notices and source/relinking ZIP. Review the generated manifest and update
  `scripts/adb-source/distribution.json` before staging a new build. The staging
  helper accepts the source build-output root, never an arbitrary SDK binary.
  Follow `docs/adb_distribution.md` when changing inputs or licensing.
- Test source-built server startup with `scripts/test_adb_source_build.ps1`.
  It owns a private loopback child and stops only that exact child; do not stop
  a foreign shared adb server to obtain a clean-server measurement.
- GUI fixtures must acquire and verify native Windows foreground focus before
  keyboard/idle assertions. Never ask the owner to click test windows to make
  assertions pass. Run GUI tests serially on a free interactive desktop; preserve
  behavior assertions and distinguish harness activation failures from app bugs.

## Native runtime preflight validation
- Release bundles deploy the /MT `okuflow_launcher` target as `oku_flow.exe`
  and the Qt application as `OkuFlowApp.exe`. Sign/hash both, deploy Qt from
  the inner application, and keep all files together. Existing-instance
  guards must recognize both names; never terminate the owner's process.
- The runtime installer and EULA are pinned in `scripts/vc-runtime/`; use
  `stage_windows_runtime.ps1` before metadata generation. Do not ship
  unreviewed installer versions or add app-local CRT DLLs silently.
  Preserve the EULA's exact bytes through its `-text -diff` Git attribute;
  line-ending or whitespace normalization would invalidate the reviewed hash.
- Keep the release batch script's Windows working copy as CRLF through its Git
  attribute; use the tracked script for packaging rather than a rewritten runner.
- Run `windows_runtime_contract` and serial `setup_assistant_interactions`
  through the tracked matrix. Do not uninstall a working system runtime or
  trigger UAC from unit tests; missing-runtime installation needs an isolated
  Windows VM. See `docs/windows_runtime_setup.md`.

## Desktop recording ownership (2026-10-12)
- The Windows app records microphone audio and video. Speech processing moves
  to okuflow.com in Chrome from the notes page; preserve recording, media-note
  callbacks, Assistant conversation history, Read/Explain, and manual TTS.
- Recording session identity belongs to `app/recording_session_info.hpp`.
  Removed feature keys have no migration and are omitted on save.

## Browser transcription notes validation
- The local notes shell and first-party handoff script are compiled from
  `common/notes_document_template.hpp` and `common/notes_transcribe_bridge.hpp`.
  Browser transcripts are versioned recording sidecars, never concurrent edits
  to the app-owned notes HTML. Keep text escaped and file identity checked.
- Validate the File/MessageChannel handoff and sidecar restoration separately
  from the native Windows notes-storage fixtures. Only use private Chrome test
  profiles and fixture folders; keep recordings and model caches outside Git.
  Browser hardware tests and Windows GUI fixtures must remain serial.

## Disposable installer guest ownership
- `scripts/test_msi_sandbox.ps1` must refuse an existing Sandbox guest or
  remote session. A Store Sandbox server daemon can remain after guest shutdown;
  query its `wsb list --raw` inventory and require an empty list rather than
  terminating the daemon. An unavailable or unrecognized inventory is a refusal.

## Windows installer and Store review validation
- Build MSI only from a clean, verified release inventory with
  `scripts/build_msi.ps1`; use standard MSI actions and retain all runtime,
  license and adb source files. Do not add WiX custom-action binaries without
  license review. Tool caches and payload snapshots stay in LocalAppData.
- Never install/uninstall an acceptance MSI on the owner's machine. Validate
  tables/extracted bytes locally and install/upgrade/downgrade/uninstall in a
  clean disposable Windows VM. Preserve user data and never force-close OkuFlow.
- Default MSI builds require trusted signing; `-AllowUnsignedPreview` must be
  visibly described as unsigned. Store review MSIX requires `-ReviewOnly` and
  is not publish-ready until packaged runtime/bootstrap acceptance is complete.
- winget generation is not catalog submission/acceptance. Publish immutable
  installer URLs only and validate exact hashes before submitting manifests.
- Render opt-in notes drafts with `python -B scripts/render_notes_preview.py`.
  It uses the real compiled template and keeps synthetic media under local app
  data; never stage that output. `scripts/test_notes_site_browser.mjs` checks
  the generated viewer and sibling website with a private Chrome profile.
  See `tests/README.md` for its explicit environment and run it before, never
  alongside, the Windows GUI matrix.

## Private Google Play CLI publishing
- Use `scripts/configure_android_play.ps1` and `scripts/publish_android_play.ps1`.
  Google credentials and DPAPI profiles stay in LocalAppData; persist only the
  `OKUFLOW_ANDROID_PLAY_PROFILE` pointer, never a key or token. Keep the dedicated
  Cloud project unlinked from billing and grant only OkuFlow testing-track access.
- Build/validate the signed bundle before publishing; bind version, reviewed hash
  and expected active track version. Do not edit the app in Console while a CLI
  transaction is active. API commit, review approval and device availability are
  separate states. Test authentication boundaries with `scripts/test_android_play.ps1`
  using its fake local identity; never substitute a live key in that fixture.

## Android diagnostic release artifacts
- Release R8 generates the actual mapping with name obfuscation, shrinking and
  optimization explicitly disabled in `android/app/diagnostic-r8.pro`. Keep the
  final `mapping.txt` and effective `configuration.txt` with their exact AAB;
  the release validator requires byte/hash equality with embedded metadata and
  readable application names. AGP's generated Compose trace alias is expected.
  Never manufacture a mapping or reuse one for another version/artifact.
- Native FULL extraction is requested, but an already stripped dependency cannot
  recreate missing native symbols. Inspect actual ELF sections and generated
  metadata; report absence and unresolved Console warnings rather than packaging
  fabricated symbols or symbols from a different binary/build ID.
- Use an isolated `OKUFLOW_ANDROID_BUILD_DIR` while a signed/uploaded release
  awaits publication. Archive the exact signed artifacts and diagnostic hashes
  before rebuilding; code 2 and code 3 are distinct Play release inventories.

## Public README and developer documentation
- Keep `README.md` as the user-facing Markdown counterpart of okuflow.com.
  Keep technical setup, build, validation and agent details in
  `docs/developer_guide.md`; read it before making changes. Repair relative
  links when moving documentation and preserve existing technical details.
- Distinguish published downloads from unreleased changes, submitted testing
  tracks from approval, and planned features from current capabilities.

## Selectable GUI test evidence (2026-10-13)
- GUI slot selectors and labels live in `tests/gui_cases.cmake`. Preserve every
  Qt data row and keep GUI cases serial; all native GUI fixtures use
  `tests/gui_test_support.hpp` to refuse concurrent desktop fixture processes.
- The tracked build/bundle scripts archive CTest through `run_test_gate.ps1`.
  Evidence is per test: `OKUFLOW_TEST_RESUME=1` reuses only passes whose own
  executable/runtime/property/environment fingerprint still matches in
  LocalAppData. Rebuild before validating, since production sources are only
  covered through rebuilt executables. Declare runtime data and helper binaries
  with absolute CTest `REQUIRED_FILES`. CPU, CUDA and bundle evidence are
  separate; desktop/hardware changes invalidate GUI evidence. Background tests
  run in parallel; keep `gpu`/`codec` and `gui` partitions serial. Never
  substitute mutable `ctest --rerun-failed` state.
- Production-source sharing in `tests/shared_sources.cmake` depends on the
  mandatory headless `shared_source_equivalence` guard. Preserve the CTest
  inventory, executable-owned MOC/RCC and code-generation flags; split a group
  rather than filtering the guard when it fails. Validate PCH ON and OFF.
- Shared source/header bytes must remain declared as runtime inputs of the
  equivalence guard, so change selection and saved evidence cannot skip it.
  Keep compact Failed/Interrupted/Unfinished fingerprint blockers even after
  reusable pass history expires. Archive CTest's detailed per-attempt logs
  before another selection overwrites Testing/Temporary.
- Use `scripts/agent_build.bat --retry-interrupted PRESET EXACT_CTEST_NAME`
  only for a recorded desktop interruption. It executes that case once, keeps
  the failed attempt and does not rebuild/publish. Application failures, mixed
  failures, timeouts and unfinished invocations remain failures until repaired;
  do not retry them until they happen to pass. See `tests/README.md`.
- Run `scripts/test_test_gate.ps1 -CMake <cmake.exe>` after runner changes.
  Its private command-line fixtures are desktop-free; they do not replace
  serial GUI acceptance or the complete shipping/CPU/CUDA/bundle gates.

## Localization validation (2026-10-13)
- Application catalogs now cover 25 UI languages; keep all 24 non-English Qt
  catalogs and Android's 24 qualified catalogs in exact source/format parity.
  See `docs/localization.md` for full canonical tags and resource qualifiers.
  Preserve saved explicit choices and the `system` preference. Do not silently
  map Traditional Chinese to Simplified Chinese.
- `language_catalog_contract` uses Qt offscreen and creates no native windows.
  It complements, rather than replaces, serial native GUI and device acceptance.
  The website's `test-transcribe-languages.mjs` runs through `npm run check`.
- Newly added translations are machine-assisted drafts; structural coverage
  does not prove fluent wording, font coverage, RTL layout or screen-reader
  quality. Keep unpublished local builds distinct from Play/Windows releases.

## Nested Windows bundle layout (2026-10-13)
- New bundles keep one root launcher (`oku_flow.exe`) and move the complete
  supporting runtime into `app/`. Deploy/hash/sign the inner
  `app/OkuFlowApp.exe` and keep its Qt plugins, runtime installer, adb notices
  and source files together. Preserve legacy flat bundle validation.
- Organize only fresh owned staging with `organize_release_bundle.ps1`, before
  metadata generation. Do not reorganize a user's existing bundle or captures.
  Run `test_bundle_layout.ps1` and the tracked Windows matrix after path changes;
  private dirty previews are not release or clean-VM MSI acceptance evidence.

## Owned adb lifecycle (2026-10-13)
- A server started by OkuFlow must run on a private loopback port as an exact
  child assigned suspended to a non-inherited kill-on-close Windows job.
  Normal shutdown and process crash clean up only that child. Existing default
  listeners and endpoint overrides remain borrowed, except for the narrowly
  reviewed legacy adoption contract below; never stop unknown/SDK servers.
- Automatic owned-server restart requires confirmed host-version failure, no
  live/starting phone session, cancellation checks and a 30-second cooldown
  across failed launches and child exits. Do not persist PIDs as ownership proof.
- Count overlapping phone occupancy reservations rather than erasing a serial
  still used by another session. Repair an idle server before a new session
  registers itself. Shutdown must prevent any later owned bootstrap.
- Disable emulator scanning, verify the private listener against the exact
  child, log its port and document WinUSB exclusivity. Five-second owned startup
  and recovery report automatic retry. Do not redirect users to Task Manager;
  preserve foreign server ownership while testing automatic recovery paths.
- Keep lifecycle validation desktop-free with `adb_owned_child_fixture`, private
  ports and scripted probes; missing fixture registration must fail the gate.

## Automatic legacy USB recovery (2026-10-14)
- Legacy adb reclamation is deliberate adoption of the reviewed OkuFlow bundle,
  not historical kernel ownership proof. Require two version failures spanning
  five seconds, phone intent, no live/starting sessions, canonical bundle layout,
  pinned manifest/adb image and launcher identity, no sibling, named serialization,
  and exact held-process/listener revalidation before every stop stage including
  after TCP connect. Unknown/SDK/path overrides never qualify; tests use private
  hashes, scan scopes and ports, never a real shared server.
- Keep published launcher and adb manifest pins aligned with verified immutable
  release evidence in docs/adb_distribution.md and docs/hardcoded_paths.md.
  Recovery UI remains automatic, with no Task Manager instructions. Ordinary
  BorrowOnly discovery has no lifecycle effects; webcam release cannot relaunch.
- Lock waits must honor cancellation, port observations must remain nonblocking,
  and server epochs must reset transport-ID budgets across adb generations.
  Shadow handback requires no raw transports and two healthy default probes.

## Translation authoring and small-agent review (2026-10-14)
- Do not translate word for word or accept unchecked automatic translations.
  Write natural target-language UI copy from the intended meaning and context.
  Prefer an established English term when a forced local calque sounds worse.
- Future translation additions and edits require read-only review by small,
  language-capable subagents, such as Luna. Split large catalogs into manageable
  chunks (about 100-200 entries); review every new or changed string, not a sample
  or a capped worst-issues list. Cover all affected app and website surfaces.
- Give reviewers the English source, target text, stable key and UI context.
  Require an OK/flagged/unread verdict for every assigned entry, with natural
  replacements and reasons for findings. Reconcile coverage against actual keys.
- Replace failed or incomplete review attempts before declaring coverage complete.
  A terminal task or a successful parity check does not prove linguistic review.
  The coordinating agent applies vetted fixes, preserving meaning, privacy claims,
  placeholders, markup, shortcuts and visible control names, then runs the
  appropriate localization validators. Keep model review distinct from native-
  speaker and rendered-UI acceptance. See `docs/localization.md` for the policy.

## Remote routine validation (2026-10-14)
- Use `.github/workflows/windows-checks.yml` for routine CPU/native GUI checks
  on GitHub's disposable Windows VM; do not install a runner on the owner's PC
  or silently fall back to local GUI execution. `run_windows_ci_gui.ps1` requires
  GitHub Actions and an unlocked interactive native input desktop.
  Prepare and verify the disposable VM's 1920×1080 display before fingerprinting
  the build; an unsupported mode is a failed prerequisite, not passing evidence.
- Keep the CPU preset/toolchain and serial native behavior assertions intact.
  Preserve logs, JUnit, gate inventories and available screenshots as artifacts.
  Manual group/two-case diagnostics are partial and cannot replace full gates;
  do not auto-repeat failed cases until they pass.
- Hosted CPU/GUI checks do not establish CUDA, Maxine, physical phone/camera,
  installer or shipping bundle acceptance. Run hardware-dependent checks on a
  suitable separate machine; owner desktop restrictions continue to apply.

## Automated Windows release orchestration (2026-10-14)
- Use `scripts/publish_windows_release.ps1` for build-to-publication automation.
  It preserves full tracked matrix/bundle gates, clean source and immutable
  versions; `-Publish` requires disposable installer acceptance and rehashed
  draft downloads. Do not substitute hosted CPU/GUI results for its hardware gate.
- Explicitly unsigned previews require `-AllowUnsignedPreview`; signed releases
  use the configured certificate. Logs/snapshots/receipts remain in LocalAppData.
  Existing instances, user captures and foreign Sandbox guests remain untouched.
  This command does not submit winget or Microsoft Store entries.
- The remote workflow additionally exercises immediate native panel movement
  at 125%, 150% and 200% Qt scale after its serial GUI group. These are separate
  environment inventories; they do not replace actual mixed-monitor acceptance.

## Agent-control offscreen screenshots (2026-10-14)
- Use the explicit `agent_control_capture` target for requested development UI
  screenshots when the owner desktop is unavailable. It forces Qt offscreen,
  uses fixture settings only and writes to LocalAppData; never treat these grabs
  as native focus, screen-reader, real-camera or release acceptance.
- Register the installed Segoe UI font before rendering with the Windows
  offscreen backend. Preserve unsuitable missing-font captures and rerender the
  actual widgets; do not repair pixels. See `docs/agent_control_ui.md`.

## MCP private validation and live compatibility (2026-10-14)
- `mcp_bridge_contract` and the independent SDK fixture use a private QCore
  application and synthetic 16×16 JPEG only. They must not target an owner's
  running app, camera, adb endpoint or live AI account.
- The opt-in `scripts/test_mcp_sdk.mjs` requires the sibling private
  `mcp-fixture` executables. Install the pinned SDK/lock from `tests/mcp-sdk`
  under LocalAppData with lifecycle scripts disabled, never under Drive.
- Development catalogs remain outside release bundles/MSI. Selecting
  `OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY` only changes the exact sibling image
  check; it is not publication, consent, a runtime listener or `v1` acceptance.
- Live provider tool-capability tests require an explicit application UI action
  and disclose their tiny no-camera request. Configuration edits must not
  trigger inference. Private protocol passes do not establish live acceptance.
## MCP packaging validation (0.3.1 development)

- Do not promote development catalogs as releases. The bundle's early cache
  gate requires accepted `v1` plus `OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY=ON`;
  setting those values does not replace Plan52's both-adapter/native/hardware
  acceptance. Current development stays `dev-camera`.
- Keep `app/okuflow_mcp.exe` in signing, build/stage/publication byte checks,
  manifest/SBOM and MSI inventory. `--build-info` is bounded offline diagnostics,
  not consent or an endpoint. Verify only reviewed Windows imports with VS2022
  dumpbin. Run `scripts/test_mcp_bundle.ps1` and `test_bundle_layout.ps1` after
  contract changes; synthetic passes do not establish release or guest acceptance.

## CUDA 12 compatibility and one-time compiler comparison
- CUDA 12.9 builds add Pascal `sm_61` for GTX 10-series testing. Keep the
  application, fixtures and cubin inventory on `cmake/CudaTargets.cmake`;
  CUDA 13 must never request a removed Pascal target.
- Use an explicit `OKUFLOW_CUDA_TOOLKIT_ROOT` and fresh cache for side-by-side
  toolkit builds. Do not replace the owner's NVIDIA driver to install a toolkit.
  CUDA 12.9 is the default; CUDA 13 requires an explicitly selected comparison
  root and must never be silently selected by normal discovery.
- The CUDA toolchain benchmark is opt-in, excluded from default builds and
  CTest. Its one-time comparison is not a recurring validation requirement.
  Compiler/cubin coverage on RTX hardware is not physical GTX 1070 acceptance.

## CI toolchain setup and runner images
- Initialize hosted MSVC with `scripts/initialize_windows_ci_toolchain.ps1` and
  Microsoft's installed VS 2022 developer shell. Keep discovery restricted to
  17.x and export its x64 environment to later steps; do not restore a deprecated
  Node-based MSVC setup action or suppress its runtime warning.
- Android host protocol checks pin Ubuntu 24.04. Review runner-image migrations
  explicitly rather than inheriting the moving `ubuntu-latest` alias.

## Bounded headless fixture registration (2026-10-16)
- Keep growing Assistant preparation slots independently selectable through
  `tests/headless_cases.cmake`; retain every Qt data row, runtime fixture input
  and the 30-second per-slot deadline. Compare registration with the built
  executable's `-functions` inventory after changing the registration helper.
- Windows Qt fixtures must capture their plain-text output with
  `QT_FORCE_STDERR_LOGGING=1`. Preserve the original failed evidence and name
  unknown causes explicitly; an empty-output CI failure followed by a local
  pass does not establish a production fix. Do not increase deadlines or add
  retries merely to make a red run green.
- Legacy recovery's native sibling scan shares a process-identity resource
  with launcher, MCP and optional native-control fixtures that create short-
  lived OkuFlow-named processes. Preserve their `okuflow_process_identity`
  CTest lock when adding related fixtures; do not relax fail-closed image
  classification to accommodate concurrent test process exits.

## Windows RC evidence prerequisites (2026-10-16)
- Resolve cached/discovered PowerShell launch aliases through
  `cmake/PowerShell.cmake`; evidence must hash the actual interpreter image.
  Keep the stable launcher cached and resolve its current image on configure.
  Keep Qt stderr logging on for registered Qt fixtures so row skips and
  assertions remain visible. Truncated passing output is incomplete evidence.
- Keep batch bookkeeping outside CUDA/Qt/OKUFLOW runtime variable namespaces.
  A resume with changed shared runtime inputs for a latest unresolved result must refuse;
  diagnose it and run an explicitly fresh gate after repair, rather than
  clearing the failure by changing the environment. Rebuilt per-test inputs
  may resume under the same shared runtime after a diagnosed repair.
- Windows Application Control refusal is a prerequisite failure. Retain the
  native error and original Code Integrity evidence; never disable security,
  turn it into a skip, or weaken a deadline to obtain passing coverage.

## Desktop-sized native visual evidence (2026-10-17)
- Keep wide/tall geometry regression inputs intact. Use the desktop visual
  cases for complete native examples on smaller/high-DPI work areas instead
  of changing OS scaling or accepting clipped screenshots.
- Preserve the capture manifest and refused-image diagnostics. A requested
  desktop visual example must fail if its full native bounds cannot be saved;
  offscreen widget grabs never substitute for native visual evidence.

## Exact-byte validation evidence (2026-10-17)
- Hash runtime, executable and declared fixture bytes afresh in the pre-run and
  post-run fingerprint passes, including resume. Persistent length/mtime caches
  are diagnostic only. Duplicate inputs may share a digest within a pass; clear
  that memo before post-run verification. Keep metadata-preserving mutation
  regressions and all unresolved-failure blockers intact.

## Ultra Clear Text development (2026-10-17)
- UCT is experimental and opt-in. Keep recognition off the UI/presentation
  path: one latest-only worker, at most one presenter readback outstanding,
  overlay reprojection on the presentation clock. Never add synchronous
  readback or block present on OCR. See `docs/ultra_clear_text.md`.
- Keep stamps from the copied pixels through jobs, results and accepted lines;
  invalidate on source/geometry changes and fail closed for uncertain, moving,
  high-risk or composite-frame text. Never fabricate math, handwriting or
  curves, and do not claim recognition accuracy without measured evidence.
- The backend is built-in `Windows.Media.Ocr`; do not add OCR models or
  libraries without license review in `docs/THIRD_PARTY_LICENSES.md`.
  `ultra_clear_text_contract` and `ultra_clear_text_windows_ocr` are
  desktop-free; native GUI, NVDA and real-camera checks stay serial and
  separate. New UCT strings still need the translation review policy.
- Treat `OnPresented` as submission preflight; only the actual present-success
  receipt may refresh presentation availability. Watchdog/failure suppression
  applies to both glyphs and reading text, including queued completions/toggles.
- Background plates and glyphs must share one drawability decision. Keep font
  and color preparation off the pan/zoom paint path, preserve ordered omission
  gaps and reading selections, and test actual pixels as well as word counts.

## Required release-capability evidence (2026-10-17)
- Bundle gates must select their strict `windows-cpu` or `windows-cuda`
  capability profile. Required native GUI, state/ownership contracts and CUDA
  kernel/cubin/GPU-fed H.264 cases need complete passes; skips or missing cases
  are blocked/not run, never hardware acceptance. Development remains explicit.
- Preserve separate capability receipts and original CTest statuses. Do not
  weaken required profiles to accommodate unavailable release hardware. Optional
  AV1 on Pascal is distinct from required H.264. See `tests/README.md` for scope
  and the independent provider/device/Maxine/installer acceptance still needed.

## Imported Android host dependencies (2026-10-17)
- Keep `scripts/android_camera_probe.py` in Android host CI and the scoped
  impact manifest. Probe-only edits require the host protocol suite without
  a Windows rebuild. Preserve that selection in `test_select_tests.ps1`.

## Preset parser baseline (2026-10-17)
- CMake 3.25 is the minimum for the checked-in schema-6 presets. Keep both
  preset entrypoints, root minimum and developer prerequisite aligned. Check
  root configuration and both manifests with the documented minimum when
  changing preset schema; the included `cmake/` directory is not a source root.

## Lecture-output readiness (2026-10-17)
- WebSocket identification is not output readiness. Poll typed OBS NotReady
  (207) within the owned startup deadline, preserving other request failures.
  Run `scripts/test_lecture_camera.ps1` after readiness changes. This private
  protocol fixture does not replace inspecting genuine virtual-camera frames.

## Packaging provenance authority (2026-10-17)
- Execute WiX only through `windows-installer/wix_tool.ps1`: verify the pinned
  archive and complete extracted tree before each invocation. Do not restore
  ambient-feed installs or reuse unverified installed shims.
- Release CUDA tier, compiler/runtime versions, architecture coverage and notice
  identity come from CMake's per-configuration `okuflow-build-facts` manifest.
  Require its successful post-build facts/executable seal, preserve
  cache/configuration and built/staged-byte checks; shell CUDA paths
  are inputs to configuration, never independent release facts. Run the private
  `scripts/test_release_provenance.ps1` fixtures after changing this contract;
  they do not replace shipping builds, hardware or disposable-VM acceptance.

## Transactional bundle publication and generator preflight (2026-10-17)
- `build_release_bundle.bat` requires Ninja, initializes only VS2022 x64, and
  forces the compilation database required by shared-source equivalence. Use
  `--check-toolchain` for read-only tool/Qt/cache checks. Unsupported generators
  and incompatible caches require a fresh `OKUFLOW_BUNDLE_BUILD_DIR`; never
  delete an existing cache as an automatic repair.
- Use unique `dist/OkuFlow.staging-<guid>` generations and the tracked
  `publish_release_bundle.ps1` transaction. Before renaming a prior bundle,
  require its exact manifest inventory/hashes, no owner output/captures,
  unknown files/directories, reparse points, running images or file locks.
  Primary refusal may select exactly `OkuFlow2`; secondary refusal is fatal.
- Exchange only same-parent directory names, retain the complete prior
  `*.previous-<guid>` generation, rehash after switching, and roll back a failed
  rename/verification. Never copy or recursively delete an old bundle to force
  publication. Preserve transaction-failure staging and recovery evidence.
- Run `test_bundle_publication.ps1` and `test_bundle_toolchain.ps1` for private
  transaction/preflight regressions. They do not establish compilation, native
  GUI, hardware, installer or full release acceptance.

## Exact notes recording identity (2026-10-17)
- Keep the immutable `.okuflow-media.json` marker with its finalized Original
  recording. Generate/reuse it only in the serial notes worker; never hash a large
  recording synchronously on the native UI thread. Existing mismatched markers
  are refusals, not permission to replace a recording's UUID.
- Bind granted directory resolution, fresh bounded file fingerprints, handoff
  messages and version-2 transcript sidecars to the same full identity. A folder
  name, basename, byte size or modification time alone cannot authorize reuse.
  Address transcript sidecars by UUID (`.okuflow-transcript-<mediaId>.json`), so
  an old awaited writer cannot overwrite a successor recording at the same video
  basename. Do not trust or automatically migrate basename-only legacy sidecars.
  Preserve immutable save snapshots, per-media write order and absolute media PTS.

## Bounded performance evidence (2026-10-17)

- Use the excluded `assistant_chat_benchmark` target only as explicit offline
  Qt-offscreen CPU evidence; preserve its raw JSON and invalid anchor/selection
  results. Controlled `DeadlineWorker` sleeps are not Maxine measurements.
- `OKUFLOW_MAXINE_PROFILE=1` retains at most the last 16,384 host-call samples
  per worker and emits a teardown summary. Keep omitted-earlier counts and
  censored timeout/throw outcomes visible. No per-frame file I/O is permitted.
- Native camera/desktop profiling still requires an explicitly available
  session. Host waits, GPU completion, sensor latency, pan-to-present and stale
  result/queue traces are distinct evidence. See `docs/performance_diagnostics.md`.

## Private Maxine lifecycle and navigation validation (2026-10-17)

- The `maxine_lifecycle_cuda` and `maxine_navigation_cuda` fixtures use privately
  built fake SDK DLLs, a real CUDA device and, for navigation, private D3D12
  textures without a native window. They are desktop-free `gpu` cases, serial
  with other GPU tests. Keep exact helper DLLs in CTest `REQUIRED_FILES` and
  outside release staging; never substitute the owner's installed runtime.
- Fake SDK transitions validate ownership, failure handling and same-frame
  fallback admission, not actual model pixels, optical quality or performance.
  Preserve separate real-runtime/pan-to-submission acceptance and the full
  independent-producer follow-up. See `tests/README.md` for scoped commands.

## Bounded bundle preflight and compiler identity (2026-10-17)
- Reject generator arguments and incompatible bundle caches with the explicit
  configuration-only check before initializing VS. That check must never claim
  toolchain or Qt readiness. Valid batch invocations still initialize the real
  installed VS2022 x64 shell and run the complete preflight before dist writes.
- Never reuse inherited version/architecture declarations as toolchain proof.
  Clear inherited `VSINSTALLDIR` before the selected Microsoft initializer;
  verify the resulting root against installed 17.x C++ discovery and the active
  compiler's exact Hostx64/x64 path. Keep environment-spoof refusal coverage.
- The bundle fixture performs exactly one real initialization and asserts early
  refusal for all invalid configurations. Keep its 60-second limit, named case
  progress and original failed evidence. Do not raise the deadline, fake VS or
  remove assertions to mask a slow setup path.

## Advisory pressure and native read progress (2026-10-17)
- Android METADATA is lossy advisory state. Preserve the bounded control reserve
  for ACK/STATE/ERROR and keep reliable/video overflow and stalled-write failure
  policy. Validate the actual `OutboundQueue` helper with JVM tests; do not
  substitute those checks for physical USB transport acceptance.
- Native camera-read watchdogs measure only an armed `ReadSample`, using working
  time that excludes system suspension. A timeout revokes delivery and enters
  existing split stop; it never frees a blocked driver or permits abandoned
  session reuse. Test late returns and cancellation without a physical camera.
- Reader adoption is not restored video. Physical reconnect completion requires
  successful frame presentation; repeated zero-frame attempts retain their
  original retry deadline, and expiry also cancels pending/adopted attempts.
  Keep the actual-app offscreen and orchestrator regressions separate from
  native GUI and hardware acceptance.

## Direct bundle toolchain discovery and diagnostics (2026-10-17)
- Resolve the installed VS2022 root through `vswhere -property installationPath`
  and its fixed `Common7/Tools/VsDevCmd.bat` path. Do not recursively scan an
  entire toolchain to locate this known entrypoint.
- Keep compiler initialization and full identity/Qt checks. Scope Microsoft's
  auxiliary telemetry suppression to the batch environment; do not set a user
  or machine preference. Stream child setup output into CTest and private case
  logs so timeout evidence retains the last completed stage.
- Preserve the preflight's 60-second deadline and all refusal/no-mutation
  assertions. Let tracked runners own setup instead of redundantly initializing
  the caller shell, which can overflow Windows batch environment lines.

## Independent Maxine producer ownership (2026-10-17)
- Keep model loading, inference, GPU completion and teardown on the private
  producer worker. One in-flight slot and one overwriteable CPU descriptor;
  never retain a borrowed scene/processing-buffer pointer as a queued job.
- Publish only the complete current camera/processing/settings/request ticket.
  Use the existing graphics fence and retain private output through its exact
  consumer-copy event. Invalidation/shutdown never joins an SDK call on UI;
  unknown completion retains the complete private state until process exit.
- Async readiness uses the existing presentation clock through
  `hasAsyncSceneWork`; it is neither camera processing nor navigation input.
  Preserve immediate same-frame capture pairing and enhancement authority for
  annotations, UCT and agent observations. See the Maxine producer policy doc.
- Native settings changes must immediately advance processing authority and
  revoke pending neural output independently of Assistant/MCP. Ordinary settings
  invalidation preserves healthy models and failure/performance latches;
  explicit reset alone permits retry of the same failed configuration.
- Run the three desktop-free Maxine CUDA fixtures and scheduler/UCT regressions
  after ownership changes. Strict Windows-CUDA release acceptance requires all
  three; private fake SDKs prove neither vendor quality nor native pan latency.

## Relocated Qt bundle preflight (2026-10-17)
- Bundle Qt discovery honors explicit `QT_PREFIX` / `Qt6_DIR`, then CI's
  `QT_ROOT_DIR`, then the local default. Invalid explicit overrides must fail;
  keep the complete deployment/license checks and selected-prefix diagnostic.
- CTest supplies the SDK and source root it configured. The preflight fixture
  uses a private junction to that actual SDK, never fake Qt tools/licenses, and
  declares all module SPDX and notice files as runtime inputs. Preserve the
  single real VS initialization, refusal assertions and 60-second deadline.
- Hosted Qt setup includes only matching Multimedia sources for the FFmpeg
  notice and uses a source-aware cache key. Do not assume a compile-only SDK
  installation contains the release preflight's source/license inventory.
- Archive SDKs may lack the online installer's license folder. Preserve the
  fallback to the matching Multimedia source's `LICENSES/LGPL-3.0-only.txt`,
  its exact staging bytes and runtime-input fingerprint. The relocated fixture
  excludes only the optional SDK `LICENSES` directory and must assert the real
  source notice is selected; never manufacture a license to make it pass.

## Focused bundle publication scenarios (2026-10-17)
- Preserve all 13 synthetic publication scenarios across the Exchange,
  Refusals and Rollback CTest groups (3/7/3 cases). Each keeps its 60-second
  limit, named progress, exact coverage-count assertion and publisher runtime
  inputs. Keep the shared process-identity lock; never overlap the hidden
  running-image fixture with other identity-sensitive checks.
- Direct `test_bundle_publication.ps1` defaults to All. Splitting the cumulative
  fixture is not permission to omit locked/running-image, owner-data, exact
  inventory, reparse, rollback or late-change assertions. Production publication
  safety behavior is unchanged; native installer/release acceptance is separate.

## Optional OCR component validation (2026-10-18)

- Prepare only pinned local OCR inputs with `scripts/prepare_uct_runtime.ps1`;
  TensorRT is an explicit optional external SDK. Keep model/runtime archives,
  engine plans and original notices under LocalAppData. CMake/app startup must
  never download or silently bundle them. Review a complete license/SBOM
  inventory before including these optional files in a public installer.
- Preserve the Qt-free public CUDA interop header through forward declarations;
  snapshot/download implementations may use QtGui. Keep the Maxine navigation
  fixture's actual pool/kernel dependency aligned with production.
- Run model-free `uct_paddle_processing`, serial GPU `uct_paddle_cuda`, and UCT
  scheduler/controller contracts after inference/ownership changes. Real pinned
  CPU/TensorRT graph rows require explicit opt-in and are separate acceptance.
  Do not repeat cold model compilation as a routine benchmark. See tests/README.
- Keep GPU neural tensors resident, retain exact owned inputs through stream
  completion and quarantine unknown completion. Independent decimated validation
  readback is intentional: slow inference must not stall motion checks. Neither
  branch may block the UI, overwrite a leased slot or borrow presenter memory.
- Native GUI/device availability remains governed by the desktop rules below;
  offscreen model/kernel passes do not establish live camera or installer quality.

## Diagnosed prerequisite repair evidence (2026-10-18)
- Never remove or edit a test ledger to clear a blocker. After diagnosing and
  repairing a cause outside a test's fingerprinted inputs, use the tracked
  runner's explicit `-RecheckAfterRepair` with a concrete repair reason. It
  requires exact `-Test` names of matching Failed/Unfinished blockers, preserves
  original logs and per-case/capability repair provenance, and refuses a second
  unchanged failing recheck. Ordinary resume/fresh modes still refuse retries.
- This mode neither grants desktop availability nor weakens assertion, runtime
  fingerprint, capability, packaging or release requirements. Validate runner
  changes with `scripts/test_test_gate.ps1` before applying them to project gates.

## One-time media output-buffer measurement (2026-10-19)
- `media_gpu_scene_cuda` keeps the one/two-output ownership and effect regressions
  in the normal gate. Its `--benchmark-output-buffers` entry point is an explicit
  one-time experiment, never routine CI or a repeated benchmark requirement.
- Serialize measurements with other GPU work, preserve raw/pilot/failure output
  in LocalAppData, and report GPU drawing completion separately from native
  input, decoder seek/preroll, VSync or screen latency. See
  `docs/media_output_buffer_benchmark.md` and `tests/README.md`.
- Prefer exact resource-owner completion for fixture teardown; do not wait for
  unrelated persistent graphics-library threads or terminate an owner's app.

## Paired temporal UCT analysis (2026-10-19)
- CUDA Temporal Smooth is an explicit exception to the earlier blanket UCT
  composite pause. Keep a neutral post-geometry, pre-appearance/zoom EMA for
  OCR, paired with the exact current single frame for references, fingerprints
  and motion checks. Display composite provenance remains unchanged for
  annotations and observations. Extra Stable uses a capture-owned acquisition
  record for exact held pixels; recovery crossfades and unnamed composites
  cannot confirm or display text. Debug composites still pause.
- Do not let agreeing smoothed OCR jobs validate their own fading pixels. New
  live/advancing lines need current-frame proof; keep glyphs, reading text and
  screen-reader eligibility together. Preserve blend provenance and paired
  ownership, atomic failure/quarantine and the whole-pair analysis cap.
- Saved-frame replay must restore pre-frame analysis history without blending
  a held sample twice or resurrecting history across an explicit reset. Test
  actual GPU halves, replay/seek/toggle resets and CPU fallback transfers.
  Native GUI, screen-reader and camera acceptance remain separate.

- Held images remain one acquisition regardless of presentation sequence.
  Never manufacture independent OCR evidence from repeated held copies. Keep
  clean Original recording before hold/effects and do not route automatic
  Extra Stable recovery through decoder pause/seek/jump.
## Explicit UCT recognition-width experiments (2026-10-21)

- Keep narrower canvases opt-in until same-source quality and warm performance
  comparisons support promotion. Prepare only explicitly selected missing
  profiles once, then benchmark with the inference-only SDK and unchanged cache.
- Preserve fixed-width keys/plans, exact crop work and original strict failures;
  report the owner's 5% word-difference tolerance separately from ground-truth
  accuracy, dispositions, omissions and the ten-pixel geometry allowance.
- Keep all inference input/retirement ownership and zero image-egress contracts.
  See `docs/uct_recognizer_width.md`; no RTX 3050/120 FPS claim follows from Ada
  measurements or padding fraction alone.

## Optional embedded selected-video validation (2026-10-21)

- `prepare_ffmpeg_playback.ps1` builds pinned LGPL FFmpeg/BSD dav1d development
  libraries in LocalAppData only. Keep Qt DLLs, owner-installed FFmpeg binaries
  and release staging untouched. SDK configuration plus explicit
  `OKUFLOW_VIDEO_BACKEND=ffmpeg` are required; Media Foundation stays default
  and owns all archive/growing-recording paths. See docs/video_ffmpeg_playback.md.
- Run focused embedded/native codec checks and same-source diagnostics only
  after parent gates exit. Use the tracked runner/global lease; no GUI, browser,
  audio endpoint or owner process is needed. Retain original failed receipts,
  strict PTS/PCM identity and held-file/consumer-lease retirement assertions.
  Code-value HDR comparisons are not tone-mapping acceptance or deployment.

## Owner workstation work storage (2026-10-21)

- On the owner's PC, put new large Windows build, SDK, test-evidence and scratch
  folders under `Z:\Projects\OkuFlow\local-work-data`. Keep the main Y: checkout
  in place. This is a workstation-specific override, not a CI requirement.
- Existing LocalAppData work directories and inactive T3 checkouts have C:-to-Z:
  junctions. Preserve them and the original Git/T3 path registrations. The active
  checkout's roughly 20 MiB of source remains on C: because Windows holds its
  directory open; its large `build` directory already redirects to Z:.
- Before configuring a new C: T3 checkout, inspect its build path and put new
  build output on Z:, retaining the preset's expected path through a junction.
  Do not replace an existing populated build directory without inspection and
  verified relocation. Use supported output-root overrides for separate builds.
- Keep private signing/Google Play credentials and app state on C:. Never remove
  test ledgers or rerun cold model preparation because of this storage move.
  Consult `docs/local_work_storage.md` and retain migration evidence.

## Supplemental active regional UCT (2026-10-21)

- `OKUFLOW_UCT_REGIONAL_OCR=1` enables current-input region OCR in addition to
  baseline full reads. Shadow-only remains observation-only. Never defer full
  deadlines using the unaccepted character verifier or count held copies as
  acquisitions. Recheck plan/authority/slack on the worker and protect queued
  full priority. Keep one region per full interval and bounded call/overrun
  metadata; measured costs cannot promise hard real-time completion.
- Only full results update planner occurrences. Preserve complete crossing and
  outside lines, their masks and exact per-observation pending tickets. Keep the
  paused selected/real-neighbour detail pair and 32-attempt cap. See
  `docs/uct_active_regional_ocr.md`; model-free
  scoped tests do not establish GPU, native UIA or physical-camera acceptance.

## Regional boundary ownership (2026-10-22)

- Admit only complete detector unions inside the backend's shared eight-pixel
  crop-edge exclusion plus two input pixels of detector-jitter slack;
  image-border edges have no extra margin. Clipped candidates supply no
  pending words or observations. Keep crossing/outside lines and masks verbatim
  without renewing proof or requiring a new probe for an unobserved region.
  If a rejected crop candidate touches an otherwise owned line, leave the whole
  optional admission unapplied before changing accepted or pending evidence.
- Expired optional slack yields without inference, rather than starting a more
  expensive full read. Only actual regional durations update its cost estimate.
  Baseline full work, independent pixel checks and freshness remain unchanged.

## Nonblocking timing diagnostics (2026-10-22)

- Concurrent timing producers may legitimately drop a try-lock record. Verify
  cached surface publication identities directly; negative-event and complete
  latency claims still require a lossless, fully closed trace window. Preserve
  drop accounting and original failed evidence instead of making tracing block
  recognition or silently treating incomplete coverage as complete.

## Explicit local BF16 recognition (2026-10-22)

- Only the app's private production factory reads
  `OKUFLOW_UCT_LOCAL_BF16_MANIFEST`; diagnostics/public TensorRT factory stay
  pinned FP32. Require the reviewed source/graph/plan/device/runtime identities,
  medium/medium N8 fixed1024, FLOAT I/O and explicit prepare=false. A nonempty
  selector is cache-only for both networks and fallback; never prepare/convert,
  mutate baseline caches or emit offline receipts from the application.
- Keep one GPU candidate/worker and the existing preparation bridge index.
  Only typed pre-inference artifact/policy rejection may select cached FP32;
  CUDA/API/inference faults preserve quarantine and normal failover. Describe
  BF16 recognition as a graph with mixed BF16/FP16/FP32 arithmetic, never pure
  BF16. Preserve the original quality-comparison failures and distinguish the
  owner-approved local choice from other users' defaults or hardware claims.
  See `docs/uct_local_bf16.md` for pins, bounds and acceptance limits.

## Paused saved-file sparse cadence (2026-10-22)

- An exact native-issued saved-file selected sample may use its validated
  positive sample duration for following-neighbour OCR proof: ordinary 250 ms,
  at most 2500 ms, with integer 100-ns comparisons. Unknown/invalid duration,
  earlier samples, recording archives, RAM recap and live/advancing media keep
  the ordinary policy. Never derive this allowance from wall time, repeated
  held pixels, job IDs, a cache copy or retimed frames.
- Obtain the opaque descriptor only through `VideoPlayback::CorroborationPolicy`
  after exact issuance/control-block/source/generation checks. Seal native
  duration/PTS/sample identity; bind the descriptor through immutable selection,
  reference, worker line/word checks and model admission. A metadata/descriptor
  change revokes old proof. Preserve source fencing, distinct source digests,
  text/geometry agreement, NCC/visibility/refinement thresholds and all budgets.
- Replay captured button results through the actual controller, model, paint
  and positioned accessibility path. Reading-pane text or accepted word counts
  alone do not establish visible glyphs or positioned text. CPU-backed resident
  oracles and Qt accessibility geometry do not establish real GPU inference,
  native UIA-client, camera or release acceptance. Keep private captures outside
  Git; see `docs/uct_initial_paused_frame.md` for exact provenance and scope.
- Configure optional controller replay with an absolute
  `OKUFLOW_UCT_CONTROLLER_REPLAY_PROOF` FILEPATH. The CTest case must declare
  that proof and both actual source images as absolute `REQUIRED_FILES` and
  fix its input environment to the configured value. Keep earlier undeclared
  diagnostic failures separate from later corrected-capture acceptance.
- The optional native sparse 0/2-second case uses an explicit
  `OKUFLOW_TEST_VIDEO_PAUSED_FILE` FILEPATH (empty by default) and the existing
  codec opt-in. Declare the actual private clip and helper/test bytes as CTest
  runtime inputs; measure native sample duration instead of borrowing software
  extraction metadata. Missing duration/neighbour must fail without expanding
  policy. Keep one Open, bounded paused waiting and no Play/GPU/audio endpoints.

## Role-aware precision experiment preparation (2026-10-23)

- Schema-2 role/profile/precision descriptors remain separate from the reviewed
  schema-1 BF16 approval. A manifest cannot approve itself; require an independent
  exact catalog plus source/config/dictionary/graph/plan/device and builder TRT identity.
  Never rekey or alias old plans. Keep loader status separate from validated
  engine selection; commit both engines and effective width/capacity/ABI together.
- No engine retry after genuine CUDA/API/inference uncertainty. Only typed
  pre-inference artifact rejection and confirmed completion permit a separately
  approved fallback pair. Preserve lookahead, integrals, leases and quarantine.
- Offline readiness is not compute permission. Follow root's finite START slots,
  preserve first failures/source-to-binary receipts and measure actual production
  worker/callback plus independent quality. See docs/uct_precision_artifacts.md.
- Owner's revised cache policy keeps driver/API/build/hash only in provenance.
  New keys omit driver; admission accepts runtime tuples >=11.4.0.106 rather
  than exact loaded TRT version/hash. Builder metadata remains plan-bound.
  Keep compiled ABI11 loading and factory/deserialization/profile authority;
  numeric admission does not guarantee newer-runtime serialization acceptance.
  Existing caches are read-only legacy lookups, never migrated or overwritten.

## Private standalone paused diagnostics (2026-10-22)

- `OKUFLOW_UCT_PAUSE_STANDALONE=1` selects standalone model admission in the
  private paused-video diagnostic. Its historical neighbour OCR is comparison
  data only and must never supply admission evidence in this mode. Label those
  extra diagnostic jobs separately from the app's selected-only paused path.
  Retain exact source hashes, fallback/preferred outputs and actual per-word
  paint decisions; a reading-pane count does not establish visible replacement.

## Exact host UCT merge replay (2026-10-23)
- Keep the bb53 merge and geometry oracle bodies frozen in the dedicated
  `tests/uct_merge_baseline_bb53*.cpp` translation units; a candidate edit must
  not update its reference. Compare every ordered word field after every call.
- `uct_merge_replay` is model-free CPU correctness coverage. Explicit timing
  uses five warmups and 31 alternating ABBA/BAAB blocks, preserves raw visits
  and serialized inputs, and does not establish GPU/full-worker or FPS gains.
  Historical final-output regrouping is not an original merge call trace.
- Respect any parent's withheld START and finite compute grant. A SCOPED CPU
  replay never replaces shipping/CPU/CUDA acceptance. See
  `docs/uct_merge_optimization.md` for source identity and limits.

## Public BF16 v2 integration (2026-10-23)

- Keep production corpus approval empty until actual strict golden execution and activation. V2 commits a fixed marker last under native read-only directory/file pins. Recognized incomplete recipe/nonce generations remain untouched and count toward the bound; no v1 migration, overwrite, repair or directory rename. See docs/uct_public_bf16.md. Source composition and model-free tests do not establish public model quality or packaging acceptance.

## Owner BF16 app precision decision (2026-10-23)

- Both selected GPU detector and recognizer tiers prefer BF16 with selected-tier
  FP32 fallback for supported loading/build/artifact refusals. The owner accepts
  normal ML errors and reduced accuracy. Perfect human text, BF16/FP32 numeric or
  text equality, golden-corpus passes and quality approval are NOT app startup or
  precision-selection requirements. Keep accuracy comparisons diagnostic only.
- The historical public-v2 exact-text attestation remains a separate optional
  experiment; its empty production corpus catalog must not disable normal BF16
  model selection. Retain verified graph/config/dictionary identities, actual
  tensor/profile compatibility, bounded preparation, cancellation and resource
  ownership. Actual CUDA/API/inference faults or unknown completion still retire
  the backend; do not retry an engine on an uncertain stream.
- Preserve saved model tiers, tiny recognizer N1 service, CPU FP32 and the one
  worker/two-slot pipeline. No automatic live/Freeze model switch is authorized.

## Selection-scoped evidence reconciliation (2026-10-24)

- The tracked test ledger preserves results outside the current selection.
  External postchecks must validate each selected case and its fresh attempt;
  never require the cumulative ledger row count to equal the selection count.
  Preserve a wrapper postcheck failure independently of passing native cases.
  Reconcile the archived attempt/logs without rerunning already-passed GUI tests.

## Owned compiler telemetry cleanup (2026-10-24)

- A successful build may leave the selected compiler's VCTIP uploader inside its
  owned Job. Allow a bounded natural drain, then use
  `scripts/windows-build/owned_job_cleanup.ps1` with an independently frozen
  exact image path and hash before classifying that specific survivor. Never
  authorize cleanup from a name or PID. Unknown or incomplete membership keeps
  exit 125; nonzero work and watchdog results remain failures. The outer
  controller still owns exact-Job cancellation and zero-members/runner release.
- Preserve old exit-125 receipts. Native fixtures use the complete Job inventory,
  including console-host children, and must leave an unrelated sentinel running.
  No registry, installed compiler file or foreign process change is required.

## Pinned OCR release staging (2026-10-24)

- Use `stage_uct_payload.ps1` and the tracked distribution manifest for all
  original/BF16 model tiers and private ORT/TRT/NVRTC files. Supply prepared
  physical source roots; never stage developer plans/caches or download during
  packaging. Preserve exact licenses, source catalogs and converter provenance.
- Generate metadata only after the final receipt marker. Verify both content
  seals and every file against the current catalog; retain per-file SPDX package
  ownership, generated model notices and unchanged root license bytes. Use the
  model-free staging/metadata contracts before the full release pipeline.
- Sandbox acceptance now covers a clean candidate install/uninstall before the
  previous-version upgrade sequence. Neither script fixtures nor extracted-byte
  checks substitute for that guest run or native GPU runtime acceptance.
