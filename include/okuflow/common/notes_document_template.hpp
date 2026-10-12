#pragma once

namespace okuflow {

// Self-contained, append-safe notes shell. It works without a network connection.
inline constexpr char kNotesDocumentTemplate[] = R"HTML(<!doctype html>
<html lang="@@LANG@@" data-l10n-conversation="@@CONVOLABEL@@">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>@@TITLE@@ - @@TIME@@</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f2ee; --panel: #ffffff; --panel-2: #faf9f6; --ink: #1a1b1d; --muted: #5b6470;
      --line: #e3ded4; --line-soft: #ece8e0; --accent: #2563eb; --accent-soft: #e8f0fe;
      --chip: #efece5; --user-bubble: #eef2fb; --radius: 12px;
      --shadow: 0 1px 2px rgba(20, 18, 12, .06), 0 1px 8px rgba(20, 18, 12, .04);
    }
    :root[data-theme="dark"] {
      color-scheme: dark;
      --bg: #0e1013; --panel: #181b1f; --panel-2: #1e2226; --ink: #e9ebee; --muted: #9aa2ab;
      --line: #2a2f36; --line-soft: #23272d; --accent: #7aa7ff; --accent-soft: #1b2740;
      --chip: #23282e; --user-bubble: #1a2237;
      --shadow: 0 1px 2px rgba(0, 0, 0, .4), 0 2px 12px rgba(0, 0, 0, .35);
    }
    @media (prefers-color-scheme: dark) {
      :root:not([data-theme="light"]) {
        color-scheme: dark;
        --bg: #0e1013; --panel: #181b1f; --panel-2: #1e2226; --ink: #e9ebee; --muted: #9aa2ab;
        --line: #2a2f36; --line-soft: #23272d; --accent: #7aa7ff; --accent-soft: #1b2740;
        --chip: #23282e; --user-bubble: #1a2237;
        --shadow: 0 1px 2px rgba(0, 0, 0, .4), 0 2px 12px rgba(0, 0, 0, .35);
      }
    }
    * { box-sizing: border-box; }
    html { font: 16px/1.6 system-ui, "Segoe UI", sans-serif; }
    body { margin: 0; background: var(--bg); color: var(--ink); }
    .topbar { position: sticky; top: 0; z-index: 10; background: color-mix(in srgb, var(--bg) 88%, transparent); backdrop-filter: blur(8px); border-bottom: 1px solid var(--line); }
    .topbar-inner { width: min(52rem, calc(100vw - 2rem)); margin: 0 auto; display: flex; align-items: center; justify-content: space-between; gap: 1rem; padding: .5rem 0; flex-wrap: wrap; }
    .brand { margin: 0; font-weight: 700; letter-spacing: .01em; font-size: 1rem; display: flex; align-items: center; gap: .5rem; }
    .brand::before { content: ""; width: .7rem; height: .7rem; border-radius: 3px; background: var(--accent); }
    .tools { display: flex; gap: .4rem; flex-wrap: wrap; }
    .tools button { font: inherit; font-size: .82rem; color: var(--ink); background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: .35rem .8rem; cursor: pointer; }
    .tools button:hover { border-color: var(--accent); color: var(--accent); }
    main { width: min(52rem, calc(100vw - 2rem)); margin: 0 auto; padding: 1.75rem 0 5rem; }
    h1 { font-size: clamp(1.5rem, 4vw, 2rem); line-height: 1.15; margin: 0; }
    .created { color: var(--muted); margin: .35rem 0 1.75rem; font-size: .9rem; }
    /* Every entry is a compact chat-like card. */
    details.note { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); margin: .55rem 0; box-shadow: var(--shadow); overflow: hidden; }
    summary { cursor: pointer; padding: .55rem .85rem; list-style: none; display: flex; align-items: center; gap: .5rem; }
    summary::-webkit-details-marker { display: none; }
    summary::before { content: "\25B8"; flex: none; color: var(--muted); font-size: .72rem; transition: transform .15s ease; }
    details[open] > summary::before { transform: rotate(90deg); }
    /* Type icon shows the entry kind at a glance while collapsed. */
    .ic { flex: none; font-size: 1rem; line-height: 1; }
    .ic::before { content: "\2022 "; color: var(--muted); }
    [data-kind="ai"]    > summary .ic::before { content: "\1F916 "; }
    [data-kind="video"] > summary .ic::before { content: "\1F3AC "; }
    [data-kind="photo"] > summary .ic::before { content: "\1F4F7 "; }
    [data-kind="draw"]  > summary .ic::before { content: "\270F "; }
    [data-kind="talk"]  > summary .ic::before { content: "\1F4AC "; }
    [data-kind="convo"] > summary .ic::before { content: "\1F5E8 "; }
    /* A multi-turn conversation wraps its turn cards; nested cards are flush. */
    details.convo > .note-body { padding: .4rem .6rem .6rem; }
    details.convo details.note { margin: .4rem 0; box-shadow: none; }
    details.convo details.note:first-child { margin-top: 0; }
    details.convo details.note:last-child { margin-bottom: 0; }
    h2.ttl { flex: none; font-size: .92rem; line-height: 1.3; margin: 0; font-weight: 600; color: var(--ink); }
    .count { flex: none; background: var(--chip); color: var(--muted); border-radius: 999px; padding: 0 .5rem; font-size: .74rem; font-weight: 500; }
    /* One-line preview fills the middle; hidden (but space kept) when open. */
    .preview { flex: 1 1 auto; min-width: 0; color: var(--muted); font-size: .85rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    details[open] > summary .preview { visibility: hidden; }
    /* Timestamp: least prominent, pinned to the far right. */
    time.at { flex: none; color: var(--muted); font-size: .76rem; font-variant-numeric: tabular-nums; }
    .note-body { padding: .1rem 1rem .85rem 2.35rem; }
    time { font-variant-numeric: tabular-nums; }
    .note-text { white-space: pre-wrap; overflow-wrap: anywhere; margin: 0; line-height: 1.55; }
    /* The question you asked the assistant, above the analyzed frame + reply. */
    .note-ask { margin: 0 0 .55rem; padding: .45rem .7rem; background: var(--user-bubble); border-radius: 9px; font-weight: 550; overflow-wrap: anywhere; }
    details.ai .note-body { border-left: 2px solid var(--accent-soft); padding-left: .9rem; margin-left: 1.4rem; }
    /* The exact frame the assistant analyzed, above its answer. */
    figure.ai-shot { margin: 0 0 .6rem; max-width: 22rem; }
    figure.ai-shot img { border-radius: 8px; }
    figure { margin: 0; }
    .media-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 18rem), 1fr)); gap: .7rem; }
    img, video { display: block; width: 100%; height: auto; border: 1px solid var(--line); border-radius: 8px; background: #000; }
    figcaption { margin-top: .35rem; color: var(--muted); font-size: .84rem; }
    figcaption a, a { color: var(--accent); }
    button:focus-visible, summary:focus-visible, a:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 6px; }
    @media (prefers-reduced-motion: reduce) { summary::before { transition: none; } }
    @media print { .topbar { display: none; } main { width: 100%; } details.note { break-inside: avoid; box-shadow: none; } details:not([open]) > summary ~ * { display: revert; } }
    button { min-height: 44px; font: inherit; }
    .transcribe-tools { border-top: 1px solid var(--line); margin-top: 1rem; padding-top: 1rem; }
    .transcribe-actions { display: flex; flex-wrap: wrap; gap: .6rem; }
    .transcribe-actions button, .transcribe-line button { background: var(--panel); color: var(--ink); border: 2px solid var(--accent); border-radius: 8px; padding: .45rem .8rem; cursor: pointer; }
    .transcribe-status { color: var(--muted); overflow-wrap: anywhere; }
    .transcribe-line { display: grid; grid-template-columns: 5rem minmax(0, 1fr); gap: .8rem; align-items: start; }
    .transcribe-lines { overflow-wrap: anywhere; }
    .transcribe-heading { font-size: 1.1rem; }
    button:disabled { cursor: wait; opacity: .65; }
    [hidden] { display: none !important; }
    @media (max-width: 600px) { .note-body { padding: .5rem .85rem 1rem; } h2.ttl { flex: 1; } .preview { display: none; } summary { flex-wrap: wrap; } .tools button { min-height: 44px; } }
  </style>
  <script>
  (function () {
    "use strict";
    var storageKey = "okuflow-notes-theme";
    function applyTheme(value) {
      if (value === "light" || value === "dark") {
        document.documentElement.setAttribute("data-theme", value);
      }
    }
    try { applyTheme(localStorage.getItem(storageKey)); } catch (e) { /* blocked storage */ }
    function isDark() {
      var explicit = document.documentElement.getAttribute("data-theme");
      if (explicit) { return explicit === "dark"; }
      return !!(window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
    }
    document.addEventListener("DOMContentLoaded", function () {
      var themeButton = document.getElementById("btn-theme");
      if (themeButton) {
        var sync = function () {
          themeButton.setAttribute("aria-pressed", isDark() ? "true" : "false");
        };
        sync();
        themeButton.addEventListener("click", function () {
          var next = isDark() ? "light" : "dark";
          applyTheme(next);
          try { localStorage.setItem(storageKey, next); } catch (e) { /* blocked */ }
          sync();
        });
      }
      var setAll = function (open) {
        var all = document.querySelectorAll("details");
        for (var i = 0; i < all.length; i++) { all[i].open = open; }
      };
      var expand = document.getElementById("btn-expand");
      var collapse = document.getElementById("btn-collapse");
      if (expand) { expand.addEventListener("click", function () { setAll(true); }); }
      if (collapse) { collapse.addEventListener("click", function () { setAll(false); }); }
      var main = document.querySelector("main");
      if (!main) return;
      // Group Advanced Assistant turns of one conversation into a single
      // collapsible "Conversation" block, each turn still individually
      // collapsible inside it. A single-turn conversation stays a plain card.
      var convoLabel =
        document.documentElement.getAttribute("data-l10n-conversation") ||
        "Conversation";
      var order = [];
      var byConversation = {};
      var turns = main.querySelectorAll(":scope > details[data-conversation]");
      for (var t = 0; t < turns.length; t++) {
        var id = turns[t].getAttribute("data-conversation");
        if (!byConversation[id]) { byConversation[id] = []; order.push(id); }
        byConversation[id].push(turns[t]);
      }
      for (var c = 0; c < order.length; c++) {
        var group = byConversation[order[c]];
        if (group.length < 2) { continue; }
        var cWrap = document.createElement("details");
        cWrap.open = true;
        cWrap.className = "note convo";
        cWrap.setAttribute("data-kind", "convo");
        var cSummary = document.createElement("summary");
        var cIc = document.createElement("span");
        cIc.className = "ic";
        cIc.setAttribute("aria-hidden", "true");
        cSummary.appendChild(cIc);
        var cH2 = document.createElement("h2");
        cH2.className = "ttl";
        cH2.textContent = convoLabel;
        cSummary.appendChild(cH2);
        var cCount = document.createElement("span");
        cCount.className = "count";
        cCount.textContent = String(group.length);
        cSummary.appendChild(cCount);
        var cPrev = document.createElement("span");
        cPrev.className = "preview";
        cPrev.textContent = group[0].querySelector("h2.ttl")
          ? group[0].querySelector("h2.ttl").textContent : "";
        cSummary.appendChild(cPrev);
        cWrap.appendChild(cSummary);
        var cBody = document.createElement("div");
        cBody.className = "note-body";
        cWrap.appendChild(cBody);
        main.insertBefore(cWrap, group[0]);
        for (var g = 0; g < group.length; g++) { cBody.appendChild(group[g]); }
      }
    });
  })();
  </script>
@@TRANSCRIBE_SCRIPT@@
</head>
<body>
<header class="topbar">
  <div class="topbar-inner">
    <p class="brand">OkuFlow</p>
    <div class="tools">
      <button type="button" id="btn-expand">@@EXPAND@@</button>
      <button type="button" id="btn-collapse">@@COLLAPSE@@</button>
      <button type="button" id="btn-theme" aria-pressed="false">&#9681; @@THEMELABEL@@</button>
    </div>
  </div>
</header>
<main>
  <h1>@@TITLE@@</h1>
  <p class="created">@@CREATED@@</p>
)HTML";

} // namespace okuflow
