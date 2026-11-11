/* SPDX-License-Identifier: MIT
 * Window, input and widgets. Nothing is redrawn unless a key arrives or the screen asks for fresh data, so the tuner
 * does not heat the console it is measuring. Design size is 640x480; U() scales to the real screen.
 *
 * Previews on a PC (no console needed):  --size 640x480 --script file
 *   script lines: up down left right a b x y l r start sel | wait <ms> | shot <file.bmp> | quit
 */
#include "tuner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Col C_BG = {0x1C, 0x1C, 0x20}, C_PANEL = {0x26, 0x26, 0x2A}, C_PANEL2 = {0x33, 0x33, 0x39},
          C_LINE = {0x3C, 0x3C, 0x44}, C_TXT = {0xF0, 0xF0, 0xF2}, C_DIM = {0xBE, 0xBE, 0xC6},
          C_MUTED = {0x8C, 0x8C, 0x96}, C_ACCENT = {0x00, 0xC3, 0xE3}, C_ON_ACCENT = {0x0B, 0x22, 0x28},
          C_OK = {0x5F, 0xD3, 0x8D}, C_WARN = {0xF5, 0xB8, 0x4B}, C_BAD = {0xF2, 0x6D, 0x6D};

volatile int ui_running = 1;
int UW = 640, UH = 480;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_GameController *pad;
static TTF_Font *fonts[F_COUNT];
static float scale = 1.0f;
static char font_path[300];
static FILE *script;

/* ── Text cache: rendering a string with FreeType every frame is what costs CPU ─ */
#define CACHE_N 96
typedef struct { char s[400]; FontId f; Col c; SDL_Texture *t; int w, h; Uint32 used; } CacheItem;
static CacheItem cache[CACHE_N];
static Uint32 cache_clock;

static void cache_clear(void) {
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].t) SDL_DestroyTexture(cache[i].t);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
}

static CacheItem *cache_get(FontId f, const char *s, Col c) {
    CacheItem *free_slot = &cache[0];
    size_t len = strlen(s);
    if (len >= sizeof(cache[0].s)) return NULL;
    cache_clock++;
    for (int i = 0; i < CACHE_N; i++) {
        CacheItem *e = &cache[i];
        if (e->t && e->f == f && e->c.r == c.r && e->c.g == c.g && e->c.b == c.b && !strcmp(e->s, s)) {
            e->used = cache_clock;
            return e;
        }
        if (e->used < free_slot->used) free_slot = e;
    }
    SDL_Color col = {c.r, c.g, c.b, 255};
    SDL_Surface *sur = TTF_RenderUTF8_Blended(fonts[f], s, col);
    if (!sur) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, sur);
    if (free_slot->t) SDL_DestroyTexture(free_slot->t);
    memcpy(free_slot->s, s, len + 1);
    free_slot->f = f;
    free_slot->c = c;
    free_slot->t = tex;
    free_slot->w = sur->w;
    free_slot->h = sur->h;
    free_slot->used = cache_clock;
    SDL_FreeSurface(sur);
    return tex ? free_slot : NULL;
}

/* ── Size ───────────────────────────────────────────────────────────────── */
int U(int v) { return (int)lroundf((float)v * scale); }

static void open_fonts(void) {
    static const int base[F_COUNT] = {14, 17, 22, 34};
    for (int i = 0; i < F_COUNT; i++) {
        if (fonts[i]) TTF_CloseFont(fonts[i]);
        int px = (int)lroundf((float)base[i] * scale);
        if (px < 10) px = 10;
        fonts[i] = TTF_OpenFont(font_path, px);
    }
}

static void apply_size(int w, int h) {
    UW = w;
    UH = h;
    float sx = (float)w / 640.0f, sy = (float)h / 480.0f;
    scale = sx < sy ? sx : sy;
    if (scale < 0.66f) scale = 0.66f;
    if (scale > 2.5f) scale = 2.5f;
    cache_clear();
    open_fonts();
}

/* ── Start / stop ───────────────────────────────────────────────────────── */
static void open_pad(void) {
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
}

int ui_init(int argc, char **argv) {
    int fw = 0, fh = 0;
    const char *script_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &fw, &fh);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_path = argv[++i];
    }
    if (script_path) script = fopen(script_path, "r");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return -1;
    if (TTF_Init() != 0) return -1;

    for (int i = 0; plat->fonts[i]; i++)
        if (sys_exists(plat->fonts[i])) { snprintf(font_path, sizeof(font_path), "%s", rp(plat->fonts[i])); break; }
    if (!font_path[0]) return -2;

    /* Ask for the mode of the output before creating the window: under Sway a window asked at another size
       comes back at the size of the panel's default and gets cut (it happened to zfront on 4:3 screens) */
    SDL_DisplayMode dm;
    int w = 640, h = 480;
    if (fw > 0) { w = fw; h = fh; }
    else if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0) { w = dm.w; h = dm.h; }
    Uint32 flags = SDL_WINDOW_SHOWN | (fw > 0 ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    win = SDL_CreateWindow("R36 Tuner Next", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win) return -1;
    /* a script on a real console uses its real renderer: that is what it is there to test */
    const char *driver = SDL_GetCurrentVideoDriver();
    ren = SDL_CreateRenderer(win, -1, driver && !strcmp(driver, "dummy") ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) return -1;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_GetRendererOutputSize(ren, &w, &h);
    apply_size(w, h);
    open_pad();
    return 0;
}

void ui_quit(void) {
    cache_clear();
    for (int i = 0; i < F_COUNT; i++) if (fonts[i]) TTF_CloseFont(fonts[i]);
    if (pad) SDL_GameControllerClose(pad);
    if (ren) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); SDL_DestroyRenderer(ren); }
    if (win) SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    if (script) fclose(script);
}

