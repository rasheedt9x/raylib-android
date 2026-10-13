#!/data/data/com.termux/files/usr/bin/bash
# raylib-android kit: builds raylib games into APKs entirely in Termux (no Gradle, no Java, no NDK download).
#   kit.sh new <dir>               new game from template/ (then: cd <dir> && ./dev.sh)
#   kit.sh build <game> [install]  release APK: <game>/<NAME>.apk (+ /sdcard/Download), install through Shizuku
#   kit.sh dev <game> [serve]      dev APK (hot reload) -> install + start it, then watch & serve src/ and assets/;
#                                  `serve` skips the APK (it's already installed)
#   kit.sh logs <game>             follow the app's logcat + crashes (needs Shizuku; dev builds don't: ./dev.sh
#                                  already shows their logs and crashes)
#   kit.sh code <game>             (used by dev.py) just the game's code .so for hot reload
# A game dir holds game.conf, src/*.c, assets/ (optional) and icon.png (optional). See NOTES.md.
set -euo pipefail
KIT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
SDK=$KIT/sdk
RAYLIB=$SDK/raylib-5.5/src
MBEDTLS_VER=3.6.7
MBEDTLS=$SDK/mbedtls-$MBEDTLS_VER
API=30
CC="clang --target=aarch64-linux-android$API"
CFLAGS="-O2 -fPIC -ffunction-sections -fdata-sections -DPLATFORM_ANDROID -DGRAPHICS_API_OPENGL_ES2 \
    -I$SDK -I$SDK/include -I$RAYLIB -I$MBEDTLS/include -I$KIT/kit"
SYSLIBS="-L$SDK/syslibs -lEGL -lGLESv2 -landroid -llog -lOpenSLES -lm -ldl"
LDFLAGS="-Wl,--gc-sections -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,--build-id"
RISH=${RISH:-$HOME/rish/rish}

usage() { sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "kit: $*" >&2; exit 1; }

loadgame() {
    GAME=$(cd "$1" 2>/dev/null && pwd) || die "no such dir: $1"
    [ -f "$GAME/game.conf" ] || die "$GAME has no game.conf"
    NAME= LABEL= PACKAGE= ORIENTATION=portrait DEV_PORT=47123 NETWORK=no LIBS= PERMS=
    # shellcheck disable=SC1091
    . "$GAME/game.conf"
    [ -n "$NAME" ] && [ -n "$PACKAGE" ] || die "game.conf needs NAME and PACKAGE"
    LABEL=${LABEL:-$NAME}
    SRCS=$(find "$GAME/src" -name '*.c' | sort)
    # extra phone libs from /system/lib64 (e.g. LIBS="jnigraphics"), linked like EGL/GLESv2
    GAMELIBS=""
    for l in $LIBS; do ln -sf "/system/lib64/lib$l.so" "$SDK/syslibs/lib$l.so"; GAMELIBS="$GAMELIBS -l$l"; done
    [ -n "$SRCS" ] || die "no .c files in $GAME/src"
}

# Once per kit: raylib + native_app_glue (rm -rf sdk/obj to rebuild), system lib links, the icon tool
# Third-party pieces, downloaded on first use (sdk/ isn't in git)
fetch() {  # <url> <dest>
    echo "downloading $(basename "$2")"
    mkdir -p "$(dirname "$2")"
    curl -fsSL "$1" -o "$2.part" && mv "$2.part" "$2" || die "download failed: $1"
}
fetchsdk() {
    if [ ! -d "$RAYLIB" ]; then
        fetch https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz "$SDK/raylib.tar.gz"
        tar xzf "$SDK/raylib.tar.gz" -C "$SDK" && rm "$SDK/raylib.tar.gz"
    fi
    # NDK's native_app_glue (googlesource serves base64 with ?format=TEXT)
    local glue=https://android.googlesource.com/toolchain/prebuilts/ndk/r23/+/refs/heads/main/sources/android/native_app_glue
    for f in c h; do
        [ -f "$SDK/android_native_app_glue.$f" ] && continue
        fetch "$glue/android_native_app_glue.$f?format=TEXT" "$SDK/glue.b64"
        base64 -d "$SDK/glue.b64" > "$SDK/android_native_app_glue.$f" && rm "$SDK/glue.b64"
    done
    # mbedTLS (LTS) for HTTPS in kit/net.c, checked against the release's sha256 file
    if [ ! -d "$MBEDTLS" ]; then
        local rel=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER
        fetch "$rel/mbedtls-$MBEDTLS_VER.tar.bz2" "$SDK/mbedtls-$MBEDTLS_VER.tar.bz2"
        fetch "$rel/mbedtls-$MBEDTLS_VER-sha256sum.txt" "$SDK/mbedtls.sha256"
        (cd "$SDK" && grep 'tar.bz2' mbedtls.sha256 | sha256sum -c --quiet -) || die "mbedTLS checksum mismatch"
        tar xjf "$SDK/mbedtls-$MBEDTLS_VER.tar.bz2" -C "$SDK" && rm "$SDK/mbedtls-$MBEDTLS_VER.tar.bz2" "$SDK/mbedtls.sha256"
    fi
    # Khronos EGL/GLES2 headers: Termux's ndk-sysroot has none
    local egl=https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api
    local gl=https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api
    for f in EGL/egl.h EGL/eglext.h EGL/eglplatform.h KHR/khrplatform.h; do
        [ -f "$SDK/include/$f" ] || fetch "$egl/$f" "$SDK/include/$f"
    done
    for f in gl2.h gl2ext.h gl2platform.h; do
        [ -f "$SDK/include/GLES2/$f" ] || fetch "$gl/GLES2/$f" "$SDK/include/GLES2/$f"
    done
}

