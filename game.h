#ifndef GAME_H
#define GAME_H

#include "types.h"
#include "player.h"
#include "combat.h"
#include "input.h"
#include "audio.h"

#define MAX_THROWN_SWORDS 4
#define WIN_SCORE         5

typedef enum GamePhase {
    PHASE_PLAYING,
    PHASE_ROUND_OVER,
    PHASE_MATCH_OVER,
} GamePhase;

typedef struct Camera2D_State {
    float x;   // camera center x in world space
    float y;
    float target_x;
    float zoom;         // current zoom (1.0 = normal)
    float target_zoom;
} Camera2D_State;

typedef struct GameState {
    Player         players[2];
    ThrowingSword  swords[MAX_THROWN_SWORDS];
    int            num_active_swords;

    Platform       platforms[MAX_PLATFORMS];
    int            num_platforms;

    GamePhase      phase;

    uint32_t       frame;
    float          dt_accum;

    Camera2D_State cam;

    // Debug flags
    bool           debug_hitboxes;

    // Round/match
    int            round_over_timer;
    int            winner_id;

    // Audio
    AudioState     audio;

    // Input buffers: polled once per render frame, consumed per fixed update
    InputBuffer    input_buf_p1;
    InputBuffer    input_buf_p2;
} GameState;

void game_init(GameState *gs);
void game_shutdown(GameState *gs);

// Main tick: accumulates dt and calls game_fixed_update as needed
void game_tick(GameState *gs, float dt);

void game_fixed_update(GameState *gs);
void game_render(const GameState *gs);

// Helpers
void game_start_round(GameState *gs);
int  game_get_winner(const GameState *gs);

#endif