/* ── Input: holding a direction repeats, the stick works like the D-pad ──── */
enum { HK_UP, HK_DOWN, HK_LEFT, HK_RIGHT, HK_N };
static Uint32 hold_at[HK_N], next_at[HK_N];
static int axis_x, axis_y;
#define REPEAT_FIRST 350
#define REPEAT_NEXT 70
#define AXIS_ON 16000

static void hold(int key, int on) {
    hold_at[key] = on ? SDL_GetTicks() : 0;
    next_at[key] = on ? hold_at[key] + REPEAT_FIRST : 0;
}

static void set_dir(Keys *k, int key) {
    if (key == HK_UP) k->up = 1;
    if (key == HK_DOWN) k->down = 1;
    if (key == HK_LEFT) k->left = 1;
    if (key == HK_RIGHT) k->right = 1;
}

static int any_key(const Keys *k) {
    return k->up || k->down || k->left || k->right || k->a || k->b || k->x || k->y || k->l || k->r || k->start || k->sel;
}

static void handle_event(const SDL_Event *e, Keys *k) {
    switch (e->type) {
    case SDL_QUIT: ui_running = 0; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            if (w != UW || h != UH) apply_size(w, h);
        }
        break;
    case SDL_CONTROLLERDEVICEADDED: if (!pad) open_pad(); break;
    case SDL_KEYDOWN:
        if (e->key.repeat) break;
        switch (e->key.keysym.sym) {
        case SDLK_UP: k->up = 1; hold(HK_UP, 1); break;
        case SDLK_DOWN: k->down = 1; hold(HK_DOWN, 1); break;
        case SDLK_LEFT: k->left = 1; hold(HK_LEFT, 1); break;
        case SDLK_RIGHT: k->right = 1; hold(HK_RIGHT, 1); break;
        case SDLK_RETURN: k->a = 1; break;
        case SDLK_BACKSPACE: k->b = 1; break;
        case SDLK_ESCAPE: k->sel = 1; break;
        case SDLK_q: k->l = 1; break;
        case SDLK_w: k->r = 1; break;
        }
        break;
    case SDL_KEYUP:
        switch (e->key.keysym.sym) {
        case SDLK_UP: hold(HK_UP, 0); break;
        case SDLK_DOWN: hold(HK_DOWN, 0); break;
        case SDLK_LEFT: hold(HK_LEFT, 0); break;
        case SDLK_RIGHT: hold(HK_RIGHT, 0); break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int on = e->type == SDL_CONTROLLERBUTTONDOWN;
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (on) k->up = 1; hold(HK_UP, on); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (on) k->down = 1; hold(HK_DOWN, on); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (on) k->left = 1; hold(HK_LEFT, on); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (on) k->right = 1; hold(HK_RIGHT, on); break;
        case SDL_CONTROLLER_BUTTON_A: k->a |= on; break;
        case SDL_CONTROLLER_BUTTON_B: k->b |= on; break;
        case SDL_CONTROLLER_BUTTON_X: k->x |= on; break;
        case SDL_CONTROLLER_BUTTON_Y: k->y |= on; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k->l |= on; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k->r |= on; break;
        case SDL_CONTROLLER_BUTTON_START: k->start |= on; break;
        case SDL_CONTROLLER_BUTTON_BACK: k->sel |= on; break;
        }
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            int d = e->caxis.value > AXIS_ON ? 1 : e->caxis.value < -AXIS_ON ? -1 : 0;
            if (d != axis_x) {
                hold(HK_LEFT, d < 0); hold(HK_RIGHT, d > 0);
                if (d < 0) k->left = 1;
                if (d > 0) k->right = 1;
                axis_x = d;
            }
        } else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            int d = e->caxis.value > AXIS_ON ? 1 : e->caxis.value < -AXIS_ON ? -1 : 0;
            if (d != axis_y) {
                hold(HK_UP, d < 0); hold(HK_DOWN, d > 0);
                if (d < 0) k->up = 1;
                if (d > 0) k->down = 1;
                axis_y = d;
            }
        }
        break;
    }
}

/* Test script: one action per call */
static const char *pending_shot;
static int script_step(Keys *k, int *wait_ms) {
    static char line[300];
    if (!script) return 0;
    if (!fgets(line, sizeof(line), script)) { ui_running = 0; return 1; }
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "wait ", 5)) { *wait_ms = atoi(line + 5); return 1; }
    if (!strncmp(line, "shot ", 5)) { pending_shot = line + 5; return 1; }
    if (!strcmp(line, "up")) k->up = 1;
    else if (!strcmp(line, "down")) k->down = 1;
    else if (!strcmp(line, "left")) k->left = 1;
    else if (!strcmp(line, "right")) k->right = 1;
    else if (!strcmp(line, "a")) k->a = 1;
    else if (!strcmp(line, "b")) k->b = 1;
    else if (!strcmp(line, "x")) k->x = 1;
    else if (!strcmp(line, "y")) k->y = 1;
    else if (!strcmp(line, "l")) k->l = 1;
    else if (!strcmp(line, "r")) k->r = 1;
    else if (!strcmp(line, "start")) k->start = 1;
    else if (!strcmp(line, "sel")) k->sel = 1;
    else if (!strcmp(line, "quit")) ui_running = 0;
    return 1;
}

