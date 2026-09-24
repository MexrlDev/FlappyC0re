/* SPDX-License-Identifier: MIT */
#include "audio.h"

extern const u8 asset_blob[];

#define NUM_VOICES 8
#define GRAIN 2048

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

int audio_init(s32 h, void *fn, void *G) {
    audio_handle = h;
    audio_out_fn = fn;
    gadget = G;
    for (int i = 0; i < NUM_VOICES; i++) voices[i].active = 0;
    return h >= 0 ? 0 : -1;
}

void audio_shutdown(void) {
    for (int i = 0; i < NUM_VOICES; i++) voices[i].active = 0;
    audio_handle = -1;
}

void audio_play(enum asset_id id, float vol) {
    if (audio_handle < 0 || !audio_out_fn) return;
    if (id < 0 || id >= ASSET_COUNT) return;
    const struct asset *a = &asset_table[id];
    if (a->fmt != ASSET_FMT_S16_MONO_48K) return;

    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].active) {
            voices[i].pcm = (const s16*)(asset_blob + a->offset);
            voices[i].len = a->h;
            voices[i].pos = 0;
            voices[i].vol = vol;
            voices[i].active = 1;
            return;
        }
    }
}

void audio_mix_tick(void) {
    if (audio_handle < 0 || !audio_out_fn) return;

    for (int i = 0; i < GRAIN; i++) {
        int acc = 0;
        for (int v = 0; v < NUM_VOICES; v++) {
            if (!voices[v].active) continue;
            if (voices[v].pos >= voices[v].len) { voices[v].active = 0; continue; }
            float s = voices[v].pcm[voices[v].pos++];
            acc += (int)(s * voices[v].vol);
        }
        if (acc > 32767)  acc = 32767;
        if (acc < -32768) acc = -32768;
        mix_buf[i*2] = (s16)acc;
        mix_buf[i*2+1] = (s16)acc;
    }
    NC(gadget, audio_out_fn, (u64)audio_handle, (u64)mix_buf, 0, 0, 0, 0);
}
