/* SPDX-License-Identifier: MIT */
#include "save.h"
#include "ps_libc.h"

#define MAGIC 0x59504C46u   /* "FLPY" */

static u32 checksum(const u32 *w, int n) {
    u32 x = 0x9E3779B9u;
    for (int i = 0; i < n; i++)
        x ^= w[i] + 0x9E3779B9u + (x << 6) + (x >> 2);
    return x;
}

static int ready;

int save_init(void) {
    mkdir("/savedata0", 0777);
    mkdir("/savedata0/.savegame", 0777);

    FILE *f = fopen(SAVE_PATH, "r");
    if (f) { fclose(f); ready = 1; return 0; }
    f = fopen(SAVE_PATH, "w");
    if (f) { fclose(f); ready = 1; return 0; }
    ready = 0;
    return -1;
}

int save_available(void) { return ready; }

int save_load(struct game *g) {
    if (!ready) return -1;
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f) return -1;
    struct save_blob b;
    size_t n = fread(&b, sizeof(b), 1, f);
    fclose(f);
    if (n != 1) return -1;
    if (b.magic != MAGIC) return -1;
    /* checksum covers every u32 except the checksum itself */
    if (checksum(&b.magic, 8) != b.checksum) return -1;

    g->high_score     = (int)b.high_score;
    g->last_score     = (int)b.last_score;
    g->lifetime_pipes = b.lifetime_pipes;
    if (b.diff < DIFF_COUNT) game_set_diff(g, (enum diff)b.diff);
    g->is_night = b.is_night ? 1 : 0;

    /* v1 didn't have the field; default to on */
    if (b.version >= 2)
        g->vibration_on = b.vibration_on ? 1 : 0;
    else
        g->vibration_on = 1;

    return 0;
}

int save_write(const struct game *g) {
    if (!ready) return -1;
    struct save_blob b;
    b.magic          = MAGIC;
    b.version        = SAVE_VERSION;
    b.high_score     = (u32)g->high_score;
    b.last_score     = (u32)g->last_score;
    b.lifetime_pipes = g->lifetime_pipes;
    b.diff           = (u32)g->diff;
    b.is_night       = g->is_night ? 1 : 0;
    b.vibration_on   = g->vibration_on ? 1 : 0;
    b.checksum       = checksum(&b.magic, 8);

    FILE *f = fopen(SAVE_PATH, "w");
    if (!f) return -1;
    size_t w = fwrite(&b, sizeof(b), 1, f);
    fclose(f);
    return w == 1 ? 0 : -1;
}
