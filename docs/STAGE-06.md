# Этап 6: безопасность, процесс на вкладку, devtools

Задача этапа — сделать движок не «доверчивым»: ввести Same-Origin Policy и CORS
для скриптовых запросов, применять Content-Security-Policy, блокировать mixed
content, показать состояние соединения, изолировать вкладку в отдельном процессе
с IPC и дать инструменты разработчика (консоль, сеть, инспектор).

## Из чего состоит этап

```
net::Url ──► security::Origin ──► Same-Origin Policy ──┐
                 │                                      ├──► bindings (XHR, document.cookie)
                 ├──► CORS (запрос + ответ)  ◄──────────┘        │
                 ├──► CSP (meta + HTTP-заголовок) ────────────────┤
                 └──► mixed content, security state ──────────────┘
                                                                  ▼
                                              shell::Session: политика, cookies, журнал сети
                                                                  │
                                    ┌─────────────────────────────┴───────────────┐
                                    ▼                                             ▼
                        shell::DevToolsSnapshot                     shell::render_isolated
                        (консоль, сеть, CSP, инспектор)              (fork + pipe IPC, PNG)
```

## Что реализовано

| Блок | Содержание |
|---|---|
| `security::Origin` | кортеж (scheme, host, port), сериализация без порта по умолчанию, `null` для `file:`/data, `Origin:` заголовок |
| SOP | `same_origin()`, небезопасные/opaque origin'ы, origin-скоуп для `localStorage` и `document.cookie` |
| CORS | классификация запроса (simple / preflight / blocked), оценка ответа (`Access-Control-Allow-Origin: *` и точный, `Allow-Credentials`, wildcard+credentials запрещён), понятные коды отказа: `cors.allow_origin_missing`, `cors.allow_origin_mismatch`, `cors.wildcard_with_credentials`, `cors.preflight_required_header` |
| CSP | `default-src`, `script-src`, `style-src`, `img-src`, `connect-src`; `'none'`, `'self'`, `*`, `'unsafe-inline'`, схемы (`https:`), хосты с `*.` и портом; применяется и к `<meta http-equiv>`, и к заголовку ответа |
| Mixed content | https-страница не грузит `http://` подресурсы: запрос блокируется, в диагностику уходит `security.mixed_content` |
| HTTPS-индикатор | `Tab::security` (`secure` / `insecure` / `none`), печатается `bonk view`/`bonk devtools` |
| XHR | синхронный `XMLHttpRequest` (`open(method, url, false)`, `send()`, `status`, `statusText`, `responseText`, `getResponseHeader(s)`, `readyState`, `error`); транспорт — shell, поэтому CORS/CSP/cookies/журнал в одном месте |
| Процесс на вкладку | `shell::render_isolated()`: `fork()` + два pipe, «BONK»-запрос и «FRAM»-ответ с длинами, PNG в памяти; падение рендерера возвращается как `shell.tab_crashed`, браузер живёт |
| DevTools | `bonk devtools <page>`: url/title/origin/security/csp/dom, список скриптов (ok/skipped/blocked), консоль, сетевой водопад, инспектор элемента (`--selector`) |

## Как выглядит devtools

```
$ bonk devtools http://127.0.0.1:8731/ --width 720 --height 420
url       http://127.0.0.1:8731/
title     CORS demo
origin    http://127.0.0.1:8731
security  insecure
csp       none
dom       nodes=17
scripts   1 run, 0 blocked
  ok      (no output)
console   3 entries
  [error] XMLHttpRequest cors.allow_origin_missing: http://127.0.0.1:8733/secret
  [log]   allowed read: 200 data-from-8732
  [log]   blocked read: 0 cors.allow_origin_missing
network   2 requests
  GET 200  14 B     0.33 ms  http://127.0.0.1:8732/data
  GET BLOCKED  0 B     0.00 ms  http://127.0.0.1:8733/secret  (cors.allow_origin_missing)
```

Инспектор:

```
$ bonk devtools docs/demo/forms.html --selector "#signup"
inspector #signup
  element  form#signup
  box      x=28 y=105 w=968 h=456
  style    display:block  color:rgba(28, 36, 48, 1.000000)  background:transparent
           font-size:16px  weight:400  margin:0px/0px  padding:0px/0px
```

CSP на локальной странице (`docs/demo/security.html`) блокирует инлайновый скрипт:

```
$ bonk devtools docs/demo/security.html
origin    null
security  none (local file, opaque origin)
csp       default-src 'self'; script-src 'self'; img-src 'self' data:; connect-src 'self'
scripts   0 run, 1 blocked
  blocked blocked by Content-Security-Policy (script-src without 'unsafe-inline')
```

