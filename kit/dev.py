"""Dev server for hot reload (started by `kit.sh dev`): watches the game's src/ and assets/, rebuilds the code .so
on changes and serves everything to the dev APK's host (kit/host.c) on 127.0.0.1.

  GET /manifest        "<md5> code.so" and "<md5> assets/<path>" lines; the host polls it ~4x a second
  GET /code.so         the newest code that compiled
  GET /assets/<path>   a file from the game's assets/
  POST /log            the game's log lines ("<level letter> <text>"), printed here
  POST /crash          a native crash backtrace ("#N <module> +0x<offset>"), printed with file:line
"""
import hashlib
import http.server
import os
import re
import shutil
import subprocess
import sys
import threading
import time

KIT_SH, GAME, PORT = sys.argv[1], sys.argv[2], int(sys.argv[3])
SRC, ASSETS = os.path.join(GAME, "src"), os.path.join(GAME, "assets")
SERVE = os.path.join(GAME, "build", "dev", "serve")
CODE = os.path.join(SERVE, "code.so")
HISTORY = os.path.join(SERVE, "history")  # every code .so served, by md5, to symbolize crashes in any of them
APKLIB = os.path.join(GAME, "build", "dev", "apk", "lib", "arm64-v8a")
COLORS = {"T": "2", "D": "2", "I": "2", "W": "33", "E": "31", "F": "1;31", "O": "0"}

manifest = b""
lock = threading.Lock()


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def snapshot(root):
    snap = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f.startswith("."):  # .keep etc.; aapt skips them too
                continue
            p = os.path.join(d, f)
            try:
                st = os.stat(p)
            except OSError:
                continue
            snap[p] = (st.st_mtime_ns, st.st_size)
    return snap


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def remanifest():
    global manifest
    lines = []
    if os.path.exists(CODE):
        lines.append(f"{md5(CODE)} code.so")
    for p in sorted(snapshot(ASSETS)):
        lines.append(f"{md5(p)} assets/{os.path.relpath(p, ASSETS)}")
    with lock:
        manifest = ("\n".join(lines) + "\n").encode()


def build():
    t = time.time()
    r = subprocess.run([KIT_SH, "code", GAME], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode == 0:
        os.makedirs(HISTORY, exist_ok=True)
        shutil.copy(CODE, os.path.join(HISTORY, f"code-{md5(CODE)}.so"))
        log(f"compiled in {time.time() - t:.1f}s" + (f"\n{out}" if out else ""))
        return True
    log(f"\033[31mbuild failed\033[0m (the game keeps running the last good code):\n{out}")
    return False


def symbolize(module, offset, frame):
    if re.fullmatch(r"code-[0-9a-f]{32}\.so", module):
        path = os.path.join(HISTORY, module)
    elif module in ("libgamecode.so", "libgame.so"):
        path = os.path.join(APKLIB, module)
    else:
        return ""
    if not os.path.exists(path):
        return ""
    addr = offset - 1 if frame > 0 else offset  # return addresses point after the call
    r = subprocess.run(["llvm-addr2line", "-f", "-i", "-C", "-p", "-e", path, hex(addr)],
                       capture_output=True, text=True)
    out = r.stdout.strip().replace(GAME + "/", "")
    return "" if "??" in out else out


def crash(text):
    lines = text.rstrip("\n").split("\n")
    print(f"\033[1;31m{time.strftime('%H:%M:%S')} the game crashed: {lines[0]}\033[0m")
    for line in lines[1:]:
        m = re.match(r"#(\d+) (\S+) \+0x([0-9a-f]+)", line)
        where = symbolize(m.group(2), int(m.group(3), 16), int(m.group(1))) if m else ""
        print(f"\033[31m  {line}\033[0m" + (f"  \033[1m{where}\033[0m" if where else ""))
    print("fix it, save, and open the app again: it starts with the newest code", flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode(errors="replace")
        if self.path == "/log":
            for line in body.splitlines():
                level, _, text = line.partition(" ")
                print(f"\033[{COLORS.get(level, '0')}m│ {text}\033[0m")
            sys.stdout.flush()
        elif self.path == "/crash":
            crash(body)
        self.reply(b"ok")

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/manifest":
            with lock:
                return self.reply(manifest)
        if path == "/code.so":
            log("phone loaded the new code")
            return self.file(CODE)
        if path.startswith("/assets/"):
            p = os.path.realpath(os.path.join(ASSETS, path[len("/assets/"):]))
            if p.startswith(os.path.realpath(ASSETS) + os.sep):
                log(f"phone loaded {os.path.relpath(p, ASSETS)}")
                return self.file(p)
        self.send_error(404)

    def file(self, p):
        try:
            with open(p, "rb") as f:
                self.reply(f.read())
        except OSError:
            self.send_error(404)

    def reply(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def main():
    os.makedirs(SERVE, exist_ok=True)
    build()
    remanifest()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    log(f"hot reload server on 127.0.0.1:{PORT}: save a file in src/ or assets/ and watch the phone (Ctrl+C stops)")
    src, assets = snapshot(SRC), snapshot(ASSETS)
    while True:
        time.sleep(0.3)
        s, a = snapshot(SRC), snapshot(ASSETS)
        if s != src:
            src = s
            time.sleep(0.1)  # let the editor finish writing
            if build():
                remanifest()
        if a != assets:
            assets = a
            log("assets changed")
            remanifest()


try:
    main()
except KeyboardInterrupt:
    pass
