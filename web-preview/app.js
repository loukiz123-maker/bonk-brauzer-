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
  if (!$('#editor').hidden && name !== 'editor' && !canDiscard()) return false;
  document.querySelectorAll('.page').forEach((el) => { el.hidden = el.id !== name; });
  document.querySelectorAll('.nav').forEach((el) => { el.classList.toggle('active', el.dataset.page === name); });
  return true;
}
function render() {
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
document.addEventListener('fullscreenchange', () => {
  const title = document.fullscreenElement ? 'Выйти из полного экрана (Esc)' : 'Полный экран (F11)';
  $('#fullscreen').textContent = document.fullscreenElement ? '❐' : '⛶';
  $('#fullscreen').title = title; $('#fullscreen').setAttribute('aria-label', title);
});
document.addEventListener('keydown', (event) => { if (event.key === 'F11') { event.preventDefault(); fullscreen(); } });
render();
// Draft layouts are stored only in this browser, never applied to Android.
const controlStorageKey = 'lynxdroid-preview-controls-v1';
const kinds = ['Нажатие', 'Двойное нажатие', 'Мультитач', 'Джойстик', 'Камера', 'Перетаскивание', 'Огонь'];
let controls = [];
let controlsValid = true;
let activeProfileKey = controlStorageKey;
let activeProfileIndex = 0;
let controlsBaseline = '[]';
let selectedControl = null;
let dragging = false;
let liveTest = false;
const liveHeld = new Set();
const livePressed = new Set();
function readControls(key = activeProfileKey) {
  const data = JSON.parse(localStorage.getItem(key) || '{"schema":1,"controls":[]}');
  if (data.schema !== 1 || !Array.isArray(data.controls) || data.controls.length > 100 || !data.controls.every(c => c && kinds.includes(c.kind) && typeof c.key === 'string' && c.key.length >= 1 && c.key.length <= 16 && ['x','y'].every(k => typeof c[k] === 'number' && c[k] >= 0 && c[k] <= 1))) throw new Error('invalid');
  return data.controls;
}
function renderControls() {
  const canvas = $('#canvas'); canvas.querySelectorAll('.marker').forEach(m => m.remove());
  controls.forEach((c, index) => { const marker = document.createElement('div'); marker.className = 'marker'; marker.classList.toggle('selected', index === selectedControl); marker.classList.toggle('active', liveHeld.has(c.key.toLowerCase())); marker.textContent = c.key; marker.title = c.kind; marker.style.left = `${c.x * 100}%`; marker.style.top = `${c.y * 100}%`; canvas.append(marker); });
  $('#apply-control').disabled = selectedControl === null;
  $('#delete-control').disabled = selectedControl === null;
}
$('#open-settings').addEventListener('click', () => $('#settings-dialog').showModal());
function canDiscard() {
  return JSON.stringify(controls) === controlsBaseline || confirm('Отменить несохранённые изменения раскладки?');
}
function loadProfile(key) {
  endLiveTest();
  activeProfileKey = key;
  try { controls = readControls(key); controlsValid = true; $('#editor-status').textContent = `Элементов: ${controls.length}`; }
  catch { controls = []; controlsValid = false; $('#editor-status').textContent = 'Раскладка повреждена или недоступна. Сохранение отключено.'; }
  controlsBaseline = JSON.stringify(controls);
  selectedControl = null; dragging = false; $('#save-controls').disabled = !controlsValid; renderControls();
}
$('#open-controls').addEventListener('click', () => {
  if (!$('#editor').hidden) return;
  const select = $('#control-profile'); select.replaceChildren();
  const general = document.createElement('option'); general.textContent = 'Общий шаблон (старый)'; general.value = controlStorageKey; select.append(general);
  entries.forEach(name => { const option = document.createElement('option'); option.value = `lynxdroid-preview-game-v1:${name}`; option.textContent = name; select.append(option); });
  activeProfileIndex = entries.length ? (selected !== null ? selected + 1 : 1) : 0;
  select.selectedIndex = activeProfileIndex; loadProfile(select.value); page('editor');
});
$('#control-profile').addEventListener('change', event => {
  const select = event.target;
  if (!canDiscard()) { select.selectedIndex = activeProfileIndex; return; }
  activeProfileIndex = select.selectedIndex; loadProfile(select.value);
});
$('#close-editor').addEventListener('click', () => page('home'));
document.querySelectorAll('[data-close]').forEach(button => button.addEventListener('click', () => document.getElementById(button.dataset.close).close()));
function selectControl(index) {
  selectedControl = index;
  if (index !== null) { $('#control-kind').value = controls[index].kind; $('#control-key').value = controls[index].key; }
  renderControls();
}
function canvasPosition(event) {
  const rect = $('#canvas').getBoundingClientRect();
  return {x: Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), y: Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height))};
}
$('#canvas').addEventListener('pointerdown', event => {
  if (event.button !== 0) return;
  if (liveTest) { $('#canvas').focus(); return; }
  const rect = $('#canvas').getBoundingClientRect();
  const x = event.clientX - rect.left, y = event.clientY - rect.top;
  let hit = null;
  for (let i = controls.length - 1; i >= 0; i--) {
    if (Math.hypot(x - controls[i].x * rect.width, y - controls[i].y * rect.height) <= 22) { hit = i; break; }
  }
  if (hit !== null) { selectControl(hit); dragging = true; $('#canvas').setPointerCapture(event.pointerId); return; }
  const key = $('#control-key').value.trim();
  if (!key || controls.length >= 100) { $('#editor-status').textContent = 'Укажите клавишу; максимум 100 элементов.'; return; }
  controls.push({kind: $('#control-kind').value, key, ...canvasPosition(event)});
  selectControl(controls.length - 1); $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`;
});
$('#canvas').addEventListener('pointermove', event => {
  if (!dragging || selectedControl === null) return;
  Object.assign(controls[selectedControl], canvasPosition(event));
  $('#editor-status').textContent = 'Элемент перемещён • не сохранено'; renderControls();
});
$('#canvas').addEventListener('pointerup', () => { dragging = false; });
$('#canvas').addEventListener('pointercancel', () => { dragging = false; });
$('#apply-control').addEventListener('click', () => {
  if (selectedControl === null) return;
  const key = $('#control-key').value.trim();
  if (!key) { $('#editor-status').textContent = 'Укажите клавишу'; return; }
  Object.assign(controls[selectedControl], {kind: $('#control-kind').value, key});
  $('#editor-status').textContent = 'Элемент изменён • не сохранено'; renderControls();
});
$('#delete-control').addEventListener('click', () => {
  if (selectedControl === null) return;
  controls.splice(selectedControl, 1); selectControl(null);
  $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`;
});
function endLiveTest() {
  if (!liveTest) return;
  liveTest = false; liveHeld.clear(); livePressed.clear();
  $('#live-test').textContent = 'Тест клавиш';
  $('#editor-status').textContent = 'Тест остановлен. Ничего не отправлено в Android.';
  renderControls();
}
$('#live-test').addEventListener('click', () => {
  if (liveTest) { endLiveTest(); return; }
  if (controls.some(c => !['Нажатие', 'Огонь'].includes(c.kind))) { $('#editor-status').textContent = 'Проверка невозможна: пока поддерживаются только нажатие и огонь.'; return; }
  const keys = controls.map(c => c.key.toLowerCase());
  if (keys.some(k => !/^[a-z0-9]$/.test(k)) || new Set(keys).size !== keys.length) { $('#editor-status').textContent = 'Укажите разные латинские буквы или цифры для каждой кнопки.'; return; }
  liveTest = true; $('#live-test').textContent = 'Остановить тест'; $('#canvas').focus();
  $('#editor-status').textContent = 'Тест без Android: нажмите клавишу на поле. Esc — выход.';
});
$('#canvas').addEventListener('keydown', event => {
  if (!liveTest) return;
  if (event.key === 'Escape') { event.preventDefault(); endLiveTest(); return; }
  const key = event.key.toLowerCase();
  if (!/^[a-z0-9]$/.test(key) || event.repeat || livePressed.has(key)) return;
  const control = controls.find(c => c.key.toLowerCase() === key);
  if (!control) return;
  event.preventDefault(); livePressed.add(key);
  if (control.kind === 'Огонь') liveHeld.add(key);
  $('#editor-status').textContent = `Тест без Android: ${key.toUpperCase()} • событий: ${control.kind === 'Огонь' ? 1 : 2} • удерживается: ${liveHeld.size}`; renderControls();
});
$('#canvas').addEventListener('keyup', event => {
  if (!liveTest) return;
  const key = event.key.toLowerCase();
  if (!livePressed.has(key)) return;
  livePressed.delete(key);
  if (liveHeld.delete(key)) $('#editor-status').textContent = `Тест без Android: ${key.toUpperCase()} • событий: 1 • удерживается: ${liveHeld.size}`;
  renderControls(); event.preventDefault();
});
$('#canvas').addEventListener('blur', endLiveTest);
$('#test-mapping').addEventListener('click', () => {
  const status = $('#editor-status');
  if (controls.some(c => !['Нажатие', 'Огонь'].includes(c.kind))) { status.textContent = 'Проверка невозможна: пока поддерживаются только нажатие и огонь.'; return; }
  if (controls.some(c => !/^[a-z0-9]$/i.test(c.key)) || !/^[a-z0-9]$/i.test($('#test-key').value.trim())) { status.textContent = 'Для проверки используйте одну латинскую букву или цифру.'; return; }
  const keys = controls.map(c => c.key.toLowerCase());
  if (new Set(keys).size !== keys.length) { status.textContent = 'Проверка невозможна: клавиша назначена несколько раз.'; return; }
  const control = controls.find(c => c.key.toLowerCase() === $('#test-key').value.trim().toLowerCase());
  if (!control) { status.textContent = 'Демо без Android: клавиша не назначена. Ничего не отправлено.'; return; }
  const x = Math.min(1919, Math.floor(control.x * 1920));
  const y = Math.min(1079, Math.floor(control.y * 1080));
  status.textContent = `Демо без Android: 2 события касания, позиция ${x} × ${y}. Ничего не отправлено.`;
});
$('#undo-control').addEventListener('click', () => { if (controls.length) controls.pop(); selectControl(null); $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`; renderControls(); });
$('#reset-controls').addEventListener('click', () => { controls = []; selectControl(null); $('#editor-status').textContent = 'Шаблон очищен • не сохранено'; renderControls(); });
$('#save-controls').addEventListener('click', () => {
  if (!controlsValid) return;
  try { readControls(); localStorage.setItem(activeProfileKey, JSON.stringify({schema:1, controls})); controlsBaseline = JSON.stringify(controls); $('#editor-status').textContent = `Сохранено элементов: ${controls.length}`; }
  catch { $('#editor-status').textContent = 'Ошибка сохранения. Исходные данные не изменены.'; }
});
// The local preview server reports a fingerprint; reload on source edits.
let version;
async function checkVersion() {
  try { const response = await fetch('/__version', {cache:'no-store'}); if (!response.ok) return; const next = await response.text(); if (version && version !== next) location.reload(); version = next; } catch { /* Static hosting does not support live reload. */ }
}
checkVersion(); setInterval(checkVersion, 1500);
