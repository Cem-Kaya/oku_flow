"""Build the local OCR timing/quality corpus used by scripts/uct/perf_measure.py.

Every scene is generated from this repository's own documentation text and
Windows system fonts, or extracted from the documented public lecture sample
(scripts/lecture_camera_sample.json). Nothing is downloaded here and nothing
is written inside the repository: pass an output directory outside Git.

The scenes imitate what OkuFlow analyses: document-camera pages with mild
perspective, blur and sensor noise, clean lecture slides from a video file,
a rendered portrait page, a curved book page, a sparse sign scene, a frame
without text and real chalkboard lecture frames. Generated scenes carry the
words that were drawn (`<name>.truth.txt`, reading order). Lecture frames have
no ground truth. These are stand-ins for camera content, not owner captures.

`--variant N` (N >= 1) writes a different labelled set of the same kinds of
scene: other text, fonts, type sizes and camera poses, plus small and large
type at 3840x2160. Use a variant that nobody has looked at when a rule chosen
on other pages needs an untouched check. Variant 0 is the standard set.

`--bent N` (N >= 1) writes a labelled set of bent book pages only, drawn the
way `book-curved-1080` is (rows bent like an open book) but with other text,
fonts, type sizes, columns, frame sizes and bend strengths, from barely bent to
strongly bent. Use it to check that really curved material is not read worse.

`--gutter N` (N >= 1) writes a labelled set of pages bent one-sidedly, as a
single photographed page of an open book is: flat for most of the width, then
curling towards the gutter on one side, with the curl's depth varying from
line to line. `--bent` bends every row with the same symmetric arc, the shape
the ribbon fit assumes; this set does not.

Requires Pillow and NumPy. Usage:
    python scripts/uct/perf_corpus.py --out <dir> [--lecture <mp4>] [--ffmpeg <exe>] [--variant N | --bent N | --gutter N]
"""
import argparse
import hashlib
import json
import math
import random
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONTS = Path("C:/Windows/Fonts")
VERSION = 1


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


def clean_markdown(text):
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"[`*_#>|]", " ", text)
    text = re.sub(r"https?://\S+", " ", text)
    return [w for w in text.split() if 1 <= len(w) <= 18 and re.search(r"[A-Za-z0-9]", w)]


def repo_words(relative, skip=0):
    return clean_markdown((ROOT / relative).read_text(encoding="utf-8"))[skip:]


def turkish_words():
    text = (ROOT / "translations/okuflow_tr.ts").read_text(encoding="utf-8")
    words = []
    for value in re.findall(r"<translation>([^<]+)</translation>", text):
        value = re.sub(r"&[a-z]+;|%\d|&", "", value)
        words += [w for w in value.split() if 1 <= len(w) <= 18 and re.search(r"[A-Za-zçğıöşüÇĞİÖŞÜ]", w)]
    return words


class Page:
    """Draws wrapped words and records exactly what was drawn."""

    def __init__(self, size, paper, ink):
        self.image = Image.new("RGB", size, paper)
        self.draw = ImageDraw.Draw(self.image)
        self.ink = ink
        self.truth = []

    def line(self, xy, text, face, fill=None):
        self.draw.text(xy, text, font=face, fill=fill or self.ink)
        self.truth.append(text.split())

    def block(self, box, words, face, leading=1.35, fill=None, justify_gap=None):
        x0, y0, x1, y1 = box
        space = justify_gap or self.draw.textlength(" ", font=face)
        height = int(face.size * leading)
        y, index = y0, 0
        while y + height <= y1 and index < len(words):
            x, row = x0, []
            while index < len(words):
                width = self.draw.textlength(words[index], font=face)
                if row and x + width > x1:
                    break
                row.append(words[index])
                x += width + space
                index += 1
            self.line((x0, y), " ".join(row), face, fill)
            y += height
        return index


def perspective_coefficients(source, target):
    rows = []
    for (x, y), (u, v) in zip(target, source):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y])
    a = np.array(rows, dtype=np.float64)
    b = np.array(source, dtype=np.float64).reshape(8)
    return np.linalg.solve(a, b).tolist()


