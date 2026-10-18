# Accessible video and UCT implementation

This development change implements the owner's UCT/accessible-video brief on
top of merged PR 8 (`d23f871`). The attached brief is a design input; its
reported defects are verified against production code before repair.

## Recording and source contract

The owner's correction supersedes the brief's concurrent foreign-file playback
rules. Opening a saved video first stops and finalizes an ongoing recording.
Starting a new recording leaves saved playback and returns to the live camera
only after the first successful Original-frame admission. Encoder startup
failure preserves the selected video, cursor and playback intent.
Same-session rewind remains available through the current Original archive.
Stopping that recording preserves the selected position and playback intent.

The bottom transport always names its source. Its forward button follows the
live camera for the current recording, or holds the last decodable frame of a
saved file. Resume continues from the held position. An ordinary live camera
without a recording has no timeline or background history collector.

New video recordings contain one clean full-source Original, including optional
microphone audio. Necessary conversion and orientation are allowed; viewport
crop, reading colours, UCT glyphs, neural enhancement and annotation ink are not.
Paired photos and existing paired-video notes/transcripts stay compatible.

## Ownership and review plan

- Separate Original admission, selected-scene presentation and UCT admission.
  Recorded bytes and accepted capture timestamps remain distinct from the
  displayed cursor, wall-clock playback position and verified seekable tail.
- Decode with one bounded native Media Foundation worker. Pending opens and
  seeks replace older requests. Retained frames own their pixels; old tickets
  cannot replace a newer source or seek. UI cancellation never joins a native
  encoder or decoder call.
- Observe the existing fMP4 writer, fence mutations before writes and index only
  complete proven sample payloads. Concurrent native read is a feasibility gate,
  rather than an assumption based on the file extension or requested GOP size.
- Give immutable UCT selected-frame authority, a separate job deadline and
  distinct-neighbour evidence. Completed unchanged frames need no periodic OCR;
  pan and zoom reproject the retained scene artifact.
- Preserve rejected OCR spans as ordered omissions and protect their geometry
  from accepted replacement plates. Retain observation mappings by insertion
  age rather than random UUID order.
- Keep transport inside the window layout below the render view, with named
  keyboard-accessible native controls and a drag-stable timeline.

Opus and Muse Wonderful independently reviewed the initial design. Both found
the same writer/notes double-path migration trap and the need to test growing
fMP4 reads before claiming recording rewind. Implementation reviews follow the
native vertical slice.

## Validation boundaries

Development uses scoped headless/offscreen checks, synthetic native media and
one tracked release/CPU/CUDA integration matrix with GUI excluded while the
owner uses the desktop. New recordings are decoded through the production
player; active archive reads are checked while the writer remains active.
Failures remain in the tracked evidence ledger. These checks do not establish
screen-reader, optical-camera, installer or public-release acceptance.

## Local validation record (2026-10-18)

The tracked `scripts/agent_build.bat` completed with
`OKUFLOW_AGENT_EXCLUDE_GUI=1`, isolated Release output, CUDA 12.9.86, Qt 6.12.0
and VS 2022. Its final summary is PASS for Release compilation, the CPU suite
and the CUDA delta, explicitly marked partial because desktop tests were
excluded. Fingerprint reuse retained unchanged passing evidence; rebuilt
inputs received new recorded attempts. Earlier failures remain archived.

| Check | Result and scope |
| --- | --- |
| Shipping Release configuration | Compiled successfully with CUDA 12.9. |
| CPU CTest selection | 98 cases passed, zero unresolved. Two cases contain optional skipped Qt rows: `recording_manager` and `mcp_bridge_contract`. The explicit Original-native case ran separately within this selection; the optional private MCP SDK integration was not claimed. |
| CUDA delta | 10 cases passed, no skipped rows or unresolved cases. Includes the three synthetic Maxine lifecycle/navigation/async fixtures and the UCT CUDA fixture. |
| Actual-app native target, Qt offscreen | 43 Qt rows passed. Includes first-frame startup failure, source transitions, exact held-frame authority, repeated paused-poll admission, Save receipt identity and Stop/Undo restoration. No native desktop, device or provider was used. |
| Shared-source equivalence | Passed with PCH enabled in the tracked matrix and disabled in the isolated native fixture configuration. |
| Positioned UCT accessibility | 24 Qt rows at each of 100%, 150% and 200% scale passed in three offscreen CTest cases. These test Qt's accessibility interfaces and paint geometry, not the Windows UIA bridge or a real screen reader. |
| Native synthetic media | Active-before-Finalize AVC/AAC and AV1/AAC decode, continued recording/rewind, sparse-video audio tail, actual EOF, Waiting/resumption pacing and 4K lease pressure passed. The twelve-frame 4K case peaked at 99,532,800 decoded bytes within the unchanged 100,663,296-byte quota. |
| Localization | All 24 non-English catalogs compiled with 872 finished entries each. Small-agent review covered all 1,440 new/changed target entries; see the [review record](uct_recap_localization_review_2026_10_18.md). |

The first final native integration run caught source-scoped undo skipping a view
change across its own temporary camera restore. The repair binds the exception
to the exact owned restore and unchanged selected-media identity/generation;
synthetic native-selection cases and the actual-app fixture then passed.
See the [review disposition](uct_recap_review_2026_10_18.md) for the other
independently verified repairs.

Native desktop/focus/layout tests, the Windows UIA bridge, Narrator/NVDA/Magnifier,
mixed-monitor DPI, physical camera/phone and microphone quality, A/V sync,
long-soak resource measurements, live providers and installer acceptance remain
separate. No installer or public release is certified by this background run.
Selected-media rendering currently supports the documented CPU view effects;
advanced GPU effects explicitly report unavailable. The seven new media tools
are in the opt-in `dev-full`/`v1` catalogs; this work does not promote the default
MCP catalog or certify V1 acceptance.
