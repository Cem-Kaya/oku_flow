I reviewed Muse's R3 adb ownership work. The Windows Job ownership is sound, and your fixes since my first read look correct. The main open issue is a design-level conflict: the private server holds the phone's USB connection exclusively, which can hide the phone from the owner's SDK adb. Nothing was built or run; everything here comes from reading the code and checking primary docs.

Your files changed while I was reviewing. This report is against `adb_owned_server.cpp` as of 23:40, `phone_camera.cpp` as of 23:43:55, and `phone_camera_tests.cpp` as of 23:43.

## Already fixed by you, and correct as far as I can tell from reading
- **Cooldown bug confirmed and fixed** (`adb_owned_server.cpp:259`). The check is now unconditional and the default `nextRestart_{}` lets the first launch through.
- **Cancellation is re-checked after every probe** (205, 223, 233), and a "Responsive" answer only counts if the child is still running (206/224/234).
- **An unexpected child exit now discards the port** (254).
- **The `INVALID_HANDLE_VALUE` bug in the null-device handles is fixed** (13, 70-72). It was a real bug in Muse's version.
- **`Endpoint()` now fails closed** (`phone_camera.cpp:81-86`). This is the right call: transport IDs restart at 1 on each server, so sending an ID from one server to another can select the wrong phone. Finding M1 below is the piece it still needs.
- By reading, the tests at `phone_camera_tests.cpp:463-551` pass against the current code. The earlier code failed 483-485, 517-519 and 542/548.

## Findings, most severe first

**H1 – The private server can take the phone away from the owner's other adb tools (needs an owner decision).**
- Windows' generic USB driver (WinUSB) only lets one program use a device at a time. Microsoft's driver comparison table says WinUSB "Supports multiple concurrent applications: No".
- The pinned adb 37.0.0 uses WinUSB on Windows, through Google's driver or the libusb backend. Each adb server opens the device separately and retries every second.
- Discovery starts unconditionally every ~5 s after startup (`app_bootstrap.cpp:1216-1219`). So any launch where nothing listens on 5037 starts the owned server, even for webcam-only users, and it claims every adb phone.
- After that, SDK adb, Android Studio and `build_android.ps1 -Install` start their own server on 5037 and see no device for as long as OkuFlow runs. Emulators are affected the same way, because adb scans emulator ports by default.
- The reverse also happens: after an owned crash or restart, a running 5037 server can grab the freed phone, and OkuFlow, which stays in owned mode, loses it.
- Mitigations:
  - (a) M1's switch back to the existing server.
  - (b) Launch the owned child with `ADB_EMU=0`. This is verified in AOSP `client/main.cpp`; OkuFlow ignores emulators anyway. It needs an environment option in `LaunchOptions`.
  - (c) Only start owned mode when phone use is intended: a saved phone preference, an explicit phone pick, or the setup assistant.
  - (d) Document the exclusivity, and log the owned port so `ANDROID_ADB_SERVER_PORT` can point other tools at it.
- Whether this trade-off is acceptable compared with the old shared-5037 behavior is the owner's call.

**M1 – Owned mode never switches back to an existing 5037 server** (`phone_camera.cpp:254`).
- Once the owned manager exists, the default port is never checked again. With fail-closed `Endpoint()`, a permanently broken bundled adb (missing `AdbWinApi.dll`, antivirus block) means no phone path even after the owner's SDK server comes up. HEAD would have used it.
- Fix: in the owned branch, if `!owned->OwnsRunningServer()` and the default port has a listener, `Stop()` the manager and compare-and-clear `runtime.ownedServer`, then use the existing server. Never switch while our child is alive.
- Test: dead child plus a listener on a private port standing in for 5037 → no relaunch, and `OwnedAdbServerPort()` is empty.

**M2 – The 3 s readiness deadline is shorter than adb's own startup, and the UI then gives wrong advice.**
- AOSP `adb_server_main` installs the listener disabled. It only starts accepting connections after `adb_wait_for_device_initialization()`, which waits for the USB scan and for every USB transport to come online. That wait is capped at 3 s and runs after `adb_auth_init()`.
- An unauthorized or still-connecting phone, the normal first-run state, therefore makes the server accept TCP connections but not answer them for about 3 s or more. Result: `NotReady`, then `ListUsbPhones` times out on our port, then `ServerUnresponsive`.
- The user is then told "USB connection is not responding … end adb in Task Manager" with assertive screen-reader politeness (`app_interaction.cpp:207-209, 279-283`). That is wrong advice for an owned server, and it appears during normal startup.
- The 30 s cooldown measured from launch does prevent a restart loop.
- Fix:
  - Set the readiness deadline to at least 5 s.
  - Have `EnsureAdbServer` return the result so `Enumerate` can report a "starting/recovering" status for an owned server that is starting or restarting.
  - Keep the Task Manager advice for borrowed servers only.
  - A new status string needs the 25-language parity work.

