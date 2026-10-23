"""Model-free payload fixtures. Authoring is not permission to execute.

Run only after root START, with --evidence-root naming an existing private
physical directory. Each attempt owns a fresh child; all bytes remain there,
including first failures. No real catalog/model/SDK is hashed or copied.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import unittest
import uuid


spec = importlib.util.spec_from_file_location("offline_payload", Path(__file__).with_name("offline_payload.py"))
payload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(payload)
EVIDENCE_ROOT = None


class OfflinePayloadTests(unittest.TestCase):
    def setUp(self):
        self.root = EVIDENCE_ROOT / (self._testMethodName + "-" + uuid.uuid4().hex)
        self.root.mkdir()

    def record(self, value=b"first-party model-free fixture\n", name="fixture.txt"):
        path = self.root / name
        path.write_bytes(value)
        return {"id": "fixture", "root": "models", "source": name, "path": "models/uct/fixture.txt",
                "bytes": len(value), "sha256": payload.digest(value), "package": "paddleocr", "pe": None}

    def manifest_file(self, manifest):
        path = self.root / ("distribution-" + uuid.uuid4().hex + ".json")
        raw = (json.dumps(manifest) + "\n").encode()
        path.write_bytes(raw)
        return path, payload.digest(raw)

    def fake_pe(self):
        # Original minimal PE metadata, not an executable DLL. A direct import
        # and RVA-based delay import deliberately name different system DLLs.
        data = bytearray(2048)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 128)
        data[128:132] = b"PE\0\0"
        struct.pack_into("<HH", data, 132, 0x8664, 1)
        struct.pack_into("<H", data, 148, 240)
        opt = 152
        struct.pack_into("<H", data, opt, 0x20B)
        struct.pack_into("<Q", data, opt + 24, 0x180000000)
        struct.pack_into("<I", data, opt + 60, 512)
        struct.pack_into("<I", data, opt + 108, 16)
        struct.pack_into("<II", data, opt + 112 + 8, 0x1000, 40)
        struct.pack_into("<II", data, opt + 112 + 13 * 8, 0x1040, 64)
        struct.pack_into("<IIII", data, opt + 240 + 8, 1536, 0x1000, 1536, 512)
        struct.pack_into("<IIIII", data, 512, 0, 0, 0, 0x1100, 0)
        struct.pack_into("<IIIIIIII", data, 576, 1, 0x1120, 0, 0, 0, 0, 0, 0)
        data[768:781] = b"KERNEL32.dll\0"
        data[800:812] = b"USER32.dll\0\0"
        return bytes(data)

    def test_exact_hash_and_changed_bytes(self):
        record = self.record()
        self.assertEqual(payload.inspect(record, {"models": self.root})["sha256"], record["sha256"])
        changed = bytearray((self.root / "fixture.txt").read_bytes())
        changed[0] ^= 1
        (self.root / "fixture.txt").write_bytes(changed)
        with self.assertRaisesRegex(payload.Refusal, "SHA-256"):
            payload.inspect(record, {"models": self.root})

    def test_candidate_matches_existing_source_pins(self):
        catalog, _ = payload.read_json(payload.CATALOG)
        pins, _ = payload.read_json(payload.CATALOG.with_name("pins.json"))
        by_id = {row["id"]: row for row in catalog["fixedFiles"]}
        medium = next(model for model in pins["models"] if model["kind"] == "rec" and model["tier"] == "medium")
        self.assertEqual(catalog["model"]["sourceSha256"], medium["files"]["inference.onnx"])
        self.assertEqual(catalog["model"]["configSha256"], medium["files"]["inference.yml"])
        self.assertEqual(by_id["recognizer-source"]["sha256"], catalog["model"]["sourceSha256"])
        self.assertEqual(by_id["bf16-graph"]["sha256"], catalog["model"]["graphSha256"])
        self.assertEqual(catalog["tensorRt"]["version"], pins["tensorrt"]["version"])
        self.assertEqual(catalog["conversionProvenance"]["converterSha256"], by_id["converter-source"]["sha256"])
        self.assertEqual(catalog["conversionProvenance"]["sourceSha256"], catalog["model"]["sourceSha256"])
        for record in catalog["fixedFiles"]:
            payload.record_shape(record)
            self.assertNotIn(".plan", record["path"])
            self.assertNotIn("trtexec", record["path"])
        self.assertEqual({row["id"] for row in catalog["fixedFiles"] if row["pe"] is not None},
                         {"tensorrt-runtime", "tensorrt-parser", "tensorrt-builder-ptx", "nvrtc", "nvrtc-builtins"})

    def test_path_alias_traversal_and_ads_refusals(self):
        for path in ("../owner.plan", "C:/owner.plan", "\\\\host\\share", "x//y", "x/./y", "x/CON.txt",
                     "x/NUL", "x/com1.bin", "x/ads:stream", "x/alias.", "x/*.dll", ""):
            with self.subTest(path=path), self.assertRaises(payload.Refusal):
                payload.relative(path)

    def test_local_path_absolute_components_and_aliases(self):
        inputs = self.root / "inputs"
        inputs.mkdir()
        self.assertEqual(payload.local_path(inputs), inputs.resolve(strict=True))
        for component in ("inputs.", "inputs ", "inputs:stream", "inputs::$DATA", "NUL.txt", "COM1",
                          "bad?name", "bad\x01name", ".."):
            for candidate in (self.root / component, self.root / component / "app"):
                with self.subTest(path=str(candidate)), self.assertRaises(payload.Refusal):
                    payload.local_path(candidate, missing_leaf=True)
        with self.assertRaises(payload.Refusal):
            payload.local_path(Path("relative-input"))

    def test_stage_absolute_alias_and_physical_overlap_refusals(self):
        inputs = self.root / "inputs"
        inputs.mkdir()
        destination = inputs / "app"
        # Both unsafe source spellings previously bypassed lexical containment.
        for alias in (self.root / "inputs.", self.root / "inputs ", self.root / "inputs:stream"):
            with self.subTest(alias=str(alias)), self.assertRaisesRegex(payload.Refusal, "absolute"):
                payload.stage(destination, {"files": []}, {"models": alias}, "4" * 64)
            self.assertFalse(destination.exists())
        aliases = [inputs]
        if os.name == "nt":
            aliases.append(Path(str(inputs).swapcase()))
            self.assertEqual(payload.directory_identity(aliases[0]), payload.directory_identity(aliases[1]))
        for alias in aliases:
            with self.subTest(alias=str(alias)), self.assertRaisesRegex(payload.Refusal, "overlaps"):
                payload.stage(destination, {"files": []}, {"models": alias}, "4" * 64)
            self.assertFalse(destination.exists())
        with self.assertRaisesRegex(payload.Refusal, "absolute"):
            payload.stage(self.root / "inputs." / "app", {"files": []}, {"models": inputs}, "4" * 64)
        self.assertFalse(destination.exists())

    def test_standalone_manifest_alias_ads_hardlink_and_nonregular_refusals(self):
        path, hashed = self.manifest_file({"schema": 1})
        for suffix in (".", " "):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(payload.Refusal, "absolute"):
                payload.read_json(Path(str(path) + suffix))
        # On Windows this creates an ADS on our own tiny fixture; elsewhere it
        # is a colon-named fixture. The bytes really exist before refusal.
        stream = Path(str(path) + ":review")
        stream.write_bytes(path.read_bytes())
        with self.assertRaisesRegex(payload.Refusal, "absolute"):
            payload.read_json(stream)
        with self.assertRaisesRegex(payload.Refusal, "absolute"):
            payload.validate(stream, hashed, {}, {}, "2" * 64)
        linked = self.root / "linked-manifest.json"
        os.link(path, linked)
        for candidate in (path, linked):
            with self.subTest(path=str(candidate)), self.assertRaisesRegex(payload.Refusal, "hardlinked"):
                payload.read_json(candidate)
            with self.subTest(path=str(candidate)), self.assertRaisesRegex(payload.Refusal, "hardlinked"):
                payload.validate(candidate, hashed, {}, {}, "2" * 64)
        with self.assertRaisesRegex(payload.Refusal, "nonregular"):
            payload.read_json(self.root)

    def test_duplicate_json_keys_and_bound(self):
        path = self.root / "bad.json"
        path.write_bytes(b'{"schema":1,"schema":1}')
        with self.assertRaisesRegex(payload.Refusal, "duplicate"):
            payload.read_json(path)
        path.write_bytes(b" " * 33)
        with self.assertRaises(payload.Refusal):
            payload.read_json(path, 32)

    def test_pe_direct_delay_and_architecture(self):
        record = self.record(self.fake_pe(), "metadata.bin")
        path = self.root / record["source"]
        self.assertEqual(payload.pe_imports(path), {"machine": "amd64", "imports": ["kernel32.dll"],
                                                  "delayImports": ["user32.dll"]})
        changed = bytearray(path.read_bytes())
        struct.pack_into("<H", changed, 132, 0xAA64)
        path.write_bytes(changed)
        with self.assertRaisesRegex(payload.Refusal, "x64"):
            payload.pe_imports(path)

    def test_pe_truncated_unbounded_and_missing_terminator(self):
        path = self.root / "metadata.bin"
        path.write_bytes(b"MZ")
        with self.assertRaises(payload.Refusal):
            payload.pe_imports(path)
        data = bytearray(self.fake_pe())
        struct.pack_into("<I", data, 152 + 112 + 12, 70000)
        path.write_bytes(data)
        with self.assertRaisesRegex(payload.Refusal, "bound"):
            payload.pe_imports(path)
        data = bytearray(self.fake_pe())
        struct.pack_into("<I", data, 152 + 112 + 12, 20)
        path.write_bytes(data)
        with self.assertRaisesRegex(payload.Refusal, "unterminated"):
            payload.pe_imports(path)

    def test_pe_direct_and_delay_descriptor_backing_spans(self):
        path = self.root / "descriptor-metadata.bin"
        for index, start, width, name_slot in ((1, 512, 20, 3), (13, 576, 32, 1)):
            for descriptor in (0, 1):
                data = bytearray(self.fake_pe())
                other = 13 if index == 1 else 1
                struct.pack_into("<II", data, 152 + 112 + other * 8, 0, 0)
                # Keep the first import name safely in the header, so the
                # intended first/null descriptor boundary is the refusal.
                data[448:461] = b"KERNEL32.dll\0"
                struct.pack_into("<I", data, start + name_slot * 4, 448)
                raw_size = start - 512 + (descriptor + 1) * width - 1
                struct.pack_into("<I", data, 408, raw_size)
                path.write_bytes(data)
                with self.subTest(index=index, descriptor=descriptor), self.assertRaisesRegex(payload.Refusal, "section span"):
                    payload.pe_imports(path)

    def test_pe_direct_and_delay_name_backing_spans(self):
        path = self.root / "name-metadata.bin"
        for index, start, name_slot in ((1, 512, 3), (13, 576, 1)):
            data = bytearray(self.fake_pe())
            other = 13 if index == 1 else 1
            struct.pack_into("<II", data, 152 + 112 + other * 8, 0, 0)
            struct.pack_into("<I", data, start + name_slot * 4, 0x1120)
            # Raw size 289 ends at byte 800, where USER32.dll begins; its
            # terminator at 810 is present in the file but is not backed.
            for raw_size in (289, 298):
                struct.pack_into("<I", data, 408, raw_size)
                path.write_bytes(data)
                with self.subTest(index=index, raw_size=raw_size), self.assertRaisesRegex(payload.Refusal, "unterminated"):
                    payload.pe_imports(path)
            struct.pack_into("<I", data, 408, 299)
            path.write_bytes(data)
            actual = payload.pe_imports(path)
            self.assertEqual(actual["imports" if index == 1 else "delayImports"], ["user32.dll"])

    def test_pe_header_descriptor_and_name_backing_spans(self):
        path = self.root / "header-metadata.bin"
        for index, width in ((1, 20), (13, 32)):
            data = bytearray(self.fake_pe())
            other = 13 if index == 1 else 1
            struct.pack_into("<II", data, 152 + 112 + other * 8, 0, 0)
            struct.pack_into("<II", data, 152 + 112 + index * 8, 512 - width + 1, width * 2)
            path.write_bytes(data)
            with self.subTest(index=index), self.assertRaisesRegex(payload.Refusal, "header span"):
                payload.pe_imports(path)
        data = bytearray(self.fake_pe())
        struct.pack_into("<II", data, 152 + 112 + 13 * 8, 0, 0)
        struct.pack_into("<I", data, 512 + 12, 500)
        data[500:512] = b"KERNEL32.dll"
        path.write_bytes(data)
        with self.assertRaisesRegex(payload.Refusal, "unterminated"):
            payload.pe_imports(path)

    def test_review_manifest_and_hash_required(self):
        path, hashed = self.manifest_file({"schema": 1, "status": "development-unstaged"})
        for candidate in ("", "0" * 64, "1" * 64):
            with self.subTest(candidate=candidate), self.assertRaises(payload.Refusal):
                payload.validate(path, candidate, {}, {}, "2" * 64)
        with self.assertRaises(payload.Refusal):
            payload.validate(path, hashed, {}, {}, "2" * 64)

    def test_unlisted_engine_missing_corpus_and_reviews(self):
        record = self.record()
        catalog = {"model": {"version": "fixture-only"}, "fixedFiles": [record], "reviewedFiles": []}
        manifest = {"schema": 1, "status": "reviewed-for-private-staging", "catalogSha256": "2" * 64,
                    "modelVersion": "fixture-only", "corpus": {}, "files": [record], "reviews": []}
        # No accepted review or readable golden corpus is manufactured here.
        bad = copy.deepcopy(manifest)
        extra = dict(record, id="owner-engine", path="owner.plan")
        bad["files"].append(extra)
        path, hashed = self.manifest_file(bad)
        with self.assertRaisesRegex(payload.Refusal, "allowlist"):
            payload.validate(path, hashed, {"models": self.root}, catalog, "2" * 64)
        path, hashed = self.manifest_file(manifest)
        with self.assertRaisesRegex(payload.Refusal, "acceptance domains"):
            payload.validate(path, hashed, {"models": self.root}, catalog, "2" * 64)
        with self.assertRaisesRegex(payload.Refusal, "corpus"):
            payload.check_corpus(manifest, {}, {}, catalog)

    def test_duplicate_destination_and_missing_pin(self):
        record = self.record()
        catalog = {"model": {"version": "fixture-only"}, "fixedFiles": [record], "reviewedFiles": []}
        manifest = {"schema": 1, "status": "reviewed-for-private-staging", "catalogSha256": "2" * 64,
                    "modelVersion": "fixture-only", "corpus": {}, "files": [record, dict(record, id="duplicate")], "reviews": []}
        path, hashed = self.manifest_file(manifest)
        with self.assertRaisesRegex(payload.Refusal, "duplicate"):
            payload.validate(path, hashed, {}, catalog, "2" * 64)
        manifest["files"] = [dict(record, sha256="3" * 64)]
        path, hashed = self.manifest_file(manifest)
        with self.assertRaisesRegex(payload.Refusal, "pinned"):
            payload.validate(path, hashed, {}, catalog, "2" * 64)

    def test_new_only_copy_and_exact_inventory(self):
        record = self.record()
        inputs = self.root / "inputs"
        inputs.mkdir()
        (self.root / record["source"]).rename(inputs / record["source"])
        destination = self.root / "app"
        # Low-level copy primitive only; no approved model/distribution is implied.
        payload.stage(destination, {"files": [record]}, {"models": inputs}, "4" * 64)
        payload.check_tree(destination, [record], {payload.RECEIPT})
        with self.assertRaisesRegex(payload.Refusal, "fresh"):
            payload.stage(destination, {"files": [record]}, {"models": inputs}, "4" * 64)
        (destination / "unlisted.plan").write_bytes(b"not an engine")
        with self.assertRaisesRegex(payload.Refusal, "unexpected"):
            payload.check_tree(destination, [record], {payload.RECEIPT})

    def test_failed_copy_preserves_first_partial_and_no_receipt(self):
        record = self.record()
        inputs = self.root / "inputs"
        inputs.mkdir()
        (self.root / record["source"]).rename(inputs / record["source"])
        record["sha256"] = "5" * 64
        destination = self.root / "app"
        with self.assertRaisesRegex(payload.Refusal, "changed during copy"):
            payload.stage(destination, {"files": [record]}, {"models": inputs}, "4" * 64)
        self.assertTrue((destination / payload.INCOMPLETE).is_file())
        self.assertFalse((destination / payload.RECEIPT).exists())
        self.assertTrue((destination / record["path"]).is_file())
        with self.assertRaisesRegex(payload.Refusal, "fresh"):
            payload.stage(destination, {"files": [record]}, {"models": inputs}, "4" * 64)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--evidence-root", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    parent = payload.local_path(args.evidence_root)
    if not parent.is_dir():
        raise SystemExit("existing physical private evidence parent required")
    EVIDENCE_ROOT = parent / ("uct-payload-fixture-" + uuid.uuid4().hex)
    EVIDENCE_ROOT.mkdir()
    print(f"Preserved fixture evidence: {EVIDENCE_ROOT}", flush=True)
    unittest.main(argv=[sys.argv[0], *remaining])
