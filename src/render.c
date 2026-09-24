#include "render.h"
#include "font.h"
#include "assets.h"

extern const u8 asset_blob[];
extern const u8 asset_blob_end[];

static u32 *fbs[2];
static u32 *cur;
static int   active;

void render_init(u32 *a, u32 *b) {
    fbs[0] = a; fbs[1] = b; cur = a; active = 0;
}

void render_begin_frame(void) { /* nothing */ }
void render_end_frame(void)   { /* nothing */ }
u32 *render_fb(void)          { return cur; }

void render_swap(void) {
    active ^= 1;
    cur = fbs[active];
}

void render_clear(u32 c) {
    for (int i = 0; i < SCR_W * SCR_H; i++) cur[i] = c;
}

void render_fill_rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *row = cur + (y + j) * SCR_W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static const struct asset *get_asset(int id) {
    if (id < 0 || id >= ASSET_COUNT) return 0;
    return &asset_table[id];
}

/* Nearest-neighbour scaled RGBA8-indexed blit with integer scale.
   alpha 0..255 multiplies the source alpha channel. */
void render_blit_scaled(int id, float xf, float yf, float scl, u8 alpha) {
    const struct asset *a = get_asset(id);
    if (!a || a->fmt != ASSET_FMT_RGBA8_INDEXED) return;

    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;
    int x = (int)xf, y = (int)yf;
    int scale = (int)scl;
    if (scale < 1) scale = 1;

    for (int sy = 0; sy < (int)a->h; sy++) {
        int dy = y + sy * scale;
        if (dy + scale <= 0 || dy >= SCR_H) continue;
        const u8 *srow = idx + sy * a->w;
        for (int sx = 0; sx < (int)a->w; sx++) {
            u8 i = srow[sx];
            u32 sr = pal[i*4+0], sg = pal[i*4+1], sb = pal[i*4+2], sa = pal[i*4+3];
            if (sa == 0) continue;
            u8 ea = (u8)((sa * alpha) >> 8);
            if (ea == 0) continue;
            u32 col = ((u32)ea << 24) | (sr << 16) | (sg << 8) | sb;
            int dx = x + sx * scale;
            for (int ty = 0; ty < scale; ty++) {
                int py = dy + ty;
                if (py < 0 || py >= SCR_H) continue;
                u32 *row = cur + py * SCR_W;
                for (int tx = 0; tx < scale; tx++) {
                    int px = dx + tx;
                    if (px < 0 || px >= SCR_W) continue;
                    row[px] = col;
                }
            }
        }
    }
}

/* Tile horizontally: draw enough copies to fill the screen */
void render_blit_tiled(int id, float scroll_x, float scale) {
    const struct asset *a = get_asset(id);
    if (!a) return;
    int tile_w = (int)(a->w * scale);
    if (tile_w <= 0) return;
    int base_x = (int)scroll_x;
    base_x = base_x % tile_w;
    if (base_x > 0) base_x -= tile_w;
    for (int x = base_x; x < SCR_W; x += tile_w)
        render_blit_scaled(id, (float)x, 0.0f, scale, 255);
}

/* For backgrounds that fill more than the screen height, use the actual
   height (512 native -> 1080) */
void render_blit_scaled_bg(int id, float xf, float scl) {
    const struct asset *a = get_asset(id);
    if (!a) return;
    int x = (int)xf;
    int scale = (int)scl;
    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;
    for (int sy = 0; sy < (int)a->h; sy++) {
        int dy = sy * scale;
        if (dy >= SCR_H) break;
        const u8 *srow = idx + sy * a->w;
        for (int sx = 0; sx < (int)a->w; sx++) {
            u8 i = srow[sx];
            u32 sr = pal[i*4+0], sg = pal[i*4+1], sb = pal[i*4+2], sa = pal[i*4+3];
            if (sa == 0) continue;
            u32 col = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
            int dx = x + sx * scale;
            for (int ty = 0; ty < scale; ty++) {
                int py = dy + ty;
                if (py < 0 || py >= SCR_H) continue;
                u32 *row = cur + py * SCR_W;
                for (int tx = 0; tx < scale; tx++) {
                    int px = dx + tx;
                    if (px < 0 || px >= SCR_W) continue;
                    row[px] = col;
                }
            }
        }
    }
}

