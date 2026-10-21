# Local development storage

On the owner's Windows workstation, large OkuFlow work data now lives under
`Z:\Projects\OkuFlow`. The main Git checkout remains at
`Y:\Drive\My_Drive\folder\Projects\oku_flow`.

| Folder on Z: | Contents |
| --- | --- |
| `local-work-data` | Build outputs, development SDKs, OCR models/engines, installer artifacts, test evidence and the older local worktrees |
| `t3-worktrees` | Relocated T3 checkouts and the active checkout's build outputs |
| `migration-20261021-181357` | Original inventory, copy failures, SHA-256/structure manifests, Git-state checks, cleanup receipts and a retired source snapshot |

Existing work-folder paths below `%LOCALAPPDATA%\OkuFlow` redirect to their
matching Z: folders through NTFS directory junctions. Inactive T3 checkout
paths below `%USERPROFILE%\.t3\worktrees\oku_flow` have individual junctions.
These preserve Git registrations, T3 thread bindings and existing absolute
CMake/SDK paths. They do not store a second copy on C:. Disk analyzers that
follow junctions can still display the relocated files below their old paths.

Windows holds this thread's active `feature-uct-recap-player` directory open.
Its roughly 20 MiB of live source remains on C:; its `build` directory is a
junction to `Z:\Projects\OkuFlow\t3-worktrees\feature-uct-recap-player\build`.
That Z: folder contains build outputs, not another live checkout. The migration
snapshot has its `.git` pointer renamed to avoid accidental use as a checkout.
Do not remove or replace the active source directory during a running session.

Private `android-signing`, `google-play`, nested app-state `OkuFlow`, and
`mcp-instances` folders remain on C:. Small loose work files also remain there:
automatic approval review rejected their move with only “blocked by policy.”
Installed Qt, Visual Studio and other shared system tools were not relocated.

For new large Windows work folders on this workstation, choose
`Z:\Projects\OkuFlow\local-work-data\<task-name>`. Use the documented output-root
overrides, including `OKUFLOW_AGENT_RELEASE_BUILD_DIR`, where supported.
Before configuring a new T3 checkout that still lives on C:, place its
`build` directory on Z: and retain a junction at the preset's expected path.
Never overwrite an existing build directory or redirect without inspecting it.
Keep existing cache aliases and private credentials in place. This is a local
workstation layout, not a compiled application default or a CI drive requirement.

The migration verified file contents and link structure before removing source
copies. All 13 registered C: worktrees retained their original HEAD and
modified/untracked-file state; the Y: checkout was unchanged. A read-only Ninja
graph check confirmed the relocated FFmpeg build remains discoverable. These
checks do not establish a new application/test pass or clear failed/unfinished
test evidence. Existing release, desktop and hardware gates still apply.