**M3 – A starting session counts itself as live, so it can never restart a hung server.**
- `Run` registers its phone as occupied (`phone_camera.cpp:994`) before `stream()` calls `EnsureAdbServer` (1009). The deferral check (220) then sees that same session.
- So a user starting the camera against a hung owned server spends ~1 s probing, gets `RestartDeferredLiveSession`, and the start fails. Only the background discovery after `Run` exits can recover it.
- Fix: count a session as live only once its control socket is connected, or exclude the caller's own serial. Keep the occupied set for suppressing discovery probes.
- Test: a "starting" session reported by the sessions callback → `Restarted`.

**L1 – The occupied set is not a counter** (`phone_camera.cpp:59`, 89-97).
- `StopCapture` gives the old session 1.5 s and then continues on a detached thread (`media_capture_facade.cpp:191-215`). A quick reselect can overlap an old and a new `Run` for the same serial.
- The old session's cleanup then erases the serial while the new one is live, and a restart becomes allowed during streaming. The structure predates R3, but R3 now uses it to decide whether to kill the server.
- Fix: `std::map<std::string,int>` or a multiset.

**L2 – Reusing the same port on a hung restart** (comment at 181-184; the restart path at 244-247 keeps `port_`).
- Microsoft's `SO_EXCLUSIVEADDRUSE` page says a port that accepted connections cannot be re-bound until those connections are inactive. adb then retries for 0.5 s and aborts (`client/main.cpp:164-168`).
- This is handled by the second attempt, but it costs 0.5 s or more, one abort, and one of the two attempts.
- Fix: `port_.reset()` alongside `child_.reset()` at 245. No outside consumer needs the port to stay the same.

**L3 – Thin evidence before restarting.** Two probes 50 ms apart (≤100 ms connect, 300 ms read each) can restart a healthy server after less than 1 s of stall under load. Keep a `firstUnresponsiveAt_` timestamp so a restart requires two separate discovery cycles some seconds apart, or lengthen the confirmation read to 1–2 s.

**L4 – Owned mode can restart during shutdown.** After `ShutdownOwnedAdbServer` (575), any late caller with an uncanceled token, such as an abandoned worker, takes the first-start path and launches a new child. The Job still reaps it at exit, but it races destruction of the static runtime. Add a shutdown latch that only `SetPhoneAdbTestHooks` clears.

**L5 – `Stop()` waits for in-flight work.** It can block teardown for roughly 8–10 s if an uncanceled caller holds the manager. Discovery is canceled and joined first (`app_bootstrap.cpp:1224-1225`), so only abandoned workers can cause this. Folding an atomic `stopRequested_` into `Canceled()` would let `Stop()` interrupt the work.

**L6 – Narrow window for a foreign listener on the picked port.** A failed-bind child stays alive about 0.5 s while it retries. If a foreign adb-protocol listener grabbed the port between pick and bind, it would pass line 206 and be treated as ours. This is very unlikely. Optional hardening: check that the listener's owning process ID is our child via `GetExtendedTcpTable` (observation only, never used to kill).

**L7 – Flaky fixture process-ID file.** `WritePid` creates the file, writes, then closes. Readers that check existence first (`tests:284-286, 344-346`, fixture line 53) can read an empty file and get pid 0. Fix: write to a temp file and `MoveFileExW` it into place, or poll until a non-zero number parses.

**Info**
- The resume-failure message calls `LastErrorText` after `TerminateJobObject` (`adb_owned_server.cpp:120-123`), so it reports the wrong error code.
- adb's stderr goes to NUL, so an owned server that fails to start (missing DLL, bind failure) leaves no diagnostics. Consider a bounded log in LocalAppData.
- `adb_owned_server.hpp:98-100` says "single-flight on the first result"; it is actually a mutex, and each waiter re-decides.
- The inheritable NUL handles exist briefly during launch and could leak into an unrelated concurrent spawn. Harmless.
- `Connect` could throw `AdbServerUnavailable` directly when the port is 0 instead of attempting the connection.

