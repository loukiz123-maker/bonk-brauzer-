'use strict';
const $ = (selector) => document.querySelector(selector);
const storageKey = 'lynxdroid-preview-library-v1';
let entries = [];
let selected = null;
let storageAvailable = true;
try {
  const saved = JSON.parse(localStorage.getItem(storageKey) || '[]');
  if (!Array.isArray(saved) || !saved.every((name) => typeof name === 'string')) throw new Error('invalid data');
  entries = saved;
} catch {
  storageAvailable = false;
}
function message(text) {
  const box = $('#message');
  box.textContent = text;
  box.classList.add('visible');
  clearTimeout(message.timer);
  message.timer = setTimeout(() => box.classList.remove('visible'), 5000);
}
function persist(next) {
  if (!storageAvailable) { message('Библиотека браузера недоступна или повреждена. Данные не изменены.'); return false; }
  try { localStorage.setItem(storageKey, JSON.stringify(next)); entries = next; return true; }
  catch { message('Не удалось сохранить библиотеку в браузере.'); return false; }
}
function page(name) {
  document.querySelectorAll('.page').forEach((el) => { el.hidden = el.id !== name; });
  document.querySelectorAll('.nav').forEach((el) => { el.classList.toggle('active', el.dataset.page === name); });
}
function render() {
  $('#count').textContent = `APK в библиотеке: ${entries.length}`;
  const list = $('#list'); list.replaceChildren();
  const query = $('#search').value.trim().toLocaleLowerCase();
  entries.forEach((name, index) => {
    if (!name.replace(/\.apk$/i, '').toLocaleLowerCase().includes(query)) return;
    const item = document.createElement('button'); item.className = 'list-item';
    item.classList.toggle('selected', index === selected); item.setAttribute('role', 'option');
    item.setAttribute('aria-selected', String(index === selected));
    item.textContent = name.replace(/\.apk$/i, '');
    const detail = document.createElement('span'); detail.className = 'item-detail'; detail.textContent = `Название APK · ${name} (путь недоступен в браузере)`; item.append(detail);
    item.addEventListener('click', () => { selected = index; render(); }); list.append(item);
  });
  $('#remove').disabled = selected === null || !storageAvailable;
  $('#library-note').textContent = !storageAvailable ? 'Библиотека браузера недоступна или повреждена.' : entries.length ? 'Выберите APK для удаления из списка.' : 'Библиотека пуста — добавьте APK, чтобы сохранить его название.';
}
document.querySelectorAll('[data-page], [data-go]').forEach((button) => button.addEventListener('click', () => page(button.dataset.page || button.dataset.go)));
document.querySelectorAll('.add-apk').forEach((button) => button.addEventListener('click', () => $('#file').click()));
$('#file').addEventListener('change', async (event) => {
  const files = Array.from(event.target.files); event.target.value = '';
  if (!files.length) return;
  const next = [...entries];
  const failures = [];
  for (const file of files) {
    try {
      const bytes = new Uint8Array(await file.slice(0, 4).arrayBuffer());
      if (!/\.apk$/i.test(file.name) || bytes.length !== 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4b || bytes[2] !== 3 || bytes[3] !== 4) { failures.push(`${file.name}: неверный заголовок APK`); continue; }
      if (next.includes(file.name)) { failures.push(`${file.name}: название уже есть в библиотеке`); continue; }
      if (next.length >= 500) { failures.push(`${file.name}: лимит 500 файлов`); continue; }
      next.push(file.name);
    } catch { failures.push(`${file.name}: не удалось прочитать файл`); }
  }
  if (next.length !== entries.length && !persist(next)) return;
  selected = null; render(); page('library');
  if (failures.length) message(`Некоторые файлы не добавлены: ${failures.slice(0, 3).join('; ')}${failures.length > 3 ? `; ещё ${failures.length - 3}` : ''}`);
});
$('#search').addEventListener('input', () => { selected = null; render(); });
$('#refresh').addEventListener('click', () => { selected = null; render(); message('Предпросмотр обновлён. Браузер не проверяет наличие файлов.'); });
$('#remove').addEventListener('click', () => {
  if (selected !== null && confirm('Удалить название из библиотеки предпросмотра? Сам APK останется на диске.')) {
    const next = entries.filter((_, index) => index !== selected);
    if (persist(next)) { selected = null; render(); }
  }
});
async function fullscreen() {
  try { if (document.fullscreenElement) await document.exitFullscreen(); else await document.documentElement.requestFullscreen(); }
  catch { message('Полный экран недоступен в этом браузере или окне предпросмотра.'); }
}
$('#fullscreen').addEventListener('click', fullscreen);
document.addEventListener('fullscreenchange', () => { $('#fullscreen').textContent = document.fullscreenElement ? '⛶  Выйти из полного экрана' : '⛶  Полный экран'; });
document.addEventListener('keydown', (event) => { if (event.key === 'F11') { event.preventDefault(); fullscreen(); } });
render();
// Draft layouts are stored only in this browser, never applied to Android.
const controlStorageKey = 'lynxdroid-preview-controls-v1';
const kinds = ['Нажатие', 'Двойное нажатие', 'Мультитач', 'Джойстик', 'Камера', 'Перетаскивание', 'Огонь'];
let controls = [];
let controlsValid = true;
function readControls() {
  const data = JSON.parse(localStorage.getItem(controlStorageKey) || '{"schema":1,"controls":[]}');
  if (data.schema !== 1 || !Array.isArray(data.controls) || data.controls.length > 100 || !data.controls.every(c => c && kinds.includes(c.kind) && typeof c.key === 'string' && c.key.length >= 1 && c.key.length <= 16 && ['x','y'].every(k => typeof c[k] === 'number' && c[k] >= 0 && c[k] <= 1))) throw new Error('invalid');
  return data.controls;
}
function renderControls() {
  const canvas = $('#canvas'); canvas.querySelectorAll('.marker').forEach(m => m.remove());
  controls.forEach(c => { const marker = document.createElement('div'); marker.className = 'marker'; marker.textContent = c.key; marker.title = c.kind; marker.style.left = `${c.x * 100}%`; marker.style.top = `${c.y * 100}%`; canvas.append(marker); });
}
$('#open-settings').addEventListener('click', () => $('#settings-dialog').showModal());
$('#open-controls').addEventListener('click', () => {
  try { controls = readControls(); controlsValid = true; $('#editor-status').textContent = `Элементов: ${controls.length}`; }
  catch { controls = []; controlsValid = false; $('#editor-status').textContent = 'Библиотека шаблона повреждена или недоступна. Сохранение отключено.'; }
  $('#save-controls').disabled = !controlsValid; renderControls(); $('#editor-dialog').showModal();
});
document.querySelectorAll('[data-close]').forEach(button => button.addEventListener('click', () => document.getElementById(button.dataset.close).close()));
$('#canvas').addEventListener('click', event => {
  const key = $('#control-key').value.trim();
  if (!key || controls.length >= 100) { $('#editor-status').textContent = 'Укажите клавишу; максимум 100 элементов.'; return; }
  const rect = $('#canvas').getBoundingClientRect();
  controls.push({kind: $('#control-kind').value, key, x: Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), y: Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height))});
  $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`; renderControls();
});
$('#undo-control').addEventListener('click', () => { if (controls.length) controls.pop(); $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`; renderControls(); });
$('#reset-controls').addEventListener('click', () => { controls = []; $('#editor-status').textContent = 'Шаблон очищен • не сохранено'; renderControls(); });
$('#save-controls').addEventListener('click', () => {
  if (!controlsValid) return;
  try { readControls(); localStorage.setItem(controlStorageKey, JSON.stringify({schema:1, controls})); $('#editor-status').textContent = `Сохранено элементов: ${controls.length}`; }
  catch { $('#editor-status').textContent = 'Ошибка сохранения. Исходные данные не изменены.'; }
});
// The local preview server reports a fingerprint; reload on source edits.
let version;
async function checkVersion() {
  try { const response = await fetch('/__version', {cache:'no-store'}); if (!response.ok) return; const next = await response.text(); if (version && version !== next) location.reload(); version = next; } catch { /* Static hosting does not support live reload. */ }
}
checkVersion(); setInterval(checkVersion, 1500);
