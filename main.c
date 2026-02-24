#include "raylib.h"
#include "game.h"
#include <stdio.h>

int main(void) {
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_FULLSCREEN_MODE);
    InitWindow(0, 0, "DUEL - Local 2-Player");
    SetTargetFPS(0);
    SetExitKey(KEY_NULL);

    GameState gs;
    game_init(&gs);

    while (!WindowShouldClose() && !IsKeyPressed(KEY_ESCAPE)) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;
        game_tick(&gs, dt);
        game_render(&gs);
    }

    game_shutdown(&gs);
    CloseWindow();
    return 0;
}
