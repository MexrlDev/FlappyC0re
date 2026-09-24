/* SPDX-License-Identifier: MIT */
#include "audio.h"

extern const u8 asset_blob[];

#define NUM_VOICES 32
#define GRAIN      2048
#define FADE_LEN   256        /* ~5.3 ms at 48 kHz */

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

    /* Prime the device with several silent buffers so the first real
       sound request doesn't stall while the hardware spins up. */
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
    if (id < 0 || id >= ASSET_COUNT) return;
    const struct asset *a = &asset_table[id];
    if (a->fmt != ASSET_FMT_S16_MONO_48K) return;

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

    voices[slot].pcm    = (const s16*)(asset_blob + a->offset);
    voices[slot].len    = a->h;
    voices[slot].pos    = 0;
    voices[slot].vol    = vol;
    voices[slot].active = 1;
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

            /* Fade-in / fade-out envelope to eliminate clicks at the
               start and end of every voice.  This is the "cutting"
               artifact — hard starts and hard stops of the source WAV. */
            float gain = voices[v].vol;
            u32 pos = voices[v].pos;
            u32 len = voices[v].len;

            if (pos < FADE_LEN) {
                gain *= (float)pos / (float)FADE_LEN;
            }
            u32 rem = len - pos;
            if (rem < FADE_LEN) {
                gain *= (float)rem / (float)FADE_LEN;
            }

            voices[v].pos++;
            acc += (int)(sample * gain);
        }
        if (acc > 32767)  acc = 32767;
        if (acc < -32768) acc = -32768;
        mix_buf[i*2]   = (s16)acc;
        mix_buf[i*2+1] = (s16)acc;
    }
    NC(gadget, audio_out_fn, (u64)audio_handle, (u64)mix_buf, 0, 0, 0, 0);
}