def camera(page, size, corners, rng, blur=0.8, noise=3.0, shade=0.12, desk=(58, 52, 47), quality=88):
    """Place a page on a desk as a camera would see it: warp, shade, blur, noise, JPEG."""
    w, h = size
    target = [(w * x, h * y) for x, y in corners]
    source = [(0, 0), (page.width, 0), (page.width, page.height), (0, page.height)]
    coefficients = perspective_coefficients(source, target)
    warped = page.transform(size, Image.PERSPECTIVE, coefficients, Image.BICUBIC, fillcolor=desk)
    mask = Image.new("L", page.size, 255).transform(size, Image.PERSPECTIVE, coefficients, Image.BILINEAR)
    frame = Image.composite(warped, Image.new("RGB", size, desk), mask)
    pixels = np.asarray(frame, dtype=np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    light = 1.0 - shade * ((xx / w - 0.25) ** 2 + (yy / h - 0.2) ** 2)
    pixels = pixels * light[..., None]
    frame = Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))
    if blur > 0:
        frame = frame.filter(ImageFilter.GaussianBlur(blur))
    pixels = np.asarray(frame, dtype=np.float32)
    generator = np.random.default_rng(rng.randrange(1 << 30))
    pixels += generator.normal(0, noise, pixels.shape).astype(np.float32)
    frame = Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))
    if quality:
        import io
        buffer = io.BytesIO()
        frame.save(buffer, "JPEG", quality=quality)
        frame = Image.open(io.BytesIO(buffer.getvalue())).convert("RGB")
    return frame


def document_page(words, size=(1700, 2200), body=30, face_name="times.ttf", columns=1):
    page = Page(size, (247, 244, 236), (26, 24, 22))
    title = font("timesbd.ttf", int(body * 1.9))
    page.line((120, 110), " ".join(words[:5]), title)
    face = font(face_name, body)
    used = 5
    top = 110 + int(body * 3.4)
    gutter = 70
    width = (size[0] - 240 - gutter * (columns - 1)) // columns
    for column in range(columns):
        left = 120 + column * (width + gutter)
        used += page.block((left, top, left + width, size[1] - 120), words[used:], face)
    return page


def slide(words, size, dark=False, scale=1.0):
    paper, ink = ((18, 22, 30), (236, 238, 242)) if dark else ((252, 252, 250), (20, 24, 34))
    accent = (255, 196, 84) if dark else (176, 38, 44)
    page = Page(size, paper, ink)
    w, h = size
    s = scale
    page.line((int(90 * s), int(60 * s)), " ".join(words[:4]), font("segoeuib.ttf", int(64 * s)), accent)
    y = int(190 * s)
    index = 4
    bullet = font("segoeui.ttf", int(36 * s))
    for _ in range(6):
        take = 7
        page.line((int(130 * s), y), " ".join(words[index:index + take]), bullet)
        page.draw.ellipse((int(96 * s), y + int(18 * s), int(110 * s), y + int(32 * s)), fill=accent)
        index += take
        y += int(64 * s)
    code = font("consola.ttf", int(26 * s))
    y += int(20 * s)
    page.draw.rectangle((int(90 * s), y - int(12 * s), w - int(90 * s), y + int(5 * 38 * s)),
                        fill=(30, 36, 48) if dark else (238, 240, 244))
    for row in ("for (int i = 0; i < count; ++i) total += values[i] * 0.5;",
                "if (x >= 10 && y != 0) return x / y - 3.75;",
                "const auto id = QString(\"O0-1lI-5S\") + suffix;",
                "result = alpha * 2.50 + beta * -0.125 + 1e-3;"):
        page.line((int(116 * s), y), row, code)
        y += int(38 * s)
    small = font("segoeui.ttf", int(20 * s))
    page.block((int(90 * s), h - int(170 * s), w - int(90 * s), h - int(40 * s)), words[index:index + 60], small)
    return page


