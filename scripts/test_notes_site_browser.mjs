// Opt-in visual/interaction checks. Uses a private Chrome profile and generated
// notes only. No camera, microphone or AI requests; fixture media stays local.
import assert from 'node:assert/strict';
import {readFile, stat, mkdir} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, join, relative, extname} from 'node:path';
import {pathToFileURL} from 'node:url';

const {default: puppeteer} = await import(process.env.OKUFLOW_PUPPETEER_MODULE);
const website = resolve(process.env.OKUFLOW_TEST_WEBSITE);
const notesPath = resolve(process.env.OKUFLOW_TEST_NOTES);
const evidence = resolve(process.env.OKUFLOW_TEST_EVIDENCE);
const profile = process.env.OKUFLOW_TEST_PROFILE;
assert(profile && process.env.OKUFLOW_TEST_CHROME, 'A private Chrome profile and executable are required');
await mkdir(evidence, {recursive:true});
const policies = [];
for (const line of (await readFile(join(website, '_headers'), 'utf8')).split(/\r?\n/)) {
  if (!line.trim() || line.startsWith('#')) continue;
  if (!/^\s/.test(line)) policies.push({pattern:line.trim(), headers:{}});
  else { const split = line.indexOf(':'); policies.at(-1).headers[line.slice(0, split).trim()] = line.slice(split + 1).trim(); }
}
const mime = {'.html':'text/html', '.js':'text/javascript', '.mjs':'text/javascript', '.css':'text/css', '.svg':'image/svg+xml', '.webp':'image/webp', '.ico':'image/x-icon'};
const server = createServer(async (request, response) => {
  try {
    const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    let path = resolve(website, '.' + pathname);
    assert(!relative(website, path).startsWith('..'));
    if ((await stat(path)).isDirectory()) path = join(path, 'index.html');
    const headers = {'Content-Type':mime[extname(path)] || 'application/octet-stream'};
    for (const policy of policies) if (policy.pattern.endsWith('*') ? pathname.startsWith(policy.pattern.slice(0,-1)) : pathname === policy.pattern) Object.assign(headers, policy.headers);
    response.writeHead(200, headers).end(await readFile(path));
  } catch { response.writeHead(404).end(); }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const origin = 'http://127.0.0.1:' + server.address().port;
const errors = [];
let browser;
try {
  browser = await puppeteer.launch({executablePath:process.env.OKUFLOW_TEST_CHROME, userDataDir:profile, headless:true, args:['--no-first-run']});
  const page = await browser.newPage();
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => {if (/Content Security|Refused to/.test(message.text())) errors.push(message.text());});
  await page.emulateMediaFeatures([{name:'prefers-color-scheme', value:'light'}]);
  await page.setViewport({width:1440, height:1050});
  await page.goto(origin + '/?lang=en');
  await page.evaluate(() => localStorage.removeItem('okuflow-theme'));
  await page.reload();
  await page.waitForFunction(() => document.querySelectorAll('.carousel-dots button').length === 4);
  assert(await page.$eval('link[rel="icon"]', async link => {
    const image = new Image(); image.src = link.href; await image.decode();
    return image.naturalWidth > 0;
  }), 'The app favicon must decode in Chrome');
  const current = () => page.$$eval('.carousel-dots button', dots => dots.findIndex(e => e.getAttribute('aria-current') === 'true'));
  const initial = await current();
  await page.mouse.move(0, 1049);
  await page.waitForFunction(index => [...document.querySelectorAll('.carousel-dots button')].findIndex(e => e.getAttribute('aria-current') === 'true') !== index, {timeout:9000}, initial);
  await page.click('.carousel-toggle');
  const pausedAt = await current();
  await page.mouse.move(0, 1049);
  await new Promise(done => setTimeout(done, 6500));
  assert.equal(await current(), pausedAt, 'Pause must prevent automatic advancement');
  await page.click('.carousel-dots button:last-child');
  await page.waitForFunction(() => document.querySelector('.carousel-dots button:last-child').getAttribute('aria-current') === 'true');
  await page.waitForFunction(() => {
    const track = document.querySelector('.slides');
    return Math.abs(track.getBoundingClientRect().left - track.lastElementChild.getBoundingClientRect().left) < 4;
  });
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  await page.screenshot({path:join(evidence, 'website-hero.png')});
  await page.$eval('#roadmap', e => e.scrollIntoView());
  assert.equal(await page.$eval('.theme-toggle', e => e.getAttribute('aria-pressed')), 'false');
  await page.screenshot({path:join(evidence, 'website-roadmap-light.png')});
  await page.click('.theme-toggle');
  assert.equal(await page.$eval('.theme-toggle', e => e.getAttribute('aria-pressed')), 'true');
  await page.$eval('#roadmap', e => e.scrollIntoView());
  await page.screenshot({path:join(evidence, 'website-roadmap-dark.png')});
  console.log('PASS: auto-advance, Pause, slide selection, website themes and production CSP');

  await page.emulateMediaFeatures([{name:'prefers-reduced-motion', value:'reduce'}, {name:'prefers-color-scheme', value:'light'}]);
  await page.reload();
  assert.equal(await page.$eval('.carousel-toggle', e => e.getAttribute('aria-pressed')), 'true');
  const reducedAt = await current();
  await new Promise(done => setTimeout(done, 6500));
  assert.equal(await current(), reducedAt, 'Reduced motion must prevent automatic advancement');
  for (const locale of ['tr','de','ar','ja']) {
    await page.setViewport({width:320,height:900});
    await page.goto(origin + '/' + locale + '/');
    await page.waitForFunction(() => document.querySelectorAll('.carousel-dots button').length === 4);
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), locale + ' overflow');
    await page.click('.carousel-dots button:nth-child(2)');
    await page.waitForFunction(() => {
      const track = document.querySelector('.slides'); const slide = track.querySelectorAll('.slide')[1];
      return Math.abs(track.getBoundingClientRect().left - slide.getBoundingClientRect().left) < 4;
    });
    assert.equal(await current(), 1, locale + ' slide alignment');
    await page.focus('.slides');
    await page.keyboard.press(locale === 'ar' ? 'ArrowLeft' : 'ArrowRight');
    await page.waitForFunction(() => document.querySelector('.carousel-dots button:nth-child(3)').getAttribute('aria-current') === 'true');
    assert.equal(await current(), 2, locale + ' keyboard direction');
    await page.$eval('#roadmap', e => e.scrollIntoView());
    await page.screenshot({path:join(evidence, 'website-roadmap-' + locale + '-320.png')});
  }
  console.log('PASS: reduced motion, RTL slide navigation and 320px translated layouts');
  await page.goto(origin + '/transcribe/');
  await page.$eval('.brand img', image => image.decode());
  await page.waitForFunction(async () => {
    await navigator.serviceWorker.ready;
    const cache = await caches.open('okuflow-transcribe-app-v2');
    return (await cache.keys()).some(request => /\/assets\/okuflow-icon-[a-f0-9]{12}\.ico$/.test(request.url));
  });
  console.log('PASS: actual app favicon and transcription static-cache icon');
  await page.close();

  const notes = await browser.newPage();
  notes.on('pageerror', error => errors.push('Notes: ' + error.message));
  await notes.setViewport({width:1440,height:1050});
  await notes.goto(pathToFileURL(notesPath).href);
  await notes.evaluate(() => {
    localStorage.removeItem('okuflow-notes-scale');
    localStorage.removeItem('okuflow-notes-theme');
  });
  await notes.reload();
  await notes.waitForFunction(() => document.querySelectorAll('.filters button').length > 2);
  await notes.click('#btn-expand');
  // Collapsed/off-screen lazy images do not start loading merely because
  // decode() was called. Expand and visit each real fixture first.
  for (const image of await notes.$$('img')) {
    await image.evaluate(element => element.scrollIntoView());
    await notes.waitForFunction(element => element.complete && element.naturalWidth > 0, {}, image);
  }
  await notes.$$eval('img', images => Promise.all(images.map(image => image.decode())));
  await notes.waitForFunction(() => [...document.querySelectorAll('video')].every(video => video.readyState >= 1));
  assert.equal(await notes.$$eval('video', videos => videos.every(video => video.readyState >= 1)), true, 'Fixture recordings must load');
  assert.equal(await notes.$$eval('details.convo', groups => groups.length), 1, 'The two sample Assistant turns must share one group');
  await notes.evaluate(() => scrollTo(0,0));
  await notes.screenshot({path:join(evidence, 'notes-desktop.png')});
  await notes.click('.filters button[data-kind="ai"]');
  assert(await notes.$$eval('main > details.note', cards => cards.every(card => card.hidden || card.dataset.kind === 'ai')));
  await notes.click('.filters button[data-kind="all"]');
  await notes.click('#btn-collapse');
  assert(await notes.$$eval('details', cards => cards.every(card => !card.open)));
  await notes.click('#btn-expand');
  assert(await notes.$$eval('details', cards => cards.every(card => card.open)));
  await notes.click('#btn-larger');
  await notes.reload();
  assert.equal(await notes.$eval('#size-value', e => e.textContent), '113%', 'Text size must survive reload');
  await notes.setViewport({width:320,height:900});
  assert(await notes.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Notes must fit at 320px');
  await notes.screenshot({path:join(evidence, 'notes-narrow.png'),fullPage:true});
  while (await notes.$eval('html', e => e.dataset.theme) !== 'contrast') await notes.click('#btn-theme');
  await notes.click('#btn-expand');
  for (let count=0; count<7; count++) await notes.click('#btn-larger');
  assert(await notes.evaluate(() => document.documentElement.scrollWidth <= innerWidth), '200% text must fit at 320px');
  assert(await notes.$$eval('button', buttons => buttons.filter(b => b.getBoundingClientRect().height).every(b => b.getBoundingClientRect().height >= 43)), 'Visible controls must retain touch target height');
  await notes.screenshot({path:join(evidence, 'notes-contrast-200.png'),fullPage:true});
  await notes.evaluate(() => scrollTo({top:0,behavior:'instant'}));
  await notes.screenshot({path:join(evidence, 'notes-contrast-controls.png')});
  for (const locale of ['tr','de']) {
    await notes.goto(pathToFileURL(notesPath.replace('_en.html', '_' + locale + '.html')).href);
    await notes.waitForFunction(() => document.querySelectorAll('.filters button').length > 2);
    await notes.click('#btn-expand');
    for (let count=0; count<8; count++) await notes.click('#btn-larger');
    assert(await notes.evaluate(() => document.documentElement.scrollWidth <= innerWidth), locale + ' notes at 200%');
    await notes.screenshot({path:join(evidence, 'notes-' + locale + '-200.png'),fullPage:true});
  }
  assert.deepEqual(errors, []);
  console.log('PASS: local notes media, filters, grouping, expand/collapse, persisted text size, contrast and 200% text at 320px');
} catch (error) {
  for (const page of await browser?.pages() || []) {
    console.log('Failure page:', page.url(), await page.evaluate(() => [...document.querySelectorAll('body *')]
      .filter(e => e.checkVisibility() && !e.closest('.slides') && e.getBoundingClientRect().right > innerWidth + 1)
      .slice(0, 18).map(e => ({tag:e.tagName, css:e.className, width:e.getBoundingClientRect().width, text:e.textContent.slice(0,70)}))));
    if (page.url() !== 'about:blank') await page.screenshot({path:join(evidence, 'failure.png'),fullPage:true});
  }
  throw error;
} finally {
  await browser?.close();
  await new Promise(done => server.close(done));
}