## Checked and fine
- **Job lifecycle:**
  - The child starts suspended, is assigned to the job, then resumed, so there is no window where it runs outside the job.
  - The job handle is not inherited.
  - Kill-on-close fires when the last handle closes.
  - Nested jobs work on Windows 8+ because our job is empty and has no UI limits.
  - Failure paths terminate the exact child through its handle.
  - The fixture's owner-terminated test exercises this with the production `Launch`.
- **adb assumptions:**
  - `-L tcp:N` with an empty host binds only IPv4 `127.0.0.1` on Windows, with no IPv6 fallback.
  - The bind uses `SO_EXCLUSIVEADDRUSE`, and a bind loser exits.
  - The `host:version` framing (`OKAY`, `0004`, four hex digits) is correct.
- **Borrowed and overridden servers** are never stopped.
- **No lock-order inversion:** the manager lock is taken before the runtime lock, and `Endpoint()` releases the runtime lock before calling `Port()`.
- **Tests stay private:** every test `Enumerate` runs inside a `FakePhoneAdb` scope, so no test reaches 5037.
- **Shutdown order:** discovery is canceled and joined, capture is stopped, then `ShutdownOwnedAdbServer` runs (1285).
- **USB recovery interaction:** recovery refuses on a hung `host:version`. The failing session is still occupied during its recovery attempt, so a restart can't race it.

## Missing tests
1. A child that exits during readiness → the second launch gets a different port (record the ports passed to launch).
2. A "Responsive" probe after our child has died → never `Launched`.
3. A probe that is never responsive → `NotReady` with the child alive, then `Throttled` within the cooldown, then `Restarted` after it.
4. Cancellation mid-restart: a sessions callback that sets the cancel flag → `Canceled` with the child alive. Today's test at 434 cancels before entry, so it doesn't cover this.
5. Cancellation between stopping and relaunching → the next call relaunches immediately.
6. Deferral is temporary: live then not live → `Restarted`.
7. Two threads calling `EnsureRunning` → exactly one launch.
8. `Stop()` during an in-flight `EnsureRunning` → the child is killed and later calls return `Stopped`.
9. The production wiring has no test at all: `EnsureAdbServer` owned branch, `Endpoint` routing, `ProbeAdbHostVersion`, the argument list, and `ShutdownOwnedAdbServer` killing the child.
   - Proposed seam: extend the test hooks with an owned-mode variant (a private closed port standing in for 5037, plus a fixture executable path).
   - Add a fake-adb mode to the fixture: parse `-L tcp:N server nodaemon`, bind exclusively, answer `host:version` and a minimal device listing, with hang and bind-fail options.
10. The shutdown latch (L4) and switching back to the existing server (M1).

## Docs still needed
- `agents.md:259-262` still describes the old direct-bind behavior with a 30 s throttle.
- `docs/adb_distribution.md` currently covers only USB recovery; it needs the owned-server model and the H1 exclusivity note.
- `android/PROTOCOL.md`.
- `docs/code_reference.md` entries for `OwnedChildProcess`, `OwnedAdbServer`, `ShutdownOwnedAdbServer`, `OwnedAdbServerPort`, `PhoneDiscoveryStatus` and `ownedByThisProcess`.
- `CHANGELOG.md`.

## Not verified
- No build, tests or device runs.
- H1's effect on a second adb server is inferred from the Microsoft docs and the AOSP USB code, not observed. Some OEM USB drivers that aren't WinUSB may behave differently.
- The adb behavior was checked on AOSP `main`, not on the exact 37.0.0 tag.
- Whether a killed server's connections actually block the port, and whether an adb abort produces Windows crash-report noise, are documented or inferred but not reproduced.

Sources:
- [Using SO_REUSEADDR and SO_EXCLUSIVEADDRUSE](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)
- [JOBOBJECT_BASIC_LIMIT_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information)
- [AssignProcessToJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject)
- [Choose a driver model (WinUSB concurrency table)](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-considerations)
- [OSR: WinUSB multiple-app access](https://community.osr.com/t/multiple-app-device-access-limitations-using-winusb-dll/27705)
- [AOSP adb source](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/), files `client/main.cpp`, `adb.cpp`, `socket_spec.cpp`, `sysdeps_win32.cpp`, `client/usb_windows.cpp`
