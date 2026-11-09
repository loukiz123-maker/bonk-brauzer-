# Game compatibility

Error text is a clue, not proof of the underlying cause. LynxDroid cannot
promise that every game will run on Waydroid. In particular, game-specific
root, emulator and device-integrity checks may reject it. Consult the game
vendor about supported devices.

```
lynxdroid game explain "Не удалось войти на сервер: 3"
lynxdroid game fix network --yes
```

The network command only runs an ICMP ping to `1.1.1.1` and checks the Android
routing table. A successful ping does not verify DNS, TCP or the game's server;
a failure may mean ICMP is blocked. No network settings are changed. Check the
reported routes and consult the game vendor if connectivity remains unclear.
The command needs a running Waydroid session and access to its Android shell.

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
