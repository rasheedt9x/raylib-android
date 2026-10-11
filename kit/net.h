// HTTP/HTTPS requests that don't block the game: start one, then check NetDone() each frame.
//
//     NetRequest *r = NetGet("https://example.com/scores.json");
//     ...
//     if (r && NetDone(r)) {
//         if (NetStatus(r) == 200) use(NetBody(r, NULL));
//         else TraceLog(LOG_WARNING, "request failed: %d %s", NetStatus(r), NetError(r));
//         NetFree(r);
//         r = NULL;
//     }
//
// Requests run one at a time on a background thread, follow redirects, and time out after ~20 s. HTTPS uses mbedTLS
// with the phone's own trusted CA certificates. Release builds need NETWORK=yes in game.conf (the INTERNET
// permission); dev builds always have it. A NetRequest belongs to the kit, not the game code, so pointers to it in
// the game state stay valid across hot reloads.
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>  // NULL, for NetBody(r, NULL)

typedef struct NetRequest NetRequest;

NetRequest *NetGet(const char *url);
NetRequest *NetPost(const char *url, const char *contentType, const void *body, int bodyLen);
// any method; headers: "Name: value" lines separated by \n (or NULL)
NetRequest *NetSend(const char *method, const char *url, const char *headers, const void *body, int bodyLen);

bool NetDone(NetRequest *r);
int NetStatus(NetRequest *r);                   // HTTP status (200, 404...), or -1 if the request failed
const char *NetBody(NetRequest *r, int *len);   // the response body, NUL-terminated ("" until done)
const char *NetError(NetRequest *r);            // why it failed ("" if it didn't)
void NetFree(NetRequest *r);                    // also fine while it's still running (it's dropped when done)

#endif