def table_page(words, size):
    page = Page(size, (255, 255, 255), (10, 10, 10))
    head, cell = font("arialbd.ttf", 30), font("arial.ttf", 26)
    page.line((80, 50), "Measured values " + " ".join(words[:3]), font("arialbd.ttf", 44))
    rng = random.Random(7)
    y = 150
    columns = [80, 520, 860, 1200, 1540]
    for x, label in zip(columns, ("Scenario", "Before", "After", "Ratio", "Count")):
        page.line((x, y), label, head)
    index = 3
    for _ in range(18):
        y += 46
        page.draw.line((80, y - 6, size[0] - 80, y - 6), fill=(190, 190, 190))
        page.line((columns[0], y), " ".join(words[index:index + 2]), cell)
        before = rng.uniform(20, 900)
        after = before / rng.uniform(1.05, 2.4)
        for x, value in zip(columns[1:], (f"{before:.2f}", f"{after:.2f}", f"{before / after:.3f}", str(rng.randrange(8, 4096)))):
            page.line((x, y), value, cell)
        index += 2
    return page


def cylinder_warp(image, strength=0.085):
    """Bend rows like an open book: text baselines become shallow arcs."""
    source = np.asarray(image)
    h, w, _ = source.shape
    xs = np.arange(w, dtype=np.float32)
    shift = (strength * h * (1 - np.cos((xs / w - 0.5) * math.pi * 1.25))).astype(np.float32)
    rows = np.arange(h, dtype=np.float32)[:, None] - shift[None, :] + shift.max() * 0.5
    low = np.clip(np.floor(rows).astype(np.int32), 0, h - 1)
    high = np.clip(low + 1, 0, h - 1)
    weight = (rows - np.floor(rows))[..., None]
    columns = np.broadcast_to(np.arange(w)[None, :], (h, w))
    blended = source[low, columns] * (1 - weight) + source[high, columns] * weight
    return Image.fromarray(np.clip(blended, 0, 255).astype(np.uint8))


def gutter_warp(image, depth, start, power, gutter_right, rows_variation, phase):
    """One-sided curl: rows stay flat up to `start` (fraction of the width from
    the outer edge), then drop towards the gutter as ((x - start) / (1 - start))
    ** power, `depth` (fraction of the page height) at the gutter edge. The depth
    varies with the row: rows_variation scales a smooth swing over the height."""
    source = np.asarray(image)
    h, w, _ = source.shape
    xs = np.arange(w, dtype=np.float32) / w
    if not gutter_right:
        xs = 1 - xs
    curl = np.clip((xs - start) / (1 - start), 0, None) ** power
    ys = np.arange(h, dtype=np.float32) / h
    swing = 1 + rows_variation * np.sin(2 * math.pi * (ys * 1.3 + phase)) + .5 * rows_variation * (ys - .5)
    shift = (depth * h * swing[:, None] * curl[None, :]).astype(np.float32)
    rows = np.arange(h, dtype=np.float32)[:, None] - shift + shift.max() * 0.5
    low = np.clip(np.floor(rows).astype(np.int32), 0, h - 1)
    high = np.clip(low + 1, 0, h - 1)
    weight = (rows - np.floor(rows))[..., None]
    columns = np.broadcast_to(np.arange(w)[None, :], (h, w))
    blended = source[low, columns] * (1 - weight) + source[high, columns] * weight
    return Image.fromarray(np.clip(blended, 0, 255).astype(np.uint8))