prepare() {
    fetchsdk
    # link against the phone's own libs: Termux's $PREFIX/lib/libEGL.so etc. are its X11/Mesa stack (soname
    # libEGL.so.1, which doesn't exist inside an app). Only these get symlinked, so libc/libm/libdl stay the NDK's.
    mkdir -p "$SDK/syslibs"
    for l in EGL GLESv2 android log OpenSLES; do ln -sf /system/lib64/lib$l.so "$SDK/syslibs/lib$l.so"; done
    if [ ! -f "$SDK/obj/libraylib.a" ]; then
        mkdir -p "$SDK/obj"
        for m in rcore rshapes rtextures rtext rmodels raudio utils; do
            echo "raylib: $m"
            $CC $CFLAGS -w -c "$RAYLIB/$m.c" -o "$SDK/obj/$m.o"
        done
        $CC $CFLAGS -w -c "$SDK/android_native_app_glue.c" -o "$SDK/obj/android_native_app_glue.o"
        llvm-ar rcs "$SDK/obj/libraylib.a" "$SDK"/obj/*.o
    fi
    if [ ! -f "$SDK/mbedtls-obj/libmbedtls.a" ]; then
        echo "mbedTLS (once, about a minute)"
        mkdir -p "$SDK/mbedtls-obj"
        # shellcheck disable=SC2016
        find "$MBEDTLS/library" -name '*.c' | xargs -P "$(nproc)" -I{} sh -c \
            '$0 -O2 -fPIC -ffunction-sections -fdata-sections -w -I"$1/include" -I"$1/library" -c {} -o "$2/$(basename {} .c).o"' \
            "$CC" "$MBEDTLS" "$SDK/mbedtls-obj"
        llvm-ar rcs "$SDK/mbedtls-obj/libmbedtls.a" "$SDK"/mbedtls-obj/*.o
    fi
    if [ ! -x "$SDK/bin/icon" ] || [ "$KIT/tools/icon.c" -nt "$SDK/bin/icon" ]; then
        mkdir -p "$SDK/bin"
        clang -O2 -o "$SDK/bin/icon" "$KIT/tools/icon.c" -lraylib -lm
    fi
}

GAMEFLAGS="-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter"

# Termux's clang always adds RUNPATH=$PREFIX/lib, which means nothing inside an app
norpath() { patchelf --remove-rpath "$1"; }

# the dev build's game code: links against the dev host's libgame.so for raylib (resolved at dlopen time)
buildcode() {  # <out.so>
    local host=$GAME/build/dev/apk/lib/arm64-v8a/libgame.so
    [ -f "$host" ] || die "no dev host yet: run ./dev.sh once"
    # shellcheck disable=SC2086
    # not stripped, with debug info and frame pointers: dev.py turns crash backtraces into file:line
    $CC $CFLAGS $GAMEFLAGS -g -fno-omit-frame-pointer -shared -o "$1.tmp" $SRCS "$host" -L"$SDK/syslibs" $GAMELIBS -lm $LDFLAGS
    norpath "$1.tmp"
    mv "$1.tmp" "$1"
}

manifest() {  # <mode> <out>
    local perms="" label=$LABEL icon=""
    [ "$NETWORK" = yes ] && perms='<uses-permission android:name="android.permission.INTERNET" />'
    if [ "$1" = dev ]; then
        perms='<uses-permission android:name="android.permission.INTERNET" />'  # the dev server (127.0.0.1)
        label="$LABEL (dev)"
    fi
    local p  # game.conf PERMS="MANAGE_EXTERNAL_STORAGE ...": extra android.permission.* lines
    for p in $PERMS; do perms="$perms <uses-permission android:name=\"android.permission.$p\" />"; done
    [ -f "$GAME/icon.png" ] && icon='android:icon="@mipmap/ic_launcher"'
    cat > "$2" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<!-- generated by raylib-android/kit.sh from game.conf. No Java: Android's own NativeActivity loads
     lib/arm64-v8a/libgame.so (kit/host.c + raylib) and calls raylib's android_main -> main() -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$PACKAGE">
    <uses-sdk android:minSdkVersion="$API" android:targetSdkVersion="35" />
    <uses-feature android:glEsVersion="0x00020000" android:required="true" />
    $perms
    <application android:label="$label" $icon android:hasCode="false" android:extractNativeLibs="false"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
        <activity android:name="android.app.NativeActivity" android:exported="true"
            android:screenOrientation="$ORIENTATION" android:launchMode="singleTask"
            android:configChanges="orientation|screenSize|screenLayout|keyboard|keyboardHidden|uiMode|density">
            <meta-data android:name="android.app.lib_name" android:value="game" />
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
EOF
}

package() {  # <mode: release|dev> <out.apk>
    local mode=$1 out=$2 B=$GAME/build/$1
    local version=$(( ($(date +%s) - 1767225600) / 60 ))  # minutes since 2026-01-01: every build installs over the last
    rm -rf "$B/apk" "$B/res" && mkdir -p "$B/apk/lib/arm64-v8a"
    local lib=$B/apk/lib/arm64-v8a
    # shellcheck disable=SC2086
    # kit/net.c: real (with mbedTLS) when the game has NETWORK=yes, stubs that explain otherwise. Dev builds always
    # have it (they need INTERNET for the dev server anyway) but warn when release builds won't.
    local net=(-DKIT_NO_NETWORK) tls=()
    if [ "$NETWORK" = yes ] || [ "$mode" = dev ]; then
        net=() tls=("$SDK/mbedtls-obj/libmbedtls.a")
        [ "$NETWORK" = yes ] || net=(-DKIT_NETWORK_DEV_ONLY)
    fi
    if [ "$mode" = release ]; then
        $CC $CFLAGS $GAMEFLAGS "${net[@]}" -shared -o "$lib/libgame.so" "$KIT/kit/host.c" "$KIT/kit/net.c" $SRCS \
            -Wl,--whole-archive "$SDK/obj/libraylib.a" -Wl,--no-whole-archive "${tls[@]}" $SYSLIBS $GAMELIBS $LDFLAGS -s
    else
        # the host exports all of raylib (whole-archive) and kit/net.h for the game code .so to use
        $CC $CFLAGS $GAMEFLAGS "${net[@]}" -DGAME_HOT -DDEV_PORT="$DEV_PORT" -shared -Wl,-soname,libgame.so \
            -o "$lib/libgame.so" "$KIT/kit/host.c" "$KIT/kit/net.c" \
            -Wl,--whole-archive "$SDK/obj/libraylib.a" -Wl,--no-whole-archive "${tls[@]}" $SYSLIBS $GAMELIBS $LDFLAGS
    fi
    norpath "$lib/libgame.so"
    [ "$mode" = dev ] && buildcode "$lib/libgamecode.so"  # fallback for when the dev server isn't running

    manifest "$mode" "$B/AndroidManifest.xml"
    local args=(-M "$B/AndroidManifest.xml" -I /system/framework/framework-res.apk)
    if [ -f "$GAME/icon.png" ]; then
        mkdir -p "$B/res" && "$SDK/bin/icon" res "$GAME/icon.png" "$B/res"
        args+=(-S "$B/res")
    fi
    # release APKs carry assets/; dev ones get them from the dev server instead
    [ "$mode" = release ] && [ -n "$(find "$GAME/assets" -type f ! -name '.*' 2>/dev/null)" ] && args+=(-A "$GAME/assets")
    # aapt2 in Termux is broken (protobuf ABI mismatch), so aapt v1; -0 arsc: targetSdk 30+ wants it uncompressed
    aapt package -f -0 arsc "${args[@]}" --version-code "$version" --version-name "1.$(date +%y%m%d.%H%M)" \
        -F "$B/unsigned.apk" 2>&1 | grep -v 'ResourceType: CREATING STRING CACHE' || true
    # the .so files stay uncompressed (extractNativeLibs=false: loaded straight from the APK, page-aligned)
    (cd "$B/apk" && zip -q -r -0 ../unsigned.apk lib)
    zipalign -f -P 16 4 "$B/unsigned.apk" "$B/aligned.apk"

    # one key for every kit game: updates must be signed with the same key, so keep sdk/debug.keystore
    [ -f "$SDK/debug.keystore" ] || keytool -genkeypair -keystore "$SDK/debug.keystore" -storepass android \
        -keypass android -alias key -keyalg RSA -keysize 2048 -validity 10000 -dname CN=raylib-android >/dev/null 2>&1
    apksigner sign --v1-signing-enabled true --v2-signing-enabled true --ks "$SDK/debug.keystore" \
        --ks-pass pass:android --out "$out" "$B/aligned.apk"
    rm -f "$out.idsig"
    echo "built $out (version code $version, $(du -h "$out" | cut -f1))"
}

shizuku() { timeout 15 "$RISH" -c true >/dev/null 2>&1; }

install() {  # <apk>
    local f
    f=$(basename "$1")
    cp "$1" "/sdcard/Download/$f"
    if ! shizuku; then
        echo "Shizuku isn't running: start it in the Shizuku app, or install /sdcard/Download/$f from Files" >&2
        return 1
    fi
    # system_server can't read /sdcard (fuse): stage it in /data/local/tmp. rish exits 0 even on failure, so
    # success is pm's own "Success" line
    local log
    log=$(timeout 120 "$RISH" -c "cp /sdcard/Download/$f /data/local/tmp/$f && pm install -r /data/local/tmp/$f; rm -f /data/local/tmp/$f" 2>&1)
    echo "$log" | tail -3
    grep -q '^Success' <<<"$log"
}

cmd=${1:-}
case $cmd in
new)
    [ $# = 2 ] || usage
    [ -e "$2" ] && die "$2 already exists"
    cp -r "$KIT/template" "$2"
    name=$(basename "$2" | tr -cd 'a-zA-Z0-9_' | tr 'A-Z' 'a-z')
    port=$(( 47000 + $(cksum <<<"$name" | cut -d' ' -f1) % 900 ))
    sed -i "s/^NAME=[^ ]*/NAME=$name/; s/^LABEL=\"[^\"]*\"/LABEL=\"$(basename "$2")\"/; s/^PACKAGE=[^ ]*/PACKAGE=local.$name/;
        s/^DEV_PORT=[0-9]*/DEV_PORT=$port/" "$2/game.conf"
    for s in build dev logs; do
        printf '#!%s\n# see %s\nexec %q %s "$(dirname "$0")" "$@"\n' "$(command -v bash)" "$KIT/NOTES.md" "$KIT/kit.sh" $s \
            > "$2/$s.sh"
        chmod +x "$2/$s.sh"
    done
    prepare
    "$SDK/bin/icon" placeholder "$2/icon.png" "$name"
    echo "new game in $2: edit game.conf / src/game.c, then ./dev.sh (hot reload) or ./build.sh (release)"
    ;;
build)
    [ $# -ge 2 ] || usage
    loadgame "$2"
    prepare
    package release "$GAME/$NAME.apk"
    cp "$GAME/$NAME.apk" /sdcard/Download/
    if [ "${3:-}" = install ]; then install "$GAME/$NAME.apk"; fi
    ;;
dev)
    [ $# -ge 2 ] || usage
    loadgame "$2"
    prepare
    if [ "${3:-}" != serve ]; then
        package dev "$GAME/build/dev/$NAME-dev.apk"
        if install "$GAME/build/dev/$NAME-dev.apk"; then
            "$RISH" -c "am start -S -n $PACKAGE/android.app.NativeActivity" >/dev/null 2>&1 || true
        else
            echo "after installing it, open the app: it connects to this server by itself" >&2
        fi
    fi
    exec python3 "$KIT/kit/dev.py" "$KIT/kit.sh" "$GAME" "$DEV_PORT"
    ;;
logs)
    [ $# = 2 ] || usage
    loadgame "$2"
    shizuku || die "needs Shizuku (rish) to read logcat. Dev builds don't: ./dev.sh shows their logs and crashes."
    echo "following $PACKAGE (Ctrl+C stops)"
    # on the phone, as the shell user: follow each run of the app, and after it dies print the crash buffer
    exec "$RISH" -c "
        pid=
        while :; do
            p=\$(pidof $PACKAGE)
            if [ -n \"\$p\" ] && [ \"\$p\" != \"\$pid\" ]; then
                pid=\$p; since=\$(date +'%m-%d %H:%M:%S.000')
                echo \"--- $PACKAGE started (pid \$p) ---\"
                logcat -v color -v brief -T 1 --pid=\$p & lc=\$!
                while [ -d /proc/\$p ]; do sleep 1; done
                kill \$lc 2>/dev/null
                echo \"--- $PACKAGE exited ---\"
                logcat -d -b crash -v brief -T \"\$since\" 2>/dev/null
            fi
            sleep 1
        done"
    ;;
code)
    [ $# = 2 ] || usage
    loadgame "$2"
    mkdir -p "$GAME/build/dev/serve"
    buildcode "$GAME/build/dev/serve/code.so"
    ;;
*) usage ;;
esac
