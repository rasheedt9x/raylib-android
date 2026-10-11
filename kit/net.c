// kit/net.h: a small HTTP/1.0 client on one background thread, with mbedTLS for HTTPS. Part of the host
// (libgame.so), so requests and their memory outlive hot reloads of the game code.
// Built with -DKIT_NO_NETWORK for release games without NETWORK=yes: then every request fails with a hint.
#include "net.h"
#include "raylib.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct NetRequest {
    char *method, *url, *headers, *body;
    int bodyLen;
    atomic_bool done, abandoned;
    int status;
    char *resp;
    int respLen;
    char error[256];
    NetRequest *next;
};

static NetRequest *newRequest(const char *method, const char *url, const char *headers, const void *body, int len)
{
    NetRequest *r = calloc(1, sizeof *r);
    r->method = strdup(method);
    r->url = strdup(url);
    r->headers = strdup(headers ? headers : "");
    r->body = malloc(len + 1);
    if (len) memcpy(r->body, body, len);
    r->bodyLen = len;
    r->resp = strdup("");
    return r;
}

static void freeRequest(NetRequest *r)
{
    free(r->method); free(r->url); free(r->headers); free(r->body); free(r->resp);
    free(r);
}

bool NetDone(NetRequest *r) { return r && atomic_load(&r->done); }
int NetStatus(NetRequest *r) { return NetDone(r) ? r->status : 0; }
const char *NetError(NetRequest *r) { return NetDone(r) ? r->error : ""; }
const char *NetBody(NetRequest *r, int *len)
{
    bool d = NetDone(r);
    if (len) *len = d ? r->respLen : 0;
    return d ? r->resp : "";
}

NetRequest *NetGet(const char *url) { return NetSend("GET", url, NULL, NULL, 0); }
NetRequest *NetPost(const char *url, const char *contentType, const void *body, int bodyLen)
{
    char h[256];
    snprintf(h, sizeof h, "Content-Type: %s", contentType ? contentType : "application/octet-stream");
    return NetSend("POST", url, h, body, bodyLen);
}

#ifdef KIT_NO_NETWORK

NetRequest *NetSend(const char *method, const char *url, const char *headers, const void *body, int bodyLen)
{
    NetRequest *r = newRequest(method, url, headers, body, bodyLen);
    r->status = -1;
    snprintf(r->error, sizeof r->error, "networking is off: add NETWORK=yes to game.conf");
    TraceLog(LOG_WARNING, "NET: %s %s: %s", method, url, r->error);
    atomic_store(&r->done, true);
    return r;
}

void NetFree(NetRequest *r) { if (r) freeRequest(r); }

#else

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"  // MBEDTLS_ERR_NET_* codes (its socket functions aren't used)
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define TIMEOUT_MS 20000
#define MAX_RESPONSE (32 << 20)

static pthread_mutex_t qLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qCond = PTHREAD_COND_INITIALIZER;
static NetRequest *qHead, *qTail;
static bool started;

typedef struct { char host[256], port[8], path[2048]; bool tls; } Url;

static bool parseUrl(const char *s, Url *u, char *err)
{
    memset(u, 0, sizeof *u);
    if (!strncasecmp(s, "https://", 8)) { u->tls = true; s += 8; }
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    else { strcpy(err, "the URL must start with http:// or https://"); return false; }
    const char *end = s + strcspn(s, "/?#"), *colon = NULL;
    if (*s == '[') {  // [ipv6]:port
        const char *close = memchr(s, ']', end - s);
        if (!close) { strcpy(err, "bad URL"); return false; }
        snprintf(u->host, sizeof u->host, "%.*s", (int)(close - s - 1), s + 1);
        if (close[1] == ':') colon = close + 1;
    } else {
        colon = memchr(s, ':', end - s);
        snprintf(u->host, sizeof u->host, "%.*s", (int)((colon ? colon : end) - s), s);
    }
    if (colon) snprintf(u->port, sizeof u->port, "%.*s", (int)(end - colon - 1), colon + 1);
    else strcpy(u->port, u->tls ? "443" : "80");
    snprintf(u->path, sizeof u->path, "%s%s", *end == '/' ? "" : "/", end);
    char *hash = strchr(u->path, '#');
    if (hash) *hash = 0;
    if (!u->host[0]) { strcpy(err, "bad URL: no host"); return false; }
    return true;
}

