// The contract between a game and the kit's host (kit/host.c). A game defines one GameApi with GAME_API(...).
//
// The host owns the window, the audio device and the game's state: one zeroed block of stateSize bytes. In dev
// builds (dev.sh) the game's code is hot reloaded and the state survives, so keep everything that must last in
// the state struct: globals and statics in the game's .c files start over on every reload.
#ifndef GAME_H
#define GAME_H

#include "raylib.h"
#include <stdbool.h>

typedef struct GameApi {
    int stateSize;
    void (*init)(void *state);                        // load assets, set things up (window + audio are ready)
    void (*frame)(void *state);                       // update and draw one frame (BeginDrawing..EndDrawing)
    void (*reload)(void *state, bool assetsChanged);  // dev builds: new code and/or new assets were just loaded
    void (*unload)(void *state);                      // free what init loaded
} GameApi;

// When stateSize changes between reloads (the struct gained or lost fields), the host can't keep the old state:
// it calls unload, zeroes a new block and calls init, i.e. the game restarts.
#define GAME_API(...) const GameApi game_api = { __VA_ARGS__ }

#endif
