# raylib-android kit

Working notes for whoever picks this up next (Claude or the user).

Builds raylib games into Android APKs entirely in Termux: no Gradle, no Java, no NDK download. Started 2026-10-11
(first as ~/dev/c/raygame, turned into this kit the same day). Games live next to it, e.g. ../pop.

## Use
- `./kit.sh new ../mygame`: copies template/ (starter game, game.conf with name/package/port filled in, a
  generated placeholder icon.png, build.sh + dev.sh wrappers).
- In a game dir: `./dev.sh` = hot reload (see below), `./build.sh [install]` = release APK (<NAME>.apk, also copied
  to /sdcard/Download; `install` goes through Shizuku like ../../java/clip).
- A game = game.conf, src/*.c, assets/ (optional), icon.png (optional, full-bleed square art, keep the subject in
  the middle ~60%: launchers mask the edges).

## Layout
- kit.sh: everything. Manifest generated from game.conf (NativeActivity, hasCode=false; dev adds INTERNET).
- kit/game.h: the contract. A game defines `GAME_API(sizeof(State), init, frame, reload, unload)`; the host owns
  the state block, so it survives hot reloads. Globals/statics in game code reset on every reload.
- kit/host.c: main(). Release: game code linked in. Dev (-DGAME_HOT): host + raylib only, game code dlopen'd.
  Also mutes audio on APP_CMD_LOST_FOCUS (raylib 5.5's Android backend has its music pause commented out, and
  with the loop blocked the stream buffer repeats). Mute/unmute is guarded against repeated LOST_FOCUS and logged
  ("HOST: focus ..."). Open issue 2026-10-11: the user heard no sound once after Pop was brought back from a few
  minutes in the background (via `am start`); not reproduced (shade + app switch restores 1.00 every time), logs
  were already gone. If it recurs: check the HOST lines and logcat for AAudio disconnects.
- kit/dev.py: the dev server (watch src/ + assets/, rebuild via `kit.sh code`, serve on 127.0.0.1:DEV_PORT).
- tools/icon.c -> sdk/bin/icon (built against Termux's desktop raylib, Image API only, no window): adaptive icon
  mipmaps (108dp foreground, black background) and the placeholder generator.
- template/: what `new` copies.
- sdk/ (not in git; kit.sh's fetchsdk downloads what's missing on first use): raylib-5.5 source (github tag), android_native_app_glue.{c,h} (from
  android.googlesource.com toolchain/prebuilts/ndk/r23; the platform/development path is gone), include/ =
  Khronos EGL/GLES2/KHR headers (Termux's ndk-sysroot has none). Generated: obj/libraylib.a (`rm -rf sdk/obj` to
  rebuild), syslibs/ (symlinks), bin/icon, debug.keystore (shared by all kit games: KEEP it, updates need it).

## Build facts (learned the hard way)
- Link against /system/lib64's EGL/GLESv2/android/log/OpenSLES (symlinked in sdk/syslibs). Termux's
  $PREFIX/lib/libEGL.so is Mesa/X11 (soname libEGL.so.1): wrong inside an app. They're never shipped in the APK.
- Termux's raylib package is the X11 build: only usable for the host-side tools.
- Termux's clang always adds RUNPATH=$PREFIX/lib: patchelf --remove-rpath on every .so.
- .so stored uncompressed, `zipalign -P 16`, -z max-page-size=16384 (extractNativeLibs=false).
- aapt v1 (aapt2 broken in Termux), -0 arsc. Version code = minutes since 2026-01-01.

## Networking (kit/net.h, kit/net.c)
- Part of the host, not the game code: NetRequest memory and the worker thread survive hot reloads. One worker
  thread, FIFO queue; NetFree on a pending request marks it abandoned and the worker frees it (both under qLock).
- HTTP/1.0 + Connection: close, so responses aren't chunked (dechunk is there anyway); redirects up to 5
  (303, and 301/302 after POST, become GET). Own connect with poll timeout (10 s) + SO_RCVTIMEO/SNDTIMEO 20 s.
- HTTPS: mbedTLS 3.6.x LTS (sdk/mbedtls-<ver>, built in parallel into sdk/mbedtls-obj/libmbedtls.a, default
  config, TLS 1.2 + 1.3, psa_crypto_init needed for 1.3). CA store: /apex/com.android.conscrypt/cacerts (updatable,
  Android 14+), else /system/etc/security/cacerts, parsed once. Verify REQUIRED + hostname (SNI).
- game.conf NETWORK=yes: release links real net.c + mbedTLS and adds INTERNET. Otherwise release compiles net.c with
  -DKIT_NO_NETWORK (stubs that fail with a hint, no mbedTLS in the APK). Dev always has the real one (INTERNET is
  there for the dev server) and logs a warning when NETWORK isn't yes.
- Verified 2026-10-11 on the phone (dev, via a hot-loaded test in Pop): https GET 200, http GET 200,
  http->https redirect 200, JSON POST echoed, expired.badssl.com rejected ("validity has expired"). Release with
  NETWORK=yes: INTERNET in the APK, HTTPS GET 200 (temporary app, uninstalled after).

## Hot reload
- Dev APK: lib/libgame.so (host, SONAME libgame.so, exports all raylib) + lib/libgamecode.so (fallback code).
  No assets in it: raylib's android_fopen falls back to <internalDataPath>/<path> when a file isn't in the APK,
  so the host syncs assets there and the same LoadTexture("images/x.png") works in both builds.
- Host thread polls GET /manifest (~4/s, md5 per file), downloads changed files; code goes to
  files/hot/code-<md5>.so (new name each time: dlopen dedups by path), swapped between frames. Old code is never
  dlclosed. Code .so links against the dev host's libgame.so, so NEEDED=libgame.so resolves to the loaded host.
  dlopen from the app's data dir is allowed (app namespaces always permit /data).
- stateSize changed -> unload + fresh init (game restarts). Assets changed -> reload(state, true).
- Startup waits for the first sync. Server down (3 failed polls): runs the last synced code/assets if this
  install ever synced (marker files/hot/synced), otherwise shows "waiting for the dev server" until it's up.
  (2026-10-11: opening the dev APK without ./dev.sh used to start Pop with no assets and crash in
  LoadSoundAlias on the unloaded sound; games should check IsSoundValid before aliasing.)
- Android 15 blocks network for background apps: fine, the game is in the foreground while you look at it.
- Logs without Shizuku: the dev host sets a TraceLog callback, pipes stdout/stderr, and POSTs lines to
  /log on each poll; dev.py prints them (│ prefix). Native crashes: a SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT handler
  (alt stack) walks frame pointers within the game thread's stack, POSTs "#N module +0xoff" lines to /crash, then
  restores the old handler (Android's crash dump). Dev code is built -g -fno-omit-frame-pointer, unstripped;
  dev.py keeps every served code .so in build/dev/serve/history/code-<md5>.so and runs llvm-addr2line.
  Verified 2026-10-11: printf shown, a NULL write reported as `update at src/game.c:174`.
- `kit.sh logs` (logs.sh): optional, Shizuku only, follows logcat --pid for each run + the crash buffer after.
- Host/manifest/icon changes need `./dev.sh` again (APK rebuild + reinstall); `./dev.sh serve` = server only.

## Status
- 2026-10-11: release + dev builds verified on the Termux side (APK contents, icon badging, NEEDED/SONAME,
  server: manifest, code rebuild in ~0.5 s, compile errors keep the last good code, asset change detection,
  path traversal blocked). Verified on the phone the same day with Pop: dev APK syncs code + 9 assets, music
  plays, and a code edit mid-game reloaded in ~1.3 s with the score/orbs kept (screenshots via rish screencap).
- The original single-file Pop (no kit) was verified by the user on the phone the same day.
