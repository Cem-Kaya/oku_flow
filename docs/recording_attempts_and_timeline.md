# Recording attempts and audio timeline validation

## Encoder attempts (NP-007)

A codec/input attempt must never overwrite another file or delete diagnostic
bytes to make a fallback work. Media Foundation sinks now receive an exclusively
created byte stream (`MF_OPENMODE_FAIL_IF_EXIST`), closing the race between an
advisory existence check and native output creation. Existing files, including
empty files and links, are refused.

The first codec/input attempt uses the usual segment filename. Each subsequent
AV1-to-H.264 or native-GPU-to-worker-readback attempt uses a fresh GUID sibling
with `_attempt-{guid}` before `.mp4`. Failed attempts remain in the recording
folder, including metadata-only files; they are not claimed playable and are
never added to lecture notes. The manager remembers each selected writer's
actual path through finalization, rather than reconstructing its original name.
Only the fully finalized selected original/processed pair enters notes. This is
notes publication, not an atomic filesystem transaction for the pair.

`recording_manager` injects a metadata-write failure in each leg of AV1 startup
and verifies H.264 is attempted at distinct names with every byte retained. It
also refuses pre-existing output without entering the encoder seam. The
`media_writer_codecs_aac` fixture checks exclusive native sink creation directly
against both empty and nonempty colliding files. `media_writer_gpu` additionally
injects native-input rejection after writing bytes to its owned stream, then
uses an actual D3D frame and H.264 encoder for the worker-readback fallback.
The manager seam is deterministic policy evidence; it is not a real codec test.

## Intended media timeline (NP-006)

Video and microphone PCM use the same segment-relative monotonic media clock.
A capture stall is elapsed media time. Content captured after a three-second
stall must not move three seconds earlier relative to the video. A decoder can
represent the absence as a PTS discontinuity or decoded silence; either is
acceptable only if subsequent audible content retains its intended media time.
The number of PCM frames alone cannot distinguish these representations.

The native diagnostic supplies 90 video frames and 144,000 PCM frames at 30 FPS
and 48 kHz. At one second, the video changes from red to green and the tone from
440 to 880 Hz. The diagnostic inserts a three-second gap immediately before
that change, so both decoded markers must occur at four seconds and playback
must extend to about six seconds. It checks decoded video count/marker PTS,
audible frequency marker position, audio/video alignment and whole-track PCM
count. It allows 150 ms for AAC packet/priming boundaries, and either compact
PCM with discontinuous PTS or explicit silence. Ordinary codec passes also
check the audio marker without a gap; only the H.264 gap diagnostic requires
video decoding. Neither fixture opens a microphone or camera.

Enable `OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC=1` for the diagnostic and set
`OKUFLOW_TEST_AAC_EVIDENCE_DIR` to a private LocalAppData evidence directory to
retain MP4s. Run the rebuilt `media_writer_audio_tests` executable (or its
`media_writer_codecs_aac` CTest case) serially with other codec/GPU fixtures.
Capture the exact OS/driver/build versions and plain-text output. Original
failed fixtures remain on disk; an evidence directory also retains passes.
The opt-in run is diagnostic evidence, separate from the ordinary gate.

Do not infer current Windows AAC behavior from a source comment or fake sink.
No timeline repair is justified until the marked native run establishes the
actual marker position. Physical microphone start offsets, device loss and
long recordings remain separate acceptance checks.

## Native finding and bounded repair (2026-10-17)

The original Windows diagnostic is retained under the private audit evidence
root in `aac-timeline.log` and its `aac-timeline` MP4 directory. Normal H.264
and AV1 placed the 880 Hz marker at 1.0027 seconds. With the three-second gap,
H.264 still placed that audible marker at 1.0026 seconds and ended audio near
2.9866 seconds (144,384 decoded PCM frames), while the decoded green video
marker stayed at 3.99997 seconds. That demonstrates roughly three seconds of
audio compaction on the tested native codec. The old printed backlog error was
left over from the fixture's intentional invalid oversized-input probe; it was
not the reason for the marker failure. The fixture now separates that probe
from later codec diagnostics.

`RecordingPcmTimeline` now maps elapsed media time to integer PCM-frame spans.
Writers emit missing intervals as zero PCM before subsequent speech, including
a delayed first microphone block. Absolute sample-count conversion prevents
rounded block-duration drift. Up to a one-frame overlap is treated as rounding;
a larger overlap trims its already-covered leading PCM rather than replaying
it late. Each segment resets its own PCM cursor.

The recording worker streams silence in blocks of at most 4,800 frames (100 ms
at 48 kHz), using at most 64 KiB scratch for ordinary channel layouts. Every
silence and speech block uses the existing AAC PCM admission/accounting path.
A gap greater than ten seconds is rejected before allocation/submission. A
repair has one 250 ms admission-loop budget; exhausted/unknown encoder credit
or cancellation stops the attempt and finalizes/retains existing video through
the normal error path. These limits bound generated work and waits, not native
`WriteSample`/COM latency, which retains the existing worker watchdog contract.
No long wait or PCM generation occurs on the UI thread.

The native post-repair marked diagnostic passed. Its separate
`aac-timeline-fixed.log` and MP4 directory retain 144,000 submitted / 144,384
decoded frames in normal passes, with the tone marker at 1.002667 seconds.
The three-second-gap pass submitted 288,000 frames and decoded 288,768 frames;
its audible marker occurred at 3.999333 seconds and the video marker at
3.999967 seconds, an audio/video difference of about -0.633 ms. The additional
decoded frames are AAC packet padding, not extra submitted speech. The original
roughly three-second-early failure remains retained separately. This closes the
measured synthetic native-codec gap defect on that Windows configuration.

These fixtures do not establish real-microphone clock accuracy, device-loss
recovery or long-soak audio fidelity. Complete release-matrix acceptance remains
separate from the marked codec diagnostic.