Keys ui_poll(int timeout_ms) {
    Keys k;
    memset(&k, 0, sizeof(k));
    if (script) {
        int w = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e, &k);
        script_step(&k, &w);
        if (w) SDL_Delay(w);
        return k;
    }
    Uint32 end = SDL_GetTicks() + (Uint32)timeout_ms;
    for (;;) {
        SDL_Event e;
        Uint32 now = SDL_GetTicks();
        int wait = (int)(end - now);
        if ((Sint32)(end - now) < 0) wait = 0;
        for (int i = 0; i < HK_N; i++)
            if (next_at[i]) {
                int d = (Sint32)(next_at[i] - now) < 0 ? 0 : (int)(next_at[i] - now);
                if (d < wait) wait = d;
            }
        if (SDL_WaitEventTimeout(&e, wait)) {
            handle_event(&e, &k);
            while (SDL_PollEvent(&e)) handle_event(&e, &k);
        }
        now = SDL_GetTicks();
        for (int i = 0; i < HK_N; i++)
            if (next_at[i] && (Sint32)(now - next_at[i]) >= 0) {
                set_dir(&k, i);
                next_at[i] = now + REPEAT_NEXT;
            }
        if (any_key(&k) || !ui_running || (Sint32)(now - end) >= 0) return k;
    }
}

/* ── Drawing ────────────────────────────────────────────────────────────── */
static void col(Col c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255); }

void ui_clear(void) { col(C_BG); SDL_RenderClear(ren); }

int ui_col(int i) { return UI_M + (int)lroundf((float)i * (float)(UW - 2 * UI_M + UI_GAP) / 12.0f); }
int ui_span(int i, int n) { return ui_col(i + n) - UI_GAP - ui_col(i); }

/* Previews: R36T_GRID=1 draws the grid over the screen */
static void grid_overlay(void) {
    if (!getenv("R36T_GRID")) return;
    SDL_SetRenderDrawColor(ren, 255, 0, 90, 40);
    for (int i = 0; i < 12; i++) {
        SDL_Rect r = {ui_col(i), 0, ui_span(i, 1), UH};
        SDL_RenderFillRect(ren, &r);
    }
    SDL_SetRenderDrawColor(ren, 255, 0, 90, 200);
    SDL_RenderDrawLine(ren, UI_M, 0, UI_M, UH);
    SDL_RenderDrawLine(ren, UW - UI_M - 1, 0, UW - UI_M - 1, UH);
}

void ui_present(void) {
    grid_overlay();
    if (pending_shot) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, UW, UH, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, pending_shot);
            SDL_FreeSurface(s);
        }
        pending_shot = NULL;
    }
    SDL_RenderPresent(ren);
}

void ui_rect(int x, int y, int w, int h, Col c) {
    SDL_Rect r = {x, y, w, h};
    col(c);
    SDL_RenderFillRect(ren, &r);
}

void ui_line(int x1, int y1, int x2, int y2, Col c) {
    col(c);
    SDL_RenderDrawLine(ren, x1, y1, x2, y2);
}

static SDL_Vertex vtx(float x, float y, Col c, int a) {
    SDL_Vertex v;
    v.position.x = x; v.position.y = y;
    v.color.r = c.r; v.color.g = c.g; v.color.b = c.b; v.color.a = (Uint8)a;
    v.tex_coord.x = v.tex_coord.y = 0;
    return v;
}

static void quad(SDL_Vertex a, SDL_Vertex b, SDL_Vertex c, SDL_Vertex d) {
    SDL_Vertex v[6] = {a, b, c, a, c, d};
    SDL_RenderGeometry(ren, NULL, v, 6, NULL, 0);
}

/* A line as a band with soft edges: SDL lines have no antialiasing */
void ui_smooth_line(float x1, float y1, float x2, float y2, float thick, Col c, int alpha) {
    float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len, ny = dx / len, h = thick / 2, f = 1.0f;
    quad(vtx(x1 + nx * h, y1 + ny * h, c, alpha), vtx(x2 + nx * h, y2 + ny * h, c, alpha),
         vtx(x2 - nx * h, y2 - ny * h, c, alpha), vtx(x1 - nx * h, y1 - ny * h, c, alpha));
    quad(vtx(x1 + nx * h, y1 + ny * h, c, alpha), vtx(x2 + nx * h, y2 + ny * h, c, alpha),
         vtx(x2 + nx * (h + f), y2 + ny * (h + f), c, 0), vtx(x1 + nx * (h + f), y1 + ny * (h + f), c, 0));
    quad(vtx(x1 - nx * h, y1 - ny * h, c, alpha), vtx(x2 - nx * h, y2 - ny * h, c, alpha),
         vtx(x2 - nx * (h + f), y2 - ny * (h + f), c, 0), vtx(x1 - nx * (h + f), y1 - ny * (h + f), c, 0));
}

/* Area under a segment, fading towards the base */
void ui_area(float x1, float y1, float x2, float y2, float base, Col c, int a_top, int a_base) {
    quad(vtx(x1, y1, c, a_top), vtx(x2, y2, c, a_top), vtx(x2, base, c, a_base), vtx(x1, base, c, a_base));
}

void ui_rect_a(int x, int y, int w, int h, Col c, int alpha) {
    SDL_Rect r = {x, y, w, h};
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, (Uint8)alpha);
    SDL_RenderFillRect(ren, &r);
}

