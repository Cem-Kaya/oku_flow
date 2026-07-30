#!/usr/bin/env python3
"""Generate OpenZoom's deterministic clamped-camera stabilization fixture."""

from __future__ import annotations

import argparse
import csv
import math
import shutil
import subprocess
import tempfile
from pathlib import Path


WIDTH = 160
HEIGHT = 90
FPS = 30
FRAME_COUNT = 120
LABEL_HEIGHT = 18

FONT_5X7 = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01110"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
}


def write_mp4(y4m_path: Path, mp4_path: Path) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError(
            "ffmpeg is required to keep the MP4 preview synchronized with the Y4M fixture"
        )
    subprocess.run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(y4m_path),
            "-c:v",
            "libx264",
            "-preset",
            "slow",
            "-crf",
            "16",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            "-an",
            str(mp4_path),
        ],
        check=True,
    )


def read_corrections(path: Path) -> list[tuple[float, float]]:
    if not path.exists():
        raise RuntimeError(
            f"{path.name} is missing; run stabilization_cuda_tests "
            f"--export-corrections {path} first"
        )
    with path.open(newline="", encoding="ascii") as source:
        rows = list(csv.DictReader(source))
    if not rows or len(rows) > FRAME_COUNT:
        raise RuntimeError(
            f"{path.name} contains {len(rows)} frames; expected 1-{FRAME_COUNT}"
        )
    return [
        (
            float(row["openzoom_correction_dx_pixels"]),
            float(row["openzoom_correction_dy_pixels"]),
        )
        for row in rows
    ]


def motion(frame: int) -> tuple[float, float]:
    t = frame / FPS
    dx = 2.8 * math.sin(2.0 * math.pi * 1.15 * t)
    dx += 1.1 * math.sin(2.0 * math.pi * 2.75 * t + 0.3)
    dy = 2.1 * math.sin(2.0 * math.pi * 0.85 * t + 0.8)
    dy += 0.9 * math.sin(2.0 * math.pi * 2.2 * t)
    impact_t = t - 1.55
    if impact_t >= 0.0:
        ring = math.exp(-4.2 * impact_t)
        dx += 8.0 * ring * math.sin(2.0 * math.pi * 5.2 * impact_t)
        dy -= 5.5 * ring * math.sin(2.0 * math.pi * 4.6 * impact_t)
    return dx, dy


