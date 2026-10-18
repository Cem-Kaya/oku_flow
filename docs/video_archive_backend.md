# Native recording archive and video playback (development)

The player uses Windows Media Foundation and the existing Original recorder.
It creates no history encoder, duplicate recording, microphone, network client,
or media-framework dependency. These are development contracts; native AVC/AAC,
AV1 and active-fragment acceptance must be reported separately from model-free
tests and from the shipping Windows matrix.

## Public contracts

`video_archive.hpp` defines `VideoFileIdentity`, whose `SameFile` compares the
held Windows volume/file ID rather than path, size or timestamp. Its remaining
fields retain initial size and last-write evidence. `OpenStableVideoReadStream`
opens the exact read-only handle used by the decoder and denies writes/deletes
until retirement; a path cannot be replaced between probing and decoding.
Ordinary file playback cannot bypass a recorder's exclusive output handle.
The controller must select `OpenArchive` for matching current output attempts.

`RecordingArchive` owns a bounded parser worker and exposes `Snapshot`,
`WriterAccepted`, `Finalized`, `Abandoned`, `ValidateRange`, `OpenReadStream`
and the producer `Observe` wrapper. Destruction requests cancellation without
joining native I/O. The parser worker owns its resources until it stops.
Permanent observer loss retires that parser. After finalization it completes
bounded parsing batches until the stable end/first unusable tail, then releases
its read handle and metadata scratch even when a receipt retains the facade.
Later finalized read admission rechecks file ID, size and last-write evidence
before returning a handle that denies writers.
`RecordingArchiveSnapshot` carries an attempt UUID, held file identity,
mutation/index generation, codec-configuration fingerprint, selected video
track ID, successfully written byte prefix, verified media prefix, successful
video WriteSample PTS, actual last verified video PTS, duration, audio presence
and finalization/completeness status. `parserBoxAttempts` counts actual box
admission attempts for bounded-work diagnostics. These clocks and watermarks
are distinct. Finalization revokes byte-range admission while retaining the
previously proven duration used by multipart receipt offsets; reparsing may
extend that duration without temporarily collapsing the mapping to zero.
Session/source-epoch/part/global-time associations belong to RecordingManager;
the archive never guesses them from filenames or joins unrelated MP4 bytes.

`VideoRecorder::Archive` returns the current successful/initializing attempt's
shared archive. `LastAdmittedVideoTimestamp100ns` is worker-only after successful
input admission and does not imply WriteSample completion. `FailedAttemptPaths`
retains actually created failed attempts through Stop, resets at next Start,
and excludes the selected successful output. Recording preservation and encoder
abandonment remain owned by the recording worker. Archive readers cannot call
Finalize or wait on that worker.

`VideoPlayback` exposes asynchronous `OpenFile`, `OpenArchive`, `Seek100ns`,
`JumpToEnd`, `SetPlaying`, `SetMuted`, `SetAudioClock100ns`, `Close`, `Snapshot`
and `TakeAudio`.
Open/seek return monotonically increasing request generations. Public methods
perform bounded bookkeeping; they never wait for native decoder teardown.
`VideoPlaybackSnapshot` carries state, generation, opened identity, metadata
duration, verified/observed actual endpoint, audio capabilities, mute/error
status and shared immutable `VideoPlaybackFrame` leases. Frames carry tight
BGRA pixels, actual PTS/duration/sample identity, rotation metadata and
request/archive generations. Native pixel aspect is normalized directly into
square-pixel rows without an additional frame allocation. Alpha is made opaque
because the system RGB32 converter's fourth byte is padding. The optional `neighborFrame`
has genuinely different decoded PTS and never replaces the selected frame.
It is absent at EOF, on insufficient pool credit, or when no distinct neighbor
is obtained within the same decode limits. Consumers must apply rotation once.
`VideoPlaybackState::Waiting` means Resume is requested at the current active
archive prefix's EOF. The selected pixels stay held, queued PCM is flushed,
Pause remains available and another verified fragment resumes playback.
That transition rebases the host clock to the held cursor and invalidates the
consumed-audio clock, so time spent waiting cannot fast-forward the new media.
`decodedBytes` and `peakDecodedBytes` report the shared staging/lease quota,
including old frame leases retained by consumers.

