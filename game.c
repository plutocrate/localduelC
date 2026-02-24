#include "game.h"
#include "physics.h"
#include "audio.h"
#include "raylib.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ----------------------------------------------------------------
// Runtime screen dimension accessors (defined here, declared in types.h)
// ----------------------------------------------------------------
int   g_screen_w(void) { return GetScreenWidth();  }
int   g_screen_h(void) { return GetScreenHeight(); }
// Fixed world dimensions - physics runs in this space on ALL machines
float g_ground_y(void) { return 620.0f; }   // fixed world Y of ground
float g_arena_w(void)  { return 4000.0f; }  // fixed world width (kept for compat)

// ----------------------------------------------------------------
// Forward declarations for render helpers
// ----------------------------------------------------------------
static void render_player(const Player *p, float cam_x, bool debug);
static void render_sword(const Player *p, float cam_x, bool debug);
static void render_thrown_sword(const ThrowingSword *s, float cam_x);
static void render_arena(float cam_x);
static void render_hud(const GameState *gs);
static void render_controls_hint(void);

// Forward declaration - defined later in rendering section
static float g_cam_zoom = 1.0f;

// Fixed world dimensions - used by both simulation and rendering
#define VIRTUAL_W    1280.0f   // world units across screen width at zoom=1
#define WORLD_GROUND 620.0f    // must match g_ground_y()

// ----------------------------------------------------------------
// Init / Shutdown
// ----------------------------------------------------------------
void game_init(GameState *gs) {
    memset(gs, 0, sizeof(*gs));
    gs->phase = PHASE_PLAYING;
    gs->frame = 0;
    gs->debug_hitboxes = false;

    InitAudioDevice();
    audio_init(&gs->audio);

    game_start_round(gs);
}

void game_shutdown(GameState *gs) {
    audio_shutdown(&gs->audio);
    CloseAudioDevice();
}

void game_start_round(GameState *gs) {
    // Symmetric spawn: player bodies centered at ±200 from world origin
    float spawn_offset = 200.0f;
    player_init(&gs->players[0], 0, -spawn_offset - PLAYER_W * 0.5f);
    player_init(&gs->players[1], 1,  spawn_offset - PLAYER_W * 0.5f);
    gs->players[1].facing = -1;

    for (int i = 0; i < MAX_THROWN_SWORDS; i++) {
        gs->swords[i].active = false;
    }

    gs->cam.x = 0.0f;  // world center
    gs->cam.y = 0.0f;
    gs->cam.target_x = 0.0f;
    gs->cam.zoom = 1.0f;
    gs->cam.target_zoom = 1.0f;
    gs->round_over_timer = 0;
    gs->winner_id = -1;

    // Platform layout (one-way, climbable from below, droppable with down)
    // All Y values are top-surface in world space; WORLD_GROUND = 620
    gs->num_platforms = 6;
    // Low side platforms (~100 units up)
    gs->platforms[0] = (Platform){ -700.0f, 620.0f - 130.0f, 220.0f };  // left-low
    gs->platforms[1] = (Platform){  480.0f, 620.0f - 130.0f, 220.0f };  // right-low
    // Mid platforms (~220 units up)
    gs->platforms[2] = (Platform){ -380.0f, 620.0f - 220.0f, 280.0f };  // left-mid
    gs->platforms[3] = (Platform){  100.0f, 620.0f - 220.0f, 280.0f };  // right-mid
    // High centre platform (~340 units up)
    gs->platforms[4] = (Platform){ -150.0f, 620.0f - 340.0f, 300.0f };  // centre-high
    // Far side ledges (~160 units up)
    gs->platforms[5] = (Platform){ -1100.0f, 620.0f - 160.0f, 180.0f }; // far-left
}

