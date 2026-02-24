#include "audio.h"
#include "raylib.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

// Footstep cadence: every N fixed-update frames (60hz)
#define FOOTSTEP_FRAMES 18

// Slight random pitch variation for naturalness (±range around 1.0)
static float rand_pitch(float range) {
    return 1.0f + ((float)(rand() % 1000) / 1000.0f - 0.5f) * range;
}

static Sound load_sfx(const char *path) {
    Sound s = LoadSound(path);
    if (!IsSoundValid(s)) {
        fprintf(stderr, "[AUDIO] Failed to load: %s\n", path);
    }
    return s;
}

bool audio_init(AudioState *a) {
    *a = (AudioState){0};

    a->sfx_whoosh        = load_sfx("WEAPSwrd_WoodwWhoosh_HoveAud_SwordCombat_06.wav");
    a->sfx_hit           = load_sfx("WEAPSwrd_SwordStabCombowRing_HoveAud_SwordCombat_17.wav");
    a->sfx_parry_clash   = load_sfx("WEAPSwrd_BaseMetal_HoveAud_SwordCombat_03.wav");
    a->sfx_parry_normal  = load_sfx("Pulling_Small_Weapon_Out_2.wav");
    a->sfx_block         = load_sfx("Random_Noise_3.wav");
    a->sfx_grunt_attack  = load_sfx("VOXEfrt_ActionGrunt_HoveAud_SwordCombat_54.wav");
    a->sfx_grunt_death   = load_sfx("VOXScrm_DamageGrunt_HoveAudio_SwordCombat_13.wav");
    a->sfx_footstep      = load_sfx("Antons_Footsteps_FS_Sand_Walk_03.wav");
    a->sfx_jump          = load_sfx("Jump.wav");
    a->sfx_throw         = load_sfx("Moving_Weapons_Around_4.wav");
    a->sfx_pickup        = load_sfx("Clink.wav");

    // Init per-player prev_has_sword to true (both start with swords)
    a->players[0].prev_has_sword = true;
    a->players[1].prev_has_sword = true;

    a->initialized = true;
    return true;
}

void audio_shutdown(AudioState *a) {
    if (!a->initialized) return;
    UnloadSound(a->sfx_whoosh);
    UnloadSound(a->sfx_hit);
    UnloadSound(a->sfx_parry_clash);
    UnloadSound(a->sfx_parry_normal);
    UnloadSound(a->sfx_block);
    UnloadSound(a->sfx_grunt_attack);
    UnloadSound(a->sfx_grunt_death);
    UnloadSound(a->sfx_footstep);
    UnloadSound(a->sfx_jump);
    UnloadSound(a->sfx_throw);
    UnloadSound(a->sfx_pickup);
    a->initialized = false;
}

