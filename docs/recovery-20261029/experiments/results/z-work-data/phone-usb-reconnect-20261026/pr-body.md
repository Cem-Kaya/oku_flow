After a manual webcam choice, OkuFlow stopped its idle USB server and had no phone entry from which the user could restart discovery. Keep an automatic OkuFlow Camera choice available before discovery, hide webcam format controls immediately while waiting, and select a real phone only from a fresh discovery result.

A new native Android USB/ADB interface arrival selects the phone again, following the requested unplug/replug behavior. Ordinary polling respects a manual webcam choice on the same connection. Existing phone/lens intent survives reconnecting; unrelated interfaces, non-USB paths, removal events, temporary Assistant camera ownership and duplicate arrivals cannot redirect or restart an active phone. Existing recording, source-retirement and USB-server ownership rules remain intact.

Validation:

- Tracked release compilation and background matrix passed: 262 CPU passes (four with optional skipped Qt rows), nine whole-case skips; 15 CUDA delta passes, five skips; zero unresolved cases.
- Focused app/source-sharing checks passed with precompiled headers enabled and disabled, including immediate control visibility before discovery, stale-result fencing, manual webcam intent, USB arrival filtering and preserved reconnect identity. The phone protocol contract also passed in both configurations.
- All 24 language catalogs retain 915 complete keys. The phone caption reuses the existing localized automatic-camera label; no translations or dependencies were added.

Local native GUI checks were excluded. Hosted run 40363120444 passed on d551fe5a, including 252 background CPU passes (six with optional skipped Qt rows), 12 whole-case skips, all 67 native GUI cases and the scaled native movement checks. The physical unplug/replug check is deferred at the owner's request and is not claimed as passed.