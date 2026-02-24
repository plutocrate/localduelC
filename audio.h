#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"
#include "combat.h"
#include "player.h"
#include "raylib.h"

// Per-player footstep timer
typedef struct PlayerAudioState {
    int  footstep_timer;    // frames until next footstep
    bool was_on_ground;     // for jump trigger (ground -> air transition)
    PlayerState prev_state; // for attack/parry entry detection
    bool prev_has_sword;    // for pickup detection
} PlayerAudioState;

typedef struct AudioState {
    Sound sfx_whoosh;       // sword swing
    Sound sfx_hit;          // sword hits body (death blow)
    Sound sfx_parry_clash;  // perfect parry / sword clash (BaseMetal)
    Sound sfx_parry_normal; // normal parry deflect (PullingWeapon)
    Sound sfx_block;        // parry button pressed but nothing connected
    Sound sfx_grunt_attack; // attacker grunt
    Sound sfx_grunt_death;  // death scream
    Sound sfx_footstep;     // footstep
    Sound sfx_jump;         // jump
    Sound sfx_throw;        // sword throw
    Sound sfx_pickup;       // sword pickup (clink)

    PlayerAudioState players[2];
    bool initialized;
} AudioState;

// Call once after InitAudioDevice()
bool audio_init(AudioState *a);
void audio_shutdown(AudioState *a);

// Call once per fixed update, after player_update and combat_resolve
// prev[2] = player states BEFORE this update (captured before player_update)
void audio_update(AudioState *a,
                  const Player players[2],
                  const PlayerState prev_states[2],
                  const bool prev_on_ground[2],
                  CombatResult combat,
                  const Input inputs[2]);

#endif
