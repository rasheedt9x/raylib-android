// The kit's main(): window, audio, main loop, and in dev builds (-DGAME_HOT) hot reloading.
//
// Release: the game's code is linked into libgame.so with this file and uses `game_api` directly.
// Dev: libgame.so is this file + raylib only; the game's code is a separate .so (libgamecode.so in the APK, then
// whatever the dev server sends) that resolves raylib from libgame.so. A thread polls the dev server (dev.py, in
// Termux, 127.0.0.1:DEV_PORT) for its manifest, downloads changed files into the app's files dir, and the main
// loop swaps them in between frames. Assets aren't in the dev APK at all: raylib's Android fopen falls back to
// <files dir>/<path> when a file isn't in the APK, which is where the synced assets land.
#include "game.h"
#include "android_native_app_glue.h"
#include <stdlib.h>
#include <string.h>

struct android_app *GetAndroidApp(void);  // (raylib's)

// raylib keeps the audio device running when the app goes to the background (its Pause/ResumeMusicStream calls
// are commented out), and with the game loop blocked the music buffer repeats: mute while unfocused.
static void (*raylibOnAppCmd)(struct android_app *, int32_t);
// Android can send LOST_FOCUS twice in a row (notification shade, then another app): only the first one saves the
// volume, or the second would save the muted 0 and "restore" silence.
static float volume = 1;
static bool muted;
static void onAppCmd(struct android_app *app, int32_t cmd)
{
    raylibOnAppCmd(app, cmd);
    if (cmd == APP_CMD_LOST_FOCUS && !muted) {
        volume = GetMasterVolume();
        SetMasterVolume(0);
        muted = true;
        TraceLog(LOG_INFO, "HOST: focus lost, audio muted (volume was %.2f)", volume);
    } else if (cmd == APP_CMD_GAINED_FOCUS && muted) {
        SetMasterVolume(volume);
        muted = false;
        TraceLog(LOG_INFO, "HOST: focus back, volume %.2f", volume);
    } else if (cmd == APP_CMD_LOST_FOCUS || cmd == APP_CMD_GAINED_FOCUS)
        TraceLog(LOG_INFO, "HOST: focus %s again (ignored)", cmd == APP_CMD_LOST_FOCUS ? "lost" : "gained");
}

static void start(void)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(0, 0, "");  // 0x0 = the whole screen
    InitAudioDevice();
    raylibOnAppCmd = GetAndroidApp()->onAppCmd;
    GetAndroidApp()->onAppCmd = onAppCmd;
}

#ifndef GAME_HOT

extern const GameApi game_api;

int main(void)
{
    start();
    void *state = calloc(1, game_api.stateSize);
    game_api.init(state);
    while (!WindowShouldClose()) game_api.frame(state);
    game_api.unload(state);
    free(state);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

#else

#include <android/log.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

static const char *filesDir;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char pendingCode[512];          // newest downloaded code .so, "" = none
static bool pendingAssets;
static atomic_bool synced;             // a full sync finished in this run
static atomic_bool offline;            // the server didn't answer (3 tries)

static void sleepMs(int ms) { nanosleep(&(struct timespec){ ms / 1000, (ms % 1000) * 1000000L }, NULL); }

// A request to the dev server; returns the malloc'd response body (NUL-terminated) or NULL
static char *httpRequest(const char *method, const char *path, const char *body, int bodyLen, int *len)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DEV_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char *buf = NULL;
    int n = 0, cap = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        char req[600];
        int rl = snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nContent-Length: %d\r\n\r\n", method, path, bodyLen);
        if (write(fd, req, rl) == rl && (bodyLen == 0 || write(fd, body, bodyLen) == bodyLen)) {
            for (;;) {
                if (n + 65536 > cap) buf = realloc(buf, cap = cap * 2 + 65536);
                int r = read(fd, buf + n, cap - n - 1);
                if (r <= 0) break;
                n += r;
            }
        }
    }
    close(fd);
    char *resp = buf ? strstr(buf, "\r\n\r\n") : NULL;
    if (!resp || n < 12 || memcmp(buf + 9, "200", 3) != 0) { free(buf); return NULL; }
    resp += 4;
    *len = n - (int)(resp - buf);
    memmove(buf, resp, *len);
    buf[*len] = 0;
    return buf;
}