static int connectTo(const Url *u, char *err)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    int rc = getaddrinfo(u->host, u->port, &hints, &res);
    if (rc) {
        snprintf(err, 256, "can't resolve %s: %s", u->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) {
            if (errno == EACCES) snprintf(err, 256, "no INTERNET permission: add NETWORK=yes to game.conf");
            else snprintf(err, 256, "socket: %s", strerror(errno));
            break;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) {
            struct pollfd p = { fd, POLLOUT, 0 };
            int soerr = 0;
            socklen_t l = sizeof soerr;
            if (poll(&p, 1, 10000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l) == 0 && soerr == 0) {
                fcntl(fd, F_SETFL, 0);
                struct timeval tv = { TIMEOUT_MS / 1000, 0 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                break;
            }
            snprintf(err, 256, "can't connect to %s:%s: %s", u->host, u->port, soerr ? strerror(soerr) : "timed out");
        } else snprintf(err, 256, "can't connect to %s:%s: %s", u->host, u->port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ---- TLS: one config for every connection, with the phone's CA store

static mbedtls_ssl_config tlsConf;
static mbedtls_x509_crt caChain;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_entropy_context entropy;
static bool tlsReady;

static bool tlsInit(char *err)
{
    if (tlsReady) return true;
    psa_crypto_init();  // TLS 1.3 needs PSA
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&caChain);
    mbedtls_ssl_config_init(&tlsConf);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"raylib-android", 14)) {
        strcpy(err, "TLS: can't seed the random generator");
        return false;
    }
    // the updatable CA store (Android 14+), then the system one. Files that aren't certificates are skipped.
    const char *dirs[] = { "/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts" };
    for (int i = 0; i < 2 && caChain.version == 0; i++) mbedtls_x509_crt_parse_path(&caChain, dirs[i]);
    if (caChain.version == 0) { strcpy(err, "TLS: no CA certificates found"); return false; }
    mbedtls_ssl_config_defaults(&tlsConf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&tlsConf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tlsConf, &caChain, NULL);
    mbedtls_ssl_conf_rng(&tlsConf, mbedtls_ctr_drbg_random, &drbg);
    tlsReady = true;
    return true;
}

static int bioSend(void *ctx, const unsigned char *b, size_t n)
{
    ssize_t r = send(*(int *)ctx, b, n, MSG_NOSIGNAL);
    return r >= 0 ? (int)r : (errno == EAGAIN ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED);
}

static int bioRecv(void *ctx, unsigned char *b, size_t n)
{
    ssize_t r = recv(*(int *)ctx, b, n, 0);
    return r >= 0 ? (int)r : (errno == EAGAIN ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED);
}

typedef struct { int fd; bool tls; mbedtls_ssl_context ssl; } Conn;

static void tlsError(char *err, const char *what, int rc)
{
    char m[160];
    mbedtls_strerror(rc, m, sizeof m);
    snprintf(err, 256, "TLS %s: %s", what, m);
}

static bool connWrite(Conn *c, const char *b, int n, char *err)
{
    while (n > 0) {
        int r = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)b, n) : (int)send(c->fd, b, n, MSG_NOSIGNAL);
        if (c->tls && (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ)) continue;
        if (r <= 0) {
            if (c->tls) tlsError(err, "write", r);
            else snprintf(err, 256, "send: %s", strerror(errno));
            return false;
        }
        b += r;
        n -= r;
    }
    return true;
}

