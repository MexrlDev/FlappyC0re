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

static const char *save_paths[] = {
    "/savedata0/.savegame/flappy.sav",
    "/av_contents/content_tmp/flappy.sav",
    "/download0/flappy.sav",
    NULL
};

static const char *active_path = NULL;
static int ready;

int save_init(void) {
    mkdir("/savedata0", 0777);
    mkdir("/savedata0/.savegame", 0777);
    mkdir("/av_contents", 0777);
    mkdir("/av_contents/content_tmp", 0777);

    for (int i = 0; save_paths[i]; i++) {
        FILE *f = fopen(save_paths[i], "r");
        if (f) { fclose(f); active_path = save_paths[i]; ready = 1; return 0; }
        f = fopen(save_paths[i], "w");
        if (f) { fclose(f); active_path = save_paths[i]; ready = 1; return 0; }
    }

    active_path = NULL;
    ready = 0;
    return -1;
}

int save_available(void) { return ready; }

int save_load(struct game *g) {
    if (!ready || !active_path) return -1;

    FILE *f = fopen(active_path, "r");
    if (!f) return -1;

    struct save_blob b;
    size_t n = fread(&b, sizeof(b), 1, f);
    fclose(f);

    if (n != 1) return -1;
    if (b.magic != MAGIC) return -1;
    if (checksum(&b.magic, 8) != b.checksum) return -1;

    g->high_score     = (int)b.high_score;
    g->last_score     = (int)b.last_score;
    g->lifetime_pipes = b.lifetime_pipes;
    if (b.diff < DIFF_COUNT) game_set_diff(g, (enum diff)b.diff);
    g->is_night = b.is_night ? 1 : 0;

    if (b.version >= 2) g->vibration_on = b.vibration_on ? 1 : 0;
    else                g->vibration_on = 1;

    if (b.version >= 3 && b.screen_mode < SCREEN_MODE_COUNT)
        g->screen_mode = (enum screen_mode)b.screen_mode;
    else
        g->screen_mode = SCREEN_FULL;

    return 0;
}

int save_write(const struct game *g) {
    if (!ready || !active_path) return -1;

    struct save_blob b;
    b.magic          = MAGIC;
    b.version        = SAVE_VERSION;
    b.high_score     = (u32)g->high_score;
    b.last_score     = (u32)g->last_score;
    b.lifetime_pipes = g->lifetime_pipes;
    b.diff           = (u32)g->diff;
    b.is_night       = g->is_night ? 1 : 0;
    b.vibration_on   = g->vibration_on ? 1 : 0;
    b.screen_mode    = (u32)g->screen_mode;
    b.checksum       = checksum(&b.magic, 8);

    FILE *f = fopen(active_path, "w");
    if (!f) return -1;
    size_t w = fwrite(&b, sizeof(b), 1, f);
    fclose(f);
    return w == 1 ? 0 : -1;
}