def gutter_scenes(out, gutter, add):
    """One-sided, gutter-weighted curls with per-line variation (see --gutter)."""
    rng = random.Random(20261117 + gutter * 7919)
    faces = ["georgia.ttf", "times.ttf", "calibri.ttf", "segoeui.ttf", "arial.ttf", "verdana.ttf", "tahoma.ttf"]
    sources = [repo_words(name) for name in ("docs/ultra_clear_text.md", "README.md", "docs/developer_guide.md",
                                             "docs/hardcoded_paths.md")]

    def words(index):
        text = sources[(index + gutter) % len(sources)]
        start = rng.randrange(0, max(1, len(text) - 1500))
        return (text[start:] + text)[:4000]

    # name, frame, page, body px, columns, depth at the gutter (fraction of page height), flat fraction, power
    for index, (name, size, page_size, body, columns, depth, start, power) in enumerate((
            ("gutter-1080-half", (1920, 1080), (2200, 1300), 34, 1, rng.uniform(.008, .012), .7, 2.0),
            ("gutter-1080-light", (1920, 1080), (2200, 1300), 32, 1, rng.uniform(.015, .025), .6, 2.0),
            ("gutter-1080-medium", (1920, 1080), (2200, 1300), 30, 1, rng.uniform(.03, .05), .55, 2.5),
            ("gutter-1080-strong", (1920, 1080), (2200, 1300), 34, 1, rng.uniform(.06, .09), .5, 3.0),
            ("gutter-1080-late", (1920, 1080), (2200, 1300), 30, 1, rng.uniform(.02, .04), .82, 2.0),
            ("gutter-1080-2col", (1920, 1080), (2200, 1300), 28, 2, rng.uniform(.02, .04), .6, 2.5),
            ("gutter-1080-small", (1920, 1080), (2200, 1300), 24, 1, rng.uniform(.01, .02), .65, 2.0),
            ("gutter-1440", (2560, 1440), (2900, 1700), 34, 1, rng.uniform(.015, .03), .6, 2.0),
            ("gutter-2160", (3840, 2160), (4200, 2400), 44, 2, rng.uniform(.02, .04), .55, 2.5))):
        page = document_page(words(index), page_size, body, faces[(index + gutter) % len(faces)], columns)
        right = rng.random() < .5
        variation = rng.uniform(.15, .35)
        warped = gutter_warp(page.image, depth, start, power, right, variation, rng.random())
        spread = .03
        pose = [(rng.uniform(.03, .03 + spread), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.98 - spread, .98)),
                (rng.uniform(.03, .03 + spread), rng.uniform(.98 - spread, .98))]
        add(f"{name}-g{gutter}", camera(warped, size, pose, rng, blur=rng.uniform(.6, 1.1), noise=rng.uniform(2, 4)),
            page.truth, {"kind": "generated-gutter-book", "gutter": gutter, "depth": round(depth, 4),
                         "flat": start, "power": power, "gutterSide": "right" if right else "left",
                         "rowVariation": round(variation, 3)})


def sparse_scene(size, rng):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    base = np.stack([92 + 50 * xx / w, 104 + 30 * yy / h, 118 - 30 * xx / w], axis=-1)
    page = Page(size, (0, 0, 0), (245, 245, 245))
    page.image = Image.fromarray(base.astype(np.uint8))
    page.draw = ImageDraw.Draw(page.image)
    signs = [((140, 120), "EXIT", "arialbd.ttf", 120, (255, 255, 255), (22, 122, 60)),
             ((1050, 200), "Room 204", "segoeuib.ttf", 72, (20, 20, 20), (240, 236, 220)),
             ((1100, 330), "Dynamics and Control", "segoeui.ttf", 44, (20, 20, 20), (240, 236, 220)),
             ((260, 640), "Office hours 14:30", "georgia.ttf", 52, (250, 250, 250), (60, 60, 70)),
             ((1220, 820), "Please keep doors closed", "arial.ttf", 34, (30, 30, 30), (230, 200, 90))]
    for (x, y), text, face_name, points, ink, plate in signs:
        face = font(face_name, points)
        box = page.draw.textbbox((x, y), text, font=face)
        page.draw.rectangle((box[0] - 24, box[1] - 18, box[2] + 24, box[3] + 18), fill=plate)
        page.line((x, y), text, face, ink)
    noise = np.random.default_rng(rng.randrange(1 << 30)).normal(0, 2.5, (h, w, 3))
    pixels = np.asarray(page.image, dtype=np.float32) + noise
    page.image = Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))
    return page


