# Long-GOP video and audio pacing repair (development)

A private 89.5-second `045_av1.mp4` reproduced the reported unavailable player.
The input is 2288×1080, 30 fps, 10-bit AV1 with BT.2020/PQ metadata and stereo
48 kHz AAC. Its 2,684 video packets contain only one keyframe, at 0.033008 s.
The input is owner media; it is neither changed nor included in Git or bundles.

The native GPU processor refused the advertised HDR-to-display color conversion
on the inspected machine, so the existing stable-handle CPU fallback was used.
That fallback is a platform capability boundary, separate from the defect below;
this repair does not certify HDR tone mapping or claim resident GPU decoding.

The original seven-second seek decoded 148 samples to PTS 4.9 s in 2,002 ms.
The original thirteen-second seek decoded 141 samples to PTS 4.666667 s in
2,001 ms. Both were making real progress when the whole-request two-second
deadline rejected them. A six-second selection in the same single GOP needs
about 180 preroll frames. The held image remains owned, but the transport becomes
unavailable. Original failed diagnostic output is retained under
`%LOCALAPPDATA%/OkuFlow/media-audio-repair-20261021/`.

## Bounded preroll

Open, seek and audio-aligned Resume keep the existing single native worker,
latest-request cancellation, held-file identity, memory limits and 4,096-read
ceiling. Each request has an absolute 60-second limit. Only strictly increasing
decoded video PTS renews a two-second progress window. Empty, repeated, negative,
backward and late-returning samples cannot renew it; progress cannot cross the
absolute limit. A native synchronous call still cannot be forcefully canceled.
Its same owned worker remains retained, and an unresponsive operation is reported
after two seconds without progress. Newer selections revoke publication between
native calls.

Healthy long-GOP selection remains Seeking while the previous selected image stays
held. Audio-aligned Resume rebases the fallback host clock after preroll; decoder
work is not counted as elapsed media playback. Earlier PCM remains excluded by
the exact selected cursor. Long single-GOP inputs can take seconds to seek;
the new bound is not a latency promise or an unlimited-file guarantee.

## Held-frame Resume

An issued frame can be held behind the decoder without changing request
generation. Resume transfers its restart flag and decoder-rewind intent in the
same locked worker handoff. Consuming them in different iterations could leave
a reposition pending after restart was consumed, stranding playback at EOS.
End publication also rechecks pending Resume, so it cannot overwrite a newer
request to replay an earlier issued frame. The scene-cut fixture retains its
four-second wait and exact final-sample/endpoint assertions.

## PCM delivery

The native reader and WASAPI output each have a separate bounded queue. Removing
packets with `TakeAudio` must not erase the reader's already-decoded media
high-water. The audio pump respects that high-water while preparing its ordinary
200 ms lead, even when a consumer drains the queue on every UI tick. It can
overshoot by at most one admitted PCM packet. This prevents repeated video-wait
ticks from feeding future AAC into the sink until its 500 ms cap fails closed.
The sink retains its hard caps and explicit failure behavior.

## Validation scope

`video_playback_contract` exercises progress windows with synthetic time,
including repeated/backward samples, a late native return and the absolute cap.
It retains the existing actual-decoder rapid-seek supersession test. Its original
`paced-audio-h264.mp4` fixture tests bounded PCM delivery while silently draining
the production reader. It never creates an output endpoint or emits sound.

The opt-in `video_playback_tests --native-playback-external <file>` and
`--native-gpu-playback-external <file>` diagnostics inspect exact private source
files, seek to six and twenty-three seconds, then decode three seconds of video
and PCM without output. Set `OKUFLOW_TEST_VIDEO_PLAYBACK_CODEC=1`; GPU preference
also needs `OKUFLOW_TEST_VIDEO_GPU=1`. These are private diagnostics, separate
from the GPU-only format fixture, which still requires actual resident output.

The five-second H.264/AAC fixture is 160×96 at 30 fps with mono 48 kHz AAC.
It contains an original generated test pattern and 440 Hz sine wave, no external
footage, music or fonts. It is 71,447 bytes, SHA-256
`adba05f04a35d7c024d4e2ff706c5d2952ace17d30db7d6c207fce0525db7a82`.
The owner's separately installed FFmpeg 7.1.1 generated it with:

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=size=160x96:rate=30:duration=5' -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=5' -c:v libx264 -preset veryfast -crf 35 -g 150 -pix_fmt yuv420p -c:a aac -b:a 32k -ac 1 -movflags +faststart -map_metadata -1 -y tests/data/paced-audio-h264.mp4
```

The tracked failing baseline delivered PCM 728.2 ms ahead within two seconds,
exceeding the output sink's 500 ms cap. Repaired contract runs measured 200.5–201.4 ms
of lead and passed mute/unmute, pause/Resume and exact PCM-floor checks. The
separate output-queue contract also passed without opening an endpoint.

The same private AV1 source passed GPU-preferred playback through the expected
CPU compatibility path: six seconds selected in 2.362 s / 181 reads, twenty-three
seconds in 9.304 s / 691 reads. A seventy-second seek was superseded by a newer
six-second selection during preroll. Unmuted Resume needed 2.518 s of preroll,
then three seconds of video advanced to PTS 9.0 s while 621,564 PCM bytes were
delivered, audio stayed available and maximum measured delivery lead was
277.9 ms. Actual End completed in 44.707 s / 2,685 reads, retaining video PTS
89.4333333 s. These are source-specific decoder/host measurements; playback
remains subject to the sixty-second request cap and actual installed codecs.

Headless PCM/codec results do not prove acoustic continuity, device switching,
native transport/focus behavior, screen-reader quality, long soak or complete
Windows release acceptance. Native GUI validation remains on the disposable
GitHub Windows desktop. The owner's running application is left untouched.

## FFmpeg feasibility probe (not integrated)

OkuFlow already deploys Qt Multimedia's FFmpeg libraries, while its custom
selected-file player uses Media Foundation. A read-only C API inventory of the
installed Qt 6.12.0 `avcodec-63.dll` found `av1`, H.264, HEVC and AAC decoder
entries, but no `libdav1d` or `av1_cuvid` entry. Presence of a codec entry alone
does not prove a usable software or hardware decode path.

A single external FFmpeg 7.1.1 CPU probe on the same owner file sought to
23 seconds and produced one BGRA frame into the null sink. It used `libdav1d`,
reported frame PTS 23.033008 s, and took 1,035.8 ms of process wall time
(FFmpeg processing `rtime` 0.981 s). The earlier native-player 23-second seek
took 9.304 s. These different paths and samples are an exploratory comparison,
not an integrated speedup, exact-frame equality or latency distribution.
The command did not create an audio endpoint, alter the video or transcode it
to a replacement file. Receipts retain the exact command, executable hash,
version, selected decoder, PTS and timing under the private evidence root.

The tested external full build is not bundled. Its enabled GPL components must
not be confused with Qt's existing LGPL/permissive inventory. A playback-only
FFmpeg/dav1d backend is a proposed follow-up: use `libavformat` for file/packet
reading, `libavcodec` for decoding and `libswresample` for audio conversion,
while preserving exact selection authority, held-file/custom I/O, bounded
memory, cancellation, audio clocks and GPU texture ownership. Benchmark that
integrated path and HDR conversion before choosing its default or deployment.
There is no new application dependency or backend in this repair.

Primary references: [Qt Multimedia backend/deployment](https://doc.qt.io/qt-6/qtmultimedia-index.html),
[FFmpeg demuxing and seeking](https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html),
[FFmpeg licensing](https://ffmpeg.org/legal.html), and
[dav1d upstream](https://github.com/videolan/dav1d).