// ----------------------------------------------------------------
// Camera
// ----------------------------------------------------------------
static void update_camera(Camera2D_State *cam, const Player *p0, const Player *p1) {
    float mid_x = (p0->body.pos.x + p1->body.pos.x) * 0.5f + PLAYER_W * 0.5f;
    cam->target_x = mid_x;
    cam->x += (cam->target_x - cam->x) * 0.08f;

    // Clamp in world units: half-screen in world units = (screen_w/2) / world_scale
    float half_screen_world = (VIRTUAL_W * 0.5f) / g_cam_zoom;
    float half_arena = 2000.0f;
    if (cam->x - half_screen_world < -half_arena) cam->x = -half_arena + half_screen_world;
    if (cam->x + half_screen_world >  half_arena) cam->x =  half_arena - half_screen_world;

    // Dynamic zoom based on player distance in WORLD units (screen-independent)
    float dist = fabsf(p1->body.pos.x - p0->body.pos.x);
    float min_dist = 150.0f;   // world units - zoom in when this close
    float max_dist = 700.0f;   // world units - zoom out when this far
    float t = (dist - min_dist) / (max_dist - min_dist);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    cam->target_zoom = 1.4f - t * 0.8f;  // 1.4 (close) -> 0.6 (far)

    cam->zoom += (cam->target_zoom - cam->zoom) * 0.05f;
}

// ----------------------------------------------------------------
// Fixed Update
// ----------------------------------------------------------------
void game_fixed_update(GameState *gs) {
    if (gs->phase == PHASE_ROUND_OVER) {
        gs->round_over_timer--;
        if (gs->round_over_timer <= 0) {
            if (gs->players[0].score >= WIN_SCORE || gs->players[1].score >= WIN_SCORE) {
                gs->phase = PHASE_MATCH_OVER;
            } else {
                game_start_round(gs);
                gs->phase = PHASE_PLAYING;
            }
        }
        return;
    }

    if (gs->phase == PHASE_MATCH_OVER) {
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
            gs->players[0].score = 0;
            gs->players[1].score = 0;
            game_start_round(gs);
            gs->phase = PHASE_PLAYING;
        }
        return;
    }

    // --- PHASE_PLAYING ---

    Input p0_in, p1_in;
    memset(&p0_in, 0, sizeof(p0_in));
    memset(&p1_in, 0, sizeof(p1_in));

    input_buffer_consume(&gs->input_buf_p1, &p0_in, gs->frame);
    input_buffer_consume(&gs->input_buf_p2, &p1_in, gs->frame);

    // Snapshot state BEFORE simulation so audio can detect transitions
    PlayerState prev_states[2] = {
        gs->players[0].state,
        gs->players[1].state,
    };
    bool prev_on_ground[2] = {
        gs->players[0].body.on_ground,
        gs->players[1].body.on_ground,
    };
    Input cur_inputs[2] = { p0_in, p1_in };

    player_update(&gs->players[0], &p0_in, FIXED_DT, gs->platforms, gs->num_platforms);
    player_update(&gs->players[1], &p1_in, FIXED_DT, gs->platforms, gs->num_platforms);

    // Spawn thrown sword on first frame of throw state
    for (int pid = 0; pid < 2; pid++) {
        Player *p = &gs->players[pid];
        if (p->state == STATE_THROW && p->state_timer == 7) {
            for (int i = 0; i < MAX_THROWN_SWORDS; i++) {
                if (!gs->swords[i].active) {
                    combat_throw_sword(p, &gs->swords[i]);
                    break;
                }
            }
        }
    }

    combat_update_thrown_swords(gs->swords, MAX_THROWN_SWORDS,
                                &gs->players[0], &gs->players[1], FIXED_DT);

    CombatResult combat_result = combat_resolve(&gs->players[0], &gs->players[1],
                                                gs->swords, MAX_THROWN_SWORDS);

    // Audio: fire sound triggers based on state transitions and combat result
    audio_update(&gs->audio, gs->players, prev_states, prev_on_ground,
                 combat_result, cur_inputs);

    // Death / respawn handling
    for (int i = 0; i < 2; i++) {
        Player *dead  = &gs->players[i];
        Player *alive = &gs->players[1 - i];

        if (dead->state == STATE_DEAD && dead->respawn_timer == 119) {
            alive->score++;
            gs->winner_id = alive->id;
            if (alive->score >= WIN_SCORE) {
                gs->phase = PHASE_MATCH_OVER;
            } else {
                gs->phase = PHASE_ROUND_OVER;
                gs->round_over_timer = 180;
            }
        }

        if (dead->state == STATE_DEAD && dead->respawn_timer == 0) {
            float respawn_x;
            int   respawn_facing;
            if (dead->id == 0) {
                respawn_x      = alive->body.pos.x - 120.0f;
                respawn_facing = 1;
            } else {
                respawn_x      = alive->body.pos.x + 120.0f;
                respawn_facing = -1;
            }
            player_respawn(dead, respawn_x, respawn_facing);
        }
    }

    update_camera(&gs->cam, &gs->players[0], &gs->players[1]);

    gs->frame++;
}

