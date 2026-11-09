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
const pagesSnapshot = pages => pages.map(page => page.evaluate(() => ({stored: localStorage.getItem('lynxdroid-preview-library-v1'), message: document.querySelector('#message').textContent, note: document.querySelector('#library-note').textContent, locks: !!navigator.locks})));
const apk = name => ({name, mimeType: 'application/vnd.android.package-archive', buffer: Buffer.from([0x50, 0x4b, 3, 4, 0])});
async function addMarker(page, x = 200) {
  await page.locator('#canvas').click({position: {x, y: 150}});
  await page.waitForSelector('#canvas .marker');
}

test('concurrent APK additions in two tabs do not overwrite one another', async () => {
  const {context, pages: [first, second]} = await freshPages(2);
  try {
    await Promise.all([first.locator('#file').setInputFiles(apk('Alpha.apk')), second.locator('#file').setInputFiles(apk('Beta.apk'))]);
    try {
      await first.waitForFunction(() => JSON.parse(localStorage.getItem('lynxdroid-preview-library-v1') || '[]').length === 2, null, {timeout: 8000});
    } catch (error) {
      const state = await Promise.all(pagesSnapshot([first, second]));
      throw new Error(`Library did not contain both files: ${JSON.stringify(state)}; ${error.message}`);
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
