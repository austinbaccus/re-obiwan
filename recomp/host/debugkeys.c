/* Debug keys, checked while the game polls the gamepad (hooks_xapi.c), so on
   the game's own thread, and only while the game window has focus:

     F1  every player's health back to full (#55)

   The fields and functions used here are in docs/recomp/game-structures.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "guest.h"
#include "kernel.h"

#define ENTITY_LIST    0x004f17c0u /* the list's sentinel node: +0 next, +8 the entity */
#define ENT_MAX_HEALTH 0x110       /* float */
#define ENT_HEALTH     0x114       /* float */
#define ENT_FLAGS      0x168
#define FLAG_PLAYER    0x4000      /* with 0x40 set, player two (Jedi Battle) */

static float rdf(uint32_t a) { float f; memcpy(&f, MEM(a), 4); return f; }

/* the walk GetPlayer (FUN_0024c030) falls back to; a dead player (health 0)
   is left alone */
static void restore_health(void) {
    __try {
        uint32_t head = RD32(MEM(ENTITY_LIST)), n = 0;
        if (!head) return;
        for (uint32_t node = RD32(MEM(head)); node && node != head && n < 100000; node = RD32(MEM(node)), n++) {
            uint32_t ent = RD32(MEM(node + 8));
            if (!ent || !(RD32(MEM(ent + ENT_FLAGS)) & FLAG_PLAYER)) continue;
            float hp = rdf(ent + ENT_HEALTH), max = rdf(ent + ENT_MAX_HEALTH);
            if (hp <= 0 || max <= 0) continue;
            memcpy(MEM(ent + ENT_HEALTH), &max, 4);
            fprintf(stderr, "debug: F1, player %d health %.0f -> %.0f\n",
                    RD32(MEM(ent + ENT_FLAGS)) & 0x40 ? 2 : 1, hp, max);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "debug: F1, the entity list could not be read\n");
    }
}

void debug_keys(void) {
    static int was;
    int f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1 && !was) restore_health();
    was = f1;
}
