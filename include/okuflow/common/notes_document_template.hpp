#pragma once

namespace okuflow {

// Self-contained, append-safe notes shell. It works without a network
// connection: styling, icons and the viewer script are inline. Every later
// entry is an appended, self-contained block (see AppendNoteHtmlSection), so
// a crash mid-lecture still leaves a readable page. Placeholders (@@NAME@@)
// are token-replaced by AssistiveRuntime::EnsureNotesFile.
//
// Design rules: low vision first. Text size and colour scheme (automatic,
// light, dark, high contrast) are user-controlled and remembered for every
// notes file; every control is at least 44 px tall and keyboard reachable.
// Kept within MSVC string-literal limits by splitting into adjacent pieces.
inline constexpr char kNotesDocumentTemplate[] = R"HTML(<!doctype html>
<html lang="@@LANG@@" dir="@@DIR@@" data-l10n-conversation="@@CONVOLABEL@@">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <title>@@TITLE@@ - @@TIME@@</title>
  <style>
    :root {
      --scale: 1;
      --bg: #f5f3ef; --panel: #ffffff; --panel-2: #f8f6f2; --ink: #17181b; --muted: #555d68;
      --line: #e4dfd6; --accent: #1d4ed8; --accent-ink: #ffffff; --accent-soft: #e7eefc;
      --bubble: #eaf0fd; --focus: #1d4ed8;
      --k-video: #b0179f; --k-photo: #0b7d6e; --k-ai: #1d4ed8; --k-draw: #b45309; --k-convo: #6d28d9; --k-talk: #555d68;
      --shadow: 0 1px 2px rgba(25, 20, 10, .05), 0 6px 22px rgba(25, 20, 10, .06);
      --radius: 18px;
      color-scheme: light;
    }
    :root[data-theme="dark"] {
      --bg: #0d0f12; --panel: #16191e; --panel-2: #1c2026; --ink: #eceef1; --muted: #a9b1bb;
      --line: #2b3139; --accent: #8ab4ff; --accent-ink: #0b1220; --accent-soft: #1a2640;
      --bubble: #1b2540; --focus: #8ab4ff;
      --k-video: #f07ae4; --k-photo: #4fd1bd; --k-ai: #8ab4ff; --k-draw: #f6ad55; --k-convo: #b794f4; --k-talk: #a9b1bb;
      --shadow: 0 1px 2px rgba(0, 0, 0, .45), 0 8px 26px rgba(0, 0, 0, .35);
      color-scheme: dark;
    }
    @media (prefers-color-scheme: dark) {
      :root:not([data-theme]) {
        --bg: #0d0f12; --panel: #16191e; --panel-2: #1c2026; --ink: #eceef1; --muted: #a9b1bb;
        --line: #2b3139; --accent: #8ab4ff; --accent-ink: #0b1220; --accent-soft: #1a2640;
        --bubble: #1b2540; --focus: #8ab4ff;
        --k-video: #f07ae4; --k-photo: #4fd1bd; --k-ai: #8ab4ff; --k-draw: #f6ad55; --k-convo: #b794f4; --k-talk: #a9b1bb;
        --shadow: 0 1px 2px rgba(0, 0, 0, .45), 0 8px 26px rgba(0, 0, 0, .35);
        color-scheme: dark;
      }
    }
    /* High contrast: black, white and yellow only (about 19:1 and 21:1). */
    :root[data-theme="contrast"] {
      --bg: #000000; --panel: #000000; --panel-2: #000000; --ink: #ffffff; --muted: #f2f2f2;
      --line: #ffffff; --accent: #ffd60a; --accent-ink: #000000; --accent-soft: #000000;
      --bubble: #000000; --focus: #ffd60a;
      --k-video: #ffd60a; --k-photo: #ffd60a; --k-ai: #ffd60a; --k-draw: #ffd60a; --k-convo: #ffd60a; --k-talk: #ffd60a;
      --shadow: none;
      color-scheme: dark;
    }
    :root[data-theme="contrast"] details.note, :root[data-theme="contrast"] .transcribe-tools { border-width: 2px; }
    * { box-sizing: border-box; }
    html { font: calc(18px * var(--scale)) / 1.65 "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif; -webkit-text-size-adjust: 100%; }
    body { margin: 0; background: var(--bg); color: var(--ink); }
    a { color: var(--accent); text-underline-offset: .18em; }
    :focus-visible { outline: 3px solid var(--focus); outline-offset: 3px; border-radius: 8px; }
    [hidden] { display: none !important; }
    button { font: inherit; min-height: 44px; max-width: 100%; overflow-wrap: anywhere; color: var(--ink); background: var(--panel); border: 2px solid var(--line); border-radius: 12px; padding: .35rem .9rem; cursor: pointer; }
    button:hover { border-color: var(--accent); }
    button[aria-pressed="true"] { background: var(--accent); color: var(--accent-ink); border-color: var(--accent); }
    button:disabled { cursor: wait; opacity: .6; }
    .skip { position: absolute; inset-inline-start: .75rem; top: -4rem; z-index: 30; background: var(--accent); color: var(--accent-ink); padding: .6rem 1rem; border-radius: 10px; font-weight: 650; }
    .skip:focus { top: .75rem; }
)HTML"
R"HTML(
    /* Header */
    .topbar { position: sticky; top: 0; z-index: 20; background: color-mix(in srgb, var(--bg) 90%, transparent); backdrop-filter: saturate(1.4) blur(10px); border-bottom: 1px solid var(--line); }
    .topbar-inner { width: min(60rem, calc(100vw - 2rem)); margin: 0 auto; display: flex; align-items: center; justify-content: space-between; gap: .8rem 1.2rem; padding: .55rem 0; flex-wrap: wrap; }
    .brand { margin: 0; display: flex; align-items: center; gap: .6rem; font-weight: 750; letter-spacing: .01em; font-size: 1.05rem; }
    .brand svg { width: 2rem; height: 2rem; flex: none; }
    .tools { display: flex; gap: .45rem; flex-wrap: wrap; align-items: center; }
    .tools button { font-size: .86rem; font-weight: 600; }
    .size-group { display: inline-flex; align-items: center; border: 2px solid var(--line); border-radius: 12px; overflow: hidden; }
    .size-group button { border: 0; border-radius: 0; min-width: 2.9rem; font-weight: 750; }
    .size-group output { min-width: 3.4rem; text-align: center; font-size: .8rem; color: var(--muted); font-variant-numeric: tabular-nums; }
    /* Page heading and filters */
    main { width: min(60rem, calc(100vw - 2rem)); margin: 0 auto; padding: 2.2rem 0 6rem; }
    h1 { font-size: clamp(1.75rem, 4.5vw, 2.6rem); line-height: 1.12; margin: 0; letter-spacing: -.01em; overflow-wrap: anywhere; }
    .created { color: var(--muted); margin: .45rem 0 1.4rem; }
    .filters { display: flex; flex-wrap: wrap; gap: .5rem; margin: 0 0 1.6rem; padding: 0; }
    .filters button { border-radius: 999px; font-size: .9rem; display: inline-flex; align-items: center; gap: .45rem; }
    .filters .n { font-variant-numeric: tabular-nums; opacity: .8; }
    /* Entry cards */
    details.note { --kind: var(--k-talk); position: relative; background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); margin: .9rem 0; box-shadow: var(--shadow); overflow: hidden; }
    details.note::before { content: ""; position: absolute; inset-block: 0; inset-inline-start: 0; width: 5px; background: var(--kind); }
    details.note[data-kind="video"] { --kind: var(--k-video); }
    details.note[data-kind="photo"] { --kind: var(--k-photo); }
    details.note[data-kind="ai"] { --kind: var(--k-ai); }
    details.note[data-kind="draw"] { --kind: var(--k-draw); }
    details.note[data-kind="convo"] { --kind: var(--k-convo); }
    summary { cursor: pointer; list-style: none; display: grid; grid-template-columns: auto minmax(0, 1fr) auto auto; grid-template-areas: "ic ttl at chev" "ic prev at chev"; column-gap: .9rem; align-items: center; padding-block: .95rem; padding-inline: 1.35rem 1.1rem; min-height: 3.8rem; }
    summary::-webkit-details-marker { display: none; }
    summary::after { content: ""; grid-area: chev; width: .7rem; height: .7rem; border-right: 2.5px solid var(--muted); border-bottom: 2.5px solid var(--muted); transform: rotate(-45deg); transition: transform .18s ease; margin: 0 .2rem 0 .1rem; }
    details[open] > summary::after { transform: rotate(45deg); }
    :root[dir="rtl"] summary::after { transform: rotate(135deg); }
    :root[dir="rtl"] details[open] > summary::after { transform: rotate(45deg); }
    details[open] > summary { border-bottom: 1px solid var(--line); }
    summary:hover h2.ttl { color: var(--accent); }
    .ic { grid-area: ic; width: 2.6rem; height: 2.6rem; border-radius: 13px; display: grid; place-items: center; color: var(--kind); background: color-mix(in srgb, var(--kind) 14%, var(--panel)); }
    :root[data-theme="contrast"] .ic { border: 2px solid var(--kind); background: none; }
    .ic svg { width: 1.4rem; height: 1.4rem; fill: none; stroke: currentColor; stroke-width: 1.9; stroke-linecap: round; stroke-linejoin: round; }
    .ic svg .solid { fill: currentColor; stroke: none; }
    h2.ttl { grid-area: ttl; font-size: 1.12rem; line-height: 1.3; margin: 0; font-weight: 650; overflow-wrap: anywhere; }
    .preview { grid-area: prev; min-width: 0; color: var(--muted); font-size: .92rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    details[open] > summary .preview { display: none; }
    details[open] > summary { grid-template-areas: "ic ttl at chev" "ic ttl at chev"; }
    time.at { grid-area: at; color: var(--muted); font-size: .85rem; font-variant-numeric: tabular-nums; background: var(--panel-2); border: 1px solid var(--line); border-radius: 999px; padding: .1rem .6rem; white-space: nowrap; }
    .count { display: inline-block; margin-inline-start: .45rem; background: var(--panel-2); color: var(--muted); border: 1px solid var(--line); border-radius: 999px; padding: 0 .55rem; font-size: .78rem; font-weight: 600; vertical-align: middle; }
    .note-body { padding-block: 1.15rem 1.4rem; padding-inline: 1.6rem 1.4rem; }
    time { font-variant-numeric: tabular-nums; }
    .note-text { white-space: pre-wrap; overflow-wrap: anywhere; margin: 0; max-width: 70ch; font-size: 1.04rem; line-height: 1.7; }
    .note-ask { margin: 0 0 1rem; margin-inline-start: auto; width: fit-content; max-width: min(100%, 60ch); padding: .65rem 1rem; background: var(--bubble); border-radius: 16px; border-end-end-radius: 4px; font-weight: 600; overflow-wrap: anywhere; }
    :root[data-theme="contrast"] .note-ask { border: 2px solid var(--ink); }
    figure { margin: 0; }
    figure.ai-shot { margin: 0 0 1rem; max-width: 26rem; }
    img, video { display: block; width: 100%; height: auto; border-radius: 12px; background: #000; border: 1px solid var(--line); }
    video { max-height: 72vh; }
    .note-body > figure > a > img { width: auto; max-width: 100%; max-height: 70vh; }
    .media-grid { display: grid; grid-template-columns: minmax(0, 1fr); gap: 1rem; }
    /* The processed (enhanced) view is the one the student read from: show it first and largest. */
    .media-grid figure:last-child { order: -1; }
    @media (min-width: 52rem) { .media-grid { grid-template-columns: minmax(0, 2.2fr) minmax(0, 1fr); align-items: start; } }
    figcaption { margin-top: .5rem; font-size: .88rem; }
    figcaption a { display: inline-flex; align-items: center; min-height: 44px; padding: 0 .2rem; color: var(--muted); }
    figcaption a:hover { color: var(--accent); }
    /* Conversation groups */
    details.convo > .note-body { padding: .7rem .8rem .9rem 1.1rem; background: var(--panel-2); }
    details.convo details.note { margin: .55rem 0; box-shadow: none; }
    /* Note actions (added by the viewer script) */
    .note-actions { display: flex; flex-wrap: wrap; gap: .5rem; margin-top: 1rem; }
    .note-actions button { font-size: .88rem; }
)HTML"
R"HTML(
    /* Transcription panel (filled in by the transcription bridge) */
    .transcribe-tools { margin-top: 1.2rem; padding: 1rem 1.1rem 1.1rem; background: var(--panel-2); border: 1px solid var(--line); border-radius: 14px; }
    .transcribe-actions { display: flex; flex-wrap: wrap; gap: .55rem; }
    .transcribe-actions .transcribe-start { background: var(--accent); color: var(--accent-ink); border-color: var(--accent); font-weight: 700; padding-inline: 1.2rem; }
    .transcribe-status { color: var(--muted); margin: .6rem 0 0; overflow-wrap: anywhere; }
    .transcribe-status:empty { display: none; }
    .transcribe-heading { font-size: 1.05rem; margin: 1rem 0 .4rem; }
    .transcribe-lines { max-height: 60vh; overflow: auto; border-top: 1px solid var(--line); overflow-wrap: anywhere; }
    .transcribe-line { display: grid; grid-template-columns: 4.8rem minmax(0, 1fr); gap: .8rem; align-items: baseline; margin: 0; padding: .4rem .2rem; border-bottom: 1px solid var(--line); line-height: 1.6; }
    .transcribe-line button { min-height: 44px; padding: .1rem .5rem; border-radius: 999px; font-size: .85rem; font-variant-numeric: tabular-nums; color: var(--muted); }
    .transcribe-line.is-now { background: var(--accent-soft); border-radius: 10px; }
    .transcribe-line.is-now button { color: var(--accent); border-color: var(--accent); }
    :root[data-theme="contrast"] .transcribe-line.is-now { outline: 2px solid var(--accent); }
    /* Back to top */
    .to-top { position: fixed; inset-inline-end: max(1rem, calc((100vw - 60rem) / 2 - 4rem)); bottom: 1.2rem; z-index: 15; border-radius: 999px; box-shadow: var(--shadow); font-weight: 650; }
    @media (prefers-reduced-motion: reduce) { summary::after { transition: none; } html { scroll-behavior: auto; } }
    @media (prefers-reduced-motion: no-preference) { html { scroll-behavior: smooth; } }
    @media (max-width: 40rem) {
      .topbar { position: static; }
      .topbar-inner, main { width: calc(100vw - 24px); }
      .topbar-inner { padding: .45rem 0; }
      .tools { min-width: 0; max-width: 100%; }
      .size-group { max-width: 100%; }
      .size-group button { min-width: 44px; padding-inline: .35rem; }
      .size-group output { min-width: 0; }
      summary { grid-template-areas: "ic ttl chev" "at at at"; grid-template-columns: auto minmax(0, 1fr) auto; padding: .8rem .75rem; column-gap: .6rem; }
      details[open] > summary { grid-template-areas: "ic ttl chev" "at at at"; }
      .ic { width: min(2.6rem, 56px); height: min(2.6rem, 56px); }
      .preview { display: none; }
      time.at { justify-self: start; margin-top: .25rem; }
      .note-body { padding: 1rem min(.75rem, 24px) 1.2rem; }
      details.convo > .note-body { padding: 12px; }
      .transcribe-tools { padding: min(.75rem, 18px); }
    }
    @media print {
      .topbar, .filters, .note-actions, .transcribe-actions, .to-top, .skip { display: none !important; }
      main { width: 100%; padding: 0; }
      details.note { break-inside: avoid; box-shadow: none; border-color: #999; }
      details:not([open]) > summary ~ * { display: revert; }
    }
  </style>
)HTML"
R"HTML(
  <script>
  (function () {
    "use strict";
    var root = document.documentElement;
    var THEME_KEY = "okuflow-notes-theme";
    var SCALE_KEY = "okuflow-notes-scale";
    var THEMES = ["auto", "light", "dark", "contrast"];
    var TEXT = {
      "en": {"smaller":"Smaller text","larger":"Larger text","colors":"Colors","auto":"Automatic","light":"Light","dark":"Dark","contrast":"High contrast","all":"All","show":"Show","video":"Recordings","photo":"Photos","ai":"Answers","draw":"Drawings","convo":"Conversations","talk":"Notes","read":"Read aloud","stopRead":"Stop reading","copy":"Copy","copied":"Copied","skip":"Skip to notes","top":"Back to top"},
)HTML"
R"HTML(
      "tr": {"smaller":"Yazıyı küçült","larger":"Yazıyı büyüt","colors":"Renkler","auto":"Otomatik","light":"Açık","dark":"Koyu","contrast":"Yüksek karşıtlık","all":"Tümü","show":"Göster","video":"Kayıtlar","photo":"Fotoğraflar","ai":"Yanıtlar","draw":"Çizimler","convo":"Sohbetler","talk":"Notlar","read":"Sesli oku","stopRead":"Okumayı durdur","copy":"Kopyala","copied":"Kopyalandı","skip":"Notlara geç","top":"Başa dön"},
)HTML"
R"HTML(
      "de": {"smaller":"Text verkleinern","larger":"Text vergrößern","colors":"Farben","auto":"Automatisch","light":"Hell","dark":"Dunkel","contrast":"Hoher Kontrast","all":"Alle","show":"Anzeigen","video":"Aufnahmen","photo":"Fotos","ai":"Antworten","draw":"Zeichnungen","convo":"Unterhaltungen","talk":"Notizen","read":"Vorlesen","stopRead":"Vorlesen beenden","copy":"Kopieren","copied":"Kopiert","skip":"Zu den Notizen","top":"Nach oben"},
)HTML"
R"HTML(
      "fr": {"smaller":"Texte plus petit","larger":"Texte plus grand","colors":"Couleurs","auto":"Automatique","light":"Clair","dark":"Sombre","contrast":"Contraste élevé","all":"Tout","show":"Afficher","video":"Enregistrements","photo":"Photos","ai":"Réponses","draw":"Dessins","convo":"Conversations","talk":"Notes","read":"Lire à haute voix","stopRead":"Arrêter de lire","copy":"Copier","copied":"Copié","skip":"Passer aux notes","top":"Retour en haut"},
)HTML"
R"HTML(
      "es": {"smaller":"Texto más pequeño","larger":"Texto más grande","colors":"Colores","auto":"Automático","light":"Luz","dark":"oscuro","contrast":"Alto contraste","all":"Todos","show":"Mostrar","video":"Grabaciones","photo":"Fotos","ai":"Respuestas","draw":"Dibujos","convo":"Conversaciones","talk":"Notas","read":"leer en voz alta","stopRead":"dejar de leer","copy":"Copiar","copied":"Copiado","skip":"Saltar a notas","top":"Volver arriba"},
)HTML"
R"HTML(
      "it": {"smaller":"Testo più piccolo","larger":"Testo più grande","colors":"Colori","auto":"Automatico","light":"Luce","dark":"Buio","contrast":"Contrasto elevato","all":"Tutto","show":"Mostra","video":"Registrazioni","photo":"Foto","ai":"Risposte","draw":"Disegni","convo":"Conversazioni","talk":"Note","read":"Leggi ad alta voce","stopRead":"Smetti di leggere","copy":"Copia","copied":"Copiato","skip":"Passa alle note","top":"Torna all'inizio"},
)HTML"
R"HTML(
      "pt-BR": {"smaller":"Texto menor","larger":"Texto maior","colors":"Cores","auto":"Automático","light":"Luz","dark":"Escuro","contrast":"Alto contraste","all":"Todos","show":"Mostrar","video":"Gravações","photo":"Fotos","ai":"Respostas","draw":"Desenhos","convo":"Conversas","talk":"Notas","read":"Leia em voz alta","stopRead":"Pare de ler","copy":"Copiar","copied":"Copiado","skip":"Pular para notas","top":"Voltar ao topo"},
)HTML"
R"HTML(
      "ru": {"smaller":"Меньший текст","larger":"Текст большего размера","colors":"Цвета","auto":"Автоматический","light":"Светлая","dark":"Темный","contrast":"Высокая контрастность","all":"Все","show":"Показать","video":"Записи","photo":"Фотографии","ai":"Ответы","draw":"Рисунки","convo":"Разговоры","talk":"Заметки","read":"Читать вслух","stopRead":"Остановить чтение","copy":"Копировать","copied":"Скопировано","skip":"Перейти к заметкам","top":"Вернуться наверх"},
)HTML"
R"HTML(
      "uk": {"smaller":"Менший текст","larger":"Більший текст","colors":"Кольори","auto":"Автоматичний","light":"світло","dark":"Темний","contrast":"Високий контраст","all":"все","show":"Показати","video":"Записи","photo":"Фотографії","ai":"Відповіді","draw":"Малюнки","convo":"Розмови","talk":"Нотатки","read":"Прочитайте вголос","stopRead":"Перестаньте читати","copy":"Копіювати","copied":"Скопійовано","skip":"Перейти до нотаток","top":"Повернутися до початку"},
)HTML"
R"HTML(
      "ar": {"smaller":"نص أصغر","larger":"نص أكبر","colors":"الألوان","auto":"تلقائي","light":"ضوء","dark":"الظلام","contrast":"التباين العالي","all":"الكل","show":"عرض","video":"التسجيلات","photo":"صور","ai":"الإجابات","draw":"الرسومات","convo":"المحادثات","talk":"ملاحظات","read":"اقرأ بصوت عالٍ","stopRead":"توقف عن القراءة","copy":"نسخ","copied":"منقول","skip":"انتقل إلى الملاحظات","top":"العودة إلى الأعلى"},
)HTML"
R"HTML(
      "hi": {"smaller":"छोटा पाठ","larger":"बड़ा पाठ","colors":"रंग","auto":"स्वचालित","light":"हल्का","dark":"गहरा","contrast":"उच्च कंट्रास्ट","all":"सब","show":"दिखाओ","video":"रिकॉर्डिंग","photo":"तस्वीरें","ai":"उत्तर","draw":"चित्र","convo":"बातचीत","talk":"नोट्स","read":"ज़ोर से पढ़ें","stopRead":"पढ़ना बंद करें","copy":"प्रतिलिपि","copied":"नकल की गई","skip":"नोट्स पर जाएं","top":"शीर्ष पर वापस जाएँ"},
)HTML"
R"HTML(
      "ja": {"smaller":"小さい文字","larger":"大きな文字","colors":"色","auto":"自動","light":"ライト","dark":"ダーク","contrast":"ハイコントラスト","all":"すべて","show":"表示する","video":"録音","photo":"写真","ai":"答え","draw":"描画","convo":"会話","talk":"ノート","read":"声に出して読む","stopRead":"読み上げを停止","copy":"コピー","copied":"コピーされました","skip":"ノートにスキップ","top":"トップに戻る"},
)HTML"
R"HTML(
      "ko": {"smaller":"더 작은 텍스트","larger":"더 큰 텍스트","colors":"색상","auto":"자동","light":"라이트","dark":"다크","contrast":"고대비","all":"모두","show":"쇼","video":"녹음","photo":"사진","ai":"답변","draw":"그림","convo":"대화","talk":"메모","read":"소리내어 읽기","stopRead":"읽기 중지","copy":"복사","copied":"복사됨","skip":"메모로 건너뛰기","top":"맨 위로 돌아가기"},
)HTML"
R"HTML(
      "zh-Hans": {"smaller":"较小的文字","larger":"较大的文字","colors":"颜色","auto":"自动","light":"浅色","dark":"深色","contrast":"高对比度","all":"全部","show":"显示","video":"录音","photo":"照片","ai":"答案","draw":"绘图","convo":"对话","talk":"笔记","read":"大声朗读","stopRead":"停止阅读","copy":"复制","copied":"已复制","skip":"跳到笔记","top":"返回顶部"},
)HTML"
R"HTML(
      "nl": {"smaller":"Kleinere tekst","larger":"Grotere tekst","colors":"Kleuren","auto":"Automatisch","light":"Licht","dark":"Donker","contrast":"Hoog contrast","all":"Allemaal","show":"Toon","video":"Opnames","photo":"Foto's","ai":"Antwoorden","draw":"tekeningen","convo":"Gesprekken","talk":"Notities","read":"Lees hardop","stopRead":"Stoppen met voorlezen","copy":"Kopieer","copied":"Gekopieerd","skip":"Ga naar notities","top":"Terug naar boven"},
)HTML"
R"HTML(
      "pl": {"smaller":"Mniejszy tekst","larger":"Większy tekst","colors":"Kolory","auto":"Automatyczny","light":"Światło","dark":"Ciemny","contrast":"Wysoki kontrast","all":"Wszystko","show":"Pokaż","video":"Nagrania","photo":"Zdjęcia","ai":"Odpowiedzi","draw":"Rysunki","convo":"Rozmowy","talk":"Notatki","read":"Przeczytaj na głos","stopRead":"Przestań czytać","copy":"Kopiuj","copied":"Skopiowano","skip":"Przejdź do notatek","top":"Powrót do góry"},
)HTML"
R"HTML(
      "cs": {"smaller":"Menší text","larger":"Větší text","colors":"Barvy","auto":"Automaticky","light":"Světlý","dark":"Tmavý","contrast":"Vysoký kontrast","all":"všechny","show":"Zobrazit","video":"Nahrávky","photo":"Fotografie","ai":"Odpovědi","draw":"Kresby","convo":"Konverzace","talk":"Poznámky","read":"Čtěte nahlas","stopRead":"Přestaňte číst","copy":"Kopírovat","copied":"Zkopírováno","skip":"Přejít na poznámky","top":"Zpět nahoru"},
)HTML"
R"HTML(
      "sv": {"smaller":"Mindre text","larger":"Större text","colors":"Färger","auto":"Automatisk","light":"Ljus","dark":"Mörkt","contrast":"Hög kontrast","all":"Alla","show":"Visa","video":"Inspelningar","photo":"Foton","ai":"Svar","draw":"Teckningar","convo":"Samtal","talk":"Anteckningar","read":"Läs högt","stopRead":"Stoppa uppläsning","copy":"Kopiera","copied":"Kopierade","skip":"Hoppa till anteckningar","top":"Tillbaka till toppen"},
)HTML"
R"HTML(
      "id": {"smaller":"Teks lebih kecil","larger":"Teks lebih besar","colors":"Warna","auto":"Otomatis","light":"Ringan","dark":"Gelap","contrast":"Kontras tinggi","all":"Semua","show":"Tunjukkan","video":"Rekaman","photo":"Foto","ai":"Jawaban","draw":"Gambar","convo":"Percakapan","talk":"Catatan","read":"Bacalah dengan lantang","stopRead":"Berhenti membaca","copy":"Salin","copied":"Disalin","skip":"Lewati ke catatan","top":"Kembali ke atas"},
)HTML"
R"HTML(
      "vi": {"smaller":"Văn bản nhỏ hơn","larger":"Văn bản lớn hơn","colors":"Màu sắc","auto":"tự động","light":"Ánh sáng","dark":"Tối","contrast":"Độ tương phản cao","all":"Tất cả","show":"Hiển thị","video":"Bản ghi âm","photo":"Ảnh","ai":"Câu trả lời","draw":"Hình vẽ","convo":"Cuộc trò chuyện","talk":"Ghi chú","read":"Đọc to","stopRead":"Dừng đọc","copy":"Sao chép","copied":"Đã sao chép","skip":"Chuyển đến ghi chú","top":"Quay lại đầu trang"},
)HTML"
R"HTML(
      "th": {"smaller":"ข้อความขนาดเล็กลง","larger":"ข้อความขนาดใหญ่ขึ้น","colors":"สี","auto":"อัตโนมัติ","light":"สว่าง","dark":"มืด","contrast":"คอนทราสต์สูง","all":"ทั้งหมด","show":"แสดง","video":"การบันทึก","photo":"ภาพถ่าย","ai":"คำตอบ","draw":"ภาพวาด","convo":"บทสนทนา","talk":"บันทึก","read":"อ่านออกเสียง","stopRead":"หยุดอ่าน","copy":"คัดลอก","copied":"คัดลอกแล้ว","skip":"ข้ามไปยังบันทึกย่อ","top":"กลับไปด้านบน"},
)HTML"
R"HTML(
      "bn": {"smaller":"ছোট লেখা","larger":"বৃহত্তর পাঠ্য","colors":"রং","auto":"স্বয়ংক্রিয়","light":"হালকা","dark":"গাঢ়","contrast":"উচ্চ বৈসাদৃশ্য","all":"সব","show":"দেখান","video":"রেকর্ডিং","photo":"ফটো","ai":"উত্তর","draw":"অঙ্কন","convo":"কথোপকথন","talk":"নোট","read":"জোরে পড়ুন","stopRead":"পড়া বন্ধ করুন","copy":"কপি","copied":"কপি করা হয়েছে","skip":"নোট এড়িয়ে যান","top":"উপরে ফিরে যান"},
)HTML"
R"HTML(
      "fa": {"smaller":"متن کوچکتر","larger":"متن بزرگتر","colors":"رنگ ها","auto":"خودکار","light":"نور","dark":"تاریک","contrast":"کنتراست بالا","all":"همه","show":"نمایش دهید","video":"ضبط ها","photo":"عکس ها","ai":"پاسخ ها","draw":"نقاشی ها","convo":"گفتگوها","talk":"یادداشت ها","read":"با صدای بلند بخوانید","stopRead":"خواندن را متوقف کنید","copy":"کپی کنید","copied":"کپی شده است","skip":"پرش به یادداشت ها","top":"بازگشت به بالا"},
)HTML"
R"HTML(
      "he": {"smaller":"טקסט קטן יותר","larger":"טקסט גדול יותר","colors":"צבעים","auto":"אוטומטי","light":"בהיר","dark":"כהה","contrast":"ניגודיות גבוהה","all":"הכל","show":"הצג","video":"הקלטות","photo":"תמונות","ai":"תשובות","draw":"ציורים","convo":"שיחות","talk":"הערות","read":"קרא בקול רם","stopRead":"עצור הקראה","copy":"העתק","copied":"הועתק","skip":"דלג להערות","top":"חזרה למעלה"},
)HTML"
R"HTML(
      "ur": {"smaller":"چھوٹا متن","larger":"بڑا متن","colors":"رنگ","auto":"خودکار","light":"ہلکا","dark":"گہرا","contrast":"ہائی کنٹراسٹ","all":"تمام","show":"دکھائیں۔","video":"ریکارڈنگز","photo":"تصاویر","ai":"جوابات","draw":"ڈرائنگ","convo":"بات چیت","talk":"نوٹس","read":"بلند آواز سے پڑھیں","stopRead":"پڑھنا بند کریں","copy":"کاپی","copied":"کاپی","skip":"نوٹس پر جائیں۔","top":"واپس اوپر"}
    };
    var language = root.lang || "en";
    var t = TEXT[language] || TEXT[language.split("-")[0]] || TEXT.en;
    var ICONS = {
      video: '<svg viewBox="0 0 24 24"><rect x="3" y="6" width="13" height="12" rx="2.5"/><path d="M16 10.5 21 7.5v9l-5-3z"/></svg>',
      photo: '<svg viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="13" rx="2.5"/><circle cx="12" cy="12.5" r="3.5"/><path d="M8.5 6 10 4h4l1.5 2"/></svg>',
      ai: '<svg viewBox="0 0 24 24"><path class="solid" d="M12 2.5c.9 5 2.6 6.7 7.5 7.5-4.9.8-6.6 2.5-7.5 7.5-.9-5-2.6-6.7-7.5-7.5 4.9-.8 6.6-2.5 7.5-7.5z"/><path class="solid" d="M19 15.5c.35 1.9 1 2.55 2.9 2.9-1.9.35-2.55 1-2.9 2.9-.35-1.9-1-2.55-2.9-2.9 1.9-.35 2.55-1 2.9-2.9z"/></svg>',
      draw: '<svg viewBox="0 0 24 24"><path d="M4 20l4.2-1L19.5 7.7a2.1 2.1 0 0 0-3-3L5.2 16z"/><path d="M14.5 6.5l3 3"/></svg>',
      convo: '<svg viewBox="0 0 24 24"><path d="M4 5.5h16v10H10l-5 4v-4H4z"/><path d="M8 10.5h8"/></svg>',
      talk: '<svg viewBox="0 0 24 24"><path d="M6 4h9l3 3v13H6z"/><path d="M9 11h6M9 15h6"/></svg>'
    };

    function readSetting(key) { try { return localStorage.getItem(key); } catch (e) { return null; } }
    function writeSetting(key, value) { try { localStorage.setItem(key, value); } catch (e) { /* storage blocked */ } }

    function applyTheme(value) {
      if (value === "light" || value === "dark" || value === "contrast") root.setAttribute("data-theme", value);
      else root.removeAttribute("data-theme");
    }
    function currentTheme() { return root.getAttribute("data-theme") || "auto"; }
    var scale = parseFloat(readSetting(SCALE_KEY));
    if (!(scale >= 0.875 && scale <= 2)) scale = 1;
    root.style.setProperty("--scale", String(scale));
    applyTheme(readSetting(THEME_KEY));
)HTML"
R"HTML(
    function iconize(details) {
      var ic = details.querySelector(":scope > summary > .ic");
      if (ic && !ic.firstChild) ic.innerHTML = ICONS[details.getAttribute("data-kind")] || ICONS.talk;
    }

    function addNoteActions(details) {
      var body = details.querySelector(":scope > .note-body");
      var text = body && body.querySelector(":scope > .note-text");
      if (!text || body.querySelector(":scope > .note-actions")) return;
      var bar = document.createElement("div");
      bar.className = "note-actions";
      if ("speechSynthesis" in window) {
        var read = document.createElement("button");
        read.type = "button";
        read.textContent = t.read;
        read.addEventListener("click", function () {
          if (read.getAttribute("aria-pressed") === "true") {
            speechSynthesis.cancel(); read.setAttribute("aria-pressed", "false"); read.textContent = t.read; return;
          }
          speechSynthesis.cancel();
          var all = document.querySelectorAll(".note-actions button[aria-pressed]");
          for (var i = 0; i < all.length; i++) { all[i].setAttribute("aria-pressed", "false"); all[i].textContent = t.read; }
          var utterance = new SpeechSynthesisUtterance(text.textContent);
          utterance.lang = details.getAttribute("lang") || root.lang || "en";
          var done = function () { read.setAttribute("aria-pressed", "false"); read.textContent = t.read; };
          utterance.onend = done; utterance.onerror = done;
          read.setAttribute("aria-pressed", "true");
          read.textContent = t.stopRead;
          speechSynthesis.speak(utterance);
        });
        read.setAttribute("aria-pressed", "false");
        bar.appendChild(read);
      }
      if (navigator.clipboard) {
        var copy = document.createElement("button");
        copy.type = "button";
        copy.textContent = t.copy;
        copy.addEventListener("click", function () {
          navigator.clipboard.writeText(text.textContent).then(function () {
            copy.textContent = t.copied;
            setTimeout(function () { copy.textContent = t.copy; }, 1800);
          }, function () {});
        });
        bar.appendChild(copy);
      }
      if (bar.firstChild) body.appendChild(bar);
    }

    // Group Advanced Assistant turns of one conversation into a single
    // collapsible "Conversation" block, each turn still individually
    // collapsible inside it. A single-turn conversation stays a plain card.
    function groupConversations(main) {
      var convoLabel = root.getAttribute("data-l10n-conversation") || "Conversation";
      var order = [];
      var byConversation = Object.create(null);
      var turns = main.querySelectorAll(":scope > details[data-conversation]");
      for (var i = 0; i < turns.length; i++) {
        var id = turns[i].getAttribute("data-conversation");
        if (!byConversation[id]) { byConversation[id] = []; order.push(id); }
        byConversation[id].push(turns[i]);
      }
      for (var c = 0; c < order.length; c++) {
        var group = byConversation[order[c]];
        if (group.length < 2) continue;
        var wrap = document.createElement("details");
        wrap.open = true;
        wrap.className = "note convo";
        wrap.setAttribute("data-kind", "convo");
        var summary = document.createElement("summary");
        var ic = document.createElement("span");
        ic.className = "ic";
        ic.setAttribute("aria-hidden", "true");
        summary.appendChild(ic);
        var heading = document.createElement("h2");
        heading.className = "ttl";
        heading.textContent = convoLabel;
        var count = document.createElement("span");
        count.className = "count";
        count.textContent = String(group.length);
        heading.appendChild(count);
        summary.appendChild(heading);
        var preview = document.createElement("span");
        preview.className = "preview";
        var firstTitle = group[0].querySelector("h2.ttl");
        preview.textContent = firstTitle ? firstTitle.textContent : "";
        summary.appendChild(preview);
        var firstTime = group[0].querySelector("time.at");
        if (firstTime) summary.appendChild(firstTime.cloneNode(true));
        wrap.appendChild(summary);
        var body = document.createElement("div");
        body.className = "note-body";
        wrap.appendChild(body);
        main.insertBefore(wrap, group[0]);
        for (var g = 0; g < group.length; g++) body.appendChild(group[g]);
      }
    }
)HTML"
R"HTML(
    function buildFilters(main) {
      var entries = main.querySelectorAll(":scope > details.note");
      var kinds = ["video", "photo", "ai", "convo", "draw", "talk"];
      var counts = {};
      for (var i = 0; i < entries.length; i++) {
        var kind = entries[i].getAttribute("data-kind") || "talk";
        counts[kind] = (counts[kind] || 0) + 1;
      }
      var present = kinds.filter(function (k) { return counts[k]; });
      if (present.length < 2) return;
      var bar = document.createElement("div");
      bar.className = "filters";
      bar.setAttribute("role", "group");
      bar.setAttribute("aria-label", t.show);
      var buttons = [];
      function choose(kind) {
        for (var b = 0; b < buttons.length; b++) buttons[b].setAttribute("aria-pressed", buttons[b].dataset.kind === kind ? "true" : "false");
        for (var e = 0; e < entries.length; e++) entries[e].hidden = kind !== "all" && (entries[e].getAttribute("data-kind") || "talk") !== kind;
      }
      ["all"].concat(present).forEach(function (kind) {
        var button = document.createElement("button");
        button.type = "button";
        button.dataset.kind = kind;
        var label = document.createElement("span");
        label.textContent = t[kind];
        var n = document.createElement("span");
        n.className = "n";
        n.textContent = String(kind === "all" ? entries.length : counts[kind]);
        button.appendChild(label);
        button.appendChild(n);
        button.addEventListener("click", function () { choose(kind); });
        buttons.push(button);
        bar.appendChild(button);
      });
      choose("all");
      var created = main.querySelector(":scope > .created");
      main.insertBefore(bar, created ? created.nextSibling : main.firstChild);
    }

    // Highlight the transcript line that matches the playing recording.
    function followPlayback(event) {
      var video = event.target;
      if (!video || video.tagName !== "VIDEO") return;
      var card = video.closest("details");
      var lines = card ? card.querySelectorAll(".transcribe-line[data-start]") : [];
      var now = video.currentTime;
      for (var i = 0; i < lines.length; i++) {
        var start = parseFloat(lines[i].dataset.start);
        var end = parseFloat(lines[i].dataset.end);
        var active = now >= start && now < (end > start ? end : start + 4);
        if (active !== lines[i].classList.contains("is-now")) {
          lines[i].classList.toggle("is-now", active);
          if (active && !video.paused) {
            var panel = lines[i].parentElement;
            var panelBox = panel.getBoundingClientRect();
            var lineBox = lines[i].getBoundingClientRect();
            var offset = lineBox.top < panelBox.top ? lineBox.top - panelBox.top
              : lineBox.bottom > panelBox.bottom ? lineBox.bottom - panelBox.bottom : 0;
            if (offset) panel.scrollTop += offset; // Follow inside the transcript; keep the document still.
          }
        }
      }
    }
    document.addEventListener("timeupdate", followPlayback, true);
)HTML"
R"HTML(
    document.addEventListener("DOMContentLoaded", function () {
      var main = document.querySelector("main");
      var skip = document.querySelector(".skip");
      if (skip) skip.textContent = t.skip;

      var themeButton = document.getElementById("btn-theme");
      if (themeButton) {
        var label = themeButton.getAttribute("aria-label") || t.colors;
        var syncTheme = function () {
          themeButton.textContent = "◑ " + t.colors + ": " + t[currentTheme()];
          themeButton.setAttribute("aria-label", label + ": " + t[currentTheme()]);
        };
        syncTheme();
        themeButton.addEventListener("click", function () {
          var next = THEMES[(THEMES.indexOf(currentTheme()) + 1) % THEMES.length];
          applyTheme(next);
          writeSetting(THEME_KEY, next);
          syncTheme();
        });
      }

      var smaller = document.getElementById("btn-smaller");
      var larger = document.getElementById("btn-larger");
      var sizeOut = document.getElementById("size-value");
      var syncSize = function () {
        root.style.setProperty("--scale", String(scale));
        if (sizeOut) sizeOut.textContent = Math.round(scale * 100) + "%";
        if (smaller) { smaller.disabled = scale <= 0.875; smaller.setAttribute("aria-label", t.smaller); smaller.title = t.smaller; }
        if (larger) { larger.disabled = scale >= 2; larger.setAttribute("aria-label", t.larger); larger.title = t.larger; }
        writeSetting(SCALE_KEY, String(scale));
      };
      if (smaller) smaller.addEventListener("click", function () { scale = Math.max(0.875, Math.round((scale - 0.125) * 1000) / 1000); syncSize(); });
      if (larger) larger.addEventListener("click", function () { scale = Math.min(2, Math.round((scale + 0.125) * 1000) / 1000); syncSize(); });
      syncSize();

      var setAll = function (open) {
        var all = document.querySelectorAll("details");
        for (var i = 0; i < all.length; i++) all[i].open = open;
      };
      var expand = document.getElementById("btn-expand");
      var collapse = document.getElementById("btn-collapse");
      if (expand) expand.addEventListener("click", function () { setAll(true); });
      if (collapse) collapse.addEventListener("click", function () { setAll(false); });

      var toTop = document.getElementById("btn-top");
      if (toTop) {
        toTop.textContent = "↑ " + t.top;
        toTop.addEventListener("click", function () { window.scrollTo(0, 0); var h = document.querySelector("h1"); if (h) h.focus({ preventScroll: true }); });
        var syncTop = function () { toTop.hidden = window.scrollY < window.innerHeight; };
        window.addEventListener("scroll", syncTop, { passive: true });
        syncTop();
      }

      if (!main) return;
      groupConversations(main);
      var cards = document.querySelectorAll("details.note");
      for (var i = 0; i < cards.length; i++) { iconize(cards[i]); addNoteActions(cards[i]); }
      buildFilters(main);
    });
  })();
  </script>