/* Round corner as a white texture with soft edges, tinted when drawn */
static SDL_Texture *corner_tex[64];
static SDL_Texture *corner(int rad) {
    if (rad < 1) rad = 1;
    if (rad > 63) rad = 63;
    if (corner_tex[rad]) return corner_tex[rad];
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, rad, rad, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    Uint32 *px = s->pixels;
    for (int y = 0; y < rad; y++)
        for (int x = 0; x < rad; x++) {
            float dx = (float)rad - (float)x - 0.5f, dy = (float)rad - (float)y - 0.5f;
            float a = (float)rad - sqrtf(dx * dx + dy * dy) + 0.5f;
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            px[y * (s->pitch / 4) + x] = ((Uint32)(a * 255) << 24) | 0xFFFFFF;
        }
    corner_tex[rad] = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureBlendMode(corner_tex[rad], SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return corner_tex[rad];
}

static int checking(void);
static int in_scroll;
void ui_round(int x, int y, int w, int h, int rad, Col c) {
    if (checking() && !in_scroll && w > U(8) && (x < UI_M || x + w > UW - UI_M))
        fprintf(stderr, "[R36T-MARGIN] %dx%d panel x=%d w=%d\n", UW, UH, x, w);
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    ui_rect(x + rad, y, w - 2 * rad, h, c);
    ui_rect(x, y + rad, rad, h - 2 * rad, c);
    ui_rect(x + w - rad, y + rad, rad, h - 2 * rad, c);
    SDL_Texture *t = corner(rad);
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_Rect d = {x, y, rad, rad};
    SDL_RenderCopyEx(ren, t, NULL, &d, 0, NULL, SDL_FLIP_NONE);
    d.x = x + w - rad;
    SDL_RenderCopyEx(ren, t, NULL, &d, 0, NULL, SDL_FLIP_HORIZONTAL);
    d.y = y + h - rad;
    SDL_RenderCopyEx(ren, t, NULL, &d, 0, NULL, (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
    d.x = x;
    SDL_RenderCopyEx(ren, t, NULL, &d, 0, NULL, SDL_FLIP_VERTICAL);
}

int ui_tw(FontId f, const char *s) { int w = 0; TTF_SizeUTF8(fonts[f], s, &w, NULL); return w; }
int ui_fh(FontId f) { return TTF_FontLineSkip(fonts[f]); }

static int in_footer;
void ui_scroll_mark(int on) { in_scroll = on; }

static int checking(void) {
    static int v = -1;
    if (v < 0) v = getenv("R36T_CHECK") != NULL;
    return v;
}

void ui_check_fits(const char *where, int bottom_of_content, int limit) {
    if (checking() && bottom_of_content > limit)
        fprintf(stderr, "[R36T-OVERLAP] %dx%d %s: %d > %d\n", UW, UH, where, bottom_of_content, limit);
}

int ui_text(FontId f, const char *s, int x, int y, Col c) {
    if (!s || !s[0]) return 0;
    CacheItem *e = cache_get(f, s, c);
    if (checking()) {
        if (!e) fprintf(stderr, "[R36T-DROP] %dx%d \"%s\"\n", UW, UH, s);
        /* every character has to exist in the font */
        for (const unsigned char *p = (const unsigned char *)s; *p;) {
            Uint32 cp = *p;
            int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
            if (n == 2) cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
            if (n == 3) cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (n == 4) cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            if (!TTF_GlyphIsProvided32(fonts[f], cp)) fprintf(stderr, "[R36T-GLYPH] U+%04X in \"%s\"\n", cp, s);
            p += n;
        }
    }
    if (!e) return 0;
    if (checking() && (x < 0 || y < 0 || x + e->w > UW || y + e->h > UH))
        fprintf(stderr, "[R36T-OUT] %dx%d \"%s\" x=%d y=%d w=%d h=%d\n", UW, UH, s, x, y, e->w, e->h);
    if (checking() && (x < UI_M || x + e->w > UW - UI_M))
        fprintf(stderr, "[R36T-MARGIN] %dx%d \"%s\" x=%d w=%d\n", UW, UH, s, x, e->w);
    if (checking() && !in_footer && y + e->h > ui_content_bottom())
        fprintf(stderr, "[R36T-FOOTER] %dx%d \"%s\" y=%d h=%d\n", UW, UH, s, y, e->h);
    SDL_Rect d = {x, y, e->w, e->h};
    SDL_RenderCopy(ren, e->t, NULL, &d);
    return e->w;
}

int ui_text_r(FontId f, const char *s, int rx, int y, Col c) { return ui_text(f, s, rx - ui_tw(f, s), y, c); }
int ui_text_c(FontId f, const char *s, int cx, int y, Col c) { return ui_text(f, s, cx - ui_tw(f, s) / 2, y, c); }

/* Breaks on spaces; UTF-8 safe because it only cuts at spaces */
static int wrap_run(FontId f, const char *s, int x, int y, int w, Col c, int draw, int center) {
    char line[400];
    const char *p = s;
    int lh = ui_fh(f);
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t para = nl ? (size_t)(nl - p) : strlen(p);
        size_t used = 0, last = 0;
        while (used < para) {
            size_t e = used;
            while (e < para && p[e] != ' ') e++;
            size_t n = e < sizeof(line) ? e : sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            if (ui_tw(f, line) > w && last > 0) break;
            last = e;
            used = e < para ? e + 1 : e;
        }
        if (last == 0) last = para;
        size_t n = last < sizeof(line) ? last : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (draw) ui_text(f, line, center ? x + (w - ui_tw(f, line)) / 2 : x, y, c);
        y += lh;
        p += last;
        if (*p == ' ' || *p == '\n') p++;
    }
    return y;
}

int ui_wrap(FontId f, const char *s, int x, int y, int w, Col c) { return wrap_run(f, s, x, y, w, c, 1, 0); }
int ui_wrap_h(FontId f, const char *s, int w) { return wrap_run(f, s, 0, 0, w, C_TXT, 0, 0); }
int ui_wrap_c(FontId f, const char *s, int y, int w, Col c) { return wrap_run(f, s, (UW - w) / 2, y, w, c, 1, 1); }

void ui_fit(char *out, int n, FontId f, const char *s, int w) {
    snprintf(out, n, "%s", s);
    if (ui_tw(f, out) <= w) return;
    size_t len = strlen(out);
    if (checking()) fprintf(stderr, "[R36T-CUT] %dx%d \"%s\"\n", UW, UH, s);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        memcpy(out + len, "…", 4);
        if (ui_tw(f, out) <= w) return;
    }
}

/* ── Widgets ────────────────────────────────────────────────────────────── */
#define HEADER_H 11   /* grid units */
#define FOOTER_H 8

int ui_content_top(void) { return G(HEADER_H); }
int ui_content_bottom(void) { return UH - G(FOOTER_H); }

void ui_header(const char *title, const char *right) {
    ui_text(F_LG, title, UI_M, (G(HEADER_H) - ui_fh(F_LG)) / 2, C_TXT);
    if (right) ui_text_r(F_SM, right, UW - UI_M, (G(HEADER_H) - ui_fh(F_SM)) / 2, C_DIM);
}

void ui_hints(const char *const hints[][2]) {
    int y0 = UH - G(FOOTER_H);
    ui_rect(0, y0, UW, G(FOOTER_H), C_PANEL);
    int x = UI_M, d = U(18), ty = y0 + (G(FOOTER_H) - ui_fh(F_SM)) / 2, by = y0 + (G(FOOTER_H) - d) / 2;
    in_footer = 1;
    for (int i = 0; hints[i][0]; i++) {
        const char *key = hints[i][0];
        int kw = ui_tw(F_SM, key), bw = kw + U(10) > d ? kw + U(10) : d;
        ui_round(x, by, bw, d, d / 2, C_DIM);
        ui_text_c(F_SM, key, x + bw / 2, by + (d - ui_fh(F_SM)) / 2, C_BG);
        x += bw + G(1);
        x += ui_text(F_SM, hints[i][1], x, ty, C_DIM) + G(3);
    }
    in_footer = 0;
}

void ui_bar(int x, int y, int w, int h, float frac, Col c) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_round(x, y, w, h, h / 2, C_PANEL2);
    int f = (int)(frac * (float)w);
    if (f > 0) ui_round(x, y, f < h ? h : f, h, h / 2, c);
}

