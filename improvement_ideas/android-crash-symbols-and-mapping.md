# Android crash symbols and release mapping

Requested 2026-10-13; the owner's later instruction prioritizes clearing the
diagnostic warnings after the code-2 / 0.1.1 USB fix reached a Console draft.

Play accepts the version-code-2 bundle with two diagnostic warnings: no Java
deobfuscation mapping and no native debug symbols. Release currently explicitly
sets `isMinifyEnabled = false`, so that code-2 artifact contains no generated
mapping. The recovered camera-service exception already has readable Kotlin
class/method names and source lines. There is no documented Play declaration
in the inspected publishing flow that suppresses the generic mapping warning.

## Implemented code-3 mapping

Version code 3 / 0.1.2 runs R8 9.4.24 with `-dontobfuscate`, `-dontshrink`,
`-dontoptimize` and `-keepattributes *`, plus platform/default and dependency
rules. R8 and AGP actually generate the final map; no hand-written identity file
is shipped. The inspected review AAB embeds 46,640,334 mapping bytes, including
15,747 unchanged runtime class mappings and AGP's generated `ComposeStackTrace`
to `$$compose` trace alias. Required DEX/desugaring and Compose source records
remain truthful, including generated methods; optimization is not claimed.
The validator binds the local map to its AAB entry by exact SHA-256, checks R8
metadata/readable application lifecycle names/effective flags and records hashes.
The signed upload's code-3 Downloads table now confirms `mapping.txt`, 46.6 MB,
as a ReTrace asset. Console also reports 0% obfuscation/shrinking/optimization,
consistent with this diagnostic policy. Native debug symbols still show Upload;
no native-warning removal is claimed.

## Remaining native symbol gate

All four original graphics-path 1.0.1 AAR libraries have GNU build IDs and
`.dynsym`, but no `.symtab`, `.debug_*`, or `.gnu_debugdata`. The AAR contains
no separate symbols or prefab debug data. With `ndk.debugSymbolLevel = "FULL"`,
AGP's `mergeReleaseNativeDebugMetadata` is `NO-SOURCE` and the inspected AAB
contains no native symbol metadata. This is not a native-warning removal.
A separate isolated SYMBOL_TABLE extraction with `--info` likewise reports
already-stripped input for every ABI. Neither flag produces matching metadata
from this dependency. Local evidence is under
`%LOCALAPPDATA%\OkuFlow\android-diagnostics-code3`; code-2 signed outputs stayed
untouched. Debug/unsigned Release gates pass 65 tests each, zero lint issues,
92 translated keys and 34 validator fixtures; Play verification belongs to
the owner's signed upload.

For genuine native diagnostic coverage:

- Obtain symbols matching every shipped native library and ABI. The current
  AndroidX graphics-path dependency is a prebuilt AAR; inspect its original
  symbol/debug sections and upstream matching-symbol availability. Rebuilding
  a different binary does not provide matching symbols for Google's binary.
- Configure `ndk.debugSymbolLevel = "FULL"` where unstripped input exists;
  fall back to `SYMBOL_TABLE` for symbol-file size limits. Validate the AAB's
  native metadata or upload the matching symbols ZIP for its exact version code.
  Already stripped dependencies cannot recover absent debug data from this flag.
- Retain the actual generated mapping for every release. Enabling R8 shrinking
  or optimization remains a separately tested change; this diagnostic build
  explicitly disables both.
- Do not manufacture an empty or guessed identity mapping. It adds no useful
  diagnostic information and cannot describe optimized line transformations.
- Preserve artifact SHA-256, version code, native build IDs, exact source commit
  and matching symbol/mapping hashes in local release evidence. Confirm Play
  symbolication with a controlled test before claiming diagnostic coverage.

Sources: [native symbols](https://developer.android.com/build/include-native-symbols),
[R8 optimization](https://developer.android.com/topic/performance/app-optimization/enable-app-optimization),
[reviewed native dependency](../android/licenses/README.md#native-graphics-path-closure).
