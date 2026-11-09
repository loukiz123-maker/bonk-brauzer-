# Game compatibility

Error text is a clue, not proof of the underlying cause. LynxDroid cannot
promise that every game will run on Waydroid. In particular, game-specific
root, emulator and device-integrity checks may reject it. Consult the game
vendor about supported devices.

```
lynxdroid game explain "Не удалось войти на сервер: 3"
lynxdroid game fix network --yes
```

The network command runs ICMP pings to `1.1.1.1` and `example.com` and checks
the Android routing table. The hostname ping exercises name resolution as well
as ICMP; a failed ping alone cannot distinguish a DNS failure from blocked ICMP.
Successful probes do not verify the game's DNS name, TCP connection or server.
No network settings are changed. The command needs a running Waydroid session
and access to its Android shell. Waydroid normally requires root for `shell`,
and LynxDroid never requests a password or invokes sudo. If access is denied,
the output lists a read-only command to run yourself. In Game help, paste
its complete terminal output and select **Review pasted output**. LynxDroid
reads each guest exit marker, reporting missing or duplicate markers as unknown.
Pasted output is user-supplied and cannot be independently authenticated;
command success is not a guarantee that the game server is reachable.
For headless use, redirect the manual command’s full output to a file and run
`lynxdroid game network-report --json < network.txt`; the report returns 6 if
any marker is missing, ambiguous or nonzero.

## In the desktop application

Open **Game help**, paste the game error, and select **Explain error**.
For connection errors, **Check Android network** attempts the limited probes in
the background and displays their output without changing settings. Qt wiring
is statically tested; actual GUI operation still needs verification on a desktop.

## Correction to earlier releases

`hide-root` and `egl-fallback` are disabled: their effectiveness was never
verified against real games or Waydroid, and earlier success messages were
unjustified. The old `hide-root` implementation could change three properties,
but did not persist their original values. **Do not use the old `game revert`
to recover them**: it guessed defaults and could modify a previously clean
system. The new revert refuses to write anything. If you ran the old command,
check your Waydroid configuration and any backups to establish the original
values before changing anything; no automatic restoration is possible without
those values. No claim is made that a property change passes root checks or
Play Integrity.

`game explain` returns 0 for a recognised message with a diagnostic action,
5 for unknown or unsupported cases. `game fix` and `game revert` return 6
when the requested operation is unavailable or fails; JSON output uses the
same exit codes as text output.

## Waydroid shell status

At upstream Waydroid commit `db7294c4c2bd43157bab94cf8a3015e22d9f1ad8`,
[`tools/helpers/lxc.py`](https://github.com/waydroid/waydroid/blob/db7294c4c2bd43157bab94cf8a3015e22d9f1ad8/tools/helpers/lxc.py)
runs the guest command without returning its exit status, and
[`tools/__init__.py`](https://github.com/waydroid/waydroid/blob/db7294c4c2bd43157bab94cf8a3015e22d9f1ad8/tools/__init__.py)
requires root for `shell`. LynxDroid uses a fixed guest wrapper with a random
status marker and argument separation. A missing marker is reported as
unavailable, never as a successful probe. This is tested with a simulated
Waydroid wrapper, not yet on a real Waydroid installation.
