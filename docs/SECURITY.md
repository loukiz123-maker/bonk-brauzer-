# Security model

Written for someone deciding whether to trust this program with a container
that shares their kernel.

## 1. Trust boundaries

```
┌───────────────────────────────────────────────────────────────┐
│ unprivileged user process                                     │
│  ┌─────────────┐   ┌──────────────┐   ┌────────────────────┐  │
│  │ GUI / CLI   │──▶│ backend      │──▶│ waydroid (setuid-  │  │
│  │ (our code)  │   │ adapter      │   │ free, but talks to │  │
│  └─────────────┘   └──────────────┘   │ a root service)    │  │
│         │                             └────────────────────┘  │
│         │ unix socket, 0600                                   │
│         ▼                                                     │
│  ┌──────────────────────────────┐                             │
│  │ Android container            │  ← root-owned namespace,    │
│  │ LineageOS image, Android 13  │    but the guest is NOT a   │
│  └──────────────────────────────┘    security boundary        │
└───────────────────────────────────────────────────────────────┘
        ▲                                    ▲
        │ never written by us                │ needs explicit consent
   /var/lib/waydroid                   persistent Android props
```

Three things this project deliberately does **not** do:

1. it never runs as root, and never asks for a password;
2. it never writes into `/var/lib/waydroid` or edits `waydroid.cfg`;
3. it never changes boot parameters, kernel modules, SELinux/AppArmor or
   systemd unit files.

Everything privileged is *described* to the user with the exact command, and
left for them to run (`backend/images.render_install_instructions`).

## 2. Honest statement about the container's isolation

Waydroid runs Android in LXC sharing the **host kernel**. That means:

* a kernel-level Android exploit is a host exploit — a VM would give a boundary
  here and a container does not;
* the guest can see the host's `/proc` subset and network namespace that
  Waydroid configures;
* `binder`, `memfd` and DRM access are real kernel interfaces, not emulated.

We say this in the UI (`capabilities.notes`) rather than in a footnote, because
a user installing banking apps deserves to know before, not after.

## 3. Command execution

`core/proc.py` is the *only* place a subprocess is created.

| Rule | Enforcement |
|---|---|
| no shell | `subprocess.run(argv, ...)` with `shell` never set; a test proves `; echo INJECTED` stays literal |
| argv validation | empty argv and NUL bytes raise `ValueError` |
| timeouts | every call takes `timeout`; expiry returns `returncode=124` and the child group is killed |
| no leaks | children run in their own session (`start_new_session=True`); `terminate_tree` sends SIGTERM then SIGKILL |
| environment hygiene | `LYNX_SECRET*` variables are stripped before spawning |
| no password prompts | nothing invokes `sudo`; if root is required the user is told which command to run |

User-controlled strings that reach argv are validated first:

| Value | Validator |
|---|---|
| Android package name | `^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$` |
| Android property name | `^[A-Za-z][A-Za-z0-9_.]{1,63}$` |
| intent action | `^[a-z]+(\.[a-zA-Z0-9_]+)+$` |
| APK path | regular file, `.apk`, ≤ 4 GiB, `PK\x03\x04` magic, not a symlink |
| profile id | `[a-z0-9][a-z0-9-]*`, no separators, no leading dot |

## 4. Filesystem

* All application state lives under XDG directories created `0700`, files `0600`.
* Writes are atomic: temp file in the same directory → `fsync` → `os.replace` →
  directory `fsync`. A crash cannot leave a half-written profile.
* Deletion goes through `paths.safe_remove()`, which:
  - resolves symlinks and refuses anything outside the app directory,
  - refuses to delete the root itself,
  - deletes symlinks rather than their targets, and only inside the root.
* Imports refuse **any** symlink. A "profile" that is really a link to
  `/etc/shadow`, a FIFO or `/dev/zero` is never read.
* File size limits: profile ≤ 1 MB, APK ≤ 4 GiB, JSON reader caps at the same
  bound, agent frames ≤ 64 KiB.

## 5. Network

| Interaction | Direction | Bounded by |
|---|---|---|
| OTA metadata / image download | outbound HTTPS, user-initiated only | a `Consent` object that only `user_consented()` creates, space check, SHA-256 computed locally |
| touch agent | **unix socket only** | `$XDG_RUNTIME_DIR`, `0600`, protocol version handshake |
| adb | local socket / USB | `adb` itself |

No telemetry, no analytics, no auto-update, no listening TCP port anywhere in
this codebase. `grep -r "bind((" src/` returns only the unix-socket agent path
in tests.

**Caveat we state in the docs and the UI:** other processes of the same user can
reach that unix socket. That is inherent to unix sockets; it is why the socket
lives in the runtime directory and is removed when the session ends.

## 6. Input injection risk (stage 8)

The specification is blunt about this and so are we.

| Approach | Risk | Mitigation here |
|---|---|---|
| `/dev/input/event*` read | **can read every keystroke, including passwords** | not implemented |
| `EVIOCGRAB` | can lock the user's keyboard out | not implemented |
| global key grab | intercepts all typing system-wide | not implemented; the GUI only receives keys while focused |
| `INJECT_EVENTS` agent | can synthesise touches for any app | opt-in APK, unix socket, pointer-id validation, release-all on disconnect |
| `adb input tap` | single touch only | labelled as such; overlay disabled |

Automatic safety behaviours that *are* implemented:

* releasing the window (`focusOutEvent`) cancels every contact and forgets held
  keys — tested as `TestFocusLoss`;
* closing the window, switching profiles or changing geometry cancels contacts
  before the change is applied;
* the input engine refuses to open more contacts than the channel advertises.

## 7. Secrets

There are no credentials to store: no accounts, no tokens, no API keys. What
could leak is *content*: a log line containing a package name or a path.
`logging_setup.redact()` scrubs `password=`, `token=`, `secret=`,
`Authorization: Bearer …` and home-directory names before formatting, and
profile export is asserted (by test) to contain no secret-shaped fields.

Reports written to `$XDG_STATE_HOME/lynxdroid/reports/` are `0600` and contain
only what `doctor` observed.

## 8. What a user should still be careful about

This project cannot protect you from:

* a malicious APK you install — that runs inside the container with whatever
  Waydroid grants it, which on a shared kernel is meaningful;
* the upstream Waydroid service running as root — it is outside this project,
  and we neither audit nor modify it;
* games with anti-cheat — we do not and will not circumvent them; using this
  tool with such a game may violate its terms.

## 9. Reporting a problem

Security issues should not be filed as public issues. Contact the maintainer
privately; the repository documents a contact address in `SECURITY_POLICY`
once one exists. Until then, do not publish a working exploit.
