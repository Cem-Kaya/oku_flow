# Chat-first Assistant concept — 2026-10-14

User direction: Assistant is a chat window, synchronized with Explain in both
Advanced and floating views; Read is separate. One compact connection/settings
bar replaces permanent permission walls and Chat/History/Control subtabs.

Visual: [generated concept](52-chat-first-concept.png). Generated with the
built-in Imagegen tool from the actual Qt development screenshot. It is a
concept, not an implemented app screenshot or camera/provider acceptance proof.
The original is retained under Codex generated_images. No third-party asset
library or runtime dependency was introduced.

Prompt: 1600×900 native Windows Qt app, preserve the existing camera viewport,
Simple/Advanced/Text Clarity switch, top-level Image/Assistant/Settings, bottom
Photo/Record/Explain/Read/Draw and quick modes. Redesign only the Assistant pane
as large high-contrast Segoe UI chat: compact ChatGPT connection/New/history/gear
bar, conversation messages, fixed composer, camera attachment, contextual
human-readable approval with Allow once/Deny, configuration behind the gear.
No persistent checkbox wall, source IDs/JSON, decorative website framing,
marketing copy or nested navigation. Preserve keyboard and theme accessibility.

Implementation qualifications: generated illustrative timestamps are optional;
chat text such as the sample “Yes, go ahead” never grants app permissions.
Explicit approval buttons remain the only consent path. Do not paint the
concept into the app: implement native Qt widgets and real shared chat state.
Keep existing icon assets; no new generated raster icons are needed. Source
image size may differ slightly from the requested viewport; compare normalized
content regions and record differences before native design acceptance.