def textless_scene(size, rng):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    base = np.stack([150 - 60 * yy / h, 140 - 40 * yy / h + 20 * np.sin(xx / 90), 120 + 30 * xx / w], axis=-1)
    image = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8))
    draw = ImageDraw.Draw(image)
    for _ in range(14):
        x, y = rng.randrange(w), rng.randrange(h)
        draw.rectangle((x, y, x + rng.randrange(80, 520), y + rng.randrange(40, 300)),
                       fill=tuple(rng.randrange(30, 220) for _ in range(3)))
    return image.filter(ImageFilter.GaussianBlur(2.0))


def lecture_frames(video, ffmpeg, out):
    """Real chalkboard frames, enlarged to 1080p as the private OBS feed presents them."""
    made = []
    for index, second in enumerate((420, 905, 1460, 2075, 2590, 3130)):
        target = out / f"board-lecture-{index:02d}.png"
        command = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-ss", str(second), "-i", str(video),
                   "-frames:v", "1", "-vf", "scale=1920:1080:flags=bicubic", str(target)]
        subprocess.run(command, check=True)
        made.append((target, None, {"kind": "real-lecture-frame", "second": second, "source": video.name}))
    return made


def variant_scenes(out, variant, add):
    """Other text, fonts, sizes and poses than the standard set; labelled scenes only."""
    rng = random.Random(20261115 + variant * 7919)
    faces = ["times.ttf", "georgia.ttf", "segoeui.ttf", "arial.ttf", "calibri.ttf", "verdana.ttf", "tahoma.ttf"]
    sources = [repo_words("docs/developer_guide.md", 5000 + variant * 911), repo_words("docs/ultra_clear_text.md", 1500 + variant * 577),
               repo_words("README.md", 200 + variant * 131), repo_words("docs/hardcoded_paths.md", 900 + variant * 733)]

    def pose(spread):
        return [(rng.uniform(.03, .03 + spread), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.98 - spread, .98)),
                (rng.uniform(.03, .03 + spread), rng.uniform(.98 - spread, .98))]

    def words(index):
        text = sources[(index + variant) % len(sources)]
        start = rng.randrange(0, max(1, len(text) - 4000))
        return text[start:]

    def face(index):
        return faces[(index + variant) % len(faces)]

    kind = {"kind": "generated-document-camera", "variant": variant}
    for index, (name, size, page_size, body, columns, spread) in enumerate((
            ("doc-camera-1080", (1920, 1080), (2200, 1300), 28, 2, .06),
            ("doc-camera-1080-small", (1920, 1080), (2200, 1300), 21, 3, .04),
            ("doc-camera-1080-large", (1920, 1080), (2200, 1300), 46, 1, .08),
            ("doc-camera-1440", (2560, 1440), (2900, 1700), 32, 2, .06),
            ("doc-camera-2160-small", (3840, 2160), (4200, 2400), 30, 3, .04),
            ("doc-camera-2160", (3840, 2160), (4200, 2400), 44, 2, .05),
            ("doc-camera-2160-large", (3840, 2160), (4200, 2400), 68, 2, .06))):
        page = document_page(words(index), page_size, body, face(index), columns)
        add(f"{name}-v{variant}", camera(page.image, size, pose(spread), rng, blur=rng.uniform(.6, 1.2), noise=rng.uniform(2, 4.5)),
            page.truth, kind)
    for index, (name, size, scale, dark) in enumerate((("slide-1080", (1920, 1080), 1.0, variant % 2 == 0),
                                                       ("slide-1440", (2560, 1440), 1.33, variant % 2 == 1),
                                                       ("slide-2160", (3840, 2160), 2.0, variant % 2 == 0))):
        page = slide(words(index + 3), size, dark=dark, scale=scale)
        add(f"{name}-v{variant}", page.image, page.truth, {"kind": "generated-slide", "variant": variant})
    page = table_page(words(7), (1920, 1080))
    add(f"table-1080-v{variant}", page.image, page.truth, {"kind": "generated-table", "variant": variant})
    page = document_page(words(8), (1654, 2339), 24 + 2 * (variant % 3), face(8), 1)
    add(f"page-portrait-v{variant}", page.image, page.truth, {"kind": "generated-rendered-page", "variant": variant})


