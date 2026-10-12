# Reviewed Android runtime licenses

The current release resolution has **65 entries, 64 unique module/artifact/hash
identities**. `androidx.core:core:1.19.1` appears twice with identical bytes.
This review starts from that resolved graph, not just direct Gradle declarations.

`app/src/main/assets/licenses/runtime-review.json` records every artifact hash,
official POM URL/hash, matching published source-archive URL/hash, original
embedded license/NOTICE entry, and copyright attribution. `poms/` retains the
65 original vendor POM documents: 64 components and Guava's inherited parent.
The official declarations identify Apache-2.0 for all these exact components;
ListenableFuture 1.0 inherits its declaration from `guava-parent:26.0-android`.
This is a result of inspecting those artifacts/evidence, not an assumption that
every AndroidX/Kotlin dependency or future version has the same license.

Original runtime/source archives contained one unique full Apache-2.0 text
after deduplication, and no additional named NOTICE/COPYING text. The complete
original text is retained verbatim alongside 56 unique matching-source copyright
attributions and the project's complete GPL license. Build-only `lint.jar`,
Gradle/AGP and JVM-test tooling are not part of the APK runtime inventory.
The app's offline Open-source licenses screen reads the consolidated notices;
the original text and machine-readable manifest are also in the APK/AAB assets.

## Native graphics-path closure

`graphics-path-native-review.json` separately records the native evidence because
the published Maven sources JAR contains Kotlin sources, not its C++ sources.
The official [Graphics-Path 1.0.1 release page](https://developer.android.com/jetpack/androidx/releases/graphics#graphics-path-1.0.1)
links its release commit range ending at
`8a05a22af450d589ef911d772a001a49dcb05b71`. The original native/build/header
bytes are read from that immutable source commit and checked against Gitiles
blob ids. The official Maven module checksum and downloaded AAR checksum must
both match the actual resolved AAR.

That release's CMake builds only `Conic.cpp`, `PathIterator.cpp` and
`pathway.cpp`; it contains no external `find_package`, `add_subdirectory` or
`target_link_libraries`. Its Gradle compiler/link inputs include `-nostdlib++`,
no exceptions and no RTTI. The native translation units and bundled local
headers declare Apache-2.0 with original AOSP attributions. The library queries
Android graphics internals; that is not evidence that the complete Skia library
is bundled or that a Skia BSD license should be guessed from its name.

Each of the four AAR native libraries is hashed and inspected using the installed
SDK's `llvm-readelf`; the record includes DT_NEEDED dependencies and undefined
symbols. Only Android system libraries are accepted by the review tool; none
are copied into the app as additional DLL/shared-library payloads. Any unexpected
runtime dependency or native license header fails this review rather than being
classified as Apache automatically. This establishes published source and binary
dependency/notice provenance; it is not an independent bit-for-bit vendor rebuild.

## Reproduce and enforce

```powershell
$inventory = "$env:LOCALAPPDATA\OkuFlow\android-build\app\reports\release\runtime-dependencies.json"
python -B android/tools/audit_graphics_path_native.py
python -B android/tools/audit_runtime_licenses.py --inventory $inventory --refresh
pwsh -NoProfile -File scripts/build_android.ps1 -Release -AllowUnsignedRelease
```

Downloads and extracted inspection binaries stay in local app data, never Drive.
The Python scripts use the standard library; they neither build native code nor
contact a phone/adb server. Their network requests are review-time only. Normal
builds do no license-download network work: the Gradle inventory gate checks the
actual resolved artifact hashes and retained evidence bytes against the checked-in
review. The release validator verifies that the same notices and manifest are
actually packaged in both APK and AAB. New/changed components require deliberate
review and regeneration, rather than automatically inheriting this conclusion.

These are source/notice and artifact checks, not a legal opinion or Google Play
approval. Complete corresponding application source must be available to GPL
recipients when a tester build is distributed; do not distribute only an
unpublished APK while referring to an older source revision as its exact source.
