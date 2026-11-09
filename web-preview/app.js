'use strict';
const $ = (selector) => document.querySelector(selector);
const storageKey = 'lynxdroid-preview-library-v1';
let entries = [];
let selected = null;
let storageAvailable = true;
try {
  const saved = JSON.parse(localStorage.getItem(storageKey) ?? '[]');
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
function validateLibrary(data) {
  if (!Array.isArray(data) || data.length > 500 || !data.every(name => typeof name === 'string') || new Set(data).size !== data.length) throw new Error('invalid library');
  return data;
}
function readLibrary() {
  return validateLibrary(JSON.parse(localStorage.getItem(storageKey) ?? '[]'));
}
async function withStorageLock(key, action) {
  if (!navigator.locks) throw new Error('Браузер не поддерживает защиту от конфликтов вкладок');
  return navigator.locks.request(`lynxdroid:${key}`, action);
}
// localStorage snapshots can be stale across renderer processes even after a
// Web Lock is acquired. IndexedDB is the authoritative saved snapshot;
// localStorage remains a compatibility mirror and cross-tab notification.
let libraryDatabase;
function openLibraryDatabase() {
  if (!libraryDatabase) libraryDatabase = new Promise((resolve, reject) => {
    const request = indexedDB.open('lynxdroid-preview-library-v1', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('state');
    request.onerror = () => reject(request.error || new Error('IndexedDB недоступна'));
    request.onblocked = () => reject(new Error('IndexedDB заблокирована другой вкладкой'));
    request.onsuccess = () => resolve(request.result);
  });
  return libraryDatabase;
}
async function readSnapshot(key) {
  const db = await openLibraryDatabase();
  return new Promise((resolve, reject) => {
    const request = db.transaction('state', 'readonly').objectStore('state').get(key);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error || new Error('Не удалось прочитать данные'));
  });
}
async function writeSnapshot(key, value) {
  const db = await openLibraryDatabase();
  await new Promise((resolve, reject) => {
    const tx = db.transaction('state', 'readwrite');
    tx.objectStore('state').put(value, key);
    tx.oncomplete = resolve;
    tx.onerror = tx.onabort = () => reject(tx.error || new Error('Не удалось сохранить данные'));
  });
}
async function librarySnapshot() {
  const value = await readSnapshot(storageKey);
  return value === undefined ? null : validateLibrary(value);
}
async function writeLibrarySnapshot(next) { await writeSnapshot(storageKey, next); }
async function initialiseLibrary() {
  if (!storageAvailable || !navigator.locks) return;
  try {
    await withStorageLock(storageKey, async () => {
      const mirror = readLibrary(); // Do not replace corrupt user data.
      const saved = await librarySnapshot();
      if (saved === null) await writeLibrarySnapshot(mirror);
      else { localStorage.setItem(storageKey, JSON.stringify(saved)); entries = saved; render(); }
    });
  } catch (error) {
    storageAvailable = false; render();
    message(`Библиотека недоступна: ${error.message}. Данные не изменены.`);
  }
}
const libraryReady = initialiseLibrary();
async function updateLibrary(change) {
  await libraryReady;
  if (!storageAvailable) { message('Библиотека браузера недоступна или повреждена. Данные не изменены.'); return false; }
  try {
    await withStorageLock(storageKey, async () => {
      readLibrary(); // Preserve a damaged compatibility mirror rather than overwrite it.
      const current = await librarySnapshot();
      if (current === null) throw new Error('Библиотека не инициализирована');
      const next = validateLibrary(change(current));
      await writeLibrarySnapshot(next);
      entries = next;
      try { localStorage.setItem(storageKey, JSON.stringify(next)); }
      catch { storageAvailable = false; message('Библиотека сохранена, но синхронизация вкладок недоступна.'); }
    });
    return true;
  } catch (error) { message(`Не удалось сохранить библиотеку: ${error.message}`); return false; }
}
window.addEventListener('storage', event => {
  if (event.key !== storageKey && event.key !== null) return;
  try { entries = validateLibrary(JSON.parse(event.newValue || '[]')); selected = null; render(); }
  catch { storageAvailable = false; selected = null; render(); message('Библиотека повреждена; запись отключена.'); }
});
function page(name) {
  if (name !== 'editor' && !$('#editor').hidden && profileBusy) { message('Подождите завершения работы с раскладкой.'); return false; }
  if (!$('#editor').hidden && name !== 'editor' && !canDiscard()) return false;
  if (name !== 'editor') endLiveTest();
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
  $('#edit-selected-controls').disabled = selected === null || !storageAvailable;
  $('#library-note').textContent = !storageAvailable ? 'Библиотека браузера недоступна или повреждена.' : entries.length ? 'Выберите APK для удаления из списка.' : 'Библиотека пуста — добавьте APK, чтобы сохранить его название.';
}
document.querySelectorAll('[data-page], [data-go]').forEach((button) => button.addEventListener('click', () => page(button.dataset.page || button.dataset.go)));
document.querySelectorAll('.add-apk').forEach((button) => button.addEventListener('click', () => $('#file').click()));
$('#file').addEventListener('change', async (event) => {
  const files = Array.from(event.target.files); event.target.value = '';
  if (!files.length) return;
  const accepted = [];
  const failures = [];
  for (const file of files) {
    try {
      const bytes = new Uint8Array(await file.slice(0, 4).arrayBuffer());
      if (!/\.apk$/i.test(file.name) || bytes.length !== 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4b || bytes[2] !== 3 || bytes[3] !== 4) { failures.push(`${file.name}: неверный заголовок APK`); continue; }
      accepted.push(file.name);
    } catch { failures.push(`${file.name}: не удалось прочитать файл`); }
  }
  if (accepted.length && !await updateLibrary(current => {
    const next = [...current];
    for (const name of accepted) {
      if (next.includes(name)) failures.push(`${name}: название уже есть в библиотеке`);
      else if (next.length >= 500) failures.push(`${name}: лимит 500 файлов`);
      else next.push(name);
    }
    return next;
  })) return;
  selected = null; render(); page('library');
  if (failures.length) message(`Некоторые файлы не добавлены: ${failures.slice(0, 3).join('; ')}${failures.length > 3 ? `; ещё ${failures.length - 3}` : ''}`);
});
$('#search').addEventListener('input', () => { selected = null; render(); });
$('#refresh').addEventListener('click', () => { selected = null; render(); message('Предпросмотр обновлён. Браузер не проверяет наличие файлов.'); });
$('#remove').addEventListener('click', async () => {
  if (selected !== null && confirm('Удалить название из библиотеки предпросмотра? Сам APK останется на диске.')) {
    const name = entries[selected];
    if (await updateLibrary(current => current.filter(entry => entry !== name))) { selected = null; render(); }
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
let storedProfileBaseline = null;
let profileGeneration = 0;
let profileBusy = false;
let selectedControl = null;
let dragging = false;
let dragRecorded = false;
let undoStack = [];
let redoStack = [];
let liveTest = false;
const liveHeld = new Set();
const livePressed = new Set();
const pendingDouble = new Map();
function cancelPendingDouble() { for (const timer of pendingDouble.values()) clearTimeout(timer); pendingDouble.clear(); }
const physicalKeys = new Map();
function validateDraft(data) {
  if (!data || typeof data !== 'object' || Object.keys(data).sort().join(',') !== 'controls,schema' || data.schema !== 1 || !Array.isArray(data.controls) || data.controls.length > 100 || !data.controls.every(c => c && typeof c === 'object' && Object.keys(c).sort().join(',') === 'key,kind,x,y' && kinds.includes(c.kind) && typeof c.key === 'string' && c.key.length >= 1 && c.key.length <= 16 && ['x','y'].every(k => typeof c[k] === 'number' && Number.isFinite(c[k]) && c[k] >= 0 && c[k] <= 1))) throw new Error('Некорректная раскладка');
  return data.controls;
}
function readControls(key = activeProfileKey) {
  return validateDraft(JSON.parse(localStorage.getItem(key) ?? '{"schema":1,"controls":[]}'));
}
function controlKeys(control) {
  return control.kind === 'Джойстик' ? control.key.split(',').map(k => k.trim().toLowerCase()) : [control.key.toLowerCase()];
}
function validateTest() {
  const allKeys = [];
  for (const control of controls) {
    if (!['Нажатие', 'Двойное нажатие', 'Огонь', 'Мультитач', 'Джойстик', 'Камера', 'Перетаскивание'].includes(control.kind)) return 'Неизвестный тип управления.';
    const keys = controlKeys(control);
    if (keys.length !== (control.kind === 'Джойстик' ? 4 : 1) || keys.some(k => !/^[a-z0-9]$/.test(k) && !(control.kind !== 'Джойстик' && ['mouse1', 'mouse2'].includes(k)))) return 'Укажите букву, цифру, Mouse1 или Mouse2. Для джойстика — четыре буквы через запятую.';
    if (control.kind === 'Джойстик' && (control.x < 0.089 || control.x > 0.911 || control.y < 0.089 || control.y > 0.911)) return 'Разместите джойстик немного дальше от края.';
    allKeys.push(...keys);
  }
  if (new Set(allKeys).size !== allKeys.length) return 'Клавиши не должны повторяться.';
  return '';
}
// Motion coordinates are normalized to the 1920 × 1080 simulated display.
const liveMotion = new Map();
let lastMotionPosition = null;
function renderControls() {
  const list = $('#control-list');
  const labels = controls.map((c, i) => `${i + 1}. ${c.kind} · ${c.key} · ${Math.round(c.x * 100)}%, ${Math.round(c.y * 100)}%`);
  if (list.options.length !== labels.length || labels.some((label, i) => list.options[i].textContent !== label)) {
    list.replaceChildren(...labels.map((label, i) => new Option(label, String(i))));
  }
  list.selectedIndex = selectedControl === null ? -1 : selectedControl;
  const canvas = $('#canvas'); canvas.querySelectorAll('.marker, .touch-point').forEach(m => m.remove());
  controls.forEach((c, index) => {
    const keys = controlKeys(c); const active = keys.some(k => liveHeld.has(k));
    const marker = document.createElement('div'); marker.className = 'marker'; marker.classList.toggle('selected', index === selectedControl); marker.classList.toggle('active', active); marker.textContent = c.kind === 'Джойстик' ? '✥' : c.key.toLowerCase() === 'mouse1' ? 'ЛКМ' : c.key.toLowerCase() === 'mouse2' ? 'ПКМ' : c.key; marker.title = `${c.kind}: ${c.key}`; marker.style.left = `${c.x * 100}%`; marker.style.top = `${c.y * 100}%`; canvas.append(marker);
    if (liveMotion.has(index)) {
      const dot = document.createElement('div'); dot.className = 'touch-point';
      const point = liveMotion.get(index); dot.style.left = `${point.x * 100}%`; dot.style.top = `${point.y * 100}%`; canvas.append(dot);
    }
    if (active && c.kind === 'Джойстик') {
      const dx = Number(liveHeld.has(keys[1])) - Number(liveHeld.has(keys[3]));
      const dy = Number(liveHeld.has(keys[2])) - Number(liveHeld.has(keys[0]));
      const length = Math.hypot(dx, dy) || 1;
      const dot = document.createElement('div'); dot.className = 'touch-point';
      dot.style.left = `${(c.x + dx / length * 0.09 * 1080 / 1920) * 100}%`;
      dot.style.top = `${(c.y + dy / length * 0.09) * 100}%`; canvas.append(dot);
    }
  });
  $('#apply-control').disabled = selectedControl === null;
  $('#delete-control').disabled = selectedControl === null;
  $('#undo-control').disabled = undoStack.length === 0;
  $('#redo-control').disabled = redoStack.length === 0;
}
function activeTouchCount() { return controls.filter(c => controlKeys(c).some(k => liveHeld.has(k))).length; }
let capturingKey = false;
let ignoreCaptureClick = false;
function cancelCapture() { capturingKey = false; $('#capture-key').textContent = 'Назначить нажатием'; }
function syncCaptureKind() { cancelCapture(); $('#capture-key').disabled = $('#control-kind').value === 'Джойстик'; }
$('#control-kind').addEventListener('change', () => { syncCaptureKind(); if ($('#control-kind').value === 'Джойстик' && $('#control-key').value.trim() === 'F') $('#control-key').value = 'W,D,S,A'; });
$('#capture-key').addEventListener('click', () => { if (ignoreCaptureClick) { ignoreCaptureClick = false; return; } if (!capturingKey) { capturingKey = true; $('#capture-key').textContent = 'Нажмите клавишу или кнопку мыши здесь • Esc — отмена'; $('#capture-key').focus(); } });
$('#capture-key').addEventListener('keydown', event => {
  if (!capturingKey) return;
  event.preventDefault();
  if (event.key === 'Escape') cancelCapture();
  else if (/^[a-z0-9]$/i.test(event.key) && !event.ctrlKey && !event.altKey && !event.metaKey) { $('#control-key').value = event.key.toUpperCase(); cancelCapture(); }
});
$('#capture-key').addEventListener('mousedown', event => {
  if (!capturingKey || ![0, 2].includes(event.button)) return;
  event.preventDefault(); if (event.button === 0) { ignoreCaptureClick = true; setTimeout(() => { ignoreCaptureClick = false; }, 1000); } $('#control-key').value = event.button === 0 ? 'Mouse1' : 'Mouse2'; cancelCapture();
});
$('#capture-key').addEventListener('contextmenu', event => event.preventDefault());
$('#capture-key').addEventListener('blur', cancelCapture);
$('#open-settings').addEventListener('click', () => $('#settings-dialog').showModal());
function canDiscard() {
  return JSON.stringify(controls) === controlsBaseline || confirm('Отменить несохранённые изменения раскладки?');
}
function setProfileBusy(busy) {
  profileBusy = busy;
  $('#editor').inert = busy;
}
function validateStoredProfile(raw) {
  if (raw === null) return [];
  if (typeof raw !== 'string') throw new Error('Некорректные данные раскладки');
  return validateDraft(JSON.parse(raw));
}
// Call only while holding the matching Web Lock. A valid legacy localStorage
// record is migrated once; after that IndexedDB is authoritative.
async function canonicalProfile(key) {
  const mirror = localStorage.getItem(key);
  validateStoredProfile(mirror); // Never silently replace corrupt data.
  let saved = await readSnapshot(key);
  if (saved === undefined) { saved = mirror; await writeSnapshot(key, saved); }
  validateStoredProfile(saved);
  if (mirror !== saved) {
    if (saved === null) localStorage.removeItem(key);
    else localStorage.setItem(key, saved);
  }
  return saved;
}
async function loadProfile(key) {
  endLiveTest(); profileGeneration++;
  activeProfileKey = key; setProfileBusy(true);
  try {
    const raw = navigator.locks ? await withStorageLock(key, () => canonicalProfile(key)) : localStorage.getItem(key);
    controls = validateStoredProfile(raw); storedProfileBaseline = raw; controlsValid = true;
    $('#editor-status').textContent = `Элементов: ${controls.length}`;
  } catch {
    controls = []; storedProfileBaseline = null; controlsValid = false;
    $('#editor-status').textContent = 'Раскладка повреждена или недоступна. Сохранение отключено.';
  } finally {
    controlsBaseline = JSON.stringify(controls);
    selectedControl = null; dragging = false; dragRecorded = false; undoStack = []; redoStack = [];
    $('#save-controls').disabled = !controlsValid; $('#import-controls').disabled = !controlsValid;
    refreshCopySources(); renderControls(); setProfileBusy(false);
  }
}
function refreshCopySources() {
  const source = $('#copy-source'); source.replaceChildren();
  for (const option of $('#control-profile').options) {
    if (option.value !== activeProfileKey) source.add(new Option(option.textContent, option.value));
  }
  source.disabled = !controlsValid || source.options.length === 0;
  $('#copy-profile').disabled = source.disabled;
}
$('#copy-profile').addEventListener('click', async () => {
  if (!controlsValid || profileBusy || !$('#copy-source').value) return;
  const key = $('#copy-source').value, generation = profileGeneration;
  $('#copy-profile').disabled = true;
  try {
    const raw = navigator.locks ? await withStorageLock(key, () => canonicalProfile(key)) : localStorage.getItem(key);
    if (generation !== profileGeneration || $('#editor').hidden) { message('Профиль изменился. Копирование отменено.'); return; }
    if (raw === null) throw new Error('У этого профиля ещё нет сохранённой раскладки.');
    const source = validateStoredProfile(raw);
    if (JSON.stringify(source) === JSON.stringify(controls)) { $('#editor-status').textContent = 'Раскладка уже совпадает с сохранённой копией.'; return; }
    if (controls.length && !confirm('Скопировать сохранённую раскладку в текущий черновик? Действие можно отменить.')) return;
    recordChange(); controls = source; selectControl(null);
    $('#editor-status').textContent = 'Раскладка скопирована в черновик • сохраните для выбранной игры';
  } catch (error) { message(`Не удалось скопировать раскладку: ${error.message}`); }
  finally { if (generation === profileGeneration) $('#copy-profile').disabled = !controlsValid || !$('#copy-source').options.length; }
});
function openControls() {
  if (!$('#editor').hidden) return;
  const select = $('#control-profile'); select.replaceChildren();
  const general = document.createElement('option'); general.textContent = 'Общий шаблон (старый)'; general.value = controlStorageKey; select.append(general);
  entries.forEach(name => { const option = document.createElement('option'); option.value = `lynxdroid-preview-game-v1:${name}`; option.textContent = name; select.append(option); });
  activeProfileIndex = entries.length ? (selected !== null ? selected + 1 : 1) : 0;
  select.selectedIndex = activeProfileIndex; loadProfile(select.value); page('editor');
}
$('#reload-profile').addEventListener('click', () => { if (canDiscard()) loadProfile(activeProfileKey); });
$('#open-controls').addEventListener('click', openControls);
$('#edit-selected-controls').addEventListener('click', openControls);
$('#control-profile').addEventListener('change', event => {
  const select = event.target;
  if (!canDiscard()) { select.selectedIndex = activeProfileIndex; return; }
  activeProfileIndex = select.selectedIndex; loadProfile(select.value);
});
$('#close-editor').addEventListener('click', () => page('home'));
document.querySelectorAll('[data-close]').forEach(button => button.addEventListener('click', () => document.getElementById(button.dataset.close).close()));
function recordChange() {
  // Live test bindings must never outlive the draft they were made from.
  endLiveTest();
  undoStack.push(JSON.stringify(controls));
  if (undoStack.length > 50) undoStack.shift();
  redoStack = [];
}
function selectControl(index) {
  selectedControl = index;
  if (index !== null) { $('#control-kind').value = controls[index].kind; syncCaptureKind(); $('#control-key').value = controls[index].key; }
  renderControls();
}
$('#control-list').addEventListener('keydown', event => {
  const directions = {ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1]};
  if (selectedControl === null || !event.altKey || event.ctrlKey || event.metaKey || !directions[event.key]) return;
  event.preventDefault();
  const step = event.shiftKey ? 0.05 : 0.01;
  const [dx, dy] = directions[event.key], item = controls[selectedControl];
  const clamp = value => Math.max(0, Math.min(1, Math.round(value * 1000000) / 1000000));
  const x = dx ? clamp(item.x + dx * step) : item.x;
  const y = dy ? clamp(item.y + dy * step) : item.y;
  if (item.x === x && item.y === y) return;
  recordChange(); Object.assign(item, {x, y});
  $('#editor-status').textContent = 'Элемент перемещён • не сохранено'; renderControls();
});
$('#control-list').addEventListener('change', event => {
  const index = event.target.selectedIndex;
  selectControl(index < 0 ? null : index);
});
function canvasPosition(event) {
  const rect = $('#canvas').getBoundingClientRect();
  return {x: Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), y: Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height))};
}
$('#canvas').addEventListener('pointerdown', event => {
  if (liveTest) { $('#canvas').focus(); $('#canvas').setPointerCapture(event.pointerId); return; }
  if (event.button !== 0) return;
  const rect = $('#canvas').getBoundingClientRect();
  const x = event.clientX - rect.left, y = event.clientY - rect.top;
  let hit = null;
  for (let i = controls.length - 1; i >= 0; i--) {
    if (Math.hypot(x - controls[i].x * rect.width, y - controls[i].y * rect.height) <= 22) { hit = i; break; }
  }
  if (hit !== null) { selectControl(hit); dragging = true; dragRecorded = false; $('#canvas').setPointerCapture(event.pointerId); return; }
  const key = $('#control-key').value.trim();
  if (!key || controls.length >= 100) { $('#editor-status').textContent = 'Укажите клавишу; максимум 100 элементов.'; return; }
  recordChange(); controls.push({kind: $('#control-kind').value, key, ...canvasPosition(event)});
  selectControl(controls.length - 1); $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`;
});
$('#canvas').addEventListener('pointermove', event => {
  if (liveTest) {
    const previous = lastMotionPosition; lastMotionPosition = {x: event.clientX, y: event.clientY};
    const rect = $('#canvas').getBoundingClientRect();
    if (previous && rect.width && rect.height && liveMotion.size) {
      for (const point of liveMotion.values()) {
        point.x = Math.max(0, Math.min(1919 / 1920, point.x + (event.clientX - previous.x) / rect.width));
        point.y = Math.max(0, Math.min(1079 / 1080, point.y + (event.clientY - previous.y) / rect.height));
      }
      $('#editor-status').textContent = `Тест без Android: движение • событий: ${liveMotion.size}`; renderControls();
    }
    return;
  }
  if (!dragging || selectedControl === null) return;
  const position = canvasPosition(event);
  if (controls[selectedControl].x === position.x && controls[selectedControl].y === position.y) return;
  if (!dragRecorded) { recordChange(); dragRecorded = true; }
  Object.assign(controls[selectedControl], position);
  $('#editor-status').textContent = 'Элемент перемещён • не сохранено'; renderControls();
});
$('#canvas').addEventListener('pointerup', () => { dragging = false; });
$('#canvas').addEventListener('pointerleave', () => { lastMotionPosition = null; });
$('#canvas').addEventListener('pointercancel', () => { dragging = false; endLiveTest(); });
$('#apply-control').addEventListener('click', () => {
  if (selectedControl === null) return;
  const key = $('#control-key').value.trim();
  if (!key) { $('#editor-status').textContent = 'Укажите клавишу'; return; }
  if (controls[selectedControl].kind === $('#control-kind').value && controls[selectedControl].key === key) return;
  recordChange(); Object.assign(controls[selectedControl], {kind: $('#control-kind').value, key});
  $('#editor-status').textContent = 'Элемент изменён • не сохранено'; renderControls();
});
$('#delete-control').addEventListener('click', () => {
  if (selectedControl === null) return;
  recordChange(); controls.splice(selectedControl, 1); selectControl(null);
  $('#editor-status').textContent = `Элементов: ${controls.length} • не сохранено`;
});
function endLiveTest() {
  if (!liveTest) return;
  liveTest = false; liveHeld.clear(); livePressed.clear(); physicalKeys.clear(); liveMotion.clear(); lastMotionPosition = null; cancelPendingDouble();
  $('#live-test').textContent = 'Тест клавиш';
  $('#editor-status').textContent = 'Тест остановлен. Ничего не отправлено в Android.';
  renderControls();
}
$('#live-test').addEventListener('click', () => {
  if (liveTest) { endLiveTest(); return; }
  const error = validateTest();
  if (error) { $('#editor-status').textContent = `Проверка невозможна: ${error}`; return; }
  liveTest = true; lastMotionPosition = null; $('#live-test').textContent = 'Остановить тест'; $('#canvas').focus();
  $('#editor-status').textContent = 'Тест без Android: камера — переключатель; перетаскивание — удержание и движение мыши. Esc — выход.';
});
function testDown(key) {
  if (livePressed.has(key)) return;
  const control = controls.find(c => controlKeys(c).includes(key));
  if (!control) return;
  livePressed.add(key);
  if (control.kind === 'Двойное нажатие') {
    if (pendingDouble.has(key)) return;
    const timer = setTimeout(() => {
      pendingDouble.delete(key);
      if (!liveTest) return;
      $('#editor-status').textContent = 'Тест без Android: второе нажатие • событий: 2';
    }, 120);
    pendingDouble.set(key, timer);
  }
  if (['Камера', 'Перетаскивание'].includes(control.kind)) {
    const index = controls.indexOf(control);
    if (control.kind === 'Камера' && liveMotion.has(index)) {
      liveMotion.delete(index); liveHeld.delete(key);
    } else {
      liveMotion.set(index, {x: Math.min(control.x, 1919 / 1920), y: Math.min(control.y, 1079 / 1080)}); liveHeld.add(key);
    }
    $('#editor-status').textContent = `Тест без Android: ${key.toUpperCase()} • удерживается: ${activeTouchCount()}`; renderControls(); return;
  }
  const wasActive = controlKeys(control).some(k => liveHeld.has(k));
  if (['Огонь', 'Мультитач', 'Джойстик'].includes(control.kind)) liveHeld.add(key);
  const events = ['Нажатие', 'Двойное нажатие'].includes(control.kind) ? 2 : control.kind === 'Джойстик' && !wasActive ? 2 : 1;
  $('#editor-status').textContent = `Тест без Android: ${key.toUpperCase()} • событий: ${events} • удерживается: ${activeTouchCount()}`; renderControls();
}
function testUp(key) {
  if (!livePressed.has(key)) return;
  livePressed.delete(key);
  const index = controls.findIndex(c => controlKeys(c).includes(key));
  if (index !== -1 && controls[index].kind === 'Камера') return;
  if (index !== -1 && controls[index].kind === 'Перетаскивание') liveMotion.delete(index);
  if (liveHeld.delete(key)) $('#editor-status').textContent = `Тест без Android: ${key.toUpperCase()} • событий: 1 • удерживается: ${activeTouchCount()}`;
  renderControls();
}
$('#canvas').addEventListener('keydown', event => {
  if (!liveTest) return;
  if (event.key === 'Escape') { event.preventDefault(); endLiveTest(); return; }
  const key = event.key.toLowerCase();
  if (!/^[a-z0-9]$/.test(key) || event.repeat) return;
  event.preventDefault(); physicalKeys.set(event.code || key, key); testDown(key);
});
$('#canvas').addEventListener('keyup', event => {
  if (!liveTest) return;
  const physical = event.code || event.key.toLowerCase();
  const key = physicalKeys.get(physical);
  if (!key) return;
  physicalKeys.delete(physical); testUp(key); event.preventDefault();
});
$('#canvas').addEventListener('mousedown', event => {
  if (!liveTest) return;
  const key = event.button === 0 ? 'mouse1' : event.button === 2 ? 'mouse2' : '';
  if (key) { event.preventDefault(); testDown(key); }
});
window.addEventListener('mouseup', event => {
  if (!liveTest) return;
  const key = event.button === 0 ? 'mouse1' : event.button === 2 ? 'mouse2' : '';
  if (key) testUp(key);
});
$('#canvas').addEventListener('contextmenu', event => { if (liveTest) event.preventDefault(); });
$('#canvas').addEventListener('blur', () => { if (liveTest) { liveHeld.clear(); livePressed.clear(); physicalKeys.clear(); liveMotion.clear(); lastMotionPosition = null; cancelPendingDouble(); renderControls(); } });
$('#test-mapping').addEventListener('click', () => {
  const status = $('#editor-status');
  const error = validateTest();
  if (error) { status.textContent = `Проверка невозможна: ${error}`; return; }
  if (!/^[a-z0-9]$/i.test($('#test-key').value.trim())) { status.textContent = 'Для проверки введите одну латинскую букву или цифру.'; return; }
  const control = controls.find(c => controlKeys(c).includes($('#test-key').value.trim().toLowerCase()));
  if (!control) { status.textContent = 'Демо без Android: клавиша не назначена. Ничего не отправлено.'; return; }
  const x = Math.min(1919, Math.floor(control.x * 1920));
  const y = Math.min(1079, Math.floor(control.y * 1080));
  status.textContent = `Демо без Android: ${['Джойстик', 'Перетаскивание'].includes(control.kind) ? 3 : control.kind === 'Двойное нажатие' ? 4 : 2} события касания, позиция ${x} × ${y}. Ничего не отправлено.`;
});
$('#undo-control').addEventListener('click', () => {
  if (!undoStack.length) return;
  endLiveTest(); redoStack.push(JSON.stringify(controls)); controls = JSON.parse(undoStack.pop());
  selectControl(null); $('#editor-status').textContent = 'Действие отменено • сохраните изменения';
});
$('#redo-control').addEventListener('click', () => {
  if (!redoStack.length) return;
  endLiveTest(); undoStack.push(JSON.stringify(controls)); controls = JSON.parse(redoStack.pop());
  selectControl(null); $('#editor-status').textContent = 'Действие повторено • сохраните изменения';
});
$('#reset-controls').addEventListener('click', () => {
  if (!controls.length) return;
  recordChange(); controls = []; selectControl(null); $('#editor-status').textContent = 'Шаблон очищен • не сохранено';
});
$('#import-controls').addEventListener('click', () => { if (controlsValid) $('#import-file').click(); });
$('#import-file').addEventListener('change', async event => {
  const file = event.target.files[0]; event.target.value = '';
  if (!file) return;
  const generation = profileGeneration;
  try {
    if (file.size > 128 * 1024) throw new Error('Файл раскладки больше 128 КБ');
    const bytes = await file.arrayBuffer();
    const text = new TextDecoder('utf-8', {fatal: true}).decode(bytes);
    const imported = validateDraft(JSON.parse(text));
    if (generation !== profileGeneration || $('#editor').hidden) { message('Профиль изменился. Импорт отменён.'); return; }
    if (controls.length && !confirm('Заменить текущий черновик? До сохранения можно отменить.')) return;
    if (JSON.stringify(imported) !== JSON.stringify(controls)) { recordChange(); controls = imported; selectControl(null); }
    $('#editor-status').textContent = 'Раскладка импортирована • сохраните для выбранной игры';
  } catch { message('Не удалось импортировать: файл повреждён или больше 128 КБ.'); }
});
$('#export-controls').addEventListener('click', () => {
  const blob = new Blob([JSON.stringify({schema:1, controls}, null, 2) + '\n'], {type:'application/json'});
  const url = URL.createObjectURL(blob); const link = document.createElement('a'); link.href = url; link.download = 'lynxdroid-layout.json';
  document.body.append(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  $('#editor-status').textContent = 'Экспорт запрошен. Профиль игры не изменён.';
});
$('#save-controls').addEventListener('click', async () => {
  if (!controlsValid || profileBusy) return;
  const key = activeProfileKey, baseline = storedProfileBaseline;
  const snapshot = JSON.stringify(controls);
  setProfileBusy(true);
  try {
    let mirrorFailed = false;
    await withStorageLock(key, async () => {
      const current = await canonicalProfile(key);
      if (current !== baseline) throw new Error('Раскладка изменена в другой вкладке. Экспортируйте черновик и нажмите «Обновить раскладку»');
      const serialized = JSON.stringify({schema:1, controls: JSON.parse(snapshot)});
      await writeSnapshot(key, serialized);
      storedProfileBaseline = serialized; controlsBaseline = snapshot;
      try { localStorage.setItem(key, serialized); }
      catch { mirrorFailed = true; }
    });
    $('#editor-status').textContent = mirrorFailed ? 'Раскладка сохранена в IndexedDB, но синхронизация localStorage недоступна.' : 'Раскладка сохранена';
  } catch (error) { $('#editor-status').textContent = `Ошибка сохранения: ${error.message}. Исходные данные не изменены.`; }
  finally { setProfileBusy(false); }
});
window.addEventListener('beforeunload', event => {
  if (JSON.stringify(controls) !== controlsBaseline && !$('#editor').hidden) { event.preventDefault(); event.returnValue = ''; }
});
// The local preview server reports a fingerprint; reload on source edits.
let version;
async function checkVersion() {
  try { const response = await fetch('/__version', {cache:'no-store'}); if (!response.ok) return; const next = await response.text(); if (version && version !== next) { if (!$('#editor').hidden && (profileBusy || JSON.stringify(controls) !== controlsBaseline)) { $('#editor-status').textContent = 'Обновление предпросмотра отложено: сначала сохраните раскладку.'; return; } location.reload(); return; } version = next; } catch { /* Static hosting does not support live reload. */ }
}
checkVersion(); setInterval(checkVersion, 1500);