`VideoPlaybackAudio` contains interleaved signed 16-bit PCM, actual sample PTS,
sample rate/channels and request generation. `TakeAudio` transfers only current
unmuted playing packets. The application's Qt audio sink owns device output,
flushes on Pause/seek/source/generation/mute changes, and must bound its own
output buffer. No playback samples enter the microphone/recording path.
`SetAudioClock100ns` accepts consumed audio PTS, sampled steady-clock ticks
(nanoseconds since `steady_clock` epoch divided by 100), and request generation.
Current unmuted samples no older than 500 ms pace video; stale/muted clocks use
the host playback clock. The native worker still owns all decode operations.
Negative PTS invalidates the consumed clock immediately. Resume establishes an
audio floor at the exact selected cursor: earlier PCM is discarded and a
straddling packet is trimmed to its first eligible PCM frame. The same worker
pumps bounded audio during at most 20 ms waits for future video timestamps, so
a sparse/VFR video gap does not stall the soundtrack. Muting/unmuting during
playback repositions track decoding at the current playback clock. At ordinary
video EOF the worker retains its last image while remaining PCM reaches its
actual audio endpoint; explicit Jump to end remains a paused frame selection.
`VideoPlaybackTimestampFits` admits nonnegative PTS/duration intervals only
when their sum fits `INT64_MAX / 100` ticks (about 292 years), making conversion
to host nanoseconds safe. Native duration, seek targets, decoded frame and PCM
intervals use that admission rule. Consumed audio clock samples are bounded to
the same range, and host/audio clock advancement saturates before addition.

## Active byte proof

`CreateRecordingArchiveOutputStream` exclusively creates the actual fMP4 file
with Windows CREATE_NEW and FILE_SHARE_READ. Its held seekable read/write IStream
is adapted by Media Foundation and wrapped by the observer. This permits actual
active read sharing while preserving no-clobber output ownership. A fixed-file
reader still refuses that active writer because it denies concurrent write
access. The recorder never reopens a failed attempt for replacement/truncation.
`Observe` forwards synchronous and asynchronous writes, reads, seeks, flushes,
position changes and length changes. Immediately before forwarding a write it
records the mutation range. A write over previously completed bytes revokes
the old generation before any bytes can change. A bounded pending-range ledger
and coalesced successful-write intervals do not depend on parser progress.
Unknown positions, overlapping asynchronous mutations, ledger overflow, failed
writes revoke active proof instead of blocking Original capture. Stream-length
changes revoke the old generation, clamp successful extents on shrink and
require a fresh parse; zero-filled extensions grant no encoded-media credit.
Asynchronous completion calls the underlying EndWrite once and preserves the
caller's callback/state/result contract.
An unobservable write position permanently revokes proof before the native
write and grants no successful-prefix credit; it cannot be mistaken for a
stream-length mutation. Each IStream cursor serializes Read/Write/Seek/Clone
before acquiring the shared handle lock, including MF adapter async calls.

The parser makes a real shared-read CreateFile probe against that output and
publishes `SharingDenied` if Windows refuses it. On a readable stream it scans
complete top-level MP4 boxes on its own worker. Bounded `moov` parsing checks
track IDs/timescales, a single supported sample description with a complete
decoder-configuration box, and trex defaults. `moof` parsing checks mfhd
sequence continuity, tfhd/tfdt/trun lengths, sample durations/sizes/composition
timestamps/flags and explicit data offsets. The tested Windows sink omits
tfdt; empty initial sample tables and strictly consecutive verified fragments
permit exact cumulative per-track decode time. Explicit tfdt is honored when
present; discontinuous sequences and edit lists are refused. A complete
following mdat must contain every nonoverlapping referenced audio/video data
span. The prefix must have a preceding verified video access point;
inter-coded later fragments need not each contain one. Only that written prefix
and its actual maximum video PTS become active seekable media. File length and
successful WriteSample alone grant no seekable credit.

