# Self-test

Runs the module's main paths on the current platform, with real ENet on localhost inside one process: a star
(`EnetStarTransport`, interpolated NPC, client event) and a hosted mesh (`EnetHostedMeshTransport`, ids from the host,
punched direct link, dolls, `TickMultiplayerPeer`). It prints one `SELFTEST` line per check, then `SELFTEST PASS` or
`SELFTEST FAIL`, and quits with exit code 0 or 1.

The engine's unit tests (`--test`) don't run on Android, so this project is how the module is validated there.

```bash
godot --headless --path .
```

On Android, export it with an Android preset (the `INTERNET` permission is needed, even for localhost sockets),
install it, and read the result with `adb logcat | grep SELFTEST`.