/* The history of a value. How much it shows depends on the room it has:
   small: line and area. Medium: also a scale and the limit. Large: also the time axis and minimum, average and maximum. */
static void card_chart(int x, int y, int w, int h, const float *v, int n, float lo, float hi, Col c, const char *unit, float limit) {
    int fh = ui_fh(F_SM), medium = h >= G(12), large = h >= G(24);
    char a[48];

    if (large && n > 1) {
        float mn = v[0], mx = v[0], sum = 0;
        for (int i = 0; i < n; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; sum += v[i]; }
        snprintf(a, sizeof(a), "%s %.0f   %s %.0f   %s %.0f %s", _("min"), mn, _("avg"), sum / (float)n, _("max"), mx, unit ? unit : "");
        if (ui_tw(F_SM, a) <= w) {
            h -= fh + G(1);
            ui_text(F_SM, a, x, y + h + G(1), C_DIM);
        }
    }
    int pb = y + h - (large ? fh + G(1) : 0), pl = x, ph = pb - y;
    if (medium) {
        snprintf(a, sizeof(a), "%.0f", hi);
        pl += ui_tw(F_SM, a) + UI_GAP;
        for (int i = 0; i < 3; i++) {
            int gy = y + i * (ph - 1) / 2;
            ui_line(pl, gy, x + w, gy, C_LINE);
            snprintf(a, sizeof(a), "%.0f", hi - (float)i * (hi - lo) / 2);
            int ty = gy - fh / 2;
            if (ty < y - fh / 4) ty = y;
            if (ty + fh > pb) ty = pb - fh;
            ui_text_r(F_SM, a, pl - UI_GAP, ty, C_MUTED);
        }
    }
    if (large) {
        ui_text(F_SM, "-1 min", pl, pb + G(1), C_MUTED);
        ui_text_r(F_SM, _("now"), x + w, pb + G(1), C_MUTED);
    }
    int pw = x + w - pl;
    if (limit > lo && limit < hi && medium) {
        float ly = (float)pb - (limit - lo) * (float)ph / (hi - lo);
        for (int dx = 0; dx < pw; dx += G(3)) ui_rect_a(pl + dx, (int)ly, G(2) < pw - dx ? G(2) : pw - dx, U(1) > 0 ? U(1) : 1, C_BAD, 200);
    }
    if (n < 2 || hi <= lo) return;
    float px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        float f = (v[i] - lo) / (hi - lo);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        float ax = (float)(x + w) - (float)(n - 1 - i) * (float)pw / (float)(HIST_N - 1);
        float ay = (float)pb - f * (float)(ph - 1);
        if (i) {
            ui_area(px, py, ax, ay, (float)pb, c, 60, 0);
            ui_smooth_line(px, py, ax, ay, (float)U(2), c, 255);
        }
        px = ax;
        py = ay;
    }
    int r = U(4);
    in_scroll = 1;     /* the last point can touch the edge of the card, never the margin */
    ui_round((int)px - r, (int)py - r, 2 * r, 2 * r, r, c);
    in_scroll = 0;
}