An active read stream fixes one verified prefix/generation. Every read checks
identity and producer generation/ranges before and after copying; decoded
publication checks again. Appends can extend the archive without changing a
held frame. Backwrites revoke admission, while already private held pixels
remain owned. A seek refreshes the byte-stream view on the same native worker
after retiring its previous reader. Playback at an active prefix's EOF waits
for another verified fragment; it does not jump live or finalize recording.

The initial proof is intentionally conservative: at most eight tracks, one
sample description per track, 64 runs and 65,536 total samples per fragment,
8 MiB initialization metadata and 1 MiB moof metadata. Open-ended mdat boxes,
unrecognized layouts, missing provable decode continuity/data offsets,
unsupported configuration or a prefix without a video access point leave active rewind
unavailable. Earlier verified pixels remain held.
Complete unsupported boxes are latched until a generation change or
finalization; same-generation appends and periodic wakeups cannot repeatedly
parse the same metadata. Incomplete boxes continue waiting for actual bytes.
No encoded payload is copied into a second archive or retained as raw history.
Successful finalization is separate from crash durability and native decoding.

## Ownership, budgets and limitations

One process-wide permit covers the entire native playback owner, including
slow SourceReader release and Media Foundation shutdown. One running request
and one latest replacement are retained. A new request has no publication
authority until its generation matches; a successor cannot be created before
the old reader releases. A native synchronous call cannot be forcefully
interrupted. Its worker and permit remain counted; after two seconds Snapshot
reports unavailable and replacement stays on that worker. No UI join, detached
replacement reader or accumulated retired decoder list is used.

Seek/open decoding checks cancellation between native steps, at most 120 video
read attempts and two monotonic seconds. The limit is cooperative, not a native
driver cancellation promise. Long-GOP or malformed inputs can return unavailable
with the existing held image. A request older than two seconds is refused before
native admission. Jump to end anchors active playback at the verified actual
last-video PTS. For saved files it begins near the metadata tail; container
duration can include later audio and need not name a video sample. Only actual
native video EOS without any decoded frame permits an earlier anchor, using
exponential backoff and at most eight total seeks (the last reaches zero).
Every attempt shares the original 120-read/two-second request budget. Once a
frame is obtained, real decoder EOS and the last decoded pixels establish the
endpoint. An anchor never substitutes for an observed endpoint. Failure reports
include read/seek/decoded counts, EOS/selected-frame presence, last PTS, anchor,
duration and request age. Files whose tail cannot be reached within the limits
remain unavailable instead of silently scanning an entire lecture. EOF Resume
does not restart. Failed open/seek keeps the displayed lease; the previous
decoder may already have retired, so another explicit open/seek can be required.

Additional CPU decoded staging plus leased BGRA frames share a 96 MiB budget.
`VideoPlaybackLayoutFits` validates dimensions (maximum 16,384 per axis), stride
and byte multiplication before frame allocation. Native coded dimensions are
checked before requesting RGB32 conversion, then decoded dimensions are checked
again. Buffer lengths, row pitches
and IMF2DBuffer2 address bounds are validated. A consumer retaining old frames
uses that same budget and can make a subsequent decode unavailable. The file
decoder must expose one RGB32 buffer whose total/current/capacity fit the
reserved staging bytes; GetBufferByIndex avoids an uncounted contiguous-copy
allocation. Source-format growth beyond the admitted staging returns unavailable.
Optional neighbors and intermediate seek frames release their leases before a
required replacement pixel allocation when credit is tight. A previous seek
frame is kept through a no-sample EOS read. Normal 4K decoding needs at most the
displayed frame, one native staging buffer and one replacement owned frame
(99,532,800 bytes for tight 3840x2160 RGB32), below the 100,663,296-byte cap.
The file
stream accepts at most a 64 MiB individual read and retains no read-ahead cache.
Parser metadata remains below 8 MiB; mutation bookkeeping is fixed size.
Audio queues retain at most 32 packets, 2 MiB and 500 ms, with individual packets
limited to 256 KiB. Native decoder reference pictures/driver/internal caches
remain separate platform allocations requiring measurement. CPU output is
requested; no new replay GPU pool is allocated by this backend.

