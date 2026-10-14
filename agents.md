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
- Use `scripts/agent_build.bat` for the tracked Windows release/CPU/CUDA test
  matrix. Do not use or recreate machine-specific test runners under the
  gitignored `build/` directory.
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
  runs the CPU and CUDA CTest presets. Its PASS/FAIL summary is the normal
  pre-submission gate for code changes that touch shared behavior.
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
  `OKUFLOW_TEST_RESUME=1` reuses only matching source/build/runtime/environment
  inventories in LocalAppData. A changed inventory needs a fresh built gate;
  CPU, CUDA and bundle evidence are separate. Desktop/hardware changes need a
  fresh gate too. Never substitute mutable `ctest --rerun-failed` state.
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