static char *httpGet(const char *path, int *len) { return httpRequest("GET", path, NULL, 0, len); }

// ---- logs: raylib's TraceLog, stdout/stderr (printf) and crashes go to the dev server, so ./dev.sh shows them
// without Shizuku/logcat. Everything still goes to logcat too.

static pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;
static char *logBuf;
static int logLen;
#define LOG_CAP (256 * 1024)  // while the server is away; older lines are dropped past this

static void logLine(char level, const char *msg)
{
    pthread_mutex_lock(&logLock);
    int n = (int)strlen(msg);
    if (!logBuf) logBuf = malloc(LOG_CAP);
    if (logLen + n + 3 > LOG_CAP) logLen = 0;
    logBuf[logLen++] = level;
    logBuf[logLen++] = ' ';
    memcpy(logBuf + logLen, msg, n);
    logLen += n;
    logBuf[logLen++] = '\n';
    pthread_mutex_unlock(&logLock);
}

static void traceLog(int level, const char *fmt, va_list args)
{
    static const char letters[] = "?TDIWEF";
    static const int prio[] = { ANDROID_LOG_UNKNOWN, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL };
    if (level < 0 || level > 6) level = 0;
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, args);
    __android_log_write(prio[level], "raylib", msg);
    logLine(letters[level], msg);
}

static void *pipeThread(void *arg)  // stdout/stderr -> log lines
{
    int fd = (int)(intptr_t)arg, n = 0;
    char line[1024];
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) return NULL;
        if (c != '\n' && n < (int)sizeof line - 1) { line[n++] = c; continue; }
        line[n] = 0;
        n = 0;
        __android_log_write(ANDROID_LOG_INFO, "stdout", line);
        logLine('O', line);
    }
}

static void flushLogs(void)
{
    pthread_mutex_lock(&logLock);
    int n = logLen;
    char *copy = n ? memcpy(malloc(n), logBuf, n) : NULL;
    pthread_mutex_unlock(&logLock);
    if (!n) return;
    int len;
    char *r = httpRequest("POST", "/log", copy, n, &len);
    if (r) {  // sent: drop those bytes (more may have arrived meanwhile)
        pthread_mutex_lock(&logLock);
        if (logLen >= n) memmove(logBuf, logBuf + n, logLen -= n);
        pthread_mutex_unlock(&logLock);
        free(r);
    }
    free(copy);
}

// Native crash: send the signal and a frame-pointer backtrace (module + offset; dev.py turns those into
// file:line), then hand the signal to the previous handler (Android's crash dump).
static struct sigaction oldActions[NSIG];
static uintptr_t stackLo, stackHi;  // the game thread's stack, to walk frame pointers safely

