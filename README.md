# raylib-android

Make Android games in C with [raylib](https://www.raylib.com), entirely on your phone in
[Termux](https://termux.dev). There's no Gradle, no Android Studio, no Java and no NDK download.

- **One command to a new game**: `kit.sh new mygame` gives you a starter game, an icon and build scripts.
- **Hot reload on the phone**: save a `.c` file and the running game swaps in the new code in about a second, with
  its state kept. Changed images and sounds reload too.
- **Logs and crashes in your terminal**: `printf`, `TraceLog` and native crashes (with `file:line` backtraces)
  show up in Termux. No adb, no Shizuku, no Wi-Fi.
- **HTTP and HTTPS that don't freeze the game**: `NetGet`/`NetPost` run in the background, with mbedTLS and the
  phone's own trusted certificates.
- **Real APKs**: release builds are normal signed APKs with your icon and assets.

Example game: [pop](https://github.com/rasheedt9x/pop).

## Setup

arm64 Android 11+ (API 30) with Termux:

```sh
pkg install clang aapt apksigner patchelf python openjdk-21 raylib curl
git clone https://github.com/rasheedt9x/raylib-android
```

The first build downloads raylib 5.5, mbedTLS 3.6 (checksum-verified), the NDK's `native_app_glue` and the Khronos
GLES/EGL headers into `sdk/`, then compiles them once (about a minute). Termux's `raylib` package is only used by the icon tool.

## Use

```sh
raylib-android/kit.sh new mygame   # starter game (tap to throw bouncing balls)
cd mygame
./dev.sh          # build + install the dev APK, then hot reload src/ and assets/ (keep it running)
./build.sh        # release APK: mygame.apk (also copied to Downloads)
```

Without Shizuku, install the APK from Downloads in your file manager. The dev APK only needs reinstalling when
`game.conf`, `icon.png` or the kit changes, not for code or asset edits. With
[Shizuku](https://shizuku.rikka.app) and `rish` (at `~/rish/rish`), `./dev.sh` and `./build.sh install` install
without the installer screen, and `./logs.sh` follows the app's logcat (useful for release builds).

A game folder:

| | |
|---|---|
| `game.conf` | name, label, package id, orientation, dev server port, `NETWORK=yes` for internet access |
| `src/*.c` | your code; it defines one `GAME_API(...)` (see `kit/game.h`) |
| `assets/` | images, sounds, fonts... loaded by relative path: `LoadTexture("images/player.png")` |
| `icon.png` | square, full-bleed; keep the subject in the middle ~60% (launchers crop the edges) |

## Network requests

```c
NetRequest *r = NetGet("https://api.example.com/scores");   // returns right away
// later, each frame:
if (r && NetDone(r)) {
    if (NetStatus(r) == 200) parse(NetBody(r, NULL));
    else TraceLog(LOG_WARNING, "failed: %s", NetError(r));
    NetFree(r);
    r = NULL;
}
```

There's also `NetPost(url, contentType, body, len)` and `NetSend(method, url, headers, body, len)`. Requests run
one at a time on a background thread, follow redirects and time out after about 20 seconds. Release APKs need
`NETWORK=yes` in `game.conf`. Dev builds always have internet access, and they warn in the log when a release
build wouldn't. See `kit/net.h`.

## How it works

- The APK has no Java: Android's built-in `NativeActivity` loads `lib/arm64-v8a/libgame.so` (raylib +
  `kit/host.c` + your code), which is linked against the phone's own EGL/GLES libraries.
- Packaging is `aapt` + `zipalign` + `apksigner`, all from Termux packages.
- Dev builds split it: `libgame.so` is raylib + the host, and your code is a separate `.so`. `kit/dev.py` watches
  your files, rebuilds the code in under a second and serves it on `127.0.0.1`. The host downloads it between
  frames and `dlopen`s it. Your state lives in one struct the host owns, so it survives the swap.
- Assets aren't in the dev APK. The host syncs them into the app's files folder, where raylib looks when a file
  isn't in the APK, so the same paths work in both builds.

`NOTES.md` has the details and the gotchas.

## Limits

- arm64 only, OpenGL ES 2.
- Hot reload resets globals and statics in your code: keep what must last in the state struct. Adding or removing
  fields restarts the game.
- One shared debug signing key in `sdk/debug.keystore` (created on first build). Back it up: updates must be signed
  with the same key.