void ui_card(int x, int y, int w, int h, const char *title, const char *value, Col value_col, const char *sub,
             const float *hist, float lo, float hi, Col c, const char *unit, float limit) {
    ui_round(x, y, w, h, UI_RAD, C_PANEL);
    ui_text(F_SM, title, x + UI_PAD, y + UI_GAP, C_DIM);
    int vy = y + UI_GAP + ui_fh(F_SM);
    ui_text(F_LG, value, x + UI_PAD, vy, value_col);
    if (sub) ui_text_r(F_SM, sub, x + w - UI_PAD, y + UI_GAP, C_MUTED);
    int top = vy + ui_fh(F_LG) + G(2), bh = y + h - UI_GAP - top;
    if (hist && bh > G(2)) card_chart(x + UI_PAD, top, w - 2 * UI_PAD - U(4), bh, hist, st.h_n, lo, hi, c, unit, limit);
}

void ui_tag(int cx, int bottom, const char *txt) {
    int w = ui_tw(F_SM, txt) + 2 * UI_GAP, h = ui_fh(F_SM) + G(1), x = cx - w / 2;
    if (x < UI_M + G(1)) x = UI_M + G(1);
    if (x + w > UW - UI_M - G(1)) x = UW - UI_M - G(1) - w;
    ui_round(x, bottom - h, w, h, U(6), C_TXT);
    ui_text(F_SM, txt, x + UI_GAP, bottom - h + G(1) / 2, C_BG);
}

int ui_banner_h(const char *msg, int w) { return ui_wrap_h(F_SM, msg, w - UI_PAD - G(4)) + G(3); }

int ui_banner(int x, int y, int w, Col c, const char *msg) {
    int h = ui_banner_h(msg, w);
    ui_round(x, y, w, h, G(2), C_PANEL2);
    ui_round(x, y, G(1), h, U(2), c);
    ui_wrap(F_SM, msg, x + G(4), y + (h - ui_wrap_h(F_SM, msg, w - UI_PAD - G(4))) / 2, w - UI_PAD - G(4), C_TXT);
    return h;
}

/* In the right margin: lists and tables keep the same width with or without it */
void ui_scrollbar(int x, int y, int h, int total, int visible, int first) {
    (void)x;
    if (total <= visible) return;
    int bar = h * visible / total, bx = UW - U(10);
    if (bar < U(20)) bar = U(20);
    in_scroll = 1;
    ui_round(bx, y, U(4), h, U(2), C_PANEL2);
    ui_round(bx, y + (h - bar) * first / (total - visible), U(4), bar, U(2), C_DIM);
    in_scroll = 0;
}

/* ── Pages ──────────────────────────────────────────────────────────────── */
static const char *const H_PICK[][2] = {{"A", "Select"}, {"B", "Back"}, {NULL, NULL}};
static const char *const H_LOOK[][2] = {{"B", "Back"}, {NULL, NULL}};

static void hints_tr(const char *const h[][2]) {
    const char *t[8][2];
    int i = 0;
    for (; h[i][0] && i < 7; i++) { t[i][0] = h[i][0]; t[i][1] = _(h[i][1]); }
    t[i][0] = t[i][1] = NULL;
    ui_hints(t);
}

#define UI_ROW_MAX G(24)
void ui_list(const Row *rows, int n, int sel, int *first, int top, int bot) {
    int rh = G(13), gap = G(1);
    int visible = (bot - top + gap) / (rh + gap);
    if (visible >= n && n > 0) {       /* everything fits: the rows share the height, up to a size that still looks like a row */
        rh = (bot - top + gap) / n - gap;
        if (rh > UI_ROW_MAX) rh = UI_ROW_MAX;
    }
    if (visible < 1) visible = 1;
    if (sel < *first) *first = sel;
    if (sel >= *first + visible) *first = sel - visible + 1;
    if (*first > n - visible) *first = n - visible > 0 ? n - visible : 0;
    for (int i = *first; i < n && i < *first + visible; i++) {
        int y = top + (i - *first) * (rh + gap), x = UI_M, w = UW - 2 * UI_M;
        int on = i == sel;
        ui_round(x, y, w, rh, UI_RAD, on ? C_ACCENT : C_PANEL);
        Col tc = on ? C_ON_ACCENT : rows[i].disabled ? C_MUTED : C_TXT;
        Col dc = on ? C_ON_ACCENT : C_DIM;
        int vw = rows[i].value[0] ? ui_tw(F_SM, rows[i].value) + UI_PAD : 0;
        char b[160];
        ui_fit(b, sizeof(b), F_MD, rows[i].label, w - 2 * UI_PAD - vw);
        if (rows[i].desc[0]) {
            int ty = y + (rh - ui_fh(F_MD) - ui_fh(F_SM)) / 2;
            ui_text(F_MD, b, x + UI_PAD, ty, tc);
            ui_fit(b, sizeof(b), F_SM, rows[i].desc, w - 2 * UI_PAD - vw);
            ui_text(F_SM, b, x + UI_PAD, ty + ui_fh(F_MD), dc);
        } else {
            ui_text(F_MD, b, x + UI_PAD, y + (rh - ui_fh(F_MD)) / 2, tc);
        }
        if (rows[i].value[0]) {
            Col vc = on ? C_ON_ACCENT : (rows[i].value_col.r || rows[i].value_col.g || rows[i].value_col.b) ? rows[i].value_col : C_DIM;
            ui_text_r(F_SM, rows[i].value, x + w - UI_PAD, y + (rh - ui_fh(F_SM)) / 2, vc);
        }
    }
    ui_scrollbar(0, top, (visible < n ? visible : n) * (rh + gap) - gap, n, visible, *first);
}

