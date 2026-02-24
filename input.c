#include "input.h"
#include "raylib.h"
#include <string.h>

void input_init(void) {
    // Nothing to initialize currently
}

// --------------------------------------------------------------------------
// InputBuffer: poll once per render frame, consume once per fixed update
// --------------------------------------------------------------------------

void input_buffer_poll_p1(InputBuffer *buf) {
    buf->left   = IsKeyDown(KEY_A);
    buf->right  = IsKeyDown(KEY_D);
    buf->crouch = IsKeyDown(KEY_S);
    // OR-latch: set if pressed this render frame, never cleared by poll
    if (IsKeyPressed(KEY_W))  buf->jump         = true;
    if (IsKeyPressed(KEY_J))  buf->attack       = true;
    if (IsKeyPressed(KEY_K))  buf->parry        = true;
    if (IsKeyPressed(KEY_L))  buf->throw_weapon = true;
}

void input_buffer_poll_p2(InputBuffer *buf) {
    buf->left   = IsKeyDown(KEY_LEFT);
    buf->right  = IsKeyDown(KEY_RIGHT);
    buf->crouch = IsKeyDown(KEY_DOWN);
    if (IsKeyPressed(KEY_UP))     buf->jump         = true;
    if (IsKeyPressed(KEY_KP_1))   buf->attack       = true;
    if (IsKeyPressed(KEY_KP_2))   buf->parry        = true;
    if (IsKeyPressed(KEY_KP_3))   buf->throw_weapon = true;
}

void input_buffer_consume(InputBuffer *buf, Input *in, uint32_t frame) {
    in->frame        = frame;
    in->left         = buf->left;
    in->right        = buf->right;
    in->crouch       = buf->crouch;
    in->jump         = buf->jump;
    in->attack       = buf->attack;
    in->parry        = buf->parry;
    in->throw_weapon = buf->throw_weapon;
    // Clear edge-triggered buttons so they fire exactly once per press
    buf->jump         = false;
    buf->attack       = false;
    buf->parry        = false;
    buf->throw_weapon = false;
}

// --------------------------------------------------------------------------
// Legacy one-shot gather (kept for network code path)
// --------------------------------------------------------------------------

Input input_gather_p1(uint32_t frame) {
    Input in;
    memset(&in, 0, sizeof(in));
    in.frame        = frame;
    in.left         = IsKeyDown(KEY_A);
    in.right        = IsKeyDown(KEY_D);
    in.jump         = IsKeyPressed(KEY_W);
    in.crouch       = IsKeyDown(KEY_S);
    in.attack       = IsKeyPressed(KEY_J);
    in.parry        = IsKeyPressed(KEY_K);
    in.throw_weapon = IsKeyPressed(KEY_L);
    return in;
}

// Player 2 uses arrow keys for local multiplayer testing
Input input_gather_p2(uint32_t frame) {
    Input in;
    memset(&in, 0, sizeof(in));
    in.frame        = frame;
    in.left         = IsKeyDown(KEY_LEFT);
    in.right        = IsKeyDown(KEY_RIGHT);
    in.jump         = IsKeyPressed(KEY_UP);
    in.crouch       = IsKeyDown(KEY_DOWN);
    in.attack       = IsKeyPressed(KEY_KP_1);
    in.parry        = IsKeyPressed(KEY_KP_2);
    in.throw_weapon = IsKeyPressed(KEY_KP_3);
    return in;
}

// Serialize: pack 8 booleans into 1 byte + 4 bytes for frame = 5 bytes
void input_serialize(const Input *in, uint8_t *buf, int *len) {
    uint8_t flags = 0;
    if (in->left)         flags |= (1 << 0);
    if (in->right)        flags |= (1 << 1);
    if (in->jump)         flags |= (1 << 2);
    if (in->crouch)       flags |= (1 << 3);
    if (in->attack)       flags |= (1 << 4);
    if (in->parry)        flags |= (1 << 5);
    if (in->throw_weapon) flags |= (1 << 6);

    buf[0] = flags;
    // frame as little-endian uint32
    buf[1] = (in->frame >>  0) & 0xFF;
    buf[2] = (in->frame >>  8) & 0xFF;
    buf[3] = (in->frame >> 16) & 0xFF;
    buf[4] = (in->frame >> 24) & 0xFF;
    *len = 5;
}

bool input_deserialize(Input *in, const uint8_t *buf, int len) {
    if (len < 5) return false;
    uint8_t flags = buf[0];
    in->left         = (flags >> 0) & 1;
    in->right        = (flags >> 1) & 1;
    in->jump         = (flags >> 2) & 1;
    in->crouch       = (flags >> 3) & 1;
    in->attack       = (flags >> 4) & 1;
    in->parry        = (flags >> 5) & 1;
    in->throw_weapon = (flags >> 6) & 1;
    in->frame  = (uint32_t)buf[1];
    in->frame |= (uint32_t)buf[2] << 8;
    in->frame |= (uint32_t)buf[3] << 16;
    in->frame |= (uint32_t)buf[4] << 24;
    return true;
}