static void sun_update(float dt);  // defined in rendering section below

void game_tick(GameState *gs, float dt) {
    // Poll inputs ONCE per real render frame
    input_buffer_poll_p1(&gs->input_buf_p1);
    input_buffer_poll_p2(&gs->input_buf_p2);

    // Debug toggles: sampled once per render frame
    if (IsKeyPressed(KEY_F1))  gs->debug_hitboxes = !gs->debug_hitboxes;
    if (IsKeyPressed(KEY_F11)) ToggleFullscreen();

    // Advance sun animation
    sun_update(dt);

    gs->dt_accum += dt;
    if (gs->dt_accum > FIXED_DT * 8) gs->dt_accum = FIXED_DT * 8;
    while (gs->dt_accum >= FIXED_DT) {
        game_fixed_update(gs);
        gs->dt_accum -= FIXED_DT;
    }
}

// ----------------------------------------------------------------
// Rendering helpers
// ----------------------------------------------------------------

static int g_local_player_id = 0;  // P1 is always local

// ----------------------------------------------------------------
// Resolution-independent rendering
// ----------------------------------------------------------------

static float world_scale(void) {
    return (float)g_screen_w() / VIRTUAL_W;
}

static int world_to_screen_x(float world_x, float cam_x) {
    float scale = world_scale() * g_cam_zoom;
    float half_sw = (float)g_screen_w() * 0.5f;
    return (int)((world_x - cam_x) * scale + half_sw);
}

static int world_to_screen_y(float world_y) {
    float scale = world_scale() * g_cam_zoom;
    // Map world ground (620) -> screen ground (82% of screen height)
    float screen_ground = (float)g_screen_h() * 0.82f;
    return (int)((world_y - WORLD_GROUND) * scale + screen_ground);
}

// ----------------------------------------------------------------
// Sun / light source — Perlin-noise drifting warm light
// ----------------------------------------------------------------

static float snoise_hash(int x) {
    x = (x << 13) ^ x;
    return 1.0f - (float)((x * (x * x * 15731 + 789221) + 1376312589) & 0x7fffffff)
                  / 1073741824.0f;
}
static float snoise_smooth(float x) {
    int   xi = (int)x;
    float xf = x - (float)xi;
    float t  = xf * xf * (3.0f - 2.0f * xf);
    return snoise_hash(xi) * (1.0f - t) + snoise_hash(xi + 1) * t;
}
static float fbm(float x, int octaves) {
    float val = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < octaves; o++) {
        val  += snoise_smooth(x * freq + (float)o * 31.7f) * amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return val;
}

static float g_sun_time = 0.0f;

static void sun_update(float dt) {
    g_sun_time += dt * 0.07f;
}

static void sun_get_pos(int *sx, int *sy) {
    int sw = g_screen_w();
    float nx = fbm(g_sun_time,        3);
    float ny = fbm(g_sun_time + 5.3f, 3);
    *sx = (int)(sw * 0.5f + nx * sw * 0.28f);
    *sy = (int)(60.0f    + ny * 40.0f + 20.0f);
}