static void onCrash(int sig, siginfo_t *si, void *ucv)
{
    static atomic_bool inCrash;
    if (!atomic_exchange(&inCrash, true)) {
        ucontext_t *uc = ucv;
        uintptr_t pcs[32];
        int np = 0;
        pcs[np++] = uc->uc_mcontext.pc;
        pcs[np++] = uc->uc_mcontext.regs[30];  // lr: the caller, if the crash was in a leaf function
        uintptr_t fp = uc->uc_mcontext.regs[29];
        while (np < 32 && fp >= stackLo && fp + 16 <= stackHi && !(fp & 7)) {
            uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
            if (!ret) break;
            if (ret != pcs[np - 1]) pcs[np++] = ret;
            if (next <= fp) break;
            fp = next;
        }
        char msg[8192];
        int n = snprintf(msg, sizeof msg, "%s (signal %d), fault address %p, thread %d\n", strsignal(sig), sig,
                         si->si_addr, gettid());
        for (int i = 0; i < np && n < (int)sizeof msg - 300; i++) {
            Dl_info d;
            if (dladdr((void *)pcs[i], &d) && d.dli_fname) {
                const char *base = strrchr(d.dli_fname, '/');
                n += snprintf(msg + n, sizeof msg - n, "#%d %s +0x%lx%s%s\n", i, base ? base + 1 : d.dli_fname,
                              (unsigned long)(pcs[i] - (uintptr_t)d.dli_fbase), d.dli_sname ? " " : "",
                              d.dli_sname ? d.dli_sname : "");
            } else n += snprintf(msg + n, sizeof msg - n, "#%d %p\n", i, (void *)pcs[i]);
        }
        __android_log_write(ANDROID_LOG_FATAL, "raylib", msg);
        int len;
        if (pthread_mutex_trylock(&logLock) == 0) {  // the last log lines first, so they read in order
            if (logLen) free(httpRequest("POST", "/log", logBuf, logLen, &len));
            logLen = 0;
            pthread_mutex_unlock(&logLock);
        }
        free(httpRequest("POST", "/crash", msg, n, &len));
    }
    sigaction(sig, &oldActions[sig], NULL);  // returning re-runs the fault, now with Android's handler
}

static void startLogs(void)
{
    SetTraceLogCallback(traceLog);
    int p[2];
    if (pipe(p) == 0) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        pthread_t t;
        pthread_create(&t, NULL, pipeThread, (void *)(intptr_t)p[0]);
    }
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *lo;
        size_t size;
        pthread_attr_getstack(&attr, &lo, &size);
        stackLo = (uintptr_t)lo;
        stackHi = stackLo + size;
        pthread_attr_destroy(&attr);
    }
    static char altStack[64 * 1024];  // stack overflows crash on the normal stack
    sigaltstack(&(stack_t){ .ss_sp = altStack, .ss_size = sizeof altStack }, NULL);
    struct sigaction sa = { .sa_sigaction = onCrash, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
    for (int i = 0; i < 6; i++) sigaction(sigs[i], &sa, &oldActions[sigs[i]]);
}

static void mkdirs(char *path)  // creates the parent dirs of path
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0700); *p = '/'; }
}

static bool fetchTo(const char *url, const char *dest)
{
    int len;
    char *data = httpGet(url, &len);
    if (!data) return false;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", dest);
    mkdirs(tmp);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, len, f) == (size_t)len;
    if (f) fclose(f);
    free(data);
    return ok && rename(tmp, dest) == 0;
}

// Manifest lines: "<md5> code.so" and "<md5> assets/<path>". A line that wasn't in the last manifest = a changed file.
static void *syncThread(void *arg)
{
    (void)arg;
    char *last = strdup("");
    int fails = 0;
    for (;;) {
        flushLogs();
        int len;
        char *man = httpGet("/manifest", &len);
        if (!man) {
            if (++fails >= 3) atomic_store(&offline, true);
            sleepMs(500);
            continue;
        }
        fails = 0;
        atomic_store(&offline, false);
        if (strcmp(man, last) == 0) { free(man); atomic_store(&synced, true); sleepMs(250); continue; }
        char code[512] = "";
        bool assets = false, ok = true;
        char *save, *copy = strdup(man);
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char find[600];
            snprintf(find, sizeof find, "%s\n", line);
            if (strstr(last, find)) continue;
            char *path = strchr(line, ' ');
            if (!path) continue;
            *path++ = 0;
            char dest[512], url[600];
            snprintf(url, sizeof url, "/%s", path);
            if (strcmp(path, "code.so") == 0) {
                // a new name every time: dlopen returns the already-loaded library for a path it has seen
                snprintf(dest, sizeof dest, "%s/hot/code-%s.so", filesDir, line);
                if ((ok = fetchTo(url, dest))) strcpy(code, dest);
            } else if (strncmp(path, "assets/", 7) == 0) {
                snprintf(dest, sizeof dest, "%s/%s", filesDir, path + 7);
                if ((ok = fetchTo(url, dest))) assets = true;
            }
            if (!ok) break;
        }
        free(copy);
        if (ok) { free(last); last = man; } else free(man);  // on a failed fetch, retry the whole manifest
        pthread_mutex_lock(&lock);
        if (code[0]) strcpy(pendingCode, code);
        pendingAssets |= assets;
        pthread_mutex_unlock(&lock);
        if (ok && !atomic_exchange(&synced, true)) {
            // remember that this install has assets, so it can start without the server next time
            char mark[512];
            snprintf(mark, sizeof mark, "%s/hot/synced", filesDir);
            mkdirs(mark);
            FILE *f = fopen(mark, "w");
            if (f) fclose(f);
        }
        sleepMs(250);
    }
    return NULL;
}

