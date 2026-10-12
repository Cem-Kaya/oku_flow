#!/usr/bin/env python3
"""Inspect the exact published graphics-path AAR and its official 1.0.1 release sources."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request
import zipfile

ANDROID = Path(__file__).resolve().parents[1]
COMMIT = "8a05a22af450d589ef911d772a001a49dcb05b71"
ROOT = f"https://android.googlesource.com/platform/frameworks/support/+/{COMMIT}/graphics/graphics-path/"
MODULE = "https://dl.google.com/dl/android/maven2/androidx/graphics/graphics-path/1.0.1/graphics-path-1.0.1.module"
AAR_SHA = "8ca4032b6d79b351f0b59ad4b580eddbb9423e1652f7c958830687f1eee2ec03"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "OkuFlow/android-license-review/native-graphics-path")
    parser.add_argument("--readelf", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe")
    args = parser.parse_args()
    cache = args.cache.resolve()
    if cache == ANDROID or ANDROID in cache.parents or cache.drive.lower() == "y:":
        raise ValueError("Native evidence cache must be outside Drive")
    cache.mkdir(parents=True, exist_ok=True)

    def download(url):
        path = cache / (digest(url.encode()) + ".bin")
        if not path.exists():
            with urllib.request.urlopen(url, timeout=45) as response:
                data = response.read(64 * 1024 * 1024 + 1)
            if len(data) > 64 * 1024 * 1024:
                raise ValueError("Native evidence exceeds bounded size")
            path.write_bytes(data)
        return path.read_bytes()

    sources = []
    copyrights = set()

    def inspect_file(path, expected_blob=None):
        url = ROOT + path + "?format=TEXT"
        data = base64.b64decode(download(url))
        git_sha = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if expected_blob and git_sha != expected_blob:
            raise ValueError("Native Gitiles blob checksum mismatch")
        text = data.decode("utf-8")
        header = text[:text.find("*/") + 2] if text.startswith("/*") else ""
        if path.endswith((".cpp", ".h", ".gradle")) and "Apache License, Version 2.0" not in header:
            raise ValueError("Native source/header requires additional license review: " + path)
        for line in header.splitlines():
            if "copyright" in line.lower():
                copyrights.add(line.strip(" /*"))
        sources.append({"path": path, "url": url, "gitBlob": git_sha, "sha256": digest(data), "licenseHeader": header})
        return text

    def inspect_tree(path):
        listing = json.loads(download(ROOT + path + "?format=JSON").decode()[4:])
        for entry in listing["entries"]:
            name = path + entry["name"]
            if entry["type"] == "tree":
                inspect_tree(name + "/")
            else:
                inspect_file(name, entry["id"])

    gradle = inspect_file("build.gradle")
    inspect_tree("src/main/cpp/")
    if "-nostdlib++" not in gradle:
        raise ValueError("Native C++ runtime link contract changed")
    cmake_path = next(x for x in sources if x["path"].endswith("CMakeLists.txt"))
    cmake = base64.b64decode(download(cmake_path["url"])).decode()
    if "target_link_libraries" in cmake or "add_subdirectory" in cmake or "find_package" in cmake:
        raise ValueError("Unexpected additional native link input")
    compiled = re.findall(r"\b[A-Za-z_][A-Za-z_0-9]*\.cpp\b", cmake)
    if sorted(compiled) != ["Conic.cpp", "PathIterator.cpp", "pathway.cpp"]:
        raise ValueError("Native translation-unit closure changed")

    module_bytes = download(MODULE)
    module = json.loads(module_bytes)
    published = [x for variant in module["variants"] for x in variant.get("files", []) if x["name"].endswith(".aar")]
    if not published or any(x["sha256"] != AAR_SHA for x in published):
        raise ValueError("Official native AAR checksum differs from reviewed runtime")
    aar_bytes = download(MODULE.rsplit("/", 1)[0] + "/graphics-path-1.0.1.aar")
    if digest(aar_bytes) != AAR_SHA:
        raise ValueError("Published native AAR payload checksum mismatch")
    aar_path = cache / "graphics-path-1.0.1.aar"
    aar_path.write_bytes(aar_bytes)
    binaries = []
    with zipfile.ZipFile(aar_path) as archive:
        for name in sorted(archive.namelist()):
            if not name.endswith(".so"):
                continue
            data = archive.read(name)
            local = cache / "native" / Path(name).relative_to("jni")
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(data)
            result = subprocess.run([str(args.readelf), "--dynamic", "--dyn-syms", "--wide", str(local)],
                                    check=True, capture_output=True, text=True)
            needed = re.findall(r"Shared library: \[([^]]+)\]", result.stdout)
            if not set(needed).issubset({"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so"}):
                raise ValueError("Unexpected non-platform native dependency: " + name)
            undefined = []
            for line in result.stdout.splitlines():
                if " UND " in line:
                    symbol = line.split(" UND ", 1)[1].strip()
                    if symbol:
                        undefined.append(symbol)
            binaries.append({"entry": name, "sha256": digest(data), "needed": needed,
                             "undefinedSymbols": undefined, "readelfOutputSha256": digest(result.stdout.encode())})
    if len(binaries) != 4:
        raise ValueError("Expected native library for each of four ABIs")
    review = {"module": "androidx.graphics:graphics-path:1.0.1", "aarSha256": AAR_SHA,
              "officialModule": {"url": MODULE, "sha256": digest(module_bytes)},
              "releasePage": "https://developer.android.com/jetpack/androidx/releases/graphics#graphics-path-1.0.1",
              "releaseCommits": "https://android.googlesource.com/platform/frameworks/support/+log/4fcd99eacd92d7c73fb1d3580fd423ed7704a98a.." + COMMIT + "/graphics/graphics-path",
              "sourceCommit": COMMIT, "compiledSources": compiled, "sources": sources,
              "runtimeLink": "-nostdlib++; no add_subdirectory/find_package/target_link_libraries in release CMake",
              "binaries": binaries, "copyrights": sorted(copyrights),
              "licenseConclusion": "Reviewed native translation units and bundled local headers declare Apache-2.0; system Android libraries are not redistributed. No additional third-party linked library identified.",
              "limits": "Source provenance and published binary dependency/notice review; not an independent reproducible rebuild of the vendor AAR."}
    output = ANDROID / "licenses/graphics-path-native-review.json"
    output.write_text(json.dumps(review, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Native review: {len(sources)} immutable source/build/header records, {len(binaries)} ABI libraries; only Android system dynamic dependencies.")


if __name__ == "__main__":
    main()
