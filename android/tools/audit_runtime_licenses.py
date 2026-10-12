#!/usr/bin/env python3
"""Review exact release artifacts against official POM/source and embedded notices.

Network refresh is explicit and only writes its download cache outside the checkout.
Generated reviewed evidence/assets are checked in; builds verify their hashes offline.
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
import os
from pathlib import Path
import re
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

ANDROID = Path(__file__).resolve().parents[1]
ASSETS = ANDROID / "app/src/main/assets/licenses"
EVIDENCE = ANDROID / "licenses/poms"
NS = {"p": "http://maven.apache.org/POM/4.0.0"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def official_url(module, suffix):
    group, artifact, version = module.split(":")
    host = "https://dl.google.com/dl/android/maven2" if group.startswith("androidx.") else "https://repo.maven.apache.org/maven2"
    return f"{host}/{group.replace('.', '/')}/{artifact}/{version}/{artifact}-{version}{suffix}"


def fetch(url, cache, refresh):
    path = cache / (sha(url.encode()) + ".bin")
    if refresh or not path.exists():
        request = urllib.request.Request(url, headers={"User-Agent": "OkuFlow-license-review/1.0"})
        with urllib.request.urlopen(request, timeout=45) as response:
            data = response.read(64 * 1024 * 1024 + 1)
        if len(data) > 64 * 1024 * 1024:
            raise ValueError("Upstream evidence exceeds bounded size")
        path.write_bytes(data)
    return path.read_bytes()


def embedded_notices(data, prefix="", depth=0):
    if depth > 2:
        raise ValueError("Unexpected nested archive depth")
    result = []
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in sorted(archive.infolist(), key=lambda item: item.filename):
            name = entry.filename
            if entry.is_dir():
                continue
            if re.search(r"(^|/)(license|notice|copying|copyright)[^/]*(/|$)", name, re.I):
                if entry.file_size > 1024 * 1024:
                    raise ValueError("Unexpected large notice")
                result.append((prefix + name, archive.read(entry)))
            elif name.endswith(".jar") and not name.endswith("lint.jar"):
                result += embedded_notices(archive.read(entry), prefix + name + "!/", depth + 1)
    return result


def pom_licenses(data):
    root = ET.fromstring(data)
    licenses = [{"name": item.findtext("p:name", "", NS), "url": item.findtext("p:url", "", NS)}
                for item in root.findall("p:licenses/p:license", NS)]
    parent = root.find("p:parent", NS)
    parent_module = ":".join(parent.findtext("p:" + item, "", NS) for item in ("groupId", "artifactId", "version")) if parent is not None else None
    return licenses, parent_module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--gradle-cache", type=Path, default=Path.home() / ".gradle/caches/modules-2/files-2.1")
    parser.add_argument("--download-cache", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "OkuFlow/android-license-review")
    parser.add_argument("--refresh", action="store_true", help="Refresh official evidence, including source archives")
    args = parser.parse_args()
    cache = args.download_cache.resolve()
    if cache == ANDROID or ANDROID in cache.parents or cache.drive.lower() == "y:":
        raise ValueError("Evidence download cache must be outside Drive/checkout")
    cache.mkdir(parents=True, exist_ok=True)
    inventory = json.loads(args.inventory.read_text(encoding="utf-8-sig"))
    unique = {(item["module"], item["artifact"], item["sha256"]): item for item in inventory["artifacts"]}
    entries = sorted(unique.values(), key=lambda item: (item["module"], item["artifact"]))
    if not entries:
        raise ValueError("Empty runtime inventory")
    native_path = ANDROID / "licenses/graphics-path-native-review.json"
    native_bytes = native_path.read_bytes()
    native_review = json.loads(native_bytes)

    def review(item):
        module = item["module"]
        group, artifact, version = module.split(":")
        files = sorted((args.gradle_cache / group / artifact / version).rglob(item["artifact"]))
        matches = [path for path in files if sha(path.read_bytes()) == item["sha256"]]
        if not matches:
            raise ValueError("No matching resolved artifact in Gradle cache: " + module)
        data = matches[0].read_bytes()
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            native_entries = [name for name in archive.namelist() if name.endswith(".so")]
        if native_entries and (module != native_review["module"] or item["sha256"] != native_review["aarSha256"]):
            raise ValueError("Native payload needs a separate source/link/license closure review: " + module)
        pom_url = official_url(module, ".pom")
        pom = fetch(pom_url, cache, args.refresh)
        licenses, parent = pom_licenses(pom)
        poms = [(module, pom_url, pom)]
        if not licenses and parent:
            parent_url = official_url(parent, ".pom")
            parent_pom = fetch(parent_url, cache, args.refresh)
            licenses, _ = pom_licenses(parent_pom)
            poms.append((parent, parent_url, parent_pom))
        if not licenses or any("apache" not in value["name"].lower() or "2.0" not in value["name"] for value in licenses):
            raise ValueError("License declaration requires independent review: " + module)
        notices = embedded_notices(data)
        # Matching published sources are evidence for original copyright/NOTICE material,
        # rather than guessing attribution from artifact names or family descriptions.
        source_url = official_url(module, "-sources.jar")
        sources = fetch(source_url, cache, args.refresh)
        notices += [("published-sources!/" + name, content) for name, content in embedded_notices(sources)]
        copyrights = set()
        with zipfile.ZipFile(io.BytesIO(sources)) as source_archive:
            for entry in source_archive.infolist():
                if entry.filename.endswith((".kt", ".java", ".cpp", ".h")) and entry.file_size < 1024 * 1024:
                    text = source_archive.read(entry).decode("utf-8", errors="replace")[:5000]
                    for line in text.splitlines():
                        if "copyright" in line.lower() and len(line) < 400:
                            copyrights.add(line.strip(" /*"))
        return item, licenses, poms, notices, sorted(copyrights), source_url, sha(sources)

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        reviewed = list(pool.map(review, entries))
    ASSETS.mkdir(parents=True, exist_ok=True)
    (ASSETS / "texts").mkdir(exist_ok=True)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    texts = {}
    components = []
    evidence = {}
    lines = ["OkuFlow Camera — Open-source runtime licenses and attributions", "",
             "Exact release runtime artifacts are listed below. Original notices are retained verbatim.",
             "Source: https://github.com/Cem-Kaya/oku_flow", "",
             "The application is available under the repository's GPL-3.0-only and commercial licensing terms.", ""]
    for item, licenses, poms, notices, copyrights, source_url, source_sha in reviewed:
        refs = []
        for module, url, content in poms:
            name = module.replace(":", "_") + ".pom"
            (EVIDENCE / name).write_bytes(content)
            evidence[name] = {"url": url, "sha256": sha(content)}
            refs.append(name)
        retained = []
        for original, content in notices:
            digest = sha(content)
            texts[digest] = content
            retained.append({"entry": original, "asset": f"texts/{digest}.txt", "sha256": digest})
        components.append(dict(item, declaredLicenses=licenses, pomEvidence=refs,
                               sourceEvidence={"url": source_url, "sha256": source_sha},
                               retainedNotices=retained, copyrights=copyrights))
        lines += [item["module"], "Artifact SHA-256: " + item["sha256"], "Declared license: " + "; ".join(x["name"] for x in licenses)]
        lines += ["POM: " + value[1] for value in poms]
        lines += copyrights + [""]
    if not texts:
        raise ValueError("No original license texts found")
    for digest, content in sorted(texts.items()):
        (ASSETS / "texts" / (digest + ".txt")).write_bytes(content)
        lines += ["Original license/notice SHA-256: " + digest, content.decode("utf-8", errors="strict"), ""]
    project_license = (ANDROID.parent / "LICENSE").read_bytes()
    (ASSETS / "OKUFLOW_LICENSE.txt").write_bytes(project_license)
    lines += ["OkuFlow application license", project_license.decode("utf-8"), ""]
    (ASSETS / "GRAPHICS_PATH_NATIVE_REVIEW.json").write_bytes(native_bytes)
    lines += ["AndroidX graphics-path native source/build and binary closure", native_review["module"],
              "Source commit: " + native_review["sourceCommit"], native_review["runtimeLink"],
              native_review["licenseConclusion"], *native_review["copyrights"], ""]
    notice_bytes = ("\n".join(lines) + "\n").encode()
    (ASSETS / "OPEN_SOURCE_NOTICES.txt").write_bytes(notice_bytes)
    manifest = {"schemaVersion": 1, "review": "official-POM-and-original-notice-evidence",
                "resolvedEntries": len(inventory["artifacts"]), "uniqueArtifacts": len(components),
                "components": components, "pomEvidence": evidence,
                "assetFiles": {"OPEN_SOURCE_NOTICES.txt": sha(notice_bytes), "OKUFLOW_LICENSE.txt": sha(project_license),
                               "GRAPHICS_PATH_NATIVE_REVIEW.json": sha(native_bytes),
                               **{f"texts/{digest}.txt": digest for digest in sorted(texts)}},
                "limits": ["Official artifact declarations and retained original material; not a legal opinion",
                           "Build tools/test-only dependencies are outside the APK runtime inventory",
                           "New artifact hashes or graph changes require a new review"]}
    (ASSETS / "runtime-review.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Reviewed {len(components)} unique runtime artifacts from {len(inventory['artifacts'])} resolved entries; retained {len(texts)} unique original notice/license texts.")


if __name__ == "__main__":
    main()