int page_list(const char *title, const char *sub, Row *rows, int n, int sel) {
    int first = 0;
    for (;;) {
        ui_clear();
        ui_header(title, sub);
        ui_list(rows, n, sel, &first, ui_content_top(), ui_content_bottom() - UI_GAP);
        hints_tr(H_PICK);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return -1;
        if (k.up) sel = (sel + n - 1) % n;
        if (k.down) sel = (sel + 1) % n;
        if (k.a) return sel;
        if (k.b || k.sel) return -1;
    }
}

int page_list_info(const char *title, const char *sub, Row *rows, int n, int sel,
                   const char *info_title, const char *const *info, int n_info) {
    int first = 0;
    for (;;) {
        ui_clear();
        ui_header(title, sub);
        int x = UI_M, w = UW - 2 * x, top = ui_content_top(), bot = ui_content_bottom() - UI_GAP;
        int tw = w - 2 * UI_PAD, need = 2 * UI_PAD + ui_fh(F_MD);
        for (int i = 0; i < n_info; i++) need += G(1) + ui_wrap_h(F_SM, info[i], tw);
        /* the rows are never smaller than a normal row; the panel takes what they leave */
        int rows_h = n * (G(13) + G(1)) - G(1);
        int ph = bot - top - rows_h - UI_GAP;
        if (ph > need + G(16)) {
            int extra = (ph - need) / 2, rows_max = n * (UI_ROW_MAX + G(1)) - G(1);
            if (rows_h + extra > rows_max) extra = rows_max - rows_h;
            rows_h += extra;
            ph -= extra;
        }
        ui_list(rows, n, sel, &first, top, top + rows_h);
        if (ph >= need) {
            int py = bot - ph, ty = py + UI_PAD;
            ui_round(x, py, w, ph, UI_RAD, C_PANEL);
            ui_text(F_MD, info_title, x + UI_PAD, ty, C_TXT);
            ty += ui_fh(F_MD);
            for (int i = 0; i < n_info; i++) ty = ui_wrap(F_SM, info[i], x + UI_PAD, ty + G(1), tw, C_DIM);
        }
        hints_tr(H_PICK);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return -1;
        if (k.up) sel = (sel + n - 1) % n;
        if (k.down) sel = (sel + 1) % n;
        if (k.a) return sel;
        if (k.b || k.sel) return -1;
    }
}

#define BTN_H 11
int ui_buttons_top(void) { return ui_content_bottom() - UI_GAP - G(BTN_H); }

void ui_buttons(const char *yes, const char *no, int sel, int danger) {
    int y = ui_buttons_top(), h = G(BTN_H);
    int x = no ? ui_col(0) : UI_M, w = no ? ui_span(0, 6) : UW - 2 * UI_M;
    ui_round(x, y, w, h, UI_RAD, sel == 0 ? (danger ? C_BAD : C_ACCENT) : C_PANEL);
    ui_text_c(F_MD, yes, x + w / 2, y + (h - ui_fh(F_MD)) / 2, sel == 0 ? C_ON_ACCENT : C_DIM);
    if (no) {
        x = ui_col(6);
        w = ui_span(6, 6);
        ui_round(x, y, w, h, UI_RAD, sel == 1 ? C_ACCENT : C_PANEL);
        ui_text_c(F_MD, no, x + w / 2, y + (h - ui_fh(F_MD)) / 2, sel == 1 ? C_ON_ACCENT : C_DIM);
    }
}

/* Starts on the safe answer: a stray press of A must not patch anything */
int page_confirm(const char *title, const char *msg, const char *const *warn, int n_warn,
                 const char *yes, const char *no, int danger) {
    int sel = 1;
    for (;;) {
        ui_clear();
        ui_header(title, NULL);
        int w = UW - 2 * UI_M, top = ui_content_top(), bot = ui_buttons_top() - UI_GAP, block = 0;
        if (msg) block += ui_wrap_h(F_MD, msg, w) + (n_warn ? G(2) : 0);
        for (int i = 0; i < n_warn; i++) block += ui_wrap_h(F_SM, warn[i], w) + (i + 1 < n_warn ? G(1) : 0);
        int y = top + (bot - top - block) / 2;
        if (y < top) y = top;
        if (msg) y = ui_wrap_c(F_MD, msg, y, w, C_TXT) + (n_warn ? G(2) : 0);
        for (int i = 0; i < n_warn; i++) y = ui_wrap_c(F_SM, warn[i], y, w, C_WARN) + (i + 1 < n_warn ? G(1) : 0);
        ui_check_fits("confirm", y, ui_buttons_top());
        ui_buttons(yes, no, sel, danger);
        hints_tr(H_PICK);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return 0;
        if (k.left || k.right || k.up || k.down) sel ^= 1;
        if (k.a) return sel == 0;
        if (k.b || k.sel) return 0;
    }
}

static void centred_message(const char *msg) {
    int w = UW - 2 * UI_M, top = ui_content_top(), bot = ui_content_bottom();
    ui_wrap_c(F_MD, msg, top + (bot - top - ui_wrap_h(F_MD, msg, w)) / 2, w, C_TXT);
}

void page_busy(const char *title, const char *msg) {
    ui_clear();
    ui_header(title, NULL);
    centred_message(msg);
    ui_present();
}

void page_info(const char *title, const char *msg, int ms) {
    Uint32 end = SDL_GetTicks() + (Uint32)ms;
    for (;;) {
        ui_clear();
        ui_header(title, NULL);
        centred_message(msg);
        if (!ms) hints_tr(H_LOOK);
        ui_present();
        Keys k = ui_poll(ms ? 100 : 1000);
        if (!ui_running) return;
        if (ms && (Sint32)(SDL_GetTicks() - end) >= 0) return;
        if (k.a || k.b || k.sel) return;
    }
}

