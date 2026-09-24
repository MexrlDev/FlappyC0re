/* SPDX-License-Identifier: MIT
   Anti-aliased text rendering.  Uses a TTF glyph atlas baked at build
   time by tools/bake_font.py, sampled with bilinear filtering. */
#include "font_aa.h"
#include "font_aa_metrics.h"

/* Embed the raw bitmap at assembly time.  The path is relative to the
   directory gcc runs from (normally the project root). */
__asm__(
    ".section .rodata\n"
    ".globl font_aa_bitmap\n"
    ".balign 16\n"
    "font_aa_bitmap:\n"
    ".incbin \"src/font_aa.bin\"\n"
    ".globl font_aa_bitmap_end\n"
    "font_aa_bitmap_end:\n"
    ".previous\n"
);

extern const u8 font_aa_bitmap[];
extern const u8 font_aa_bitmap_end[];

/* Sample the atlas at fractional source coordinate (8.8 fixed-point). */
static u8 sample_bilinear(const u8 *src, int src_w, int src_h,
                          int fx, int fy)
{
    int x0 = fx >> 8;
    int y0 = fy >> 8;
    int tx = fx & 0xFF;
    int ty = fy & 0xFF;

    if (x0 < 0) { x0 = 0; tx = 0; }
    if (y0 < 0) { y0 = 0; ty = 0; }
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 >= src_w) x1 = src_w - 1;
    if (y1 >= src_h) y1 = src_h - 1;
    if (x0 >= src_w) x0 = src_w - 1;
    if (y0 >= src_h) y0 = src_h - 1;

    u32 v00 = src[y0 * src_w + x0];
    u32 v10 = src[y0 * src_w + x1];
    u32 v01 = src[y1 * src_w + x0];
    u32 v11 = src[y1 * src_w + x1];

    u32 a = v00 + (((s32)(v10 - v00) * tx) >> 8);
    u32 b = v01 + (((s32)(v11 - v01) * tx) >> 8);
    return (u8)(a + (((s32)(b - a) * ty) >> 8));
}

void font_aa_draw(u32 *fb, int x0, int y0, const char *s,
                  u32 color, int target_h)
{
    if (!s || !*s) return;
    if (target_h < 4) target_h = 4;

    /* Fixed-point 16.16 scale: target_h / FONT_AA_PX_HEIGHT */
    u32 scale = ((u32)target_h << 16) / (u32)FONT_AA_PX_HEIGHT;

    const u8 *bmp = font_aa_bitmap;
    int pen_x = x0;

    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = '?';
        const struct font_aa_glyph *g = &font_aa_metrics[ch - 32];

        if (g->w && g->h) {
            int gx = pen_x + (int)(((s64)g->bx * (s64)scale) >> 16);
            int gy = y0    + (int)(((s64)g->by * (s64)scale) >> 16);
            int gw = (int)(((u64)g->w * scale) >> 16);
            int gh = (int)(((u64)g->h * scale) >> 16);
            if (gw < 1) gw = 1;
            if (gh < 1) gh = 1;

            for (int dy = 0; dy < gh; dy++) {
                int py = gy + dy;
                if (py < 0 || py >= SCR_H) continue;

                u32 src_y = ((u32)dy * (u32)g->h * 256) / (u32)gh;
                int fy    = (int)src_y;

                u32 *row = fb + py * SCR_W;

                for (int dx = 0; dx < gw; dx++) {
                    int px = gx + dx;
                    if (px < 0 || px >= SCR_W) continue;

                    u32 src_x = ((u32)dx * (u32)g->w * 256) / (u32)gw;
                    int fx    = (int)src_x;

                    u8 a = sample_bilinear(bmp, (int)g->w, (int)g->h, fx, fy);
                    if (a == 0) continue;

                    u32 dst = row[px];
                    u32 inv = 255 - a;
                    u32 cr  = (color >> 16) & 0xFF;
                    u32 cg  = (color >>  8) & 0xFF;
                    u32 cb  =  color        & 0xFF;
                    u32 dr  = (dst   >> 16) & 0xFF;
                    u32 dg  = (dst   >>  8) & 0xFF;
                    u32 db  =  dst          & 0xFF;
                    u32 r   = (cr * a + dr * inv) / 255;
                    u32 gg  = (cg * a + dg * inv) / 255;
                    u32 b   = (cb * a + db * inv) / 255;
                    row[px] = 0xFF000000u | (r << 16) | (gg << 8) | b;
                }
            }
        }

        bmp += (u32)g->w * (u32)g->h;
        u32 adv = ((u32)g->adv * scale) >> 16;
        if (adv < 1) adv = 1;
        pen_x += (int)adv;
    }
}

int font_aa_width(const char *s, int target_h) {
    if (!s) return 0;
    if (target_h < 4) target_h = 4;
    u32 scale = ((u32)target_h << 16) / (u32)FONT_AA_PX_HEIGHT;
    int w = 0;
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = '?';
        const struct font_aa_glyph *g = &font_aa_metrics[ch - 32];
        u32 adv = ((u32)g->adv * scale) >> 16;
        if (adv < 1) adv = 1;
        w += (int)adv;
    }
    return w;
}
