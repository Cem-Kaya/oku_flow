# Native UI captures

The real application can save one unmodified PNG of its composed window during
an explicitly enabled startup-profiling session. This includes the live D3D
preview and native sibling corner controls inside the main window's frame.
It does not render a simulated interface or remove a cursor from an image.

## Reproduce a capture

Use a separate trial directory containing `settings.json`, copied from the
chosen settings fixture. Set `paths.userDataRoot` to a trial-local data directory
and `ui.setupAssistantDeclined` to `true`. Disable unsolicited AI requests and
lecture-note recording as in `scripts/profile_startup.ps1`. Profiling loads and
saves only this sibling settings file; an APPDATA override alone does not
isolate Windows known-folder settings. Hash the source settings before and
after the session to verify they stayed unchanged.

From PowerShell 7, with the deployed executable and an existing trial directory:

```powershell
$captureTrial = 'C:\Users\cemka\AppData\Local\OkuFlow\ui-capture\simple'
$env:OKUFLOW_UI_CAPTURE_PATH = Join-Path $captureTrial 'simple.png'
$env:OKUFLOW_UI_CAPTURE_DELAY_MS = '30000'
try {
    & 'Y:\Drive\My_Drive\folder\Projects\oku_flow\dist\OkuFlow2\oku_flow.exe' `
        "--startup-profile=$captureTrial\startup.json" '--startup-profile-ms=60000'
} finally {
    Remove-Item Env:OKUFLOW_UI_CAPTURE_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:OKUFLOW_UI_CAPTURE_DELAY_MS -ErrorAction SilentlyContinue
}
```

Select the intended view before the capture delay and leave OkuFlow in the
foreground, entirely on one monitor. The child exits normally at the profiling
deadline. Use a separate directory/session for each requested view. Do not
terminate or relaunch an existing user instance to obtain screenshots. The
[lecture camera workflow](lecture_camera.md) supplies genuine test frames
through the private OBS/DroidCam feed; its idle placeholder is not a valid
lecture capture. Keep the provenance of content intended for a public website.

The hook requires an absolute `.png` output path whose parent already exists.
`OKUFLOW_UI_CAPTURE_DELAY_MS` defaults to `15000`; explicit values must be
integers from `1` through `60000`, strictly before `--startup-profile-ms`.
Leave more than 100 ms before that deadline: visible owned native peers are
repainted at the requested delay, and the unmodified desktop grab follows
100 ms later so their layered backing stores can settle.
The default profiling duration also equals `15000`, so choose a longer profiling
duration or a shorter capture delay. Without `--startup-profile`, both capture
variables have no effect. The hook makes one attempt, logs its result, and never
retries by raising windows or moving input.

Before a website capture, move input away from buttons that show hover tooltips.
If computer-use automation was used to arrange the view, reset its Node REPL
session before the capture delay. Its pointer overlay is a foreign desktop
window and is deliberately rejected by the overlap check. No input should be
needed when launching the remaining sessions from prepared settings fixtures.

## Capture checks and limits

A successful camera presentation must precede capture by at least 80 ms.
The complete Qt frame must lie within the selected screen. Before and after
grabbing, the foreground process must be OkuFlow and no visible foreign
top-level window above its main window may intersect the native frame.
Own-process native peers are allowed. The main frame and screen must remain
unchanged through the grab. Failed checks log `UI capture skipped` and save no
new image. Saving uses `QSaveFile` to atomically replace the explicitly selected
output only after all checks pass.
Foreground and overlap diagnostics report process IDs and rectangles without
logging foreign window titles or content.

Qt's [QScreen documentation](https://doc.qt.io/qt-6/qscreen.html#grabWindow)
states that screen grabs generally omit the mouse cursor, include overlapping
window pixels, and should use desktop capture for layered Windows surfaces.
The hook therefore grabs the composed desktop rectangle, with screen-local
logical offsets on Windows and the returned pixmap's native device pixel ratio.
It does not hide the OS cursor or modify PNG pixels. Inspect the raw image:
an independently rendered pointer halo or other overlay can still be desktop
content. Retake any unsuitable image instead of editing such content out.

Native overlap checks use Win32 rectangles only, separate from Qt logical
coordinates. Microsoft's [GetWindowRect documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrect)
notes DPI virtualization and invisible resize borders. The checks are
conservative and may reject border-only overlaps or cloaked foreign windows.
The [GetWindow traversal](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow)
is bounded to 4096 entries; incomplete traversal rejects the capture.
Pre/post checks reduce desktop-change races but cannot make desktop composition
and window enumeration one atomic operation. Review the raw result before
copying approved real-app captures into a website's `public/media` directory.
