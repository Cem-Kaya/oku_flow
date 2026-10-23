"""Offline UCT payload inventory and explicit, new-only private staging.

Standard library only. Never loads a DLL/model, invokes a tool, downloads,
prepares an engine, discovers a cache, or changes an environment variable.
The checked-in inventory is a candidate, not redistribution approval.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import stat
import struct
import sys


CATALOG = Path(__file__).with_name("offline-payload-candidate.json")
DOMAINS = {"redistribution", "model", "corpus", "runtime-closure"}
ROOTS = {"models", "variant", "converter", "repo", "tensorrt", "cuda", "review", "corpus"}
FILE_KEYS = {"id", "root", "source", "path", "bytes", "sha256", "package", "pe"}
MAX_JSON = 1024 * 1024
MAX_FILE = 600 * 1024 * 1024
MAX_TOTAL = 2 * 1024 * 1024 * 1024
RECEIPT = "uct-offline-payload.json"
INCOMPLETE = ".uct-offline-incomplete.json"


class Refusal(ValueError):
    """A failed offline boundary; callers retain the original failed output."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Refusal(message)


def keys(value: dict, expected: set[str], label: str) -> None:
    require(type(value) is dict and set(value) == expected, f"{label}: unexpected/missing fields")


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def hash_text(value: str) -> bool:
    return type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None and value != "0" * 64


def relative(value: str) -> str:
    # Also reject Win32 aliases, device names, ADS, case collisions and glob syntax.
    require(type(value) is str and 0 < len(value) <= 240, "invalid relative path")
    parts = value.split("/")
    for part in parts:
        require(re.fullmatch(r"[A-Za-z0-9_.-]+", part) is not None and part not in {".", ".."}
                and not part.endswith((".", " ")), "unsafe relative path")
        require(part.split(".")[0].upper() not in {"CON", "PRN", "AUX", "NUL", "CLOCK$"}
                and re.fullmatch(r"(?:COM|LPT)[0-9]+", part.split(".")[0], re.I) is None,
                "reserved path component")
    return value


def local_path(path: Path, *, missing_leaf: bool = False) -> Path:
    # Require the physical local path. Workstation LocalAppData junctions must
    # be resolved by the caller, never followed as implicit staging authority.
    def checked_chain(value):
        require(value.is_absolute() and not str(value).startswith(("\\\\", "//")), "explicit local absolute path required")
        if os.name == "nt":
            require(re.fullmatch(r"[A-Za-z]:", value.drive) is not None, "local drive required")
        for part in value.parts[1:]:
            require(part not in {".", ".."} and not part.endswith((".", " "))
                    and re.search(r'[<>:"/\\|?*\x00-\x1f]', part) is None,
                    "unsafe absolute path component/stream")
            stem = part.split(".")[0].rstrip(" ").upper()
            require(stem not in {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"}
                    and re.fullmatch(r"(?:COM|LPT)[0-9¹²³]+", stem) is None,
                    "reserved absolute path component")
        exists = True
        for item in [*reversed(value.parents), value]:
            try:
                info = item.lstat()
            except FileNotFoundError:
                require(missing_leaf and item == value, "missing path component")
                exists = False
                continue
            require(not stat.S_ISLNK(info.st_mode) and not (getattr(info, "st_file_attributes", 0) & 0x400),
                    "reparse/symlink path refused")
            if item != value:
                require(stat.S_ISDIR(info.st_mode), "non-directory ancestor")
        return exists

    exists = checked_chain(path)
    # Canonicalize only after rejecting reparse ancestors; check both spellings
    # again so resolution never confers authority on an untrusted alias.
    physical = path.resolve(strict=True) if exists else path.parent.resolve(strict=True) / path.name
    checked_chain(path)
    checked_chain(physical)
    return physical


def regular_file(path: Path) -> Path:
    path = local_path(path)
    info = path.stat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "nonregular/hardlinked input refused")
    return path


def directory_identity(path: Path) -> tuple[int, int]:
    info = local_path(path).stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_ino != 0, "physical directory identity required")
    return info.st_dev, info.st_ino


def owned_file(root: Path, name: str) -> Path:
    root = local_path(root)
    path = regular_file(root / relative(name))
    require(path.is_relative_to(root), "file escaped root")
    return path


def read_json(path: Path, limit: int = MAX_JSON) -> tuple[dict, bytes]:
    path = regular_file(path)
    require(path.stat().st_size <= limit, "JSON file exceeds bound")
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    require(0 < len(raw) <= limit, "JSON file exceeds bound")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result

    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(Refusal("nonfinite JSON")))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise Refusal("invalid bounded UTF-8 JSON") from error
    require(type(value) is dict, "JSON object required")
    return value, raw