def bent_scenes(out, bent, add):
    """Bent book pages only: other text, fonts, sizes, columns and bend strengths."""
    rng = random.Random(20261116 + bent * 6271)
    faces = ["times.ttf", "georgia.ttf", "segoeui.ttf", "arial.ttf", "calibri.ttf", "verdana.ttf", "tahoma.ttf"]
    sources = [repo_words(name) for name in ("docs/developer_guide.md", "docs/ultra_clear_text.md", "README.md",
                                             "docs/hardcoded_paths.md")]

    def words(index):
        # Any N gives text: the start wraps inside the source instead of running past its end.
        text = sources[(index + bent) % len(sources)]
        start = rng.randrange(0, max(1, len(text) - 1500))
        return (text[start:] + text)[:4000]

    # name, frame, page, body px, columns, bend strength (cylinder_warp; book-curved-1080 uses 0.085)
    for index, (name, size, page_size, body, columns, strength) in enumerate((
            ("bent-1080-strong", (1920, 1080), (2200, 1300), 34, 1, rng.uniform(.08, .11)),
            ("bent-1080-medium", (1920, 1080), (2200, 1300), 32, 1, rng.uniform(.03, .05)),
            ("bent-1080-mild", (1920, 1080), (2200, 1300), 34, 1, rng.uniform(.010, .016)),
            ("bent-1080-slight", (1920, 1080), (2200, 1300), 30, 1, rng.uniform(.003, .006)),
            ("bent-1080-2col", (1920, 1080), (2200, 1300), 28, 2, rng.uniform(.05, .09)),
            ("bent-1080-small", (1920, 1080), (2200, 1300), 24, 1, rng.uniform(.02, .04)),
            ("bent-1440", (2560, 1440), (2900, 1700), 34, 1, rng.uniform(.02, .05)),
            ("bent-2160", (3840, 2160), (4200, 2400), 44, 2, rng.uniform(.03, .06)),
            ("bent-2160-slight", (3840, 2160), (4200, 2400), 40, 1, rng.uniform(.004, .008)))):
        page = document_page(words(index), page_size, body, faces[(index + bent) % len(faces)], columns)
        spread = .03
        pose = [(rng.uniform(.03, .03 + spread), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.02, .02 + spread)),
                (rng.uniform(.97 - spread, .97), rng.uniform(.98 - spread, .98)),
                (rng.uniform(.03, .03 + spread), rng.uniform(.98 - spread, .98))]
        add(f"{name}-b{bent}", camera(cylinder_warp(page.image, strength), size, pose, rng, blur=rng.uniform(.6, 1.1),
                                       noise=rng.uniform(2, 4)),
            page.truth, {"kind": "generated-curved-book", "bent": bent, "strength": round(strength, 4)})