static void render_sun(void) {
    int sx, sy;
    sun_get_pos(&sx, &sy);

    for (int r = 420; r >= 20; r -= 20) {
        float t   = 1.0f - (float)r / 420.0f;
        float t2  = t * t;
        uint8_t a = (uint8_t)(t2 * 38.0f);
        DrawCircle(sx, sy, (float)r,
                   (Color){255, 210 + (uint8_t)(t * 30), 80, a});
    }

    for (int r = 80; r >= 4; r -= 4) {
        float t  = 1.0f - (float)r / 80.0f;
        uint8_t a = (uint8_t)(t * t * 160.0f);
        DrawCircle(sx, sy, (float)r,
                   (Color){255, 240, 160, a});
    }

    DrawCircle(sx, sy, 18, (Color){255, 255, 220, 230});
    DrawCircle(sx, sy, 10, (Color){255, 255, 255, 255});

    int num_rays = 12;
    for (int ri = 0; ri < num_rays; ri++) {
        float base_angle = 3.14159f * 0.5f;
        float spread     = 3.14159f * 0.55f;
        float angle = base_angle - spread * 0.5f
                      + spread * (float)ri / (float)(num_rays - 1);
        float ray_noise = fbm(g_sun_time * 3.0f + ri * 0.7f, 2) * 0.5f + 0.5f;
        float ray_len   = 90.0f + ray_noise * 140.0f;
        int ex = sx + (int)(cosf(angle) * ray_len);
        int ey = sy + (int)(sinf(angle) * ray_len);
        uint8_t ra = (uint8_t)(30.0f + ray_noise * 30.0f);
        DrawLine(sx, sy, ex, ey, (Color){255, 240, 160, ra});
    }

    int gscreen_y = (int)((float)g_screen_h() * 0.82f);
    int pool_w    = 180 + (int)(fbm(g_sun_time * 1.3f, 2) * 0.5f + 0.5f) * 60;
    for (int pw = pool_w; pw > 10; pw -= 12) {
        float t  = 1.0f - (float)pw / (float)pool_w;
        uint8_t a = (uint8_t)(t * t * 45.0f);
        DrawEllipse(sx, gscreen_y, (float)pw, 8.0f,
                    (Color){255, 230, 150, a});
    }
}

static void render_arena(float cam_x) {
    int sw = g_screen_w();
    int sh = g_screen_h();
    float gy = g_ground_y();
    float aw = 4000.0f;

    ClearBackground((Color){20, 15, 30, 255});

    int sun_sx, sun_sy;
    sun_get_pos(&sun_sx, &sun_sy);
    for (int y = 0; y < sh / 2; y++) {
        float t   = 1.0f - (float)y / (float)(sh / 2);
        float dx  = (float)(sun_sx - sw / 2) / (float)sw;
        float warm = t * (0.6f + dx * 0.2f);
        warm = warm < 0.0f ? 0.0f : (warm > 1.0f ? 1.0f : warm);
        uint8_t r = (uint8_t)(20 + warm * 40);
        uint8_t g2 = (uint8_t)(15 + warm * 20);
        uint8_t b = (uint8_t)(30  - warm * 12);
        DrawLine(0, y, sw, y, (Color){r, g2, b, 255});
    }

    render_sun();

    float para = cam_x * 0.3f;
    for (int i = 0; i < 10; i++) {
        float bx = fmodf(i * 160.0f - para * 0.5f, (float)sw + 160.0f) - 80.0f;
        float col_bright = 1.0f + (bx < (float)sun_sx ? 0.15f : 0.0f);
        uint8_t cb = (uint8_t)((40 + i * 5) * col_bright);
        DrawRectangle((int)bx, 80 + i * 20, 2, 60 + i * 8,
                      (Color){cb, (uint8_t)(30 + i * 5), (uint8_t)(60 + i * 5), 120});
    }

    int gx0 = world_to_screen_x(-aw * 0.5f, cam_x);
    int gx1 = world_to_screen_x( aw * 0.5f, cam_x);
    int gyi = world_to_screen_y(WORLD_GROUND);
    DrawRectangle(gx0, gyi, gx1 - gx0, sh - gyi, (Color){45, 38, 55, 255});
    DrawLine(gx0, gyi, gx1, gyi, (Color){180, 140, 200, 255});

    int num_tiles = (int)(aw / 200.0f) + 2;
    for (int tx = -num_tiles; tx <= num_tiles; tx++) {
        int tile_x = world_to_screen_x(tx * 200.0f, cam_x);
        DrawLine(tile_x, gyi, tile_x, gyi + 8, (Color){80, 60, 90, 200});
    }

    int left_goal  = world_to_screen_x(-1960.0f, cam_x);
    int right_goal = world_to_screen_x( 1960.0f, cam_x);
    DrawLine(left_goal,  0, left_goal,  sh, (Color){255, 80, 80, 180});
    DrawLine(right_goal, 0, right_goal, sh, (Color){80, 80, 255, 180});

    (void)sw;
    (void)gy;
}

