/* SPDX-License-Identifier: MIT */
#include "audio.h"

extern const u8 asset_blob[];

#define NUM_VOICES 32
#define GRAIN      1024           /* must match the buffer size opened in main.c */

/* Asymmetric fades.  A short fade-in kills the initial click without
   affecting the attack, and a very short fade-out leaves the natural
   decay of the source WAV intact.  The previous symmetric 256-sample
   fade chopped the tail of every SFX and produced an audible "swish"
   when the score sound played. */
#define FADE_IN_LEN   128         /* ~2.7 ms at 48 kHz */
#define FADE_OUT_LEN   48         /* ~1.0 ms at 48 kHz */

struct voice {
    const s16 *pcm;
    u32 len;
    u32 pos;
    float vol;
    u8  active;
};

PERSIST static struct voice voices[NUM_VOICES];
PERSIST static s32 audio_handle = -1;
PERSIST static void *audio_out_fn;
PERSIST static void *gadget;
PERSIST static s16 mix_buf[GRAIN * 2];

PERSIST static u8 audio_master = 100;

void audio_set_master(u8 v) {
    if (v > 100) v = 100;
    audio_master = v;
}

int audio_is_active(void) {
    return audio_handle >= 0 && audio_out_fn != 0;
}

int audio_init(s32 h, void *fn, void *G) {
    audio_handle = h;
    audio_out_fn = fn;
    gadget = G;
    for (int i = 0; i < NUM_VOICES; i++) voices[i].active = 0;

    if (h >= 0 && fn) {
        static s16 silence[GRAIN * 2];
        for (int i = 0; i < GRAIN * 2; i++) silence[i] = 0;
        for (int k = 0; k < 3; k++)
            NC(G, fn, (u64)h, (u64)silence, 0, 0, 0, 0);
    }
    return h >= 0 ? 0 : -1;
}

void audio_shutdown(void) {
    for (int i = 0; i < NUM_VOICES; i++) voices[i].active = 0;
    audio_handle = -1;
}

void audio_play(enum asset_id id, float vol) {
    if (audio_handle < 0 || !audio_out_fn) return;
    if (audio_master == 0) return;
    if (id < 0 || id >= ASSET_COUNT) return;
    const struct asset *a = &asset_table[id];
    if (a->fmt != ASSET_FMT_S16_MONO_48K) return;

    const s16 *pcm = (const s16*)(asset_blob + a->offset);

    /* One-at-a-time per asset: kill any running instance of the same
       sound before starting a new one.  Without this, quickly scoring
       two pipes in a row leaves two overlapping score voices that sum
       and hit the soft-clipper. */
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].active && voices[i].pcm == pcm) {
            voices[i].active = 0;
        }
    }

    int slot = -1;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        u32 oldest = 0;
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].pos > oldest) { oldest = voices[i].pos; slot = i; }
        }
    }
    if (slot < 0) return;

    voices[slot].pcm    = pcm;
    voices[slot].len    = a->h;
    voices[slot].pos    = 0;
    voices[slot].vol    = vol;
    voices[slot].active = 1;
}

/* Gentler soft-clip: linear up to ±26000, then 1/2 slope, ceiling
   around ±29000.  This is transparent for a single voice and only
   compresses when two or more sounds overlap. */
static inline int soft_clip(int s) {
    if (s >  32760) return  32760;
    if (s < -32760) return -32760;
    if (s >  26000) return  26000 + (s -  26000) / 2;
    if (s < -26000) return -26000 + (s +  26000) / 2;
    return s;
}

void audio_mix_tick(void) {
    if (audio_handle < 0 || !audio_out_fn) return;

    for (int i = 0; i < GRAIN; i++) {
        int acc = 0;
        for (int v = 0; v < NUM_VOICES; v++) {
            if (!voices[v].active) continue;
            if (voices[v].pos >= voices[v].len) {
                voices[v].active = 0;
                continue;
            }

            s32 sample = voices[v].pcm[voices[v].pos];

            float gain = voices[v].vol;
            u32 pos = voices[v].pos;
            u32 len = voices[v].len;

            if (pos < FADE_IN_LEN)
                gain *= (float)pos / (float)FADE_IN_LEN;
            u32 rem = len - pos;
            if (rem < FADE_OUT_LEN)
                gain *= (float)rem / (float)FADE_OUT_LEN;

            voices[v].pos++;
            acc += (int)(sample * gain);
        }

        acc = (acc * audio_master) / 100;
        acc = soft_clip(acc);

        mix_buf[i*2]   = (s16)acc;
        mix_buf[i*2+1] = (s16)acc;
    }
    NC(gadget, audio_out_fn, (u64)audio_handle, (u64)mix_buf, 0, 0, 0, 0);
}
