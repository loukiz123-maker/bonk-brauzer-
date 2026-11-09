'use strict';

// Real Chromium against the unchanged preview files, with isolated browser
// contexts and a controllable version endpoint. No Android device is involved.
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.resolve(__dirname, '../../web-preview');
const allowed = new Set(['index.html', 'app.js', 'style.css', 'desktop-bg.jpg', 'launcher-bg.jpg']);
let server, browser, url, version = 'first';

before(async () => {
  server = http.createServer((request, response) => {
    const name = request.url === '/' ? 'index.html' : request.url.slice(1);
    if (name === '__version') { response.end(version); return; }
    if (!allowed.has(name)) { response.writeHead(404).end(); return; }
    const types = {'.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.jpg': 'image/jpeg'};
    response.setHeader('Content-Type', types[path.extname(name)]);
    fs.createReadStream(path.join(root, name)).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  url = `http://127.0.0.1:${server.address().port}/`;
  browser = await chromium.launch({headless: true, ...(process.env.CHROMIUM_PATH ? {executablePath: process.env.CHROMIUM_PATH} : {}), args: ['--no-sandbox']});
});
after(async () => { if (browser) await browser.close(); if (server) await new Promise(resolve => server.close(resolve)); });

async function freshPages(count = 1) {
  const context = await browser.newContext({viewport: {width: 1400, height: 900}});
  const pages = await Promise.all(Array.from({length: count}, () => context.newPage()));
  await Promise.all(pages.map(page => page.goto(url)));
  return {context, pages};
}
const pagesSnapshot = pages => pages.map(page => page.evaluate(() => ({stored: localStorage.getItem('lynxdroid-preview-library-v1'), message: document.querySelector('#message').textContent, note: document.querySelector('#library-note').textContent, locks: !!navigator.locks, changes: window.debugChanges, log: window.debugLog})));
const apk = name => ({name, mimeType: 'application/vnd.android.package-archive', buffer: Buffer.from([0x50, 0x4b, 3, 4, 0])});
async function addMarker(page, x = 200) {
  await page.locator('#canvas').click({position: {x, y: 150}});
  await page.waitForSelector('#canvas .marker');
}

test('concurrent APK additions in two tabs do not overwrite one another', async () => {
  const {context, pages: [first, second]} = await freshPages(2);
  try {
    const errors = [];
    for (const page of [first, second]) page.on('pageerror', error => errors.push(error.message));
    for (const page of [first, second]) await page.evaluate(() => {
      window.debugChanges = []; window.debugLog = [];
      document.querySelector('#file').addEventListener('change', event => window.debugChanges.push([...event.target.files].map(f => f.name)), true);
      const read = Blob.prototype.arrayBuffer;
      Blob.prototype.arrayBuffer = async function() { window.debugLog.push('read-start'); try { const bytes = await read.call(this); window.debugLog.push('read-done'); return bytes; } catch (error) { window.debugLog.push(`read-error:${error.message}`); throw error; } };
      const request = navigator.locks.request.bind(navigator.locks);
      navigator.locks.request = (name, callback) => { window.debugLog.push(`request:${name}`); return request(name, (...args) => { window.debugLog.push(`acquire:${name}`); const result = callback(...args); window.debugLog.push(`done:${name}`); return result; }); };
    });
    // Hold the shared lock while both tabs start an import, then release it.
    // Sequential file-selection automation avoids competing native file
    // choosers while keeping both storage updates concurrent.
    await first.evaluate(() => {
      navigator.locks.request('lynxdroid:lynxdroid-preview-library-v1', () => new Promise(resolve => { window.releaseTestLock = resolve; window.testLockHeld = true; }));
    });
    await first.waitForFunction(() => window.testLockHeld);
    await first.locator('#file').setInputFiles(apk('Alpha.apk'));
    await second.locator('#file').setInputFiles(apk('Beta.apk'));
    await first.evaluate(() => window.releaseTestLock());
    try {
      await first.waitForFunction(() => JSON.parse(localStorage.getItem('lynxdroid-preview-library-v1') || '[]').length === 2, null, {timeout: 8000});
    } catch (error) {
      const state = await Promise.all(pagesSnapshot([first, second]));
      throw new Error(`Library did not contain both files: ${JSON.stringify(state)}; errors: ${JSON.stringify(errors)}; ${error.message}`);
    }
    await second.waitForFunction(() => document.querySelectorAll('#list .list-item').length === 2);
    const items = await first.evaluate(() => JSON.parse(localStorage.getItem('lynxdroid-preview-library-v1')));
    assert.deepEqual(items.sort(), ['Alpha.apk', 'Beta.apk']);
  } finally { await context.close(); }
});

test('two tabs cannot silently overwrite the same saved profile', async () => {
  const {context, pages: [first, second]} = await freshPages(2);
  try {
    await Promise.all([first.click('#open-controls'), second.click('#open-controls')]);
    await addMarker(first, 180); await addMarker(second, 300);
    await first.click('#save-controls');
    await first.waitForSelector('#editor-status:text("Раскладка сохранена")');
    const saved = await first.evaluate(() => localStorage.getItem('lynxdroid-preview-controls-v1'));
    await second.click('#save-controls');
    await second.waitForSelector('#editor-status:text("изменена в другой вкладке")');
    assert.equal(await second.evaluate(() => localStorage.getItem('lynxdroid-preview-controls-v1')), saved);
    assert.equal(await second.locator('#canvas .marker').count(), 1);
    const before = await second.locator('#canvas .marker').evaluate(el => el.style.left);
    second.once('dialog', dialog => dialog.dismiss());
    await second.click('#reload-profile');
    assert.equal(await second.locator('#canvas .marker').evaluate(el => el.style.left), before);
    second.once('dialog', dialog => dialog.accept());
    await second.click('#reload-profile');
    const firstPosition = await first.locator('#canvas .marker').evaluate(el => el.style.left);
    assert.equal(await second.locator('#canvas .marker').evaluate(el => el.style.left), firstPosition);
    await second.click('#save-controls');
    await second.waitForSelector('#editor-status:text("Раскладка сохранена")');
  } finally { await context.close(); }
});

test('automatic reload waits for a dirty draft and resumes after save', async () => {
  version = 'before';
  const {context, pages: [page]} = await freshPages();
  try {
    await page.click('#open-controls'); await addMarker(page);
    await page.waitForTimeout(1800); // allow the initial version request
    version = 'after';
    await page.waitForSelector('#editor-status:text("отложено")', {timeout: 7000});
    assert.equal(await page.locator('#canvas .marker').count(), 1);
    const reload = page.waitForNavigation({waitUntil: 'load', timeout: 8000});
    await page.click('#save-controls');
    await reload;
    await page.click('#open-controls');
    assert.equal(await page.locator('#canvas .marker').count(), 1);
  } finally { await context.close(); }
});

test('shifted release clears the physical key and clicking stop does not restart', async () => {
  const {context, pages: [page]} = await freshPages();
  try {
    await page.click('#open-controls');
    await page.selectOption('#control-kind', {label: 'Огонь'});
    await page.locator('#control-key').fill('1'); await addMarker(page);
    await page.click('#live-test');
    await page.locator('#canvas').evaluate(el => el.dispatchEvent(new KeyboardEvent('keydown', {key:'1', code:'Digit1', bubbles:true})));
    assert.equal(await page.locator('#canvas .marker.active').count(), 1);
    await page.locator('#canvas').evaluate(el => el.dispatchEvent(new KeyboardEvent('keyup', {key:'!', code:'Digit1', shiftKey:true, bubbles:true})));
    assert.equal(await page.locator('#canvas .marker.active').count(), 0);
    await page.locator('#canvas').evaluate(el => el.dispatchEvent(new KeyboardEvent('keydown', {key:'1', code:'Digit1', bubbles:true})));
    await page.click('#live-test');
    assert.equal(await page.locator('#live-test').textContent(), 'Тест клавиш');
    assert.equal(await page.locator('#canvas .marker.active').count(), 0);
  } finally { await context.close(); }
});

test('layout import is validated, undoable and export does not save it', async () => {
  const {context, pages: [page]} = await freshPages();
  try {
    await page.click('#open-controls');
    const imported = {schema: 1, controls: [{kind: 'Огонь', key: 'G', x: 0.4, y: 0.6}]};
    const jsonFile = data => ({name: 'layout.json', mimeType: 'application/json', buffer: data});
    await page.locator('#import-file').setInputFiles(jsonFile(Buffer.from(JSON.stringify(imported))));
    await page.waitForSelector('#editor-status:text("Раскладка импортирована")');
    assert.equal(await page.locator('#canvas .marker').textContent(), 'G');
    assert.equal(await page.evaluate(() => localStorage.getItem('lynxdroid-preview-controls-v1')), null);

    const downloadEvent = page.waitForEvent('download');
    await page.click('#export-controls');
    const download = await downloadEvent;
    assert.equal(download.suggestedFilename(), 'lynxdroid-layout.json');
    const stream = await download.createReadStream();
    const chunks = [];
    for await (const chunk of stream) chunks.push(chunk);
    assert.deepEqual(JSON.parse(Buffer.concat(chunks).toString('utf8')), imported);
    assert.equal(await page.evaluate(() => localStorage.getItem('lynxdroid-preview-controls-v1')), null);

    await page.locator('#import-file').setInputFiles(jsonFile(Buffer.from([0xff])));
    await page.waitForSelector('#message:text("Не удалось импортировать")');
    assert.equal(await page.locator('#canvas .marker').textContent(), 'G');
    await page.click('#undo-control');
    assert.equal(await page.locator('#canvas .marker').count(), 0);
  } finally { await context.close(); }
});

test('corrupt profile remains read-only and is never overwritten', async () => {
  const {context, pages: [page]} = await freshPages();
  try {
    await page.evaluate(() => localStorage.setItem('lynxdroid-preview-controls-v1', '{broken'));
    await page.click('#open-controls');
    assert.equal(await page.locator('#save-controls').isDisabled(), true);
    assert.equal(await page.locator('#import-controls').isDisabled(), true);
    await addMarker(page);
    assert.equal(await page.evaluate(() => localStorage.getItem('lynxdroid-preview-controls-v1')), '{broken');
    await page.evaluate(() => localStorage.setItem('lynxdroid-preview-controls-v1', '{"schema":1,"controls":[]}'));
    page.once('dialog', dialog => dialog.accept());
    await page.click('#reload-profile');
    assert.equal(await page.locator('#canvas .marker').count(), 0);
    assert.equal(await page.locator('#save-controls').isEnabled(), true);
  } finally { await context.close(); }
});

test('mouse binding capture and held fire work locally', async () => {
  const {context, pages: [page]} = await freshPages();
  try {
    await page.click('#open-controls');
    await page.selectOption('#control-kind', {label: 'Огонь'});
    await page.click('#capture-key');
    await page.click('#capture-key');
    assert.equal(await page.locator('#control-key').inputValue(), 'Mouse1');
    assert.equal(await page.locator('#capture-key').textContent(), 'Назначить нажатием');
    await addMarker(page);
    assert.equal(await page.locator('#canvas .marker').textContent(), 'ЛКМ');
    await page.click('#live-test');
    await page.locator('#canvas').scrollIntoViewIfNeeded();
    const canvas = await page.locator('#canvas').boundingBox();
    await page.mouse.move(canvas.x + 90, canvas.y + 90);
    await page.mouse.down();
    assert.equal(await page.locator('#canvas .marker.active').count(), 1);
    await page.mouse.up();
    assert.equal(await page.locator('#canvas .marker.active').count(), 0);
    await page.click('#live-test');
    assert.equal(await page.locator('#live-test').textContent(), 'Тест клавиш');
  } finally { await context.close(); }
});


test('changing a binding stops the old live test before applying the edit', async () => {
  const {context, pages: [page]} = await freshPages();
  try {
    await page.click('#open-controls');
    await page.selectOption('#control-kind', {label: 'Огонь'});
    await addMarker(page);
    await page.click('#live-test');
    await page.keyboard.down('f');
    assert.equal(await page.locator('#canvas .marker.active').count(), 1);
    await page.locator('#control-key').fill('G');
    await page.click('#apply-control');
    await page.keyboard.up('f');
    assert.equal(await page.locator('#live-test').textContent(), 'Тест клавиш');
    assert.equal(await page.locator('#canvas .marker.active').count(), 0);
    assert.equal(await page.locator('#canvas .marker').textContent(), 'G');
    await page.click('#live-test');
    await page.keyboard.down('f');
    assert.equal(await page.locator('#canvas .marker.active').count(), 0);
    await page.keyboard.up('f');
    await page.keyboard.down('g');
    assert.equal(await page.locator('#canvas .marker.active').count(), 1);
    await page.keyboard.up('g');
  } finally { await context.close(); }
});