static void render_platforms(const GameState *gs, float cam_x) {
    for (int i = 0; i < gs->num_platforms; i++) {
        const Platform *p = &gs->platforms[i];
        int px  = world_to_screen_x(p->x, cam_x);
        int py  = world_to_screen_y(p->y);
        int pw  = (int)(p->w * world_scale() * g_cam_zoom);
        DrawRectangle(px, py, pw, 14, (Color){65, 55, 80, 255});
        DrawLine(px, py, px + pw, py, (Color){180, 140, 200, 200});
        DrawLine(px,      py, px,      py + 8, (Color){140, 110, 160, 180});
        DrawLine(px + pw, py, px + pw, py + 8, (Color){140, 110, 160, 180});
    }
}

static void render_player(const Player *p, float cam_x, bool debug) {
    if (!p->ragdoll.active) {
        int foot_y = world_to_screen_y(p->body.pos.y + p->body.size.y);
        int top_y  = world_to_screen_y(p->body.pos.y);
        int h      = foot_y - top_y;
        int w      = (int)(p->body.size.x * world_scale() * g_cam_zoom);
        int sy     = top_y;
        int sx     = world_to_screen_x(p->body.pos.x + p->body.size.x * 0.5f, cam_x);

        bool is_local = (p->id == g_local_player_id);
        Color body_color = is_local ? (Color){100, 180, 255, 255}
                                    : (Color){255, 120,  80, 255};
        Color stun_flash = (Color){255, 255, 100, 200};
        Color cur_color  = (p->state == STATE_STUNNED && (p->stun_timer % 6 < 3))
                           ? stun_flash : body_color;

        int hip_y      = sy + h - h / 3;
        int shoulder_y = sy + h / 3;
        int mid_x      = sx;
        float s        = world_scale() * g_cam_zoom;

        int leg_spread  = (int)(7.0f * s);
        int arm_reach   = (int)((p->crouching ? 8.0f : 14.0f) * s);
        int arm_drop    = (int)(10.0f * s);
        int head_r      = (int)(9.0f * s);
        if (head_r < 3) head_r = 3;

        DrawLine(mid_x, hip_y, mid_x - leg_spread, foot_y, cur_color);
        DrawLine(mid_x, hip_y, mid_x + leg_spread, foot_y, cur_color);
        DrawLine(mid_x, hip_y, mid_x, shoulder_y, cur_color);

        int sword_arm_x = mid_x + p->facing * arm_reach;
        int sword_arm_y = shoulder_y + arm_drop;
        DrawLine(mid_x, shoulder_y, sword_arm_x, sword_arm_y, cur_color);
        int off_arm_x = mid_x - p->facing * (int)(8.0f * s);
        DrawLine(mid_x, shoulder_y, off_arm_x, sword_arm_y + (int)(4.0f * s), cur_color);

        DrawCircle(mid_x, sy + head_r, head_r, cur_color);
        DrawCircle(mid_x + p->facing * (int)(4.0f * s), sy + (int)(7.0f * s), (int)(2.0f * s) + 1, (Color){20, 20, 40, 255});

        (void)w;
    } else {
        bool is_local_dead = (p->id == g_local_player_id);
        Color dead_color = is_local_dead ? (Color){60, 100, 160, 200}
                                         : (Color){160,  70,  40, 200};
        int alpha_mod = p->ragdoll.timer * 2;
        if (alpha_mod > 255) alpha_mod = 255;
        dead_color.a = (uint8_t)alpha_mod;

        const Ragdoll *rd = &p->ragdoll;
        for (int i = 0; i < RAGDOLL_BONES; i++) {
            int bx = world_to_screen_x(rd->bones[i].pos.x, cam_x);
            int by = world_to_screen_y(rd->bones[i].pos.y);
            DrawCircle(bx, by, (i == 0) ? 8 : 4, dead_color);
        }
        if (RAGDOLL_BONES >= 2) {
            int ax  = world_to_screen_x(rd->bones[0].pos.x, cam_x);
            int ay  = world_to_screen_y(rd->bones[0].pos.y);
            int bx2 = world_to_screen_x(rd->bones[1].pos.x, cam_x);
            int by2 = world_to_screen_y(rd->bones[1].pos.y);
            DrawLine(ax, ay, bx2, by2, dead_color);
        }
    }

    if (debug) {
        float s = world_scale() * g_cam_zoom;
        int hx = world_to_screen_x(p->hurtbox.x, cam_x);
        int hy = world_to_screen_y(p->hurtbox.y);
        int hw = (int)(p->hurtbox.w * s);
        int hh = (int)(p->hurtbox.h * s);
        DrawRectangleLines(hx, hy, hw, hh, GREEN);
        if (p->weapon_hitbox.w > 0) {
            int wx = world_to_screen_x(p->weapon_hitbox.x, cam_x);
            int wy = world_to_screen_y(p->weapon_hitbox.y);
            int ww = (int)(p->weapon_hitbox.w * s);
            int wh = (int)(p->weapon_hitbox.h * s);
            DrawRectangleLines(wx, wy, ww, wh, RED);
        }
        if (p->parry_box.w > 0) {
            int px2 = world_to_screen_x(p->parry_box.x, cam_x);
            int py2 = world_to_screen_y(p->parry_box.y);
            int pw  = (int)(p->parry_box.w * s);
            int ph  = (int)(p->parry_box.h * s);
            DrawRectangleLines(px2, py2, pw, ph, YELLOW);
        }
    }
}