void audio_update(AudioState *a,
                  const Player players[2],
                  const PlayerState prev_states[2],
                  const bool prev_on_ground[2],
                  CombatResult combat,
                  const Input inputs[2]) {
    if (!a->initialized) return;

    // --- COMBAT SOUNDS ---

    // Death blow: sword hits body
    bool p0_died = (combat.p0_hit == HIT_BODY);
    bool p1_died = (combat.p1_hit == HIT_BODY);

    if (p0_died || p1_died) {
        // Sword impact sound
        SetSoundPitch(a->sfx_hit, rand_pitch(0.08f));
        SetSoundVolume(a->sfx_hit, 1.0f);
        PlaySound(a->sfx_hit);

        // Death grunt for the player who died
        SetSoundPitch(a->sfx_grunt_death, rand_pitch(0.10f));
        SetSoundVolume(a->sfx_grunt_death, 1.0f);
        PlaySound(a->sfx_grunt_death);
    }

    // Parry sounds — distinguish normal parry from clash/thrown-sword parry
    // sword_clash = blades meet simultaneously → BaseMetal clang (perfect)
    // HIT_PARRY   = one player actively deflected the other's swing → pulling sound (normal)
    if (combat.sword_clash) {
        SetSoundPitch(a->sfx_parry_clash, rand_pitch(0.06f));
        SetSoundVolume(a->sfx_parry_clash, 1.0f);
        PlaySound(a->sfx_parry_clash);
    }
    if (combat.p0_hit == HIT_PARRY || combat.p1_hit == HIT_PARRY) {
        SetSoundPitch(a->sfx_parry_normal, rand_pitch(0.08f));
        SetSoundVolume(a->sfx_parry_normal, 0.90f);
        PlaySound(a->sfx_parry_normal);
    }

    // --- PER-PLAYER STATE-TRANSITION SOUNDS ---
    for (int i = 0; i < 2; i++) {
        const Player *p      = &players[i];
        PlayerAudioState *pa = &a->players[i];
        PlayerState prev     = prev_states[i];

        bool just_attacked = (prev != STATE_ATTACK && p->state == STATE_ATTACK);
        bool just_thrown   = (prev != STATE_THROW  && p->state == STATE_THROW);
        bool just_jumped   = (prev_on_ground[i] && !p->body.on_ground && inputs[i].jump);
        bool just_parried  = (prev != STATE_PARRY  && p->state == STATE_PARRY);

        // Block: parry button pressed but nothing connected this frame
        // (if something DID connect, the parry_normal/clash sounds cover it instead)
        bool parry_connected = (combat.p0_hit == HIT_PARRY || combat.p1_hit == HIT_PARRY
                                || combat.sword_clash);
        if (just_parried && !parry_connected) {
            SetSoundPitch(a->sfx_block, rand_pitch(0.08f));
            SetSoundVolume(a->sfx_block, 0.70f);
            PlaySound(a->sfx_block);
        }

        // Sword gained this frame and player wasn't throwing (throw gives sword to opponent)
        bool just_picked_up = (!pa->prev_has_sword && p->has_sword &&
                               prev != STATE_THROW && p->state != STATE_THROW);
        pa->prev_has_sword = p->has_sword;

        // Attack swing — whoosh + grunt
        if (just_attacked) {
            SetSoundPitch(a->sfx_whoosh, rand_pitch(0.10f));
            SetSoundVolume(a->sfx_whoosh, 0.85f);
            PlaySound(a->sfx_whoosh);

            // Grunt: muffled if this player's attack killed the opponent
            bool this_player_killed = (i == 0 && p1_died) || (i == 1 && p0_died);
            float grunt_vol = this_player_killed ? 0.25f : 0.80f;
            SetSoundPitch(a->sfx_grunt_attack, rand_pitch(0.12f));
            SetSoundVolume(a->sfx_grunt_attack, grunt_vol);
            PlaySound(a->sfx_grunt_attack);
        }

        // Sword throw
        if (just_thrown) {
            SetSoundPitch(a->sfx_throw, rand_pitch(0.09f));
            SetSoundVolume(a->sfx_throw, 0.80f);
            PlaySound(a->sfx_throw);
        }

        // Sword pickup (walk over grounded sword, or catch a parried thrown sword)
        if (just_picked_up) {
            SetSoundPitch(a->sfx_pickup, rand_pitch(0.07f));
            SetSoundVolume(a->sfx_pickup, 0.85f);
            PlaySound(a->sfx_pickup);
        }

        // Jump
        if (just_jumped) {
            SetSoundPitch(a->sfx_jump, rand_pitch(0.08f));
            SetSoundVolume(a->sfx_jump, 0.75f);
            PlaySound(a->sfx_jump);
        }

        // Footsteps: only while walking on ground, timed to cadence
        bool is_walking = (p->state == STATE_WALK || p->state == STATE_CROUCH) &&
                          p->body.on_ground &&
                          fabsf(p->body.vel.x) > 20.0f;

        if (is_walking) {
            if (pa->footstep_timer <= 0) {
                SetSoundPitch(a->sfx_footstep, rand_pitch(0.12f));
                SetSoundVolume(a->sfx_footstep, 0.45f + rand_pitch(0.1f) * 0.1f);
                PlaySound(a->sfx_footstep);
                float speed_t = fabsf(p->body.vel.x) / 330.0f;
                pa->footstep_timer = (int)(FOOTSTEP_FRAMES * (1.0f - speed_t * 0.35f));
            } else {
                pa->footstep_timer--;
            }
        } else {
            pa->footstep_timer = 0;
        }
    }
}