def base_luma(x: float, y: float) -> float:
    checker = 22.0 if (int(x // 10) + int(y // 10)) & 1 else -18.0
    value = 128.0 + checker
    if 18.0 <= y <= 66.0 and 15.0 <= x <= 145.0:
        value += 25.0
    for row in (25.0, 36.0, 48.0, 60.0):
        if abs(y - row) < 1.3 and 24.0 <= x <= 133.0:
            value = 30.0
    for column in (31.0, 58.0, 91.0, 125.0):
        if abs(x - column) < 1.2 and 20.0 <= y <= 68.0:
            value = 225.0
    radius = math.hypot(x - 128.0, y - 22.0)
    if 8.0 < radius < 10.5:
        value = 18.0
    return max(0.0, min(255.0, value))


def bilinear_base(x: float, y: float) -> float:
    x0 = math.floor(x)
    y0 = math.floor(y)
    fx = x - x0
    fy = y - y0
    a = base_luma(x0, y0) * (1.0 - fx) + base_luma(x0 + 1, y0) * fx
    b = base_luma(x0, y0 + 1) * (1.0 - fx) + base_luma(x0 + 1, y0 + 1) * fx
    return a * (1.0 - fy) + b * fy


def make_frame(frame: int) -> tuple[bytes, dict[str, object]]:
    dx, dy = motion(frame)
    brightness = -28.0 if 78 <= frame < 94 else 0.0
    blurred = 33 <= frame < 39
    occluded = 54 <= frame < 74
    plane = bytearray(WIDTH * HEIGHT)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            source_x = x - dx
            source_y = y - dy
            if blurred:
                value = sum(
                    bilinear_base(source_x + ox, source_y + oy)
                    for oy in (-1.0, 0.0, 1.0)
                    for ox in (-1.0, 0.0, 1.0)
                ) / 9.0
            else:
                value = bilinear_base(source_x, source_y)
            value += brightness
            if occluded:
                left = 30 + (frame - 54) * 2
                if left <= x < left + 42 and 18 <= y < 76:
                    value = 72.0 + ((x + y) & 7)
            plane[y * WIDTH + x] = max(0, min(255, round(value)))
    metadata = {
        "frame": frame,
        "time_seconds": f"{frame / FPS:.6f}",
        "dx_pixels": f"{dx:.6f}",
        "dy_pixels": f"{dy:.6f}",
        "brightness_delta": int(brightness),
        "blurred": int(blurred),
        "occluded": int(occluded),
    }
    return bytes(plane), metadata


def make_target_frame() -> bytes:
    return bytes(
        max(0, min(255, round(base_luma(x, y))))
        for y in range(HEIGHT)
        for x in range(WIDTH)
    )


def sample_plane(plane: bytes, x: float, y: float) -> float:
    x = max(0.0, min(WIDTH - 1.0, x))
    y = max(0.0, min(HEIGHT - 1.0, y))
    x0 = math.floor(x)
    y0 = math.floor(y)
    x1 = min(x0 + 1, WIDTH - 1)
    y1 = min(y0 + 1, HEIGHT - 1)
    fx = x - x0
    fy = y - y0
    top = plane[y0 * WIDTH + x0] * (1.0 - fx)
    top += plane[y0 * WIDTH + x1] * fx
    bottom = plane[y1 * WIDTH + x0] * (1.0 - fx)
    bottom += plane[y1 * WIDTH + x1] * fx
    return top * (1.0 - fy) + bottom * fy


def stabilize_frame(
    disturbed: bytes, correction_x: float, correction_y: float
) -> bytes:
    return bytes(
        max(
            0,
            min(
                255,
                round(
                    sample_plane(
                        disturbed,
                        x - correction_x,
                        y - correction_y,
                    )
                ),
            ),
        )
        for y in range(HEIGHT)
        for x in range(WIDTH)
    )


def draw_label(
    plane: bytearray, width: int, panel: int, label: str, scale: int = 2
) -> None:
    glyph_width = 5 * scale
    spacing = scale
    text_width = len(label) * glyph_width + (len(label) - 1) * spacing
    origin_x = panel * WIDTH + max(0, (WIDTH - text_width) // 2)
    origin_y = 2
    for character in label:
        glyph = FONT_5X7[character]
        for row, bits in enumerate(glyph):
            for column, bit in enumerate(bits):
                if bit == "0":
                    continue
                for offset_y in range(scale):
                    for offset_x in range(scale):
                        x = origin_x + column * scale + offset_x
                        y = origin_y + row * scale + offset_y
                        plane[y * width + x] = 235
        origin_x += glyph_width + spacing


def make_comparison_frame(
    target: bytes,
    disturbed: bytes,
    stabilized: bytes,
) -> bytes:
    comparison_width = WIDTH * 3
    comparison_height = HEIGHT + LABEL_HEIGHT
    plane = bytearray([18]) * (comparison_width * comparison_height)
    for panel, source in enumerate((target, disturbed, stabilized)):
        destination_x = panel * WIDTH
        for y in range(HEIGHT):
            destination = (y + LABEL_HEIGHT) * comparison_width + destination_x
            source_offset = y * WIDTH
            plane[destination : destination + WIDTH] = source[
                source_offset : source_offset + WIDTH
            ]
    for separator_x in (WIDTH, WIDTH * 2):
        for y in range(comparison_height):
            plane[y * comparison_width + separator_x] = 235
    draw_label(plane, comparison_width, 0, "TARGET")
    draw_label(plane, comparison_width, 1, "INPUT")
    draw_label(plane, comparison_width, 2, "OUTPUT")
    return bytes(plane)


def write_comparison_y4m(
    path: Path,
    target: bytes,
    disturbed_frames: list[bytes],
    corrections: list[tuple[float, float]],
) -> None:
    comparison_width = WIDTH * 3
    comparison_height = HEIGHT + LABEL_HEIGHT
    chroma = bytes([128]) * (
        (comparison_width // 2) * (comparison_height // 2)
    )
    with path.open("wb") as video:
        video.write(
            (
                f"YUV4MPEG2 W{comparison_width} H{comparison_height} "
                f"F{FPS}:1 Ip A1:1 C420jpeg\n"
            ).encode("ascii")
        )
        for disturbed, (correction_x, correction_y) in zip(
            disturbed_frames, corrections, strict=True
        ):
            stabilized = stabilize_frame(
                disturbed, correction_x, correction_y
            )
            video.write(b"FRAME\n")
            video.write(
                make_comparison_frame(target, disturbed, stabilized)
            )
            video.write(chroma)
            video.write(chroma)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate OpenZoom's deterministic stabilization fixture."
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Write generated media here instead of beside this script.",
    )
    parser.add_argument(
        "--test-input-only",
        action="store_true",
        help="Generate only the deterministic Y4M input and motion CSV.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    root = Path(__file__).resolve().parent
    output_dir = args.output_dir.resolve() if args.output_dir else root
    output_dir.mkdir(parents=True, exist_ok=True)
    video_path = output_dir / "clamp_bump_160x90_30fps.y4m"
    preview_path = output_dir / "clamp_bump_160x90_30fps.mp4"
    comparison_path = output_dir / "clamp_bump_comparison.mp4"
    corrections_path = root / "clamp_bump_openzoom_corrections.csv"
    manifest_path = output_dir / "clamp_bump_motion.csv"
    chroma = bytes([128]) * ((WIDTH // 2) * (HEIGHT // 2))
    rows = []
    disturbed_frames = []
    with video_path.open("wb") as video:
        video.write(
            f"YUV4MPEG2 W{WIDTH} H{HEIGHT} F{FPS}:1 Ip A1:1 C420jpeg\n".encode(
                "ascii"
            )
        )
        for frame in range(FRAME_COUNT):
            luma, metadata = make_frame(frame)
            video.write(b"FRAME\n")
            video.write(luma)
            video.write(chroma)
            video.write(chroma)
            disturbed_frames.append(luma)
            rows.append(metadata)
    with manifest_path.open("w", newline="", encoding="ascii") as manifest:
        writer = csv.DictWriter(manifest, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    if args.test_input_only:
        return
    write_mp4(video_path, preview_path)
    corrections = read_corrections(corrections_path)
    with tempfile.TemporaryDirectory() as temporary_directory:
        comparison_y4m = (
            Path(temporary_directory) / "clamp_bump_comparison.y4m"
        )
        write_comparison_y4m(
            comparison_y4m,
            make_target_frame(),
            disturbed_frames[: len(corrections)],
            corrections,
        )
        write_mp4(comparison_y4m, comparison_path)


if __name__ == "__main__":
    main()