static void render_sword(const Player *p, float cam_x, bool debug) {
    if (!p->has_sword || p->state == STATE_DEAD) return;

    int bx = world_to_screen_x(p->sword_base.x, cam_x);
    int by = world_to_screen_y(p->sword_base.y);
    int tx = world_to_screen_x(p->sword_tip.x, cam_x);
    int ty = world_to_screen_y(p->sword_tip.y);

    Color blade_color = (p->state == STATE_PARRY) ? (Color){255, 255, 150, 255}
                                                   : (Color){220, 220, 255, 255};
    DrawLineEx((Vector2){(float)bx, (float)by}, (Vector2){(float)tx, (float)ty},
               3.0f, blade_color);

    float gx   = bx + (tx - bx) * 0.15f;
    float gy2  = by + (ty - by) * 0.15f;
    float perp_x = -(ty - by) * 0.15f;
    float perp_y =  (tx - bx) * 0.15f;
    DrawLine((int)(gx - perp_x), (int)(gy2 - perp_y),
             (int)(gx + perp_x), (int)(gy2 + perp_y),
             (Color){180, 160, 100, 255});

    if (p->state == STATE_ATTACK && p->attack_frame >= 2 && p->attack_frame <= 9) {
        DrawCircle(tx, ty, 6, (Color){255, 200, 100, 120});
    }
    (void)debug;
}

static void render_thrown_sword(const ThrowingSword *s, float cam_x) {
    if (!s->active) return;
    int sx = world_to_screen_x(s->pos.x, cam_x);
    int sy = world_to_screen_y(s->pos.y);
    float dx = cosf(s->angle) * 20.0f;
    float dy = sinf(s->angle) * 20.0f;
    DrawLine((int)(sx - dx), (int)(sy - dy), (int)(sx + dx), (int)(sy + dy),
             (Color){220, 220, 255, 255});
    DrawLine((int)(sx - dx), (int)(sy - dy), (int)(sx + dx), (int)(sy + dy),
             (Color){255, 240, 180, 120});
}

