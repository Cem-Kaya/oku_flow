#pragma once

namespace okuflow {

// Inline in the local notes document: no server, recognition runtime or model
// runs in the Windows process. The HTTPS tool receives a File over a private
// MessageChannel and returns plain segments. Only sidecars are written here.
inline constexpr char kNotesTranscribeBridge[] = R"JS(<script>
(function () {
  'use strict';
  const labels = {
    en: {go:'Transcribe', folder:'Allow folder access', choose:'Choose the OkuFlow folder containing this recording. Then press Transcribe again.', ready:'Folder ready. Press Transcribe to start.', unsupported:'Open these notes in Google Chrome to transcribe recordings.', blocked:'Allow pop-ups for this notes page, then try again.', waiting:'Opening OkuFlow Transcribe…', reading:'Reading audio…', loading:'Preparing the speech model…', downloading:'Downloading the speech model…', running:'Transcribing…', queued:'Waiting for another recording…', stop:'Stop', stopped:'Stopped.', done:'Transcript ready.', saved:'Transcript saved beside the recording.', saveFailed:'Transcript ready. Folder access was lost; use Save as text.', text:'Save as text', restore:'Load saved transcript', title:'Transcript', missing:'Choose the OkuFlow folder containing this recording, then try again.', connection:'The transcription tab closed or did not respond. Try again.', empty:'No speech was found.', failed:'Transcription failed.'},
    tr: {go:'Metne dök', folder:'Klasöre erişim izni ver', choose:'Bu kaydın bulunduğu OkuFlow klasörünü seçin. Sonra Metne dök düğmesine tekrar basın.', ready:'Klasör hazır. Başlamak için Metne dök düğmesine basın.', unsupported:'Kayıtları metne dökmek için bu notları Google Chrome’da açın.', blocked:'Bu not sayfası için açılır pencerelere izin verip tekrar deneyin.', waiting:'OkuFlow Döküm açılıyor…', reading:'Ses okunuyor…', loading:'Konuşma modeli hazırlanıyor…', downloading:'Konuşma modeli indiriliyor…', running:'Metne dökülüyor…', queued:'Başka bir kayıt bekleniyor…', stop:'Durdur', stopped:'Durduruldu.', done:'Döküm hazır.', saved:'Döküm kaydın yanına kaydedildi.', saveFailed:'Döküm hazır. Klasör erişimi kaybedildi; Metin olarak kaydet düğmesini kullanın.', text:'Metin olarak kaydet', restore:'Kaydedilen dökümü yükle', title:'Döküm', missing:'Bu kaydın bulunduğu OkuFlow klasörünü seçip tekrar deneyin.', connection:'Döküm sekmesi kapandı veya yanıt vermedi. Tekrar deneyin.', empty:'Konuşma bulunamadı.', failed:'Döküm başarısız oldu.'},
    de: {go:'Transkribieren', folder:'Ordnerzugriff erlauben', choose:'Wählen Sie den OkuFlow-Ordner mit dieser Aufnahme. Drücken Sie danach erneut Transkribieren.', ready:'Ordner bereit. Drücken Sie Transkribieren zum Starten.', unsupported:'Öffnen Sie diese Notizen in Google Chrome, um Aufnahmen zu transkribieren.', blocked:'Erlauben Sie Pop-ups für diese Notizenseite und versuchen Sie es erneut.', waiting:'OkuFlow Transkription wird geöffnet…', reading:'Audio wird gelesen…', loading:'Sprachmodell wird vorbereitet…', downloading:'Sprachmodell wird heruntergeladen…', running:'Transkription läuft…', queued:'Warten auf eine andere Aufnahme…', stop:'Stopp', stopped:'Gestoppt.', done:'Transkript bereit.', saved:'Transkript neben der Aufnahme gespeichert.', saveFailed:'Transkript bereit. Der Ordnerzugriff wurde entzogen; verwenden Sie Als Text speichern.', text:'Als Text speichern', restore:'Gespeichertes Transkript laden', title:'Transkript', missing:'Wählen Sie den OkuFlow-Ordner mit dieser Aufnahme und versuchen Sie es erneut.', connection:'Die Transkriptionsregisterkarte wurde geschlossen oder antwortet nicht. Versuchen Sie es erneut.', empty:'Keine Sprache gefunden.', failed:'Transkription fehlgeschlagen.'}
  };
  const lang = document.documentElement.lang.split('-')[0];
  const l = labels[lang] || labels.en;
  const toolUrl = 'https://okuflow.com/transcribe/?ui=' + encodeURIComponent(lang);
  const toolOrigin = new URL(toolUrl).origin;
  let root = null;
  let popup = null;
  function database() {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open('okuflow-notes-files', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('handles');
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }
  async function remember(handle) {
    try {
      const db = await database();
      await new Promise((resolve, reject) => {
        const tx = db.transaction('handles', 'readwrite');
        tx.objectStore('handles').put(handle, 'root');
        tx.oncomplete = resolve; tx.onerror = () => reject(tx.error);
      });
      db.close();
    } catch (_) { /* Folder access remains usable for this page. */ }
  }
  async function recall() {
    try {
      const db = await database();
      root = await new Promise((resolve, reject) => {
        const request = db.transaction('handles').objectStore('handles').get('root');
        request.onsuccess = () => resolve(request.result || null);
        request.onerror = () => reject(request.error);
      });
      db.close();
    } catch (_) { root = null; }
  }
  async function recording(source) {
    const url = new URL(source, location.href);
    if (url.protocol !== 'file:') throw new Error(l.missing);
    const parts = url.pathname.split('/').filter(Boolean).map(decodeURIComponent);
    const index = parts.lastIndexOf(root.name);
    if (index < 0 || index === parts.length - 1) throw new Error(l.missing);
    let directory = root;
    for (const part of parts.slice(index + 1, -1)) {
      if (part === '.' || part === '..') throw new Error(l.missing);
      directory = await directory.getDirectoryHandle(part);
    }
    const name = parts.at(-1);
    const handle = await directory.getFileHandle(name);
    return {file:await handle.getFile(), directory, name};
  }
  function clock(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  }
  function validSegments(segments) {
    return Array.isArray(segments) && segments.length <= 50000 && segments.every(s =>
      typeof s.text === 'string' && s.text.length < 20000 && Number.isFinite(s.start) &&
      Number.isFinite(s.end) && s.start >= 0 && s.end >= s.start);
  }
  function showLine(card, segment) {
    const row = document.createElement('p'); row.className = 'transcribe-line';
    const time = document.createElement('button'); time.type = 'button'; time.textContent = clock(segment.start);
    time.addEventListener('click', () => {
      card.closest('details').querySelectorAll('video').forEach(video => {video.currentTime = segment.start;});
    });
    const text = document.createElement('span'); text.textContent = segment.text;
    row.append(time, text); card.querySelector('.transcribe-lines').append(row);
  }
  function saveText(card) {
    const text = card.segments.map(s => '[' + clock(s.start) + '] ' + s.text).join('\n');
    const url = URL.createObjectURL(new Blob([text], {type:'text/plain;charset=utf-8'}));
    const a = document.createElement('a'); a.href = url;
    a.download = 'transcript.txt'; a.click(); setTimeout(() => URL.revokeObjectURL(url), 10000);
  }
  async function loadSaved(card) {
    if (!root || await root.queryPermission({mode:'readwrite'}) !== 'granted') return;
    try {
      const entry = await recording(card.dataset.transcribeSrc);
      const file = await (await entry.directory.getFileHandle(entry.name + '.okuflow-transcript.json')).getFile();
      if (file.size > 8 * 1024 * 1024) return;
      const data = JSON.parse(await file.text());
      if (data.version !== 1 || data.recording?.size !== entry.file.size ||
          data.recording?.lastModified !== entry.file.lastModified || !validSegments(data.segments)) return;
      card.segments = data.segments;
      card.querySelector('.transcribe-lines').replaceChildren();
      card.segments.forEach(s => showLine(card, s));
      card.querySelector('.transcribe-result').hidden = false;
      card.querySelector('.transcribe-export').hidden = !card.segments.length;
      card.querySelector('.transcribe-status').textContent = l.saved;
    } catch (_) { /* No saved transcript for this recording. */ }
  }
  async function begin(card) {
    const status = card.querySelector('.transcribe-status');
    const button = card.querySelector('.transcribe-start');
    if (!window.showDirectoryPicker) { status.textContent = l.unsupported; return; }
    try {
      if (!root || await root.queryPermission({mode:'readwrite'}) !== 'granted') {
        if (root && await root.requestPermission({mode:'readwrite'}) === 'granted') {
          status.textContent = l.ready;
        } else {
          status.textContent = l.choose;
          root = await showDirectoryPicker({id:'okuflow-notes', mode:'readwrite'});
          await remember(root); status.textContent = l.ready;
        }
        await loadSaved(card);
        return; // Opening a tab consumes activation: keep permission as its own click.
      }
      if (!popup || popup.closed) popup = window.open(toolUrl, 'okuflow-transcribe-' + crypto.randomUUID());
      else popup.focus();
      if (!popup) { status.textContent = l.blocked; return; }
      status.textContent = l.waiting; button.disabled = true;
      const entry = await recording(card.dataset.transcribeSrc);
      card.segments = []; card.querySelector('.transcribe-lines').replaceChildren();
      card.querySelector('.transcribe-result').hidden = false;
      card.querySelector('.transcribe-export').hidden = true;
      const stop = card.querySelector('.transcribe-stop'); stop.hidden = false;
      const channel = new MessageChannel();
      let complete = false;
      let receiveReady;
      const finish = () => {complete = true; window.removeEventListener('message', receiveReady); clearInterval(ping); clearTimeout(timeout); button.disabled = false; stop.hidden = true; channel.port1.close();};
      stop.onclick = () => {channel.port1.postMessage({type:'cancel'}); status.textContent = l.stopped; finish();};
      channel.port1.onmessage = async ({data}) => {
        if (complete || !data || typeof data !== 'object') return;
        if (data.type === 'segment' && validSegments([data])) {card.segments.push({start:data.start,end:data.end,text:data.text}); showLine(card, data);}
        if (data.type === 'status') {
          const phase = {queued:l.queued, loading:l.loading, downloading:l.downloading, decoding:l.reading, transcribing:l.running, cancelled:l.stopped};
          const percent = Number.isFinite(data.fraction) ? ' ' + Math.round(Math.min(1, Math.max(0, data.fraction)) * 100) + '%' : '';
          status.textContent = (phase[data.state] || l.waiting) + percent;
        }
        if (data.type === 'error' || data.type === 'cancelled') {status.textContent = data.type === 'cancelled' ? l.stopped : l.failed + ' ' + String(data.message || ''); finish();}
        if (data.type === 'done' && validSegments(data.segments)) {
          card.segments = data.segments; card.querySelector('.transcribe-lines').replaceChildren();
          card.segments.forEach(s => showLine(card, s));
          card.querySelector('.transcribe-export').hidden = !card.segments.length;
          status.textContent = card.segments.length ? l.done : l.empty;
          finish();
          try {
            const handle = await entry.directory.getFileHandle(entry.name + '.okuflow-transcript.json', {create:true});
            const writable = await handle.createWritable();
            await writable.write(JSON.stringify({version:1, recording:{name:entry.name,size:entry.file.size,lastModified:entry.file.lastModified}, model:data.model,device:data.device,language:data.language,segments:card.segments}));
            await writable.close(); status.textContent = l.saved;
          } catch (_) {status.textContent = l.saveFailed;}
        }
      };
      receiveReady = event => {
        if (event.source !== popup || event.origin !== toolOrigin || event.data?.type !== 'okuflow-transcribe-ready' || event.data.version !== 1) return;
        window.removeEventListener('message', receiveReady); clearTimeout(timeout);
        popup.postMessage({type:'okuflow-transcribe-request', version:1, file:entry.file, title:document.title, language:lang}, toolOrigin, [channel.port2]);
      };
      window.addEventListener('message', receiveReady);
      const ping = setInterval(() => {
        if (popup.closed) {status.textContent = l.connection; window.removeEventListener('message', receiveReady); finish();}
        else popup.postMessage({type:'okuflow-transcribe-ping'}, toolOrigin);
      }, 500);
      const timeout = setTimeout(() => {status.textContent = l.connection; window.removeEventListener('message', receiveReady); finish();}, 30000);
      popup.postMessage({type:'okuflow-transcribe-ping'}, toolOrigin);
    } catch (error) {
      button.disabled = false;
      if (error?.name !== 'AbortError') {root = null; status.textContent = l.missing;}
    }
  }
  document.addEventListener('DOMContentLoaded', async () => {
    const cards = [...document.querySelectorAll('[data-transcribe-src]')];
    for (const card of cards) {
      card.segments = [];
      card.querySelector('.transcribe-start').textContent = l.go;
      card.querySelector('.transcribe-stop').textContent = l.stop;
      card.querySelector('.transcribe-export').textContent = l.text;
      card.querySelector('.transcribe-heading').textContent = l.title;
      card.querySelector('.transcribe-start').addEventListener('click', () => begin(card));
      card.querySelector('.transcribe-export').addEventListener('click', () => saveText(card));
    }
    await recall();
    await Promise.all(cards.map(loadSaved));
  });
})();
</script>
)JS";

} // namespace okuflow
