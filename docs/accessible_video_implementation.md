# Accessible video and UCT implementation

This development change implements the owner's UCT/accessible-video brief on
top of merged PR 8 (`d23f871`). The attached brief is a design input; its
reported defects are verified against production code before repair.

## Recording and source contract

The owner's correction supersedes the brief's concurrent foreign-file playback
rules. Opening a saved video first stops and finalizes an ongoing recording.
Starting a new recording leaves saved playback and returns to the live camera.
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

This document records the intended contract and implementation boundaries.
Validation results are added after the corresponding checks actually run.