def file_hash(path: Path, size: int) -> str:
    require(type(size) is int and 0 < size <= MAX_FILE and path.stat().st_size == size, "file length mismatch/bound")
    result = hashlib.sha256()
    count = 0
    with path.open("rb") as source:
        while block := source.read(min(1024 * 1024, size + 1 - count)):
            count += len(block)
            require(count <= size, "file grew while hashing")
            result.update(block)
    require(count == size, "file changed while hashing")
    return result.hexdigest()


def pe_imports(path: Path) -> dict:
    """Inspect PE32+ metadata only, including delay imports; never load code."""
    with path.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def unpack(fmt, offset):
            require(0 <= offset <= len(data) - struct.calcsize(fmt), "truncated PE")
            return struct.unpack_from(fmt, data, offset)

        require(data[:2] == b"MZ", "missing MZ header")
        pe = unpack("<I", 60)[0]
        require(data[pe:pe + 4] == b"PE\0\0", "missing PE signature")
        machine, sections = unpack("<HH", pe + 4)
        optional_size = unpack("<H", pe + 20)[0]
        optional = pe + 24
        require(machine == 0x8664 and 1 <= sections <= 128 and optional_size >= 240
                and unpack("<H", optional)[0] == 0x20B, "Windows x64 PE32+ required")
        image_base = unpack("<Q", optional + 24)[0]
        require(unpack("<I", optional + 108)[0] >= 14, "missing PE data directories")
        headers = unpack("<I", optional + 60)[0]
        table = optional + optional_size
        spans = [unpack("<IIII", table + i * 40 + 8) for i in range(sections)]

        def offset(rva, length):
            require(length > 0, "invalid PE span length")
            if 0 <= rva < headers:
                end = min(headers, len(data))
                require(rva + length <= end, "unbacked PE header span")
                return rva, end
            for virtual_size, address, raw_size, raw_offset in spans:
                if address <= rva < address + max(virtual_size, raw_size):
                    delta = rva - address
                    end = min(raw_offset + raw_size, len(data))
                    require(delta + length <= raw_size and raw_offset + delta + length <= end,
                            "unbacked PE section span")
                    return raw_offset + delta, end
            raise Refusal("invalid PE RVA")

        def name(rva):
            start, backed_end = offset(rva, 1)
            end = data.find(b"\0", start, min(backed_end, start + 256))
            require(end > start, "unterminated import name")
            value = data[start:end].decode("ascii")
            require(re.fullmatch(r"[a-z0-9_.-]+\.dll", value, re.I) is not None, "unsafe DLL import")
            return value.lower()

        imports = []
        for index, width, fmt, name_slot in ((1, 20, "<IIIII", 3), (13, 32, "<IIIIIIII", 1)):
            rva, size = unpack("<II", optional + 112 + index * 8)
            values = []
            if rva:
                require(width <= size <= 65536, "import directory exceeds bound")
                for i in range(min(size // width, 256)):
                    row_offset, _ = offset(rva + i * width, width)
                    row = unpack(fmt, row_offset)
                    if not any(row):
                        break
                    target = row[name_slot]
                    if index == 13 and not row[0] & 1:
                        target -= image_base
                    values.append(name(target))
                else:
                    raise Refusal("unterminated import directory")
            imports.append(sorted(set(values)))
        return {"machine": "amd64", "imports": imports[0], "delayImports": imports[1]}


def record_shape(record: dict) -> None:
    keys(record, FILE_KEYS, "file")
    require(type(record["id"]) is str and re.fullmatch(r"[a-z0-9-]{1,64}", record["id"]) is not None,
            "invalid file ID")
    require(record["root"] in ROOTS, "unknown source root")
    relative(record["source"])
    relative(record["path"])
    require(type(record["bytes"]) is int and 0 < record["bytes"] <= MAX_FILE and hash_text(record["sha256"]),
            "file size/hash invalid")
    require(record["package"] in {"paddleocr", "okuflow-converter", "tensorrt", "cuda", "golden"}, "unknown package")
    require(record["pe"] is None or type(record["pe"]) is dict, "invalid PE metadata")


def inspect(record: dict, roots: dict[str, Path]) -> dict:
    record_shape(record)
    require(record["root"] in roots, f"missing explicit root: {record['root']}")
    path = owned_file(roots[record["root"]], record["source"])
    actual = file_hash(path, record["bytes"])
    require(actual == record["sha256"], f"{record['id']}: SHA-256 mismatch")
    metadata = pe_imports(path) if record["pe"] is not None else None
    require(metadata == record["pe"], f"{record['id']}: PE imports/architecture changed")
    return {"id": record["id"], "bytes": record["bytes"], "sha256": actual, "pe": metadata}


def files_hash(records: list[dict]) -> str:
    return digest(json.dumps(records, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8"))


def check_corpus(manifest: dict, records: dict[str, dict], roots: dict[str, Path], catalog: dict) -> None:
    pin = manifest["corpus"]
    keys(pin, {"version", "manifestSha256"}, "corpus pin")
    version = pin["version"]
    require(type(version) is str and re.fullmatch(r"[a-z0-9-]{1,96}", version) is not None
            and hash_text(pin["manifestSha256"]), "missing reviewed corpus pin")
    entry = records["golden-manifest"]
    require(entry["sha256"] == pin["manifestSha256"] and entry["bytes"] <= 65536, "corpus manifest pin mismatch")
    corpus, raw = read_json(owned_file(roots[entry["root"]], entry["source"]), 65536)
    require(len(raw) == entry["bytes"] and digest(raw) == pin["manifestSha256"], "corpus changed during validation")
    keys(corpus, {"schema", "version", "model", "policy", "encoding", "provenance", "samples"}, "golden corpus")
    require(type(corpus["schema"]) is int and corpus["schema"] == 1 and corpus["version"] == version
            and corpus["model"] == catalog["model"] and corpus["policy"] == catalog["goldenPolicy"]
            and corpus["encoding"] == catalog["goldenEncoding"], "corpus model/policy mismatch")
    require(type(corpus["provenance"]) is str and 0 < len(corpus["provenance"].encode("utf-16-le")) <= 4096,
            "missing corpus provenance")
    samples = corpus["samples"]
    require(type(samples) is list and len(samples) == 8, "eight frozen golden samples required")
    ids, coverage = set(), set()
    for sample in samples:
        keys(sample, {"id", "expected", "tags", "width", "pixelsSha256"}, "sample")
        sample_id = sample["id"]
        require(type(sample_id) is str and re.fullmatch(r"[a-z0-9-]{1,48}", sample_id) is not None
                and sample_id not in ids, "duplicate/invalid sample ID")
        ids.add(sample_id)
        require(type(sample["expected"]) is str and "\0" not in sample["expected"]
                and 0 < len(sample["expected"].encode("utf-16-le")) <= 512, "invalid golden truth")
        require(type(sample["width"]) is int and 1 <= sample["width"] <= 1024, "invalid crop width")
        tags = sample["tags"]
        require(type(tags) is list and len(tags) <= 8 and all(type(t) is str and 0 < len(t.encode("utf-16-le")) <= 96 for t in tags),
                "invalid sample tags")
        coverage.update(tags)
        crop = records.get("crop-" + sample_id)
        require(crop is not None and crop["root"] == "corpus" and crop["source"] == sample_id + ".gray8"
                and crop["path"] == f"models/uct/golden/{version}/{sample_id}.gray8"
                and crop["bytes"] == 48 * sample["width"] and crop["sha256"] == sample["pixelsSha256"]
                and crop["package"] == "golden" and crop["pe"] is None, "golden crop inventory mismatch")
    require({"O0", "Il1", "numbers", "negation", "accents"} <= coverage, "missing sensitive-token tags")
    require({r for r in records if r.startswith("crop-")} == {"crop-" + i for i in ids}, "unlisted golden crop")


def validate(manifest_path: Path, reviewed_sha256: str, roots: dict[str, Path],
             catalog: dict, catalog_sha256: str) -> dict:
    require(hash_text(reviewed_sha256), "explicit independently reviewed manifest SHA-256 required")
    manifest, raw = read_json(manifest_path)
    require(digest(raw) == reviewed_sha256, "reviewed distribution manifest changed")
    keys(manifest, {"schema", "status", "catalogSha256", "modelVersion", "corpus", "files", "reviews"}, "distribution")
    require(type(manifest["schema"]) is int and manifest["schema"] == 1
            and manifest["status"] == "reviewed-for-private-staging"
            and manifest["catalogSha256"] == catalog_sha256
            and manifest["modelVersion"] == catalog["model"]["version"], "unreviewed/incompatible distribution inventory")
    records = manifest["files"]
    require(type(records) is list and 1 <= len(records) <= 64, "bounded exact file inventory required")
    by_id, paths = {}, set()
    for record in records:
        record_shape(record)
        require(record["id"] not in by_id and record["path"].lower() not in paths, "duplicate file/path")
        by_id[record["id"]] = record
        paths.add(record["path"].lower())
    require(sum(r["bytes"] for r in records) <= MAX_TOTAL, "payload total exceeds 2 GiB")
    for expected in catalog["fixedFiles"]:
        require(by_id.get(expected["id"]) == expected, f"required pinned file changed/missing: {expected['id']}")
    version = manifest["corpus"].get("version", "") if type(manifest["corpus"]) is dict else ""
    required = {r["id"] for r in catalog["fixedFiles"]}
    for entry in catalog["reviewedFiles"]:
        expected_path = entry["path"].replace("{corpus}", version)
        record = by_id.get(entry["id"])
        require(record is not None and record["root"] == entry["root"] and record["path"] == expected_path
                and record["source"] == entry["source"] and record["package"] == entry["package"]
                and record["bytes"] <= MAX_JSON and record["pe"] is None, f"missing reviewed asset: {entry['id']}")
        required.add(entry["id"])
    require(set(by_id) == required | {i for i in by_id if i.startswith("crop-")}, "file outside exact allowlist")
    # Reviews bind *this* inventory, graph and corpus, not a stale blanket OK.
    reviews = manifest["reviews"]
    require(type(reviews) is list and len(reviews) == len(DOMAINS), "four acceptance domains required")
    seen = set()
    for review in reviews:
        keys(review, {"domain", "source", "bytes", "sha256"}, "review")
        domain = review["domain"]
        require(domain in DOMAINS and domain not in seen and hash_text(review["sha256"]), "missing/duplicate review domain")
        seen.add(domain)
        require(type(review["bytes"]) is int and 0 < review["bytes"] <= MAX_JSON and "review" in roots, "bounded review evidence required")
        evidence_path = owned_file(roots["review"], review["source"])
        require(file_hash(evidence_path, review["bytes"]) == review["sha256"], "changed acceptance evidence")
        evidence, evidence_raw = read_json(evidence_path)
        require(len(evidence_raw) == review["bytes"] and digest(evidence_raw) == review["sha256"],
                "acceptance evidence changed during validation")
        keys(evidence, {"schema", "domain", "decision", "catalogSha256", "filesSha256", "graphSha256", "corpusSha256", "evidence"}, "acceptance evidence")
        require(type(evidence["schema"]) is int and evidence["schema"] == 1 and evidence["domain"] == domain
                and evidence["decision"] == "accepted" and evidence["catalogSha256"] == catalog_sha256
                and evidence["filesSha256"] == files_hash(records)
                and evidence["graphSha256"] == catalog["model"]["graphSha256"]
                and evidence["corpusSha256"] == manifest["corpus"]["manifestSha256"]
                and type(evidence["evidence"]) is str and 16 <= len(evidence["evidence"]) <= 8192,
                f"unaccepted/unbound {domain} evidence")
    for record in records:
        inspect(record, roots)
    check_corpus(manifest, by_id, roots, catalog)
    provenance_entry = by_id["model-provenance"]
    provenance, _ = read_json(owned_file(roots["review"], provenance_entry["source"]))
    require(provenance == catalog["conversionProvenance"], "model conversion provenance mismatch")
    return manifest


def write_new(path: Path, value: dict) -> None:
    with path.open("xb") as output:
        output.write((json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))


def make_parents(destination: Path, name: str) -> Path:
    current = destination
    for part in relative(name).split("/")[:-1]:
        current = local_path(current / part, missing_leaf=True)
        if not current.exists():
            current.mkdir()
        require(current.is_dir(), "staging parent is not a directory")
    return current


def check_tree(destination: Path, records: list[dict], extra: set[str]) -> None:
    expected = {r["path"] for r in records} | extra
    directories = {str(parent).replace("\\", "/") for name in expected for parent in Path(name).parents if str(parent) != "."}
    actual = set()
    for parent, dirs, names in os.walk(destination, followlinks=False):
        for name in dirs + names:
            path = local_path(Path(parent) / name)
            rel = path.relative_to(destination).as_posix()
            require(rel in directories if path.is_dir() else rel in expected, "unexpected staged file/directory")
            if path.is_file():
                require(path.stat().st_nlink == 1, "hardlinked staging file")
                actual.add(rel)
    require(actual == expected, "incomplete exact staging inventory")
    for record in records:
        require(file_hash(owned_file(destination, record["path"]), record["bytes"]) == record["sha256"], "staged byte mismatch")


def stage(destination: Path, manifest: dict, roots: dict[str, Path], reviewed_sha256: str) -> None:
    destination = local_path(destination, missing_leaf=True)
    require(destination.name == "app" and not destination.exists(), "fresh nonexistent private app folder required")
    parent_ids = {directory_identity(parent) for parent in [destination.parent, *destination.parent.parents]}
    for root in roots.values():
        root = local_path(root)
        require(directory_identity(root) not in parent_ids and not destination.is_relative_to(root)
                and not root.is_relative_to(destination), "staging overlaps an input root")
    # First mutation follows complete validation; mkdir/open are exclusive.
    destination.mkdir()
    write_new(destination / INCOMPLETE, {"schema": 1, "status": "incomplete-private-staging", "manifestSha256": reviewed_sha256})
    for record in manifest["files"]:
        source = owned_file(roots[record["root"]], record["source"])
        target = destination / record["path"]
        make_parents(destination, record["path"])
        result, count = hashlib.sha256(), 0
        with source.open("rb") as input_file, target.open("xb") as output:
            while block := input_file.read(min(1024 * 1024, record["bytes"] + 1 - count)):
                count += len(block)
                require(count <= record["bytes"], "source grew during copy; incomplete staging preserved")
                result.update(block)
                output.write(block)
        require(count == record["bytes"] and result.hexdigest() == record["sha256"], "source changed during copy; incomplete staging preserved")
    # Verify inputs again as well as copied bytes. A changed source never earns a receipt.
    for record in manifest["files"]:
        inspect(record, roots)
    check_tree(destination, manifest["files"], {INCOMPLETE})
    write_new(destination / RECEIPT, {"schema": 1, "status": "private-staging-complete-not-release",
                                    "manifestSha256": reviewed_sha256, "distribution": manifest})
    (destination / INCOMPLETE).unlink()
    try:
        check_tree(destination, manifest["files"], {RECEIPT})
    except Exception:
        # Preserve any seal and the failed tree for diagnosis, but restore a
        # blocking marker so a late verification failure never looks complete.
        write_new(destination / INCOMPLETE, {"schema": 1, "status": "failed-final-verification",
                                            "manifestSha256": reviewed_sha256})
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inventory", "validate", "stage"))
    parser.add_argument("--root", action="append", default=[], metavar="NAME=ABSOLUTE_PHYSICAL_PATH")
    parser.add_argument("--reviewed-manifest", type=Path)
    parser.add_argument("--reviewed-sha256")
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args(argv)
    try:
        roots = {}
        for value in args.root:
            name, separator, path = value.partition("=")
            require(separator == "=" and name in ROOTS and name not in roots, "invalid/duplicate root")
            roots[name] = local_path(Path(path))
            require(roots[name].is_dir(), "input root must be a directory")
        catalog, raw = read_json(CATALOG)
        require(catalog["schema"] == 1 and catalog["status"] == "development-unstaged", "unsupported candidate catalog")
        if args.action == "inventory":
            require(not args.reviewed_manifest and not args.reviewed_sha256 and not args.destination,
                    "inventory cannot approve or stage")
            facts, errors = [], []
            for record in catalog["fixedFiles"]:
                try:
                    facts.append(inspect(record, roots))
                except (OSError, ValueError, struct.error) as error:
                    errors.append({"id": record["id"], "reason": str(error)})
            print(json.dumps({"status": "candidate-only-not-redistribution-approval", "catalogSha256": digest(raw),
                              "files": facts, "errors": errors, "blockers": catalog["blockers"]}, indent=2))
            return 1 if errors else 0
        require(args.reviewed_manifest is not None and args.reviewed_sha256 is not None,
                "separately reviewed distribution manifest and explicit SHA-256 required")
        require(args.action == "stage" or args.destination is None, "validate is read-only")
        manifest = validate(args.reviewed_manifest, args.reviewed_sha256, roots, catalog, digest(raw))
        if args.action == "stage":
            require(args.destination is not None, "explicit new private app destination required")
            stage(args.destination, manifest, roots, args.reviewed_sha256)
        print(json.dumps({"status": "private-staging-complete-not-release" if args.action == "stage" else "offline-inventory-validated-not-release",
                          "manifestSha256": args.reviewed_sha256, "files": len(manifest["files"])}))
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
