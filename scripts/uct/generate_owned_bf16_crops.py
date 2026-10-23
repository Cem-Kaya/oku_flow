#!/usr/bin/env python3
"""One-shot, offline typesetting of eight UNAPPROVED synthetic GRAY8 crops.

No model/runtime imports, inference, subprocesses, downloads or app-state writes.
Requires an already installed Pillow and the exact separately installed fonts.
Output must be a new directory; failures and partial output are never removed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
import time


VERSION = "okuflow-owned-typeset-gray8-v1"
BOUNDARY_COMMIT = "faa7fbfe7b87983802c881411a856328bc873dfd"
POLICY = "uct-bf16-exact-text-n1-n8-v1"
ENCODING = "gray8-upright-h48-zero-pad1024-v1"
HEIGHT = 48
MARGIN_X = 8
FONT_PINS = {
    "DejaVuSans.ttf": "3fdf69cabf06049ea70a00b5919340e2ce1e6d02b0cc3c4b44fb6801bd1e0d22",
    "DejaVuSans-Bold.ttf": "b184b89e3c1075f22f6b71575b6fc20d4972b3cfd3b23322ca6fd596dcaef167",
}
FONT_NOTICE_SHA256 = "d75938dec098f06f0ac3c00853065d94f020be1c3c62ef1dc2975ba15b4d9b0e"
PILLOW_NOTICE_SHA256 = "17f240ae101143707e5e7303a5d800450d9ccc7475b463cedb555cefdb3c6ece"

# These strings are first-party source authorship, never candidate OCR outputs.
# Tuple: owned id, exact Unicode expected text, installed font, px size, bg, ink, tags.
SAMPLES = (
    ("words-regular", "OkuFlow", "DejaVuSans.ttf", 36, 255, 0, ()),
    ("phrase-bold", "Please read this line", "DejaVuSans-Bold.ttf", 32, 255, 0, ()),
    ("date-regular", "2026-10-23", "DejaVuSans.ttf", 32, 255, 0, ("numbers",)),
    ("decimal-bold", "+12.50 -0.75", "DejaVuSans-Bold.ttf", 32, 255, 0, ("numbers",)),
    ("o-zero-regular", "O0 OO 00", "DejaVuSans.ttf", 36, 255, 0, ("O0", "numbers")),
    ("i-l-one-inverse", "Il1 I l 1", "DejaVuSans-Bold.ttf", 36, 0, 255, ("Il1", "numbers")),
    ("negation-bold", "Do not enter", "DejaVuSans-Bold.ttf", 34, 255, 0, ("negation",)),
    ("accents-inverse", "caf\u00e9 na\u00efve", "DejaVuSans.ttf", 34, 0, 255, ("accents",)),
)

# Exact ModelIdentity() object at BOUNDARY_COMMIT; do not add provenance fields.
MODEL = {
    "version": "ppocrv6-medium-rec-bf16-linear-v1",
    "precision": "bf16-linear",
    "sourceSha256": "9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba",
    "graphSha256": "ecd8ffc92aa1b9f7a33b0ad39757b64887c465f15bb54853936b2136f94e5835",
    "configSha256": "991b700facf5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129",
    "dictionarySha256": "f1d7cde73a39cc887dd68d5c64b2756b4962ae28594d3a610c683b2fc47110f1",
    "classes": 18710,
    "inputName": "x",
    "outputName": "fetch_name_0",
    "inputType": "FLOAT",
    "outputType": "FLOAT",
    "inputLocation": "DEVICE",
    "outputLocation": "DEVICE",
    "format": "LINEAR",
    "outputAbi": "ctc-probabilities-v1",
    "widthPolicy": "fixed1024",
    "capacity": 8,
    "preprocessing": "paddle-rectify-normalized-zero-pad-v1",
    "minimum": [1, 3, 48, 1024],
    "optimum": [8, 3, 48, 1024],
    "maximum": [8, 3, 48, 1024],
    "tf32": "default",
    "optimizationLevel": 2,
    "workspaceMiB": 256,
    "auxStreams": 0,
}


def digest(data: bytes) -> str:
    """Return lowercase SHA-256 of the exact supplied bytes."""
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: object) -> bytes:
    """Serialize stable sorted UTF-8 JSON with LF and one final newline."""
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def write_new(path: Path, data: bytes) -> str:
    """Exclusively create one output, retain it on failure, and return its hash."""
    with path.open("xb") as stream:
        stream.write(data)
    if path.read_bytes() != data:
        raise RuntimeError(f"output readback differs: {path.name}")
    return digest(data)


def generate(output: Path, font_dir: Path, pillow_notice: Path) -> dict:
    """Render once into new owned storage; emit manifest, provenance and proposal."""
    started = time.monotonic()
    from PIL import Image, ImageChops, ImageDraw, ImageFont, features, __version__

    if __version__ != "11.1.0":
        raise RuntimeError(f"installed Pillow differs from 11.1.0: {__version__}")
    if not output.is_absolute() or not font_dir.is_absolute() or not pillow_notice.is_absolute():
        raise ValueError("all input/output paths must be explicit absolute local paths")
    if any(str(path).startswith(("\\\\", "//")) for path in (output, font_dir, pillow_notice)):
        raise ValueError("UNC/network paths are not allowed")
    for filename, expected in FONT_PINS.items():
        if digest((font_dir / filename).read_bytes()) != expected:
            raise RuntimeError(f"installed font hash differs: {filename}")
    if digest((font_dir / "LICENSE_DEJAVU").read_bytes()) != FONT_NOTICE_SHA256:
        raise RuntimeError("installed DejaVu license notice hash differs")
    if digest(pillow_notice.read_bytes()) != PILLOW_NOTICE_SHA256:
        raise RuntimeError("installed Pillow license notice hash differs")
    # No exist_ok: an earlier failed or successful render cannot be overwritten.
    output.mkdir(parents=True, exist_ok=False)

    renderer = {
        "name": "Pillow",
        "version": __version__,
        "freetypeVersion": features.version_module("freetype2"),
        "zlibVersion": features.version_codec("zlib"),
        "pythonVersion": platform.python_version(),
        "layoutEngine": "ImageFont.Layout.BASIC",
    }
    if not renderer["freetypeVersion"]:
        raise RuntimeError("installed FreeType renderer is unavailable")
    label_font = ImageFont.truetype(str(font_dir / "DejaVuSans.ttf"), 14, index=0,
                                   layout_engine=ImageFont.Layout.BASIC)
    sheet = Image.new("L", (1040, 60 + 8 * 172), 232)
    sheet_draw = ImageDraw.Draw(sheet)
    sheet_draw.text((16, 10), "UNAPPROVED - synthetic source-authored truth; no model run", font=label_font, fill=0)
    sheet_draw.text((16, 30), "Each row: exact 1x crop, then nearest-neighbour 2x view. PNG is review-only.", font=label_font, fill=0)
    manifest_samples = []
    provenance_samples = []
    raw_hashes = {}
    for index, (sample_id, expected, filename, size, background, ink, tags) in enumerate(SAMPLES):
        if time.monotonic() - started > 100:
            raise RuntimeError("single render pass exceeded its internal 100-second budget")
        font = ImageFont.truetype(str(font_dir / filename), size, index=0,
                                 layout_engine=ImageFont.Layout.BASIC)
        left, top, right, bottom = font.getbbox(expected, mode="L", anchor="lt", stroke_width=0)
        ink_width, ink_height = right - left, bottom - top
        width = ink_width + 2 * MARGIN_X
        if not 1 <= width <= 1024 or not 1 <= ink_height <= HEIGHT - 8:
            raise RuntimeError(f"source glyph extent outside crop bounds: {sample_id}")
        position = (MARGIN_X - left, (HEIGHT - ink_height) // 2 - top)
        crop = Image.new("L", (width, HEIGHT), background)
        ImageDraw.Draw(crop).text(position, expected, font=font, fill=ink, anchor="lt",
                                  stroke_width=0, embedded_color=False)
        actual_ink = ImageChops.difference(crop, Image.new("L", crop.size, background)).getbbox()
        if (actual_ink is None or actual_ink[0] < MARGIN_X or actual_ink[2] > width - MARGIN_X
                or actual_ink[1] < 4 or actual_ink[3] > HEIGHT - 4):
            raise RuntimeError(f"empty or clipped rendered ink: {sample_id}")
        pixels = crop.tobytes("raw", "L")
        if len(pixels) != width * HEIGHT:
            raise RuntimeError(f"GRAY8 row-major payload length differs: {sample_id}")
        pixel_hash = write_new(output / f"{sample_id}.gray8", pixels)
        raw_hashes[f"{sample_id}.gray8"] = pixel_hash
        manifest_samples.append({"id": sample_id, "expected": expected, "width": width,
                                 "pixelsSha256": pixel_hash, "tags": list(tags)})
        provenance_samples.append({
            "id": sample_id, "expectedText": expected, "textUtf8Sha256": digest(expected.encode("utf-8")),
            "truthSource": "first-party SAMPLES literal; synthetic ground truth; not camera transcription",
            "fontFilename": filename, "fontSha256": FONT_PINS[filename],
            "fontSizePx": size, "fontIndex": 0, "backgroundGray8": background, "inkGray8": ink,
            "polarity": "dark-on-light" if background == 255 else "light-on-dark",
            "textAnchor": "lt", "measuredTextBox": [left, top, right, bottom],
            "drawPosition": list(position), "actualInkBox": list(actual_ink),
            "width": width, "height": HEIGHT, "payloadBytes": len(pixels),
            "pixelsSha256": pixel_hash, "tags": list(tags),
        })
        row_y = 60 + index * 172
        caption = f"{index + 1:02} {sample_id} | expected: {expected!r} | {width}x48 | {filename} {size}px"
        sheet_draw.text((16, row_y), caption, font=label_font, fill=0)
        sheet.paste(crop, (16, row_y + 22))
        enlarged = crop.resize((width * 2, HEIGHT * 2), resample=Image.Resampling.NEAREST)
        sheet.paste(enlarged, (16, row_y + 74))

    from io import BytesIO
    png = BytesIO()
    sheet.save(png, format="PNG", optimize=False, compress_level=9)
    sheet_hash = write_new(output / "contact-sheet.png", png.getvalue())
    provenance = {
        "schema": "okuflow-owned-typeset-provenance-v1", "version": VERSION,
        "status": "synthetic-unapproved-not-attested", "boundaryCommit": BOUNDARY_COMMIT,
        "generator": {"path": "scripts/uct/generate_owned_bf16_crops.py",
                      "sha256": digest(Path(__file__).read_bytes())},
        "renderer": renderer,
        "parameters": {"mode": "L", "height": HEIGHT, "maxUsefulWidth": 1024,
                       "sideMarginPx": MARGIN_X, "verticalAlignment": "integer centered measured ink box",
                       "strokeWidth": 0, "embeddedColor": False, "resampling": "none",
                       "rowOrder": "top-to-bottom", "columnOrder": "left-to-right", "bytesPerPixel": 1,
                       "runtimeNormalization": "(sample/255-.5)*2; repeat gray to 3 channels",
                       "runtimePadding": "normalized zero to width1024; no pad bytes stored in crops",
                       "png": {"mode": "L", "width": 1040, "height": 1436, "compressLevel": 9,
                               "optimize": False, "previewScale": [1, 2], "previewResampling": "NEAREST"}},
        "fontNotices": {"installedFilename": "LICENSE_DEJAVU", "sha256": FONT_NOTICE_SHA256,
                        "facts": "Installed DejaVu notice: Bitstream Vera terms; DejaVu changes public domain; Arev terms retained.",
                        "distribution": "font binaries and renderer are not copied; no new legal clearance asserted"},
        "toolNotices": {"installedPillowLicenseSha256": PILLOW_NOTICE_SHA256,
                        "facts": "Installed Pillow 11.1.0 notice identifies MIT-CMU; FreeType remains a separately installed tool component."},
        "contactSheet": {"filename": "contact-sheet.png", "sha256": sheet_hash,
                         "purpose": "actual raw crops at 1x and 2x for visual review; not model input"},
        "samples": provenance_samples,
    }
    provenance_hash = write_new(output / "render-provenance.json", json_bytes(provenance))
    manifest = {
        "schema": 1, "version": VERSION, "model": MODEL, "policy": POLICY, "encoding": ENCODING,
        "provenance": "Original first-party text typeset with installed hash-pinned DejaVu Sans via Pillow; "
                      "synthetic ground truth from generator source, NOT human-transcribed real-camera quality. "
                      f"Detailed source/render provenance: render-provenance.json SHA-256 {provenance_hash}. "
                      "Unapproved candidate; no inference, N1/N8 attestation or public readiness established.",
        "samples": manifest_samples,
    }
    manifest_bytes = json_bytes(manifest)
    if len(manifest_bytes) > 64 * 1024 or len(manifest) != 7 or len(manifest_samples) != 8:
        raise RuntimeError("strict corpus manifest shape/bound exceeded")
    manifest_hash = write_new(output / "manifest.json", manifest_bytes)
    proposal = {
        "schema": "okuflow-owned-corpus-pin-proposal-v1", "version": VERSION,
        "status": "UNAPPROVED-CANDIDATE-NOT-FOR-PRODUCTION-CATALOG",
        "boundaryCommit": BOUNDARY_COMMIT, "manifestFilename": "manifest.json",
        "manifestSha256": manifest_hash, "renderProvenanceSha256": provenance_hash,
        "contactSheetSha256": sheet_hash, "rawFilesSha256": raw_hashes,
        "requiredBeforeProductionPin": ["independent review of every source string, crop and provenance",
                                        "parent visual review of actual contact sheet",
                                        "strict production N1 plus full N8 exact-text attestation and runtime quality gate",
                                        "parent review and explicit separate catalog approval"],
    }
    proposal_hash = write_new(output / "pin-proposal.json", json_bytes(proposal))
    return {"version": VERSION, "manifestSha256": manifest_hash, "proposalSha256": proposal_hash,
            "samples": len(manifest_samples), "rawBytes": sum(row["payloadBytes"] for row in provenance_samples),
            "renderer": renderer, "status": "ASSETS-GENERATED-UNAPPROVED-NOT-ATTESTED"}


def main() -> None:
    """Accept explicit installed inputs and a fresh output; preserve first errors."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--font-dir", type=Path, required=True)
    parser.add_argument("--pillow-license", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(generate(args.output, args.font_dir, args.pillow_license), sort_keys=True))
    except Exception as error:
        print(f"FIRST-ERROR: {type(error).__name__}: {error}", file=sys.stderr)
        raise


if __name__ == "__main__":
    main()
