# Saved-video audio output

`common/video_audio_output.hpp` and `src/common/video_audio_output.cpp` provide
one WASAPI shared-mode playback sink for decoded `VideoPlaybackAudio` PCM.
This is a first-party Windows system API integration. It adds no codec,
third-party implementation, Qt Multimedia dependency, or redistributable.
Link its target to the existing Windows `ole32` system library.

## Calling contract

The selected-document owner calls `Configure(requestGeneration, playing,
muted)` before moving current-generation packets into `Push`. Output starts
only for explicit playing and unmuted state. A new document/seek generation,
pause, mute, `Reset`, and `Close` revoke queued output and clock publication
immediately. Native stop/flush happens on the worker; `flushPending` remains
true until that worker acknowledges invalidation. `Close` is reusable; the
destructor requests cancellation and returns without joining the worker.

The app must configure muted output while a recording is active and during
recording-session review. This sink never captures microphone audio, changes
microphone settings, or submits decoded audio to a recorder. The selected
source owner retains the recording policy and separately controls decoded
track selection. Source-reader audio availability and endpoint output
availability are separate: PCM must be pushed before the endpoint can become
available. A missing audio track must not be reported as an output error.

`Push` accepts owned interleaved signed PCM16 with 1–8 channels, 8–384 kHz,
nonnegative media PTS and complete sample frames. Device support is established
by the actual shared-mode initialization, not assumed from admission. Windows
handles conversion to the endpoint's mix format. A format change or timestamp
discontinuity drains the previous short device buffer before opening the new
format. Pause/seek/mute supersede that drain.

## Bounds and failure behavior

The pending queue and the one packet held by the worker together have hard
limits of 2 MiB, 32 packets, and 500 ms. Duration accounting rounds each packet
up. Cancellation drops pending packets, but a packet retained by a native call
remains in `queuedBytes`, `queuedPackets`, `queuedDuration100ns` and
`inFlightBytes` until the worker releases it. There is no unaccounted copy of
the in-flight PCM. The separate WASAPI endpoint buffer requests 100 ms and is
rejected if Windows allocates more than 250 ms. Its current padded duration
is reported separately in `deviceBufferedDuration100ns`.

Queue overflow or invalid PCM fails the output for the current generation,
discards the pending output, invalidates the audio clock and reports an error.
An endpoint failure has the same policy. Polling an unchanged configuration
does not repeatedly retry a failed endpoint; a new generation or explicit
`Reset` permits a retry on the same worker. The player must retain a monotonic
video-clock fallback and show output failure truthfully.

The upstream decoder retains its PCM media high-water after transferring packets
to this sink. Ordinary delivery prepares about 200 ms of lead, plus at most one
decoded packet, rather than resetting that lead whenever the UI drains the
decoder queue. The sink's caps and fail-closed overflow behavior remain unchanged.
See the [silent pacing and long-GOP regression](video_audio_preroll_repair.md).

One process-wide native-owner lease covers the worker's complete lifetime,
including COM and endpoint teardown. All native calls, COM initialization,
waiting, and interface release happen outside the UI thread and outside the
state mutex. A native call exceeding two seconds causes snapshots to report
unavailable output and reject further PCM while that call remains in flight.
Cancellation does not free its state or create a replacement worker. If the
driver never returns, that one bounded retired worker retains its lease until
process exit. No second sink can obtain a native owner while it exists.

## Playback clock

`Snapshot` exposes media PTS derived from `IAudioClock::GetPosition` and its
reported frequency, mapped through at most 128 bounded submitted-PCM spans.
It never reports the newest queued packet as already rendered. It clamps the
device position to submitted PCM, resets the stream origin after an underrun,
and refuses delayed/inaccurate clock readings. Paused, muted, superseded and
failed output cannot publish a valid clock.

The document controller may use the clock only when `available && clockValid`,
the request generation matches, and `sampledAtSteady100ns` is fresh. That field
uses `std::chrono::steady_clock`'s epoch in 100 ns units. It is a host sample
timestamp, not a sensor-to-speaker latency measurement. Once the device drains,
the clock becomes invalid and the player uses its fallback. The worker waits
without periodic endpoint/OCR work when no PCM or device drain is pending.

The Windows contracts are documented in Microsoft's
[stream-conversion flags](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)
and [audio-clock position](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
references. These contracts do not establish a measured A/V error bound.

## Validation

`tests/video_audio_output_tests.cpp` is a desktop-free contract fixture by
default. Its admission-only option creates no worker, COM apartment, endpoint
or sound. The cases exercise explicit playback admission, source supersession,
pause/mute/close, all three independent queue caps, malformed formats,
time-overflow refusal, truthful clock absence, recovery and concurrent resets.

`OKUFLOW_TEST_VIDEO_AUDIO_OUTPUT=1` explicitly opts into a short, quiet 440 Hz
speaker tone and checks a genuinely advancing rendered-device PTS. It must
remain outside ordinary background checks. This test does not prove perceptual
quality, endpoint-switch recovery, acoustic isolation, long-soak behavior or
the proposed 80 ms A/V performance target. Native endpoint/hardware acceptance
and the tracked complete Windows matrix remain separate from pure queue tests.