def build(out, lecture, ffmpeg, variant=0, bent=0, gutter=0):
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261115)
    guide = repo_words("docs/developer_guide.md", 400)
    readme = repo_words("README.md", 30)
    uct = repo_words("docs/ultra_clear_text.md", 200)
    turkish = turkish_words()
    scenes = []

    def add(name, image, truth, meta):
        path = out / f"{name}.png"
        image.save(path)
        scenes.append((path, truth, meta))

    if variant:
        variant_scenes(out, variant, add)
        return write_manifest(out, scenes)
    if bent:
        bent_scenes(out, bent, add)
        return write_manifest(out, scenes)
    if gutter:
        gutter_scenes(out, gutter, add)
        return write_manifest(out, scenes)
    flat = [(0.06, 0.03), (0.95, 0.05), (0.97, 0.98), (0.04, 0.96)]
    tilt = [(0.16, 0.06), (0.90, 0.02), (0.99, 0.93), (0.05, 0.99)]
    page = document_page(guide, (2200, 1300), 30, "times.ttf", 2)
    add("doc-camera-1080", camera(page.image, (1920, 1080), flat, rng), page.truth, {"kind": "generated-document-camera"})
    page = document_page(readme, (2200, 1300), 34, "georgia.ttf", 2)
    add("doc-camera-tilt-1080", camera(page.image, (1920, 1080), tilt, rng, blur=1.0, noise=4.0), page.truth,
        {"kind": "generated-document-camera"})
    page = document_page(uct, (4200, 2400), 40, "times.ttf", 3)
    add("doc-camera-2160", camera(page.image, (3840, 2160), flat, rng, blur=1.1, noise=3.5), page.truth,
        {"kind": "generated-document-camera"})
    page = document_page(turkish, (2200, 1300), 34, "segoeui.ttf", 2)
    add("doc-camera-turkish-1080", camera(page.image, (1920, 1080), flat, rng), page.truth,
        {"kind": "generated-document-camera", "language": "tr"})
    page = document_page(guide[900:], (2200, 1300), 34, "times.ttf", 1)
    curved = cylinder_warp(page.image)
    add("book-curved-1080", camera(curved, (1920, 1080), flat, rng, blur=0.9), page.truth, {"kind": "generated-curved-book"})
    page = slide(readme[300:], (1920, 1080))
    add("slide-light-1080", page.image, page.truth, {"kind": "generated-slide"})
    page = slide(uct[500:], (1920, 1080), dark=True)
    add("slide-dark-1080", page.image, page.truth, {"kind": "generated-slide"})
    page = slide(guide[2000:], (3840, 2160), scale=2.0)
    add("slide-light-2160", page.image, page.truth, {"kind": "generated-slide"})
    page = table_page(readme[600:], (1920, 1080))
    add("table-1080", page.image, page.truth, {"kind": "generated-table"})
    page = document_page(guide[3000:], (1654, 2339), 26, "arial.ttf", 1)
    add("page-portrait-1654x2339", page.image, page.truth, {"kind": "generated-rendered-page"})
    page = sparse_scene((1920, 1080), rng)
    add("signs-sparse-1080", page.image, page.truth, {"kind": "generated-sparse"})
    add("no-text-1080", textless_scene((1920, 1080), rng), [], {"kind": "generated-no-text"})
    if lecture:
        scenes += lecture_frames(lecture, ffmpeg, out)

    return write_manifest(out, scenes)


def write_manifest(out, scenes):
    manifest = {"version": VERSION, "generator": "scripts/uct/perf_corpus.py", "scenes": []}
    for path, truth, meta in scenes:
        with Image.open(path) as image:
            size = image.size
        entry = {"name": path.stem, "file": path.name, "width": size[0], "height": size[1],
                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), **meta}
        if truth is not None:
            truth_path = path.with_suffix(".truth.txt")
            truth_path.write_text("\n".join(" ".join(row) for row in truth) + "\n", encoding="utf-8")
            entry["truth"] = truth_path.name
            entry["truthWords"] = sum(len(row) for row in truth)
        manifest["scenes"].append(entry)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--lecture", type=Path, help="local copy of the documented public lecture sample")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--variant", type=int, default=0, help="0: standard set; N >= 1: another labelled set")
    parser.add_argument("--bent", type=int, default=0, help="N >= 1: a labelled set of bent book pages only")
    parser.add_argument("--gutter", type=int, default=0,
                        help="N >= 1: a labelled set of pages curled one-sidedly towards a gutter")
    arguments = parser.parse_args()
    if sum(bool(v) for v in (arguments.variant, arguments.bent, arguments.gutter)) > 1:
        sys.exit("--variant, --bent and --gutter select different sets; pass one.")
    out = arguments.out.resolve()
    if ROOT in out.parents or out == ROOT:
        sys.exit("Refusing to write the corpus inside the repository.")
    manifest = build(out, arguments.lecture, arguments.ffmpeg, arguments.variant, arguments.bent, arguments.gutter)
    print(json.dumps({"scenes": len(manifest["scenes"]), "out": str(out)}))


if __name__ == "__main__":
    main()
