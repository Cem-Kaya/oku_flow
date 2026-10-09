# Lecture test camera

Double-click `scripts/start_lecture_camera.bat`. It starts a private OBS
instance, loops the downloaded lecture through **OBS Virtual Camera** and the
**DroidCam Video** Media Foundation output, and
opens `dist/OkuFlow/oku_flow.exe` if OkuFlow is closed. On first use select
**Advanced > Image > Device > Camera > DroidCam Video** in OkuFlow.
OkuFlow remembers that camera selection normally.

Double-click `scripts/stop_lecture_camera.bat` to stop the test camera. The
downloaded video stays on disk. OkuFlow stays open; stopping the camera can
exercise its device-loss/reconnection UI.

## Requirements and local files

- Windows, PowerShell 7 (`pwsh.exe`), and installed OBS Studio 28 or newer
  with its virtual-camera driver. Validated locally with OBS 32.2.2.
- Installed modern **DroidCam Video** driver and 7-Zip. Both were already on
  the setup machine. OBS's standard camera is DirectShow-only; OkuFlow uses
  Media Foundation. The [DroidCam Virtual Output plugin](https://github.com/dev47apps/droidcam-obs-virtual-output)
  sends the same OBS scene to the existing Media Foundation driver. If the
  driver is missing, install it from the vendor's releases separately. The
  script does not install kernel drivers or change machine registry settings.
- The ordinary OBS instance must be closed before starting this test. The
  virtual-camera driver has a shared output.
- First run downloads the selected lecture if missing and copies installed OBS binaries into
  a separate portable directory. No installer, administrator rights, or OBS
  scene editing is needed. The private copy includes obs-websocket v5.
  The script extracts DroidCam output plugin 0.2.2 into this copy only,
  verifies the official package SHA-256, and selects its x64 DLL by PE machine
  type. The vendor installer is not executed, and ordinary OBS is not modified.
- Default video: `%LOCALAPPDATA%\OkuFlow\lecture-camera\media\lecture.mp4`.
  Its adjacent `.source.json` retains attribution, source and SHA-256.
- Portable OBS, its own profiles/scenes/logs, the remembered video selection,
  and PID/start-time ownership record live beneath
  `%LOCALAPPDATA%\OkuFlow\lecture-camera\`. This is outside the synced repo.
- The private OBS control server uses a random available TCP port and an
  authenticated random password. The script connects via `127.0.0.1`;
  OBS's server may listen on other interfaces according to OBS defaults.
  The password stays in the private local config/session files, not console
  output or command arguments.

## Replace the video

Stop first. Either replace `media\lecture.mp4` with another local video, or
pass a new file path (the path and start offset are remembered for subsequent starts):

```powershell
pwsh -NoProfile -File scripts/lecture_camera.ps1 -Action Start `
    -VideoPath 'C:\Videos\another-lecture.mp4' -StartSeconds 0 -LaunchOkuFlow
```

When manually replacing the default file, update or remove the old adjacent
`.source.json` attribution so it does not describe the previous lecture.

The default seeks to about 25 minutes, at a useful classroom-board section.
For shorter clips use `-StartSeconds 0`. Playback loops the entire video after its
first pass. The output is 1280x720 at 30 FPS, fitting the complete source
without stretching or cropping. The 4:3 default has side bars. Upscaling
does not create source detail; no blur, stabilization, or enhancement is
added by this harness. The selected local download is **320x240 at 15 FPS**,
77 minutes long (about 85 MiB). OBS's 30 FPS output repeats source frames;
it does not create 30 distinct lecture frames each second. Audio is muted in
the test scene. OBS Virtual Camera carries **video only**, so this
does not test microphone transcription or sensor/autofocus behavior.

The default publisher copy can be downloaded again with:

```powershell
pwsh -NoProfile -File scripts/lecture_camera.ps1 -Action Download -Replace
```

For a different YouTube source, install `yt-dlp` separately, then use:

```powershell
pwsh -NoProfile -File scripts/lecture_camera.ps1 -Action Download `
    -Url 'https://www.youtube.com/watch?v=VIDEO_ID' `
    -VideoPath 'C:\Videos\replacement.mp4' -DownloaderPath 'C:\Tools\yt-dlp.exe'
```

YouTube downloads choose available MP4 video up to 480p without an audio
merge; YouTube availability depends on the downloader and source. An existing
destination is overwritten only with `-Replace`. A failed download preserves
the previous video. Retain the replacement publisher's applicable attribution
and license. When yt-dlp is available the default downloads the YouTube video
(preferring HLS); FFmpeg on PATH is needed for HLS MP4 remuxing. Without a
downloader the default uses MIT's linked offline copy, checking its exact
byte length and upstream SHA-1. That publisher host can be slow. The downloaded
sample is already present on the setup machine, so normal Start is offline.
The script discovers yt-dlp on PATH or in the existing local `.oz-tools` venv;
use `-DownloaderPath` for another installation. Public YouTube download URLs
can expire or fail; unsuccessful/partial downloads never replace a good file.

## Status and shutdown

```powershell
pwsh -NoProfile -File scripts/lecture_camera.ps1 -Action Status
pwsh -NoProfile -File scripts/lecture_camera.ps1 -Action Stop
```

Start verifies an authenticated OBS connection, successful media decode, and
active OBS virtual-camera output and the created Media Foundation output before
reporting ready. DroidCam starts delivering the scene when a camera consumer
opens it. Repeated Start reports
the running session rather than creating another instance. Concurrent script
operations use an exclusive file lock. Stop checks the executable path and
process start time before targeting the recorded PID, stops Virtual Camera,
requests a normal OBS exit, and uses a force-stop fallback only for that
verified test instance if it does not exit within ten seconds. If Start fails
after launching OBS, it cleans up its own process by the same path.

`-ObsPath` supports another OBS installation; `-SevenZipPath` selects another
7-Zip executable; `-StateDirectory` supports a
different local scratch root. Each installed executable hash gets a separate
portable copy, avoiding mixed old/new plugin DLLs after an OBS update. Do not
run two scratch roots simultaneously against the shared virtual-camera driver.

## Sample selection (2026-10-09)

Compared **20 lectures** using YouTube thumbnails and sampled storyboard
frames; this was a visual suitability screen, not viewing every complete
lecture or estimating population-average blur. Downloaded lecture frames were
used to confirm the selected source. Local comparison sheets are retained in
`%LOCALAPPDATA%\OkuFlow\lecture-camera\research\` on the setup machine.

Selected: [Sanjay Sarma, Lecture 2: The spider on a Frisbee problem](https://www.youtube.com/watch?v=vIjmMUI2w4c),
[MIT publisher page](https://ocw.mit.edu/courses/2-003j-dynamics-and-control-i-fall-2007/resources/lecture-2-the-spider-on-a-frisbee-problem/).
Its classroom views include students in front of the camera, a distant board,
small chalk writing, soft detail, camera reframing, and teacher occlusion. It
also includes closer views; it is not a fixed back-seat phone recording.
It provides a representative moderately difficult classroom input rather than
an artificially destroyed image.

| # | Lecture (MIT OCW / YouTube) | Preview assessment |
|---|---|---|
| 1 | [18.06 L1: Geometry of linear equations](https://www.youtube.com/watch?v=J7DzL2_Na80) | Frequent close board views |
| 2 | [18.06 L2: Elimination with matrices](https://www.youtube.com/watch?v=QVKj3LADCnA) | Mixed wide/close views |
| 3 | [18.06 L3: Multiplication and inverse matrices](https://www.youtube.com/watch?v=FX4C-JpTFgY) | Wide views, camera reframing |
| 4 | [18.06 L4: Factorization into A = LU](https://www.youtube.com/watch?v=MsIvs_6vC38) | Useful distant-board alternative |
| 5 | [18.06 L5: Transposes, permutations, spaces R^n](https://www.youtube.com/watch?v=JibVXBElKL0) | Mixed board framing |
| 6 | [18.06 L6: Column space and nullspace](https://www.youtube.com/watch?v=8o5Cmfpeo6g) | Audience and dense board |
| 7 | [18.06 L7: Solving Ax = 0](https://www.youtube.com/watch?v=VqP2tREMvt0) | Many closer board shots |
| 8 | [18.06 L8: Solving Ax = b](https://www.youtube.com/watch?v=9Q1q7s1jTzU) | Many closer board shots |
| 9 | [18.06 L9: Independence, basis, dimension](https://www.youtube.com/watch?v=yjBerM5jWsc) | Good wide/dense-board alternative |
| 10 | [18.06 L10: Four fundamental subspaces](https://www.youtube.com/watch?v=nHlE7EgJFds) | Mixed framing, close details |
| 11 | [18.06 L11: Matrix spaces, rank 1](https://www.youtube.com/watch?v=2IdtqGM6KWU) | Dense writing, mixed framing |
| 12 | [2.003J L1: Course information / kinematics](https://www.youtube.com/watch?v=K_6tRupDxF4) | Talking head and slides dominate previews |
| 13 | [2.003J L2: Spider on a Frisbee](https://www.youtube.com/watch?v=vIjmMUI2w4c) | Selected: audience, distant soft board, moving lecturer |
| 14 | [2.003J L3: Pulley problem](https://www.youtube.com/watch?v=PJ905OQGsBA) | Close lecturer/board views frequent |
| 15 | [2.003J L4: Magic and super-magic formulae](https://www.youtube.com/watch?v=K1pZ90xp2Gw) | Useful wide-board alternative |
| 16 | [2.003J L5: Constraints / degrees of freedom](https://www.youtube.com/watch?v=yYDEpATHF7o) | Slides/close-ups in sampled sequence |
| 17 | [2.003J L7: Impulse / skier separation](https://www.youtube.com/watch?v=0nFNEpIYmk8) | Close-up-heavy sampled sequence |
| 18 | [2.003J L8: Single particle / two particles](https://www.youtube.com/watch?v=ejvzjQ1wQ-U) | Mixed framing |
| 19 | [2.003J L9: Dumbbell / torque](https://www.youtube.com/watch?v=uhjq0UQqkVg) | Mixed framing, board diagrams |
| 20 | [2.003J L10: Three cases / rolling disc](https://www.youtube.com/watch?v=5iTUUeKjiEM) | Mixed framing, board diagrams |

Complete source galleries: [18.06](https://ocw.mit.edu/courses/18-06-linear-algebra-spring-2010/video_galleries/video-lectures/)
and [2.003J](https://ocw.mit.edu/courses/2-003j-dynamics-and-control-i-fall-2007/video_galleries/video-lectures/).
The default sample is attributed to Sanjay Sarma / MIT OpenCourseWare,
2.003J Dynamics and Control I, Fall 2007, under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/).
It is a local test asset, excluded from git and OkuFlow release bundles;
OkuFlow's dual license does not relicense it. Publisher terms:
[MIT OCW](https://ocw.mit.edu/pages/privacy-and-terms-of-use/).
OBS is a separately installed GPL-2.0-or-later tool; its copied binaries stay
local and are not part of the OkuFlow release.

Automation references: [OBS launch parameters](https://obsproject.com/kb/launch-parameters)
and [obs-websocket v5 protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md).

## Local validation

Validated 2026-10-09: full YouTube download (all 930 HLS fragments), seekable
MP4 decode, live OBS Virtual Camera frames, live DroidCam Video lecture frames,
idempotent Start, Status, owned-instance Stop, replacement with a five-second
clip and looping playback, then restoration of the default file. Partial
download failures preserved the existing file. PowerShell parsing passed.

The tracked `scripts/profile_startup.ps1` completed an isolated eight-second
OkuFlow run against the now-fed DroidCam Video device (sorted camera index
0 on this machine): 212 camera frames received, 156 presented, first present
at 1094 ms, no camera error, and direct D3D11/D3D12/CUDA texture ingress.
Observed arrivals were about 30 FPS; distinct source content remains 15 FPS.
This verifies capture/processing integration, not sensor latency or optical
quality. The report is at
`local_evidence/lecture_camera/mf-bridge-okuflow/worker-1/startup.json`.
Earlier probes made before the bridge was active are not lecture validation.
