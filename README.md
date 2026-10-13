<p align="center">
  <img src="https://okuflow.com/assets/okuflow-icon-a49f3da6e643.png" width="96" alt="OkuFlow star magnifier logo">
</p>

# OkuFlow

**Agentic accessibility, from any seat.**

**Built by the legally blind, for the legally blind.**

Point a webcam, a document camera or your phone at the board. OkuFlow magnifies it live, sharpens the text and holds the picture steady. When you ask, its assistant reads the text, explains what it shows and keeps your lecture notes. Choose Read Aloud to hear an answer.

[Website](https://okuflow.com) · [Download MSI](https://github.com/Cem-Kaya/oku_flow/releases/download/v0.2.3/OkuFlow-0.2.3-x64.msi) · [Portable ZIP](https://github.com/Cem-Kaya/oku_flow/releases/download/v0.2.3/OkuFlow-0.2.3-windows-x64.zip) · [Release notes](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.3)

![OkuFlow Simple view showing a classroom chalkboard, with large camera controls](https://okuflow.com/media/okuflow-simple-206165dcd785.webp)

*OkuFlow running on a demonstration scene. These screenshots show the interface and its tools; they are not real-camera quality or performance measurements.*

| At a glance | |
| --- | --- |
| Desktop | Windows 10 and 11, x64 |
| Cameras | Compatible USB/document cameras; optional Android USB companion in testing |
| Enhancement | GPU-accelerated with NVIDIA CUDA; raw video only without a compatible GPU |
| Languages | English, Turkish and German, switchable without restarting |
| Cost | Free and open source; hardware and optional AI provider access are separate |

## Accessibility should be accessible

Assistive technology is too often expensive, tied to special hardware and hard to set up, even though the people who need it most cannot read the small print. OkuFlow grows from lived experience of low vision. We want affordable, independent use with cameras you already have, large high-contrast controls and spoken answers.

- **It sees:** a live, steady, magnified view with Text Clarity, comfortable reading colors and correction for screens seen at an angle.
- **It understands:** ask the assistant to read the text in view, explain the scene or answer follow-up questions.
- **It remembers:** photos, recordings and answers gather in lecture notes you can review after class; add transcripts locally from your recordings.

OkuFlow is a **live camera magnifier**, for a page on your desk, a whiteboard or a projected slide. It does not magnify your computer's desktop.

## One live view. Every adjustment within reach.

**Text Clarity** helps camera text stand out with background flattening, glare suppression, automatic text polarity, sharpening and stroke-weight controls. It chooses a paper, board or mixed-content treatment, with two-color reading when you prefer it.

![OkuFlow Text Clarity applied to the classroom chalkboard demonstration scene](https://okuflow.com/media/okuflow-text-162a80541c93.webp)

**Display colors** turn the picture into a high-contrast pair. Choose colors such as yellow on black, white on black or black on cyan, then adjust contrast and brightness for your eyes.

![OkuFlow high-contrast display colors on the classroom demonstration scene](https://okuflow.com/media/okuflow-colors-4655e6a5b477.webp)

**Start simple. Go deeper when you need to.** Simple gives the camera the whole window, with quick modes such as Read a Page, Projector Screen and Whiteboard. Advanced keeps the live view beside every setting. Zoom and pan, stabilize a mounted camera, or straighten a screen viewed at an angle.

**Zoom into the detail.** Magnify the desktop camera view up to 16x in Fill framing. Keep the controls within reach while a single equation fills the view.

![OkuFlow showing the enlarged kinetic-energy equation with its Simple controls](https://okuflow.com/media/okuflow-equation-79862fa42241.webp)

![OkuFlow Advanced view with the live chalkboard image beside the settings inspector](https://okuflow.com/media/okuflow-advanced-13b63c54fa43.webp)

## Keep the whole lecture

Photos and recordings are saved in pairs: the original camera image and OkuFlow's processed view. Draw on the picture to mark what matters. Recorded ink goes into the processed copy while the original stays clean.

Record microphone audio or choose no microphone for silent video. Captures, readings and explanations gather in local HTML lecture notes. The default folder is `Documents\OkuFlow\`; you can choose another location.

**Privacy-respecting local transcription:** open recorded-video notes in Chrome and choose Transcribe. Grant access to the folder containing your recording, then explicitly choose a speech model and processor in the [OkuFlow transcription tool](https://okuflow.com/transcribe/). First use downloads the selected model and runtime. Speech recognition runs on your own computer, without an inference server or audio/text upload. Returned text appears beneath the recording and is saved beside it as a transcript sidecar. Browser caches can be cleared or evicted, so a later session may need another download.

Ask permission before recording a lecture or other people.

## Get started

You need three things:

1. **A Windows 10 or 11 x64 PC.** Download the [v0.2.3 MSI installer](https://github.com/Cem-Kaya/oku_flow/releases/download/v0.2.3/OkuFlow-0.2.3-x64.msi), or extract the complete [portable ZIP](https://github.com/Cem-Kaya/oku_flow/releases/download/v0.2.3/OkuFlow-0.2.3-windows-x64.zip) and run `oku_flow.exe`. Keep its adjacent runtime files together.
2. **A compatible camera.** Use a USB or document camera with Windows Media Foundation support, or the optional Android companion described below.
3. **A compatible NVIDIA GPU for enhancement.** CUDA powers Text Clarity, stabilization, screen correction and colors. Without it, OkuFlow shows raw, unprocessed camera video with a clear on-screen notice.

**v0.2.3 is an unsigned preview.** Windows SmartScreen may warn about an unrecognized app. Check the [release notes and known issues](https://github.com/Cem-Kaya/oku_flow/releases/tag/v0.2.3) before installing. Development documentation may describe changes newer than this download.

In the portable ZIP, open **oku_flow.exe**. It starts the required
`OkuFlowApp.exe` component automatically. Keep all ZIP contents together.
The MSI installer gives you a single **OkuFlow** shortcut to open.
The next portable ZIP puts supporting files in an `app` folder, leaving one EXE
at the top level. Keep that folder beside `oku_flow.exe`.

The next app update adds 25 languages, including Dutch, a system-language
default and a saved manual choice. This work is local and unreleased; see the
[language coverage and validation notes](docs/localization.md).

Optional extras, including the Codex CLI for Read and Explain and NVIDIA Super Resolution, are available through **Setup & Downloads** inside the app. Super Resolution requires the separately installed supported NVIDIA runtime. SuperRes powered by NVIDIA Maxine™.

### Your Android phone as the camera

OkuFlow Camera sends your phone's camera to the Windows app over USB. It is optional; compatible USB/document cameras work without it.

The companion is available through Google Play closed testing. **Android 0.1.2 (version code 3) is available to invited Alpha testers** through the [closed-test link](https://play.google.com/apps/testing/com.okuflow).

When the test is available to you, install OkuFlow Camera, enable USB debugging and authorize your computer on the phone. Connect the cable, keep the phone app open and select **Advanced → Settings → Camera → OkuFlow Camera**. Phone and desktop compatibility testing continues; see the [Android guide](android/README.md) for setup and current limits.

## Your camera. Your PC. Your assistant.

The magnifier works without AI. **Read** transcribes the text in view, **Explain** describes the scene, and **Assistant** supports follow-up questions. Requests run when you ask. Answers appear in large text; **Read Aloud** speaks only when you choose it, using a Windows voice installed on your PC.

Choose where the requested image goes:

| Provider | What to expect |
| --- | --- |
| Codex / ChatGPT | Uses your signed-in Codex CLI and an online service. Requests can send text and attached camera images to the provider. Access, usage limits and any charges are separate. |
| OpenAI-compatible vision endpoint | Configure your own endpoint, including a local model. Its network behavior depends on your setup. |

Check your provider's privacy terms before sending sensitive material. Local recording transcription is a separate browser workflow: it downloads its model, but does not upload the recording or transcript.

## What's next

| Work | Status |
| --- | --- |
| Claude through the Anthropic CLI for Read, Explain and Assistant | In development for a future release |
| GPU acceleration for AMD and Intel graphics | Planned |
| Android USB camera companion | Available to invited Alpha testers; compatibility testing continues |
| `OkuFlow.OkuFlow` in the winget catalog | [Submitted](https://github.com/microsoft/winget-pkgs/pull/451412), pending catalog acceptance |

Microsoft Store submission and automatic updates remain future work. Current enhancement uses NVIDIA CUDA.

## Free and open source

OkuFlow is free under [GPL-3.0-only](LICENSE), with an additional permission described in the license. [Commercial licensing](COMMERCIAL.md) is also available. We do not make money from OkuFlow yet. To support the project or discuss commercial use, contact [info@okuflow.com](mailto:info@okuflow.com).

Found a problem or have an idea? [Open a GitHub issue](https://github.com/Cem-Kaya/oku_flow/issues).

Developers and contributors: the complete technical README has moved to the [developer guide](docs/developer_guide.md), covering builds, runtime controls, tests and contribution terms. The [documentation index](docs/README.md) covers architecture, setup and distribution; [third-party notices](docs/THIRD_PARTY_LICENSES.md) describe dependencies and redistribution obligations.
