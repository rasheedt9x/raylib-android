// Starter game: tap to throw balls, they bounce around. Run ./dev.sh, then edit this file and save: the phone
// picks up the change in about a second and keeps the balls where they are (state lives in Game).
// Assets: put files in assets/ and load them by relative path, e.g. LoadTexture("images/player.png").
#include "game.h"
#include <math.h>

#define MAX_BALLS 64

typedef struct { Vector2 pos, vel; float r; Color col; } Ball;

typedef struct {
    Ball balls[MAX_BALLS];
    int n;
} Game;

static void init(void *state)
{
    SetTargetFPS(60);
}

static void frame(void *state)
{
    Game *g = state;
    int w = GetScreenWidth(), h = GetScreenHeight();
    float dt = GetFrameTime();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g->n < MAX_BALLS)  // the first finger counts as the mouse
        g->balls[g->n++] = (Ball){ GetMousePosition(), { GetRandomValue(-600, 600), GetRandomValue(-900, -300) },
                                   GetRandomValue(30, 70), ColorFromHSV(GetRandomValue(0, 360), 0.6f, 1) };
    for (int i = 0; i < g->n; i++) {
        Ball *b = &g->balls[i];
        b->vel.y += 1500 * dt;
        b->pos.x += b->vel.x * dt;
        b->pos.y += b->vel.y * dt;
        if (b->pos.x < b->r || b->pos.x > w - b->r) { b->vel.x *= -1; b->pos.x = fminf(fmaxf(b->pos.x, b->r), w - b->r); }
        if (b->pos.y > h - b->r) { b->vel.y *= -0.85f; b->pos.y = h - b->r; }
    }

    BeginDrawing();
    ClearBackground((Color){ 20, 20, 30, 255 });
    for (int i = 0; i < g->n; i++) DrawCircleV(g->balls[i].pos, g->balls[i].r, g->balls[i].col);
    DrawText("tap to throw a ball", 40, 80, 60, RAYWHITE);
    DrawText("edit src/game.c and save: it hot reloads", 40, 160, 36, GRAY);
    EndDrawing();
}

static void reload(void *state, bool assetsChanged) {}

static void unload(void *state) {}

GAME_API(sizeof(Game), init, frame, reload, unload);