void page_steps(const char *title, const char *const *steps, int n) {
    int first = 0;
    for (;;) {
        ui_clear();
        ui_header(title, NULL);
        int x = UI_M, w = UW - 2 * x, top = ui_content_top(), bot = ui_content_bottom() - UI_GAP;
        int num = G(7), tw = w - 2 * UI_PAD - num - UI_PAD, gap = G(1), pad = UI_PAD;
        /* the cards share the free height, up to a limit */
        int total = 0;
        for (int i = 0; i < n; i++) total += ui_wrap_h(F_MD, steps[i], tw) + 2 * UI_PAD;
        total += (n - 1) * gap;
        if (total < bot - top) {
            pad += (bot - top - total) / (2 * n);
            if (pad > G(11)) pad = G(11);
        }
        int y = top, last = first;
        for (int i = first; i < n; i++) {
            int th = ui_wrap_h(F_MD, steps[i], tw), h = th + 2 * pad;
            if (h < num + 2 * UI_GAP) h = num + 2 * UI_GAP;
            if (y + h > bot) break;
            ui_round(x, y, w, h, UI_RAD, C_PANEL);
            char b[8];
            snprintf(b, sizeof(b), "%d", i + 1);
            ui_round(x + UI_PAD, y + (h - num) / 2, num, num, num / 2, C_ACCENT);
            ui_text_c(F_MD, b, x + UI_PAD + num / 2, y + (h - ui_fh(F_MD)) / 2, C_ON_ACCENT);
            ui_wrap(F_MD, steps[i], x + 2 * UI_PAD + num, y + (h - th) / 2, tw, C_TXT);
            y += h + gap;
            last = i;
        }
        hints_tr(H_LOOK);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return;
        if (k.up && first > 0) first--;
        if (k.down && last < n - 1) first++;
        if (k.a || k.b || k.sel) return;
    }
}

static void (*table_extra)(int, int, int, int, void *);
static void *table_extra_ctx;
void page_table_extra(void (*fn)(int, int, int, int, void *), void *ctx) { table_extra = fn; table_extra_ctx = ctx; }

int page_table(const char *title, const char *msg, const char *const hdr[3], const TableRow *rows, int n,
               const char *const *warn, int n_warn, const char *yes, const char *no) {
    int first = 0, sel = 1;
    for (;;) {
        ui_clear();
        ui_header(title, NULL);
        int x = UI_M, w = UW - 2 * x, y = ui_content_top();
        if (msg) y = ui_wrap(F_MD, msg, x, y, w, C_TXT) + UI_GAP;
        int warn_h = 0;
        for (int i = 0; i < n_warn; i++) warn_h += ui_wrap_h(F_SM, warn[i], w) + G(1);
        if (n_warn) warn_h += UI_GAP - G(1);
        int bot = (yes ? ui_buttons_top() : ui_content_bottom()) - UI_GAP - warn_h - (n_warn ? UI_GAP : 0);
        int rh = G(7), head = G(7), foot = G(1);
        int visible = (bot - y - head - foot) / rh;
        if (visible < 1) visible = 1;
        if (visible > n) visible = n;
        if (first > n - visible) first = n - visible;
        int th = head + visible * rh + foot;
        ui_round(x, y, w, th, UI_RAD, C_PANEL);
        int cx[3] = {x + UI_PAD, x + w / 2, x + w - UI_PAD};
        int ty = (rh - ui_fh(F_SM)) / 2;
        ui_text(F_SM, hdr[0], cx[0], y + ty, C_MUTED);
        ui_text_c(F_SM, hdr[1], cx[1], y + ty, C_MUTED);
        ui_text_r(F_SM, hdr[2], cx[2], y + ty, C_MUTED);
        for (int i = first; i < first + visible; i++) {
            int ry = y + head + (i - first) * rh + ty;
            Col c = (rows[i].col.r || rows[i].col.g || rows[i].col.b) ? rows[i].col : C_TXT;
            ui_text(F_SM, rows[i].c[0], cx[0], ry, C_DIM);
            ui_text_c(F_SM, rows[i].c[1], cx[1], ry, C_DIM);
            ui_text_r(F_SM, rows[i].c[2], cx[2], ry, c);
        }
        ui_scrollbar(0, y + head, visible * rh, n, visible, first);
        y += th;
        /* the warnings stay right above the buttons; what is left goes to the extra drawing */
        int limit = (yes ? ui_buttons_top() : ui_content_bottom()) - UI_GAP, wy = limit - warn_h;
        if (wy < y + UI_GAP) wy = y + UI_GAP;
        if (table_extra && wy - UI_GAP - (y + UI_GAP) >= G(20)) table_extra(x, y + UI_GAP, w, wy - UI_GAP - (y + UI_GAP), table_extra_ctx);
        for (int i = 0; i < n_warn; i++) wy = ui_wrap(F_SM, warn[i], x, wy, w, C_WARN) + (i + 1 < n_warn ? G(1) : 0);
        ui_check_fits("table", wy, limit + UI_GAP);
        if (yes) ui_buttons(yes, no, sel, 0);
        if (yes) hints_tr(H_PICK); else hints_tr(H_LOOK);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return 0;
        if (k.up && first > 0) first--;
        if (k.down && first < n - visible) first++;
        if (yes) {
            if (k.left || k.right) sel ^= 1;
            if (k.a) return sel == 0;
        } else if (k.a) return 0;
        if (k.b || k.sel) return 0;
    }
}