static void render_hud(const GameState *gs) {
    int sw = g_screen_w();
    int sh = g_screen_h();
    char buf[64];

    snprintf(buf, sizeof(buf), "P1: %d", gs->players[0].score);
    DrawText(buf, 40, 20, 28, (Color){100, 180, 255, 255});

    snprintf(buf, sizeof(buf), "P2: %d", gs->players[1].score);
    DrawText(buf, sw - 120, 20, 28, (Color){255, 120, 80, 255});

    snprintf(buf, sizeof(buf), "First to %d", WIN_SCORE);
    DrawText(buf, sw / 2 - 60, 20, 18, (Color){200, 200, 200, 180});

    snprintf(buf, sizeof(buf), "F:%u", gs->frame);
    DrawText(buf, sw - 80, sh - 24, 14, (Color){120, 120, 120, 180});

    if (gs->phase == PHASE_ROUND_OVER) {
        const char *msg = (gs->winner_id == 0) ? "PLAYER 1 WINS ROUND!" : "PLAYER 2 WINS ROUND!";
        int tw = MeasureText(msg, 40);
        DrawText(msg, sw / 2 - tw / 2, sh / 2 - 20, 40,
                 (gs->winner_id == 0) ? (Color){100, 180, 255, 255} : (Color){255, 120, 80, 255});
    }

    if (gs->phase == PHASE_MATCH_OVER) {
        const char *msg = (gs->players[0].score >= WIN_SCORE) ? "PLAYER 1 WINS!" : "PLAYER 2 WINS!";
        int tw = MeasureText(msg, 52);
        DrawText(msg, sw / 2 - tw / 2, sh / 2 - 50, 52,
                 (gs->players[0].score >= WIN_SCORE) ? (Color){100, 180, 255, 255} : (Color){255, 120, 80, 255});
        DrawText("Press ENTER to play again", sw / 2 - 160, sh / 2 + 20, 24, WHITE);
    }

    const char *state_names[] = {"IDLE","WALK","JUMP","CROUCH","ATTACK","PARRY","STUN","DEAD","THROW"};
    for (int i = 0; i < 2; i++) {
        const Player *p = &gs->players[i];
        int state_idx = (int)p->state;
        if (state_idx >= 0 && state_idx < 9) {
            int xpos = (i == 0) ? 40 : sw - 120;
            DrawText(state_names[state_idx], xpos, 54, 14, (Color){180, 180, 180, 200});
        }
        if (!p->has_sword) {
            int xpos = (i == 0) ? 40 : sw - 120;
            DrawText("[NO SWORD]", xpos, 72, 12, (Color){255, 200, 80, 255});
        }
    }
}

static void render_controls_hint(void) {
    int sh = g_screen_h();
    DrawText("P1: WASD=Move  J=Attack  K=Parry  L=Throw", 10, sh - 46, 13,
             (Color){120, 120, 150, 200});
    DrawText("P2: Arrows=Move  Num1=Atk  Num2=Parry  Num3=Throw  |  F1=Hitboxes  F11=Fullscreen",
             10, sh - 28, 13, (Color){120, 120, 150, 200});
}

void game_render(const GameState *gs) {
    BeginDrawing();

    float cam_x = gs->cam.x;
    g_cam_zoom = gs->cam.zoom;
    g_local_player_id = 0;

    render_arena(cam_x);
    render_platforms(gs, cam_x);

    for (int i = 0; i < MAX_THROWN_SWORDS; i++) {
        render_thrown_sword(&gs->swords[i], cam_x);
    }

    for (int i = 0; i < 2; i++) {
        render_player(&gs->players[i], cam_x, gs->debug_hitboxes);
        render_sword(&gs->players[i], cam_x, gs->debug_hitboxes);
    }

    render_hud(gs);
    render_controls_hint();

    EndDrawing();
}

int game_get_winner(const GameState *gs) {
    if (gs->players[0].score >= WIN_SCORE) return 0;
    if (gs->players[1].score >= WIN_SCORE) return 1;
    return -1;
}
