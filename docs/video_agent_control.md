# Selected-media Assistant and MCP controls (development)

The `dev-full` and `v1` finite catalogs include seven selected-media tools;
these editions now contain 43 tools. Earlier development editions keep their
existing inventory. This is development integration, not V1/native/hardware
acceptance evidence.

| Tool | Native action and receipt |
| --- | --- |
| `video.state.get` | Read opaque selected document identity, selection/request generations, actual media PTS, independent recording state and capability flags. |
| `video.pause` | Hold the actually presented scene. A provisional live hold reports a capture-clock mapping and null media PTS until recorder admission supplies its time. |
| `video.resume` | Play forward from the selected held position. Waiting and EOF remain explicit; this does not jump live. |
| `video.seek` | Request absolute milliseconds within the displayed document. Completion reports actual selected frame PTS separately. |
| `video.jump_latest_end` | Follow the active recording's latest scene, or pause at a saved video's last decodable frame. Receipt modes distinguish `latest_live` and `file_end`. |
| `video.return_to_camera` | Return to the live camera view without stopping an independent recording. |
| `video.save_frame` | Save the admitted selected document/frame/view derivative under existing photo consent. The original video stays unchanged. |

The table uses internal action IDs. MCP/Codex tool names replace dots with
underscores: for example `video.state.get` is `video_state_get`, and
`video.save_frame` is `video_save_frame`.

Every mutation requires the exact opaque `documentId` and
`selectionGeneration` returned by state. These identifiers are process-local;
no tool accepts a path, URL or filename to open. `video.open` is explicitly
native-UI-only discovery metadata because this build has no approved file-handle
handoff. The keyboard-accessible native chooser and native drop action own file
selection.

The shared `AppControlService` retains context grants, exact approvals, bounded
budgets, cancellation and native-user precedence. `ControlPlan.requiredFields`
adds revision dependencies without writes or undo entries. Selected-media and
view actions depend on `video.selection`; a native open/seek/source transition
advances its revision and invalidates previous coordinates through
`SourceChanged`. Re-read `video.state.get` or `app.state.get` after `stale_state`.
View undo entries retain their source epoch, so a native seek/open prevents
restoring coordinates from an older selected scene. Undo's exact-owner temporary
camera restoration can retain its view inverses only when the captured selected
media identity and generation still match; it cannot bypass a native video
selection or a user's intervening field edit. File controls do not acquire
source-device authority, stop another owner's
recording, or redirect Original recording admission.

Admitted operation authority survives normal embedded turn completion; explicit
cancellation, owner retirement, disabling control and applicable photo-grant
revocation remain effective across later turns.

Native requests return a durable pending operation. `app.operation.get` reports
completion only after native selected-frame presentation, or actual image commit
for Save frame. Pending, waiting, unavailable, stale, canceled and timeout
outcomes do not claim Saved or an exact requested timestamp. The operation
retains document, selected PTS and request generation; requested seek time is a
separate field. A Save operation retains its original admitted scene identity
even when a later native selection changes. The 30-second observation deadline
bounds operation tracking; it does not claim to terminate a blocked native call.

Observe uses the selected view, including immutable media identity and media
PTS, under its existing frame grant. A seek/open revokes previous frame and
coordinate authority. File stills have no camera frame-age or camera-FPS claim;
hidden camera processing/readbacks cannot replace their pixels. User-invoked
Read/Explain keeps its separate existing authorization. `capture.photo.take`
remains next-live-camera semantics and is unavailable while selected media is
being displayed; use Save frame for the displayed scene.

Selected playback uses the same production processing settings as the camera
when CUDA processing is available: blur/sharpen, Text Clarity, auto contrast,
stabilization, keystone, temporal smoothing and Super Resolution, in addition to
pan/zoom, orientation and reading colors. Native controls and MCP setters use
`SelectedMediaGpuProcessingAvailable` for capability; a CPU decoder can still
feed GPU processing. Effective values instead use the backend of the last
successful display receipt (`gpu_selected_media` or `cpu_selected_media`). A
preparing GPU scene reports `no_processed_frame`, rather than treating a hidden
camera's output as effective video processing. `SelectedMediaImageFieldAvailable`
retains the CPU compatibility whitelist and excludes camera Debug. Debug and
live-camera AutoAdjust remain outside selected-media processing. Unsupported
writes return `unavailable`; unsupported effective values remain null with
`selected_media_unsupported_effect`. Keystone Step and SR performance override
route to the independent media helper; UCT model preferences keep their own
native/backend reporting.

`recording.canvas` remains legacy enum metadata only. Its runtime value reports
`source`, `writable=false`, `unsupportedReason=deprecated_original_only`, and no
persisted value. It is absent from the setter schema; direct legacy setter calls
return `unavailable` before planning any microphone change or recording stop.
Original dimensions always follow the selected camera mode. Microphone preference
changes retain their exact-session approval and deferred-write fences.

`video_agent_control` is a desktop-free synthetic target/catalog/policy fixture.
It verifies finite schemas, no path-opening tool, selection revision fences,
photo authority, atomic legacy-canvas rejection and safe requested/actual-time
receipts. Production playback, native controls, visible frame equality,
accessibility, camera/microphone and provider validation remain separate gates.
No third-party dependency is introduced.