static int connRead(Conn *c, char *b, int n)  // >0 bytes, 0 = end, <0 = error
{
    for (;;) {
        if (!c->tls) return (int)recv(c->fd, b, n, 0);
        int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
            r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
}

// ---- HTTP

static char *findHeader(char *head, const char *name)  // value of a response header, or NULL
{
    size_t n = strlen(name);
    for (char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n"))
        if (!strncasecmp(l + 2, name, n) && l[2 + n] == ':') {
            char *v = l + 3 + n;
            while (*v == ' ') v++;
            return v;
        }
    return NULL;
}

static int dechunk(char *b, int n)  // in place; returns the new length
{
    char *in = b, *out = b, *end = b + n;
    while (in < end) {
        char *nl = strstr(in, "\r\n");
        if (!nl) break;
        long size = strtol(in, NULL, 16);
        in = nl + 2;
        if (size <= 0 || in + size > end) break;
        memmove(out, in, size);
        out += size;
        in += size + 2;
    }
    return (int)(out - b);
}

// One request/response. Returns false with r->error set on failure; *location = redirect target or "".
static bool fetch(NetRequest *r, const char *method, const char *url, char *location)
{
    Url u;
    location[0] = 0;
    if (!parseUrl(url, &u, r->error)) return false;
    if (u.tls && !tlsInit(r->error)) return false;
    Conn c = { .fd = connectTo(&u, r->error), .tls = u.tls };
    if (c.fd < 0) return false;
    bool ok = false;
    char *buf = NULL;
    if (c.tls) {
        mbedtls_ssl_init(&c.ssl);
        mbedtls_ssl_setup(&c.ssl, &tlsConf);
        mbedtls_ssl_set_hostname(&c.ssl, u.host);  // SNI + the name the certificate must match
        mbedtls_ssl_set_bio(&c.ssl, &c.fd, bioSend, bioRecv, NULL);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&c.ssl)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
            if (flags && flags != (uint32_t)-1) {
                char info[200];
                mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
                info[strcspn(info, "\n")] = 0;
                snprintf(r->error, sizeof r->error, "TLS: %s's certificate isn't trusted: %s", u.host, info);
            } else tlsError(r->error, "handshake", rc);
            goto out;
        }
    }

    // request: HTTP/1.0, so the response is never chunked and the server closes the connection when it's done
    bool hasBody = r->bodyLen > 0 || strcmp(method, "GET") != 0;
    int hl = (int)strlen(r->headers);
    char *head = malloc(4096 + hl * 2);
    int n = sprintf(head, "%s %s HTTP/1.0\r\nHost: %s", method, u.path, u.host);
    if (strcmp(u.port, u.tls ? "443" : "80")) n += sprintf(head + n, ":%s", u.port);
    n += sprintf(head + n, "\r\nUser-Agent: raylib-android\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
    for (const char *h = r->headers; *h;) {  // the caller's headers, one per line
        int l = (int)strcspn(h, "\r\n");
        if (l) n += sprintf(head + n, "%.*s\r\n", l, h);
        h += l;
        h += strspn(h, "\r\n");
    }
    if (hasBody && strcmp(method, "GET")) n += sprintf(head + n, "Content-Length: %d\r\n", r->bodyLen);
    n += sprintf(head + n, "\r\n");
    bool sent = connWrite(&c, head, n, r->error) && (!r->bodyLen || connWrite(&c, r->body, r->bodyLen, r->error));
    free(head);
    if (!sent) goto out;

    int cap = 65536, len = 0, rd;
    buf = malloc(cap + 1);
    while ((rd = connRead(&c, buf + len, cap - len)) > 0) {
        len += rd;
        if (len == cap) {
            if (cap >= MAX_RESPONSE) { strcpy(r->error, "response too big"); goto out; }
            buf = realloc(buf, (cap *= 2) + 1);
        }
    }
    buf[len] = 0;
    char *bodyStart = strstr(buf, "\r\n\r\n");
    if (!bodyStart || strncmp(buf, "HTTP/", 5)) {
        if (rd < 0 && c.tls) tlsError(r->error, "read", rd);
        else snprintf(r->error, sizeof r->error, rd < 0 ? "read: %s" : "the server closed the connection",
                      strerror(errno));
        goto out;
    }
    *bodyStart = 0;  // headers are now a string of their own
    bodyStart += 4;
    r->status = atoi(strchr(buf, ' ') ? strchr(buf, ' ') + 1 : "0");
    char *loc = findHeader(buf, "Location");
    if (loc && r->status / 100 == 3) {
        int l = (int)strcspn(loc, "\r\n");
        if (!strncasecmp(loc, "http", 4)) snprintf(location, 2048, "%.*s", l, loc);
        else snprintf(location, 2048, "%s://%s:%s%.*s", u.tls ? "https" : "http", u.host, u.port, l, loc);
    }
    int bodyLen = len - (int)(bodyStart - buf);
    char *te = findHeader(buf, "Transfer-Encoding");
    if (te && !strncasecmp(te, "chunked", 7)) bodyLen = dechunk(bodyStart, bodyLen);
    char *cl = findHeader(buf, "Content-Length");
    if (cl && atoi(cl) < bodyLen) bodyLen = atoi(cl);
    free(r->resp);
    r->resp = malloc(bodyLen + 1);
    memcpy(r->resp, bodyStart, bodyLen);
    r->resp[bodyLen] = 0;
    r->respLen = bodyLen;
    ok = true;
out:
    free(buf);
    if (c.tls) {
        mbedtls_ssl_close_notify(&c.ssl);
        mbedtls_ssl_free(&c.ssl);
    }
    close(c.fd);
    return ok;
}

static void perform(NetRequest *r)
{
    char url[2048], location[2048];
    char method[16];
    snprintf(url, sizeof url, "%s", r->url);
    snprintf(method, sizeof method, "%s", r->method);
    for (int hops = 0;; hops++) {
        if (!fetch(r, method, url, location)) { r->status = -1; break; }
        if (!location[0]) break;
        if (hops == 5) { r->status = -1; strcpy(r->error, "too many redirects"); break; }
        if (r->status == 303 || ((r->status == 301 || r->status == 302) && strcmp(method, "POST") == 0)) {
            strcpy(method, "GET");  // what browsers do
            r->bodyLen = 0;
        }
        snprintf(url, sizeof url, "%s", location);
    }
    if (r->status < 0) TraceLog(LOG_WARNING, "NET: %s %s failed: %s", r->method, r->url, r->error);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qLock);
        while (!qHead) pthread_cond_wait(&qCond, &qLock);
        NetRequest *r = qHead;
        qHead = r->next;
        if (!qHead) qTail = NULL;
        pthread_mutex_unlock(&qLock);
        if (!atomic_load(&r->abandoned)) perform(r);
        pthread_mutex_lock(&qLock);  // pairs with NetFree: exactly one of us frees it
        atomic_store(&r->done, true);
        bool abandoned = atomic_load(&r->abandoned);
        pthread_mutex_unlock(&qLock);
        if (abandoned) freeRequest(r);
    }
    return NULL;
}

NetRequest *NetSend(const char *method, const char *url, const char *headers, const void *body, int bodyLen)
{
#ifdef KIT_NETWORK_DEV_ONLY
    static bool warned;
    if (!warned) {
        warned = true;
        TraceLog(LOG_WARNING, "NET: game.conf doesn't have NETWORK=yes: requests work in dev builds only");
    }
#endif
    NetRequest *r = newRequest(method, url, headers, body, bodyLen);
    pthread_mutex_lock(&qLock);
    if (!started) {
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        started = true;
    }
    if (qTail) qTail->next = r;
    else qHead = r;
    qTail = r;
    pthread_cond_signal(&qCond);
    pthread_mutex_unlock(&qLock);
    return r;
}

void NetFree(NetRequest *r)
{
    if (!r) return;
    // the worker frees it if it's still queued or running; the done check and the flag must not race with it
    pthread_mutex_lock(&qLock);
    bool done = atomic_load(&r->done);
    if (!done) atomic_store(&r->abandoned, true);
    pthread_mutex_unlock(&qLock);
    if (done) freeRequest(r);
}

#endif
