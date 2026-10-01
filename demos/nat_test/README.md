# NAT traversal test

A hosted mesh over the internet: a host, a player on the host's machine and a player on a phone. The players try a
punched direct link and fall back to the host's relay; every node reports its paths, round trip and dolls every second
(`NATTEST` lines, on screen on the phone).

```bash
godot --headless --path . -- host [--port=9500] [--dtls=1] [--duration=S] [--password=P] [--end=1] [--freeze=S]
godot --headless --path . -- player --address=<host address> [--port=9500] [--dtls=1] [--relay=1] [--duration=S] [--password=P] [--freeze=S] [--takeover=PORT]
```

On a phone, export the project (Android, `INTERNET` permission) with a `nat_test.cfg` next to `project.godot` holding the
default address:

```ini
[test]
address="203.0.113.7"
port=9500
password=""
```

The app shows the address and two buttons, without and with DTLS. With DTLS, the host also needs the next port
(`--port` + 1) reachable. `--relay=1` makes a player refuse direct links, so its pairs go through the host. A host with
a shorter `--duration` than the players hands the mesh over when its time is up, which shows the host migration; with
`--end=1`, it ends the mesh instead. With `--password`, the host admits only the players that send the same one (the
phone reads it from `nat_test.cfg`). With `--freeze=S`, a node stops servicing the network after S seconds, like a hung
process: a frozen host shows how the players confirm its loss and migrate. A player with `--takeover=PORT` takes new
players on that port if it becomes the host (without DTLS: the demo gives the players no server certificate). Every
node logs why it left the mesh.

Automated runs on a phone: the app also reads `user://nat_test.cfg`, with the same keys as the command line (`role`,
`address`, `dtls`, `duration`, `password`...); with a `role`, it starts without the buttons and quits at the end. The
debug build lets `adb` write it (`run.cfg` is a local file with a `[test]` section):

```bash
adb shell run-as org.ticksynchronizer.nattest mkdir -p files
adb exec-in "run-as org.ticksynchronizer.nattest sh -c 'cat > files/nat_test.cfg'" < run.cfg
adb shell am start -n org.ticksynchronizer.nattest/com.godot.game.GodotAppLauncher
adb logcat -s godot:I | grep NATTEST
```

Remove `files/nat_test.cfg` (`adb shell run-as org.ticksynchronizer.nattest rm files/nat_test.cfg`) to get the buttons back.