@@TRANSCRIBE_SCRIPT@@
</head>
<body>
<a class="skip" href="#notes">Skip to notes</a>
<header class="topbar">
  <div class="topbar-inner">
    <p class="brand"><svg viewBox="0 0 108 108" aria-hidden="true"><rect width="108" height="108" rx="26" fill="#121316"/><circle cx="50" cy="50" r="21" fill="none" stroke="#fff" stroke-width="7"/><path d="M65.5 65.5 77 77" stroke="#fff" stroke-width="9" stroke-linecap="round"/><path d="M50 37c1.4 8.6 4.4 11.6 13 13-8.6 1.4-11.6 4.4-13 13-1.4-8.6-4.4-11.6-13-13 8.6-1.4 11.6-4.4 13-13z" fill="#d633d3"/></svg>OkuFlow</p>
    <div class="tools">
      <span class="size-group"><button type="button" id="btn-smaller">A&minus;</button><output id="size-value" aria-live="polite">100%</output><button type="button" id="btn-larger">A+</button></span>
      <button type="button" id="btn-theme" aria-label="@@THEMELABEL@@">&#9681; @@THEMELABEL@@</button>
      <button type="button" id="btn-expand">@@EXPAND@@</button>
      <button type="button" id="btn-collapse">@@COLLAPSE@@</button>
    </div>
  </div>
</header>
<button type="button" class="to-top" id="btn-top" hidden>Back to top</button>
<main id="notes">
  <h1 tabindex="-1">@@TITLE@@</h1>
  <p class="created">@@CREATED@@</p>
)HTML";

} // namespace okuflow