/* Fixed-point (8.8) RGBA8-indexed blit.  scale_fp = scale * 256. */
void render_blit_scaled_fp(int id, float xf, float yf, int scale_fp, u8 alpha) {
    const struct asset *a = get_asset(id);
    if (!a || a->fmt != ASSET_FMT_RGBA8_INDEXED) return;
    if (scale_fp < 1) scale_fp = 1;

    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;

    int x0 = (int)xf;
    int y0 = (int)yf;

    for (int sy = 0; sy < (int)a->h; sy++) {
        int dy0 = (sy * scale_fp) >> 8;
        int dy1 = ((sy + 1) * scale_fp) >> 8;
        if (dy1 <= dy0) dy1 = dy0 + 1;

        int py0 = y0 + dy0;
        int py1 = y0 + dy1;
        if (py1 <= 0 || py0 >= SCR_H) continue;
        if (py0 < 0) py0 = 0;
        if (py1 > SCR_H) py1 = SCR_H;

        const u8 *srow = idx + sy * a->w;

        for (int sx = 0; sx < (int)a->w; sx++) {
            u8 i = srow[sx];
            u32 sa = pal[i*4+3];
            if (sa == 0) continue;
            u8 ea = (u8)((sa * alpha) >> 8);
            if (ea == 0) continue;

            u32 col = ((u32)ea << 24) | (pal[i*4+0] << 16)
                                       | (pal[i*4+1] << 8) | pal[i*4+2];

            int dx0 = (sx * scale_fp) >> 8;
            int dx1 = ((sx + 1) * scale_fp) >> 8;
            if (dx1 <= dx0) dx1 = dx0 + 1;

            int px0 = x0 + dx0;
            int px1 = x0 + dx1;
            if (px1 <= 0 || px0 >= SCR_W) continue;
            if (px0 < 0) px0 = 0;
            if (px1 > SCR_W) px1 = SCR_W;

            for (int py = py0; py < py1; py++) {
                u32 *row = cur + py * SCR_W;
                for (int px = px0; px < px1; px++) row[px] = col;
            }
        }
    }
}

/* Fixed-point background variant — opaque, ignores source alpha. */
void render_blit_scaled_bg_fp(int id, float xf, int scale_fp) {
    const struct asset *a = get_asset(id);
    if (!a || a->fmt != ASSET_FMT_RGBA8_INDEXED) return;
    if (scale_fp < 1) scale_fp = 1;

    const u8 *pal = asset_blob + a->offset;
    const u8 *idx = pal + 256 * 4;

    int x0 = (int)xf;

    for (int sy = 0; sy < (int)a->h; sy++) {
        int dy0 = (sy * scale_fp) >> 8;
        int dy1 = ((sy + 1) * scale_fp) >> 8;
        if (dy1 <= dy0) dy1 = dy0 + 1;
        if (dy0 >= SCR_H) break;

        int py1 = dy1; if (py1 > SCR_H) py1 = SCR_H;
        const u8 *srow = idx + sy * a->w;

        for (int sx = 0; sx < (int)a->w; sx++) {
            u8 i = srow[sx];
            u32 col = 0xFF000000u | (pal[i*4+0] << 16)
                                   | (pal[i*4+1] << 8) | pal[i*4+2];

            int dx0 = (sx * scale_fp) >> 8;
            int dx1 = ((sx + 1) * scale_fp) >> 8;
            if (dx1 <= dx0) dx1 = dx0 + 1;

            int px0 = x0 + dx0;
            int px1 = x0 + dx1;
            if (px1 <= 0 || px0 >= SCR_W) continue;
            if (px0 < 0) px0 = 0;
            if (px1 > SCR_W) px1 = SCR_W;

            for (int py = dy0; py < py1; py++) {
                u32 *row = cur + py * SCR_W;
                for (int px = px0; px < px1; px++) row[px] = col;
            }
        }
    }
}

void render_text(int x, int y, const char *s, u32 c, int scale) {
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 127) ch = '?';
        const u8 *g = ps_font8x8[ch - 32];
        for (int r = 0; r < 8; r++) {
            u8 bits = g[r];
            if (!bits) continue;
            for (int b = 0; b < 8; b++) {
                if (!(bits & (0x80 >> b))) continue;
                int bx = x + b * scale;
                int by = y + r * scale;
                for (int ty = 0; ty < scale; ty++) {
                    int py = by + ty;
                    if (py < 0 || py >= SCR_H) continue;
                    u32 *row = cur + py * SCR_W;
                    for (int tx = 0; tx < scale; tx++) {
                        int px = bx + tx;
                        if (px < 0 || px >= SCR_W) continue;
                        row[px] = c;
                    }
                }
            }
        }
        x += 8 * scale;
    }
}

int render_text_width(const char *s, int scale) {
    int n = 0; while (s[n]) n++;
    return n * 8 * scale;
}

void render_text_center(int y, const char *s, u32 c, int scale) {
    int w = render_text_width(s, scale);
    render_text((SCR_W - w) / 2, y, s, c, scale);
}