static const GameApi *loadCode(const char *path)
{
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);  // never dlclosed: the old code may still be referenced
    const GameApi *api = lib ? dlsym(lib, "game_api") : NULL;
    if (!api) TraceLog(LOG_ERROR, "HOT: can't load %s: %s", path, dlerror());
    else TraceLog(LOG_INFO, "HOT: loaded %s", path);
    return api;
}

int main(void)
{
    startLogs();  // before start(), so raylib's init messages are included
    start();
    filesDir = GetAndroidApp()->activity->internalDataPath;

    pthread_t t;
    pthread_create(&t, NULL, syncThread, NULL);
    // Wait for the first sync, so the game starts with the newest code and assets. Without the server, start
    // with the last synced ones; if there never was a sync (no assets yet), wait for the server.
    char mark[512];
    snprintf(mark, sizeof mark, "%s/hot/synced", filesDir);
    bool hadSync = access(mark, F_OK) == 0;
    while (!atomic_load(&synced) && !(atomic_load(&offline) && hadSync) && !WindowShouldClose()) {
        int u = GetScreenWidth() / 100;
        BeginDrawing();
        ClearBackground(BLACK);
        if (!atomic_load(&offline)) DrawText("syncing with dev server...", 6 * u, 10 * u, 5 * u, GRAY);
        else {
            DrawText("waiting for the dev server", 6 * u, 10 * u, 5 * u, RAYWHITE);
            DrawText("in Termux, run ./dev.sh in the game's folder", 6 * u, 18 * u, 4 * u, GRAY);
            DrawText(TextFormat("(127.0.0.1:%d)", DEV_PORT), 6 * u, 24 * u, 4 * u, DARKGRAY);
        }
        EndDrawing();
    }
    pthread_mutex_lock(&lock);
    const GameApi *api = pendingCode[0] ? loadCode(pendingCode) : NULL;
    pendingCode[0] = 0;
    pendingAssets = false;
    pthread_mutex_unlock(&lock);
    if (!api) api = loadCode("libgamecode.so");  // the one built into the APK
    while (!api && !WindowShouldClose()) {  // say so instead of just closing
        BeginDrawing();
        ClearBackground(BLACK);
        DrawText("couldn't load the game code (see logcat, tag raylib)", 40, 80, 40, RED);
        EndDrawing();
    }
    if (!api) return 1;

    int size = api->stateSize;
    void *state = calloc(1, size);
    api->init(state);
    while (!WindowShouldClose()) {
        pthread_mutex_lock(&lock);
        char code[512];
        strcpy(code, pendingCode);
        bool assets = pendingAssets;
        pendingCode[0] = 0;
        pendingAssets = false;
        pthread_mutex_unlock(&lock);

        const GameApi *next = code[0] ? loadCode(code) : NULL;
        if (next && next->stateSize != size) {
            TraceLog(LOG_INFO, "HOT: state size %d -> %d, restarting the game", size, next->stateSize);
            api->unload(state);
            free(state);
            state = calloc(1, size = next->stateSize);
            (api = next)->init(state);
        } else if (next || assets) {
            if (next) api = next;
            api->reload(state, assets);
        }
        api->frame(state);
    }
    api->unload(state);
    free(state);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

#endif