Spatial display effects and UCT belong to the selected-view controller. This
backend applies no live temporal filter and never changes capture timestamps,
recording origin, microphone intent, source preferences or user media bytes.
Optional codec support is determined by installed native codecs, with no
download. Source-color interpretation follows the system RGB32 converter;
HDR/wide-gamut accuracy and physical audio/video synchronization need native
acceptance. Damaged files preserve a held frame and return a truthful error;
automatic exhaustive repair/index recovery is outside this bounded reader.

## Validation

`tests/video_playback.cmake` registers `video_playback_contract`. Default cases
are desktop-free/model-free: frame/clock admission, same-handle identity and write
exclusion, partial-fragment non-admission, synchronous backwrite revocation,
unknown-position no-credit fencing, unsupported-box parse latching,
writer/seekable timestamp separation, abandonment and latest-open supersession.
Set `OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1` only for coordinated native codec runs
`video_archive_native_avc_aac` and `video_archive_native_av1_aac`. They create a
private synthetic file and require active metadata proof, real frame decode
and active seeks before Finalize. They retain a paused active reader while
submitting more Original video/AAC, check write acceptance and verified-prefix
growth without replacing the held frame, then seek newly recorded media on the
same worker. They finalize under recorder ownership and
check paused first-frame/latest seek/PCM/actual EOF behavior, and use no camera,
microphone or native GUI. A reported SharingDenied/unsupported active state is
feasibility evidence, not a passed active-decoding claim. Preserve the reported
reason and actual codec/build identity. The coordinating native run first
rejected the native writer's omitted tfdt and non-keyframe fragments; the
repaired basic active-before-Finalize/AVC/AV1/AAC/saved-seek/EOF cases passed.
The subsequent coordinated gate passed the active-seek/continued-writer,
resource-accounting, PCM preroll and sparse-video/continuous-AAC cases for both
native codecs. A subsequent gate passed the final stream-lock/clock/dimension
guards, 4K lease-pressure, active Waiting/resumption, stable finalized duration
and unsupported-parser cases, but the longer 124-frame AVC/AV1 files exposed
the fixed end-seek lookback crossing an extra GOP and exceeding the unchanged
120-read cap. A subsequent endpoint-minus-one-tick anchor exposed native EOS
without video because audio/container duration extends beyond the last video
sample. The bounded observation-driven fallback and detailed counters then passed
the final tracked CPU gate for both native codecs, including actual EOF after
the longer files and the sparse-video audio tail. The native active cases also hold a one-second
Waiting interval, append without another seek/Resume, and check resumed PTS
advancement against elapsed host time to catch waiting-clock fast-forward.
`video_playback_native_4k`
creates twelve 4K AVC samples, requires five distinct playback frames, a
multi-sample seek and actual EOS while checking the exact unchanged quota.
The sparse fixture submits video
at 0/33 ms followed by a two-second gap, then requires at least 700 ms of PCM
while the exact early video image is still held. Its original video samples
remain unchanged while AAC continues alone for over two seconds afterward;
end seeking must retain the actual last video frame before that audio tail.
These local codec cases
do not establish long-soak, physical camera, GPU, GUI or release acceptance.

API references: [Source Reader processing and seeking](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader),
[IMFByteStream BeginWrite](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfbytestream-beginwrite),
[IMFByteStream capabilities](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfbytestream-getcapabilities).
