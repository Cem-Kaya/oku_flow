"""Render a local preview of the OkuFlow lecture-notes page.

Builds a notes page from the real template headers
(include/okuflow/common/notes_document_template.hpp and
notes_transcribe_bridge.hpp) plus sample entries whose markup mirrors
AssistiveRuntime (AppendNoteHtmlSection, AppendNoteMediaPair, the AI and
Assistant note paths). The output folder has the same layout as a real
OkuFlow data folder (Notes/, Recordings/<date>/, Photos/<date>/, images/), so
"Allow folder access" and "Transcribe" can be tested in Chrome by choosing the
preview folder itself.

Sample media is generated with ffmpeg when it is on PATH; otherwise the page
still renders, with missing media. Pass --speech to use your own WAV for the
recording's audio (the default synthesizes English speech with Windows SAPI
when available).

Usage:
    python scripts/render_notes_preview.py [--out DIR] [--lang en|tr|de] [--speech file.wav]

The sample markup is a copy of the C++ markup: when AssistiveRuntime changes
the structure of an entry, update the matching sample below.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "include" / "okuflow" / "common" / "notes_document_template.hpp"
BRIDGE = ROOT / "include" / "okuflow" / "common" / "notes_transcribe_bridge.hpp"

STRINGS = {
    "en": {
        "title": "OkuFlow Lecture Notes", "started": "Started %1", "conversation": "Conversation",
        "expand": "Expand all", "collapse": "Collapse all", "theme": "Light or dark colors",
        "video": "Video recorded", "photo": "Photo captured", "explain": "Scene explanation",
        "read": "Text on screen", "annotated": "Annotated view",
        "orig_video": "Original camera video", "proc_video": "Processed camera video",
        "orig_photo": "Original camera photo", "proc_photo": "Processed camera photo",
        "pair_video": "Original and processed video", "pair_photo": "Original and processed photo",
        "open_orig": "Open original video", "open_proc": "Open processed video",
        "orig_view": "Original camera view", "proc_view": "Processed camera view",
        "analyzed": "View the assistant analyzed", "captured_proc": "Captured processed camera view",
    },
    "tr": {
        "title": "OkuFlow Ders Notları", "started": "Başlangıç %1", "conversation": "Sohbet",
        "expand": "Tümünü genişlet", "collapse": "Tümünü daralt", "theme": "Açık veya koyu renkler",
        "video": "Video kaydedildi", "photo": "Fotoğraf çekildi", "explain": "Sahne açıklaması",
        "read": "Ekrandaki metin", "annotated": "İşaretli görünüm",
        "orig_video": "Özgün kamera videosu", "proc_video": "İşlenmiş kamera videosu",
        "orig_photo": "Özgün kamera fotoğrafı", "proc_photo": "İşlenmiş kamera fotoğrafı",
        "pair_video": "Özgün ve işlenmiş video", "pair_photo": "Özgün ve işlenmiş fotoğraf",
        "open_orig": "Özgün videoyu aç", "open_proc": "İşlenmiş videoyu aç",
        "orig_view": "Özgün kamera görüntüsü", "proc_view": "İşlenmiş kamera görüntüsü",
        "analyzed": "Asistanın incelediği görüntü", "captured_proc": "Yakalanan işlenmiş kamera görüntüsü",
    },
    "de": {
        "title": "OkuFlow-Vorlesungsnotizen", "started": "Begonnen %1", "conversation": "Gespräch",
        "expand": "Alle aufklappen", "collapse": "Alle zuklappen", "theme": "Helle oder dunkle Farben",
        "video": "Video aufgenommen", "photo": "Foto aufgenommen", "explain": "Szenenbeschreibung",
        "read": "Text auf dem Bildschirm", "annotated": "Markierte Ansicht",
        "orig_video": "Originales Kameravideo", "proc_video": "Bearbeitetes Kameravideo",
        "orig_photo": "Originales Kamerafoto", "proc_photo": "Bearbeitetes Kamerafoto",
        "pair_video": "Originales und bearbeitetes Video", "pair_photo": "Originales und bearbeitetes Foto",
        "open_orig": "Originalvideo öffnen", "open_proc": "Bearbeitetes Video öffnen",
        "orig_view": "Originale Kameraansicht", "proc_view": "Bearbeitete Kameraansicht",
        "analyzed": "Vom Assistenten analysierte Ansicht", "captured_proc": "Aufgenommene bearbeitete Kameraansicht",
    },
}

LECTURE_SPEECH = (
    "Good morning everyone. Today we will talk about the Fourier transform. "
    "A signal in time can be written as a sum of sine waves. "
    "The frequency spectrum shows how strong each sine wave is. "
    "Please open chapter four of the textbook. "
    "We will solve the first exercise together after the break."
)


def raw_pieces(path: Path, delimiter: str) -> str:
    text = path.read_text(encoding="utf-8")
    pieces = re.findall(r'R"' + delimiter + r"\((.*?)\)" + delimiter + '"', text, re.S)
    if not pieces:
        sys.exit(f"No raw string found in {path}")
    return "".join(pieces)


def esc(value: str) -> str:
    return html.escape(value, quote=True)


def section(time: str, heading: str, content: str, *, css: str = "", kind: str = "", lang: str = "en",
            open_: bool = True, preview: str = "", conversation: str = "") -> str:
    # Mirrors AssistiveRuntime::AppendNoteHtmlSection.
    classes = "note" + (f" {css}" if css else "")
    attributes = f' class="{esc(classes)}"'
    if kind:
        attributes += f' data-kind="{kind}"'
    if conversation:
        attributes += f' data-conversation="{esc(conversation)}"'
    preview_html = f'<span class="preview">{esc(preview)}</span>'
    return (
        f'  <details{attributes} lang="{esc(lang)}"{" open" if open_ else ""}>\n'
        f'    <summary><span class="ic" aria-hidden="true"></span><h2 class="ttl">{esc(heading)}</h2>{preview_html}'
        f'<time class="at">{esc(time)}</time></summary>\n'
        f'    <div class="note-body">\n{content}    </div>\n'
        f'  </details>\n'
    )


def media_pair(s: dict, original: str, processed: str, video: bool) -> str:
    # Mirrors AssistiveRuntime::AppendNoteMediaPair.
    if video:
        def player(url: str, label: str) -> str:
            return (f'        <video controls preload="metadata">\n'
                    f'          <source src="{esc(url)}" type="video/mp4">\n'
                    f'          <a href="{esc(url)}">{esc(label)}</a>\n'
                    f'        </video>\n')
        original_media, processed_media = player(original, s["open_orig"]), player(processed, s["open_proc"])
    else:
        def image(url: str, alt: str) -> str:
            return f'        <a href="{esc(url)}"><img src="{esc(url)}" alt="{esc(alt)}" loading="lazy"></a>\n'
        original_media, processed_media = image(original, s["orig_view"]), image(processed, s["proc_view"])
    original_caption = s["orig_video"] if video else s["orig_photo"]
    processed_caption = s["proc_video"] if video else s["proc_photo"]
    content = (
        '    <div class="media-grid">\n'
        '      <figure>\n'
        f'{original_media}'
        f'        <figcaption><a href="{esc(original)}">{esc(original_caption)}</a></figcaption>\n'
        '      </figure>\n'
        '      <figure>\n'
        f'{processed_media}'
        f'        <figcaption><a href="{esc(processed)}">{esc(processed_caption)}</a></figcaption>\n'
        '      </figure>\n'
        '    </div>\n'
    )
    if video:
        content += (
            f'    <section class="transcribe-tools" data-transcribe-src="{esc(original)}">\n'
            '      <div class="transcribe-actions"><button type="button" class="transcribe-start">Transcribe</button>'
            '<button type="button" class="transcribe-stop" hidden>Stop</button>'
            '<button type="button" class="transcribe-export" hidden>Save as text</button></div>\n'
            '      <p class="transcribe-status" role="status"></p>\n'
            '      <div class="transcribe-result" hidden><h3 class="transcribe-heading">Transcript</h3>'
            '<div class="transcribe-lines"></div></div>\n'
            '    </section>\n'
        )
    return content


def ai_note(question: str, image: str, answer: str, s: dict) -> str:
    content = ""
    if question:
        content += f'    <p class="note-ask">{esc(question)}</p>\n'
    if image:
        content += ('    <figure class="ai-shot">\n'
                    f'      <a href="{esc(image)}"><img src="{esc(image)}" alt="{esc(s["analyzed"])}" loading="lazy"></a>\n'
                    '    </figure>\n')
    content += f'    <p class="note-text">{esc(answer)}</p>\n'
    return content


def run(command: list[str]) -> bool:
    try:
        subprocess.run(command, check=True, capture_output=True)
        return True
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"  skipped: {' '.join(command[:2])} ({error})")
        return False


def synthesize_speech(path: Path) -> bool:
    if os.name != "nt":
        return False
    wave_path = str(path).replace("'", "''")
    script = (
        "Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        f"$s.SetOutputToWaveFile('{wave_path}'); $s.Speak('{LECTURE_SPEECH}'); $s.Dispose()"
    )
    shell = shutil.which("pwsh")
    return bool(shell) and run([shell, "-NoProfile", "-Command", script])


def slide_filter(text: str, font: str) -> str:
    return (f"drawtext=fontfile='{font}':text='{text}':fontcolor=0x222222:fontsize=54:x=(w-text_w)/2:y=h*0.32,"
            f"drawtext=fontfile='{font}':text='X(f) = sum of sine waves':fontcolor=0x444444:fontsize=40:x=(w-text_w)/2:y=h*0.55")


def make_media(out: Path, day: str, speech: Path | None) -> dict:
    """Creates sample recordings and photos; returns paths relative to Notes/."""
    rec = out / "Recordings" / day
    pho = out / "Photos" / day
    img = out / "Notes" / "images"
    for folder in (rec, pho, img):
        folder.mkdir(parents=True, exist_ok=True)
    names = {
        "vid_o": rec / "VID_101500_original.mp4", "vid_p": rec / "VID_101500_processed.mp4",
        "pho_o": pho / "IMG_102210_original.jpg", "pho_p": pho / "IMG_102210_processed.jpg",
        "ai": img / "AI_scene.jpg", "draw": pho / "IMG_103005_annotated.png",
    }
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found: the page renders without sample media.")
    else:
        font = "C\:/Windows/Fonts/segoeui.ttf" if os.name == "nt" else "DejaVuSans.ttf"
        board = f"color=c=0xf4f1e8:s=1280x720:r=30,{slide_filter('Fourier transform', font)}"
        audio = ["-i", str(speech)] if speech else ["-f", "lavfi", "-t", "20", "-i", "anullsrc=r=48000:cl=stereo"]
        # Original: a slightly dim, soft camera view. Processed: OkuFlow-style clarity.
        dim = "eq=brightness=-0.08:contrast=0.8:saturation=0.7,gblur=sigma=1.4,noise=alls=10:allf=t"
        clear = "eq=contrast=1.5:brightness=0.02,unsharp=7:7:1.6,colorlevels=rimin=0.25:gimin=0.25:bimin=0.25"
        fmp4 = ["-movflags", "frag_keyframe+empty_moov+default_base_moof"]
        common = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-ac", "2", "-shortest", *fmp4]
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, *audio, "-vf", dim, *common, str(names["vid_o"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, *audio, "-vf", clear, *common, str(names["vid_p"])])
        still = ["-frames:v", "1", "-update", "1"]
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", dim, *still, str(names["pho_o"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", clear, *still, str(names["pho_p"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", f"{clear},scale=960:-2", *still, str(names["ai"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf",
             f"{clear},drawbox=x=300:y=200:w=680:h=110:color=0xd633d3@0.9:t=8", *still, str(names["draw"])])
    notes = out / "Notes"
    return {key: os.path.relpath(path, notes).replace(os.sep, "/") for key, path in names.items()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_out = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "OkuFlow" / "notes-preview"
    parser.add_argument("--out", type=Path, default=default_out)
    parser.add_argument("--lang", choices=sorted(STRINGS), default="en")
    parser.add_argument("--speech", type=Path, help="WAV file used as the recording's audio")
    args = parser.parse_args()
    s = STRINGS[args.lang]
    out = args.out.resolve()
    (out / "Notes").mkdir(parents=True, exist_ok=True)

    speech = args.speech
    if not speech:
        candidate = out / "speech.wav"
        if candidate.exists() or synthesize_speech(candidate):
            speech = candidate
    now = datetime.now()
    day = now.strftime("%Y-%m-%d")
    media = make_media(out, day, speech if speech and speech.exists() else None)

    document = raw_pieces(TEMPLATE, "HTML").replace("@@TRANSCRIBE_SCRIPT@@", raw_pieces(BRIDGE, "JS"))
    created = s["started"].replace("%1", f'<time datetime="{now.isoformat(timespec="seconds")}">{now:%Y-%m-%d %H:%M:%S}</time>')
    for token, value in {
        "@@LANG@@": args.lang, "@@CONVOLABEL@@": esc(s["conversation"]), "@@TITLE@@": esc(s["title"]),
        "@@TIME@@": f"{now:%Y-%m-%d %H:%M:%S}", "@@CREATED@@": created, "@@EXPAND@@": esc(s["expand"]),
        "@@COLLAPSE@@": esc(s["collapse"]), "@@THEMELABEL@@": esc(s["theme"]),
    }.items():
        document = document.replace(token, value)

    lang = args.lang
    blocks = [
        section("10:15:00", s["video"], media_pair(s, media["vid_o"], media["vid_p"], True),
                css="media", kind="video", lang=lang, preview=s["pair_video"]),
        section("10:22:10", s["photo"], media_pair(s, media["pho_o"], media["pho_p"], False),
                css="media", kind="photo", lang=lang, preview=s["pair_photo"]),
        section("10:24:31", s["explain"], ai_note("", media["ai"],
                "A lecture slide titled \"Fourier transform\". Below it the formula reads: X(f) = sum of sine waves. "
                "The lecturer is pointing at the second line.", s),
                css="ai", kind="ai", lang=lang, open_=False,
                preview="A lecture slide titled \"Fourier transform\". Below it the formula reads…"),
        section("10:26:02", s["read"], ai_note("", media["ai"], "Fourier transform\nX(f) = sum of sine waves", s),
                css="ai", kind="ai", lang=lang, open_=False, preview="Fourier transform X(f) = sum of sine waves"),
        section("10:28:40", "What does the second line on the slide mean?", ai_note(
                "What does the second line on the slide mean?", media["ai"],
                "It says that any signal can be described as a sum of sine waves with different frequencies. "
                "X(f) tells you how much of each frequency is present.", s),
                css="ai", kind="ai", lang=lang, open_=False, conversation="thr-preview-1",
                preview="It says that any signal can be described as a sum of sine waves…"),
        section("10:29:15", "Can you give a simple example?", ai_note(
                "Can you give a simple example?", "",
                "A musical chord: three notes played together. The spectrum would show three peaks, "
                "one for each note's frequency.", s),
                css="ai", kind="ai", lang=lang, open_=False, conversation="thr-preview-1",
                preview="A musical chord: three notes played together…"),
        section("10:30:05", s["annotated"],
                f'    <figure>\n      <a href="{esc(media["draw"])}"><img src="{esc(media["draw"])}" '
                f'alt="{esc(s["captured_proc"])}" loading="lazy"></a>\n      <figcaption>{esc(s["proc_view"])}</figcaption>\n    </figure>\n',
                css="media", kind="draw", lang=lang),
    ]
    page = out / "Notes" / f"NOTES_preview_{lang}.html"
    page.write_text(document + "".join(blocks) + "</main>\n</body>\n</html>\n", encoding="utf-8")
    print(f"Preview: {page}")
    print(f"To test Transcribe, choose this folder when asked for folder access: {out}")


if __name__ == "__main__":
    main()
