#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

// Fixed timestep
#define FIXED_DT        (1.0f / 60.0f)
// Screen dimensions and GROUND_Y are dynamic (fullscreen support).
// Use g_screen_w(), g_screen_h(), g_ground_y(), g_arena_w() at runtime.
#define GRAVITY         1400.0f
#define MAX_PLAYERS     2

// Player dimensions
#define PLAYER_W        28.0f
#define PLAYER_HEIGHT   56.0f
#define CROUCH_H        32.0f

// Combat constants
#define ATTACK_FRAMES       12
#define PARRY_FRAMES        10
#define PARRY_ACTIVE_START  2
#define PARRY_ACTIVE_END    7
#define STUN_FRAMES         25
#define RECOVERY_FRAMES     18
#define SWORD_LENGTH        52.0f
#define SWORD_THROW_SPEED   1050.0f

// Physics
#define WALK_SPEED          330.0f
#define RUN_ACCEL           1350.0f
#define FRICTION_GROUND     0.82f
#define FRICTION_AIR        0.98f
#define JUMP_VEL           -780.0f

// Input buffer size (kept for input.c compatibility)
#define INPUT_BUFFER_FRAMES 5
#define MAX_PACKET_SIZE     512

// ---------------------------------------------------------------
// Runtime screen / arena helpers -- defined in game.c
// ---------------------------------------------------------------
int   g_screen_w(void);
int   g_screen_h(void);
float g_ground_y(void);
float g_arena_w(void);

typedef struct Vec2 {
    float x, y;
} Vec2;

typedef struct Rect {
    float x, y, w, h;
} Rect;

typedef enum PlayerState {
    STATE_IDLE,
    STATE_WALK,
    STATE_JUMP,
    STATE_CROUCH,
    STATE_ATTACK,
    STATE_PARRY,
    STATE_STUNNED,
    STATE_DEAD,
    STATE_THROW,
} PlayerState;

typedef struct Input {
    bool left;
    bool right;
    bool jump;
    bool crouch;
    bool attack;
    bool parry;
    bool throw_weapon;
    uint32_t frame;
} Input;

typedef enum HitResult {
    HIT_NONE,
    HIT_BODY,
    HIT_PARRY,
    HIT_CLASH,
} HitResult;

#endif