## Процесс на вкладку

```
parent                          child (renderer)
  │  pipe(req) ── BONK header ──►│  Session::navigate → layout → paint
  │            target/font/profile│
  │◄── FRAM header + title/error ─┤  canvas.encode_png()
  │◄── PNG bytes ─────────────────┘  _exit(0)
  │
  └─ waitpid → WIFSIGNALED ⇒ "shell.tab_crashed (signal 6)", браузер продолжает работу
```

`bonk view page.html --isolated --screenshot out.png` рендерит вкладку в дочернем
процессе и получает PNG по IPC (без временных файлов). На платформах без `fork()`
`isolation_supported()` возвращает `false`, и вызывающий код рендерит в процессе —
явно, а не «молча».

## Тесты

```
make -j && make test        → 316 тестов, 0 падений
```

Новые тесты этапа:

* `tests/security/security_test.cpp` — origin/сериализация/`null`, правила
  same-origin (включая «file никогда не same-origin»), security state, mixed
  content, классификация CORS-запроса, оценка CORS-ответа (wildcard, credentials,
  отсутствующий заголовок), CSP (inline-скрипты, схемы, `*.host`, `'self'`,
  `'none'`, `connect-src`).
* `tests/shell/security_test.cpp` — реальные HTTP-обмены с локальным сервером в
  процессе теста: CSP из `<meta>` и из заголовка блокирует инлайн-скрипт;
  same-origin XHR читает тело; cross-origin без `Access-Control-Allow-Origin`
  блокируется (`status == 0`, `cors.allow_origin_missing` в журнале); с
  `Access-Control-Allow-Origin: *` — читается; запрос с кастомным заголовком
  отклоняется как требующий preflight; `security state` и opaque origin для файлов.
* `tests/shell/isolate_test.cpp` — рендер в дочернем процессе приходит как PNG
  (проверяется сигнатура), ошибка открытия файла приходит как ошибка, а не как
  падение, и «упавшая» вкладка не убивает браузер (после неё рендерится следующая).

## Демонстрация

* `docs/demo/security.png` — страница с CSP, все три «плохих» случая помечены
  (инлайновый скрипт заблокирован, cross-origin чтение запрещено, файл = `null`)
* `docs/demo/security-cors.png` — страница, где скрипт делает два XHR-запроса:
  разрешённый (CORS-заголовок) и заблокированный; оба видны в консоли и в сети
* `docs/demo/security.html` — исходник первой страницы

## Что работает

* Кросс-доменный `XMLHttpRequest` без CORS-заголовков блокируется, с заголовком —
  читается; причина отказа видна и скрипту (`xhr.error`), и в devtools.
* CSP из `<meta>` и из HTTP-заголовка реально останавливает исполнение скриптов.
* Mixed content блокируется на уровне загрузки подресурсов.
* `document.cookie` и `localStorage` разграничены по origin (для файлов — по
  каталогу).
* Вкладка рендерится в дочернем процессе; её падение возвращается как ошибка.
* `bonk devtools` показывает консоль, сеть, CSP и стили элемента.

## Что пока не сделано

* **Preflight (`OPTIONS`)**: cross-origin запрос, требующий preflight, сейчас
  отклоняется, а не согласуется с сервером. Это осознанное ограничение до
  появления асинхронного цикла событий.
* `fetch`/`Promise`: API ещё нет (нужны микротаски), поэтому «кросс-доменный fetch»
  проверяется через синхронный `XMLHttpRequest`.
* `SameSite` для cookies, Public Suffix List, `document.domain`, sandbox-iframe,
  CSP nonce/hash, `report-only`, CORS `Access-Control-Expose-Headers`.
* Персистентное хранилище cookies между запусками (банка живёт в сессии).
* Процесс на вкладку — только для headless-рендера: интерактивное окно пока
  однопроцессное (нужен message loop поверх X11-событий).
* DevTools — CLI-панель, а не GUI; нет правки DOM/CSS на лету и таймлайна.

## Предлагаемый следующий шаг

Этап 7 — конформность: прогнать html5lib-tests (токенайзер и tree builder),
test262 (парсер + встроенные объекты; сначала те, что не требуют Promise),
Acid2 как визуальный ориентир, WPT-подмножество по тем возможностям, что уже
есть (CSS 2.1-блоки, flexbox, формы). Результат — таблица «пройдено/провалено»
и список баг-фиксов, а не «зелёный экран».
