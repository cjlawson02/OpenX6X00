/* Landscape strip layout vs the portrait path (BOARD_FB_ROTATE_90=1), host/ARM, no DRM device.
 *
 * Code under test is extracted from lv_drivers/display/drm.c at build time (see
 * drm_landscape_test.sh): drm_wf_present / drm_wf_present_ring / copy_cols, built with
 * BOARD_FB_ROTATE_90=0. The producers' index math (WFP_AT, the pan memmove) is mirrored here.
 *
 * Reference: the screen is L[y][x] = G(row id shown at age y, centre + x). A portrait producer
 * (wfp[(W-1-x)*h + (head+age)%h]) presented the portrait way, mapped through
 * drm_compose_landscape (landscape (x, y) = portrait row W-1-x, column y), must equal the landscape
 * producer presented by the extracted code, for full redraws, ring turns and pans in both directions.
 *
 *   drm_landscape_test           check
 *   drm_landscape_test bench     also time present of a 1024 x 300 strip
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LAND_W 1024
#define LAND_H 600
#define PANEL_W LAND_W
#define PANEL_H LAND_H
#define WIDTH LAND_W
#define MAX_RECTS 1

struct drm_buffer { uint32_t pitch; uint8_t *map; };
typedef struct { struct drm_buffer bufs[2]; int back; bool ready; } plane_t;
static plane_t primary;
static bool planes = true, portrait_mode;
static uint32_t *wf_shadow;
static void wait_flip(void) {}
void drm_wf_present_ring(const uint32_t *src, int32_t x, int32_t w, int32_t head);

#include "drm_extract.inc"      /* drm_wf_present, MAX_STRIPS, strips, copy_cols, drm_wf_present_ring */

/* ---- reference model ---- */
static uint32_t G(int rid, int xabs) { uint32_t v = (uint32_t) rid * 2654435761u ^ (uint32_t) xabs * 40503u; return v | 0xff000000u; }

typedef struct {
    int h, head, center;
    int rid[LAND_H];            /* row id at each age */
    uint32_t *buf;
    bool portrait;
} view_t;

#define AT_P(b, x, y, h) (&(b)[(WIDTH - 1 - (x)) * (h) + (y)])
#define AT_L(b, x, y, h) (&(b)[(y) * WIDTH + (x)])
static uint32_t *at(view_t *v, int x, int y) { return v->portrait ? AT_P(v->buf, x, y, v->h) : AT_L(v->buf, x, y, v->h); }

static void column(view_t *v, int age, int i0, int i1) {
    for (int i = i0; i < i1; i++) *at(v, i, (v->head + age) % v->h) = G(v->rid[age], v->center + i);
}
static void full(view_t *v) { for (int a = 0; a < v->h; a++) column(v, a, 0, WIDTH); }
static int next_rid = 1000;
static void new_rows(view_t *v, int n, int base) {
    memmove(v->rid + n, v->rid, (v->h - n) * sizeof(int));
    for (int a = 0; a < n; a++) v->rid[a] = base + n - 1 - a;
    v->head = (v->head - n + v->h) % v->h;
    for (int a = 0; a < n; a++) column(v, a, 0, WIDTH);
}
static void pan(view_t *v, int px) {
    int i0, i1;
    v->center += px;
    if (v->portrait) {
        if (px > 0) { memmove(v->buf + px * v->h, v->buf, (WIDTH - px) * v->h * 4); i0 = WIDTH - px; i1 = WIDTH; }
        else        { memmove(v->buf, v->buf - px * v->h, (WIDTH + px) * v->h * 4); i0 = 0; i1 = -px; }
    } else {
        for (int r = 0; r < v->h; r++) {
            uint32_t *line = v->buf + r * WIDTH;
            if (px > 0) memmove(line, line + px, (WIDTH - px) * 4);
            else        memmove(line - px, line, (WIDTH + px) * 4);
        }
        if (px > 0) { i0 = WIDTH - px; i1 = WIDTH; } else { i0 = 0; i1 = -px; }
    }
    for (int a = 0; a < v->h; a++) column(v, a, i0, i1);
}

/* the portrait present (drm.c, BOARD_FB_ROTATE_90=1): panel LAND_H wide, LAND_W rows */
static void present_portrait(uint32_t *ps, const uint32_t *src, int x, int w, int head) {
    for (int y = 0; y < LAND_W; y++) {
        const uint32_t *s = src + y * w;
        uint32_t *d = ps + y * LAND_H + x;
        memcpy(d, s + head, (w - head) * 4);
        memcpy(d + w - head, s, head * 4);
    }
}

static int check(view_t *p, view_t *l, int y0, uint32_t *ps, uint32_t *scr, const char *what) {
    present_portrait(ps, p->buf, y0, p->h, p->head);
    drm_wf_present_ring(l->buf, y0, l->h, l->head);
    for (int y = 0; y < l->h; y++)
        for (int x = 0; x < LAND_W; x++) {
            uint32_t want = G(l->rid[y], l->center + x);
            uint32_t viaP = ps[(LAND_W - 1 - x) * LAND_H + y0 + y];     /* drm_compose_landscape mapping */
            uint32_t got = wf_shadow[(y0 + y) * PANEL_W + x];
            if (viaP != want || got != want) {
                printf("FAIL %s at (%d,%d): want %08x portrait %08x landscape %08x\n", what, x, y, want, viaP, got);
                return 1;
            }
        }
    /* what the GUI sees in the plane buffer (copy_cols of the last present) */
    for (int y = 0; y < l->h; y++)
        if (memcmp(primary.bufs[primary.back].map + (size_t) (y0 + y) * primary.bufs[primary.back].pitch,
                   wf_shadow + (y0 + y) * PANEL_W, LAND_W * 4)) {
            printf("FAIL %s: plane buffer row %d\n", what, y0 + y);
            return 1;
        }
    (void) scr;
    return 0;
}

/* drm_commit for the primary plane: the stale strips into the back buffer, then the buffers swap */
static void drm_commit_stale(void) {
    for (int i = 0; i < nstrips; i++)
        if (strips[i].stale[primary.back]) { copy_cols(&primary.bufs[primary.back], strips[i].x, strips[i].w); strips[i].stale[primary.back] = false; }
    primary.ready = false;
    primary.back ^= 1;
}

/* ---- strip planes (splane_t): what a plane shows after the commit ---- */
static void sp_commit(void) {
    for (int i = 0; i < nsplanes; i++)
        if (splanes[i].pl.ready) { splanes[i].pl.ready = false; splanes[i].shown = splanes[i].pl.back; splanes[i].setup = false; }
}

static splane_t *sp_at(int x, int w) {
    for (int i = 0; i < nsplanes; i++) if (splanes[i].w == w && splanes[i].x == x) return &splanes[i];
    return NULL;
}

/* rows win .. win + w - 1 of the buffer on screen; want(k) for screen row k */
static int sp_check(int x, int w, const view_t *l, const uint32_t *flat, const char *what) {
    splane_t *s = sp_at(x, w);
    if (!s || !s->on || !s->valid) { printf("FAIL %s: no plane on for strip %d+%d\n", what, x, w); return 1; }
    const struct drm_buffer *b = &s->pl.bufs[s->shown];
    for (int k = 0; k < w; k++) {
        const uint32_t *row = (const uint32_t *) (b->map + (size_t) (s->win + k) * b->pitch);
        for (int i = 0; i < LAND_W; i++) {
            uint32_t want = l ? G(l->rid[k], l->center + i) : flat[k * LAND_W + i];
            if (row[i] != want) { printf("FAIL %s: strip %d+%d row %d px %d win %d\n", what, x, w, k, i, s->win); return 1; }
        }
    }
    return 0;
}

static int sp_tests(void) {
    int fails = 0;
    nsplanes = 2;
    for (int i = 0; i < nsplanes; i++)
        for (int j = 0; j < 2; j++) {
            splanes[i].pl.bufs[j].pitch = PANEL_W * 4 + 64;
            splanes[i].pl.bufs[j].map = calloc(2 * PANEL_H, splanes[i].pl.bufs[j].pitch);
        }
    int hs[] = {1, 2, 7, 300, 450}, y0s[] = {0, 33, 100, 150, 0};
    for (unsigned k = 0; k < sizeof hs / sizeof hs[0] && !fails; k++) {
        int h = hs[k], y0 = y0s[k], sy = y0 + h, sh = 50;     /* the spectrum under it */
        view_t l = { .h = h, .portrait = false, .buf = calloc(LAND_W * h, 4) };
        uint32_t *spec = calloc(LAND_W * sh, 4);
        srand(100 + k);
        for (int a = 0; a < h; a++) l.rid[a] = a;
        l.center = 5000;
        full(&l);
        drm_wf_present_ring(l.buf, y0, h, l.head);
        sp_commit();
        fails += sp_check(y0, h, &l, NULL, "sp full");
        for (int step = 0; step < 400 && !fails; step++) {
            int op = rand() % 6, n = 1 + rand() % (h < 4 ? 1 : 3), px = 1 + rand() % 200;
            if (op <= 2) { if (n >= h) n = 1; new_rows(&l, n, next_rid); next_rid += n; drm_wf_scroll(l.buf, y0, h, l.head, n); }
            else if (op == 3) { pan(&l, rand() % 2 ? px : -px); drm_wf_present_ring(l.buf, y0, h, l.head); }
            else { for (int i = 0; i < LAND_W * sh; i++) spec[i] = rand() | 0xff000000u; drm_wf_present(spec, sy, sh); }
            sp_commit();
            fails += sp_check(y0, h, &l, NULL, "sp step");
            if (op > 3 && !fails) fails += sp_check(sy, sh, NULL, spec, "sp spectrum");
        }
        /* the waterfall moves down and grows by one row: its plane follows, the spectrum's is let go */
        if (!fails && y0 + h + 1 <= LAND_H - 1) {
            view_t m = { .h = h + 1, .portrait = false, .buf = calloc(LAND_W * (h + 1), 4) };
            for (int a = 0; a < m.h; a++) m.rid[a] = a + 7;
            m.center = 4000;
            full(&m);
            drm_wf_scroll(m.buf, y0 + 1, m.h, m.head, 1);
            sp_commit();
            fails += sp_check(y0 + 1, m.h, &m, NULL, "sp moved");
            for (int step = 0; step < 50 && !fails; step++) {
                new_rows(&m, 1, next_rid++);
                drm_wf_scroll(m.buf, y0 + 1, m.h, m.head, 1);
                sp_commit();
                fails += sp_check(y0 + 1, m.h, &m, NULL, "sp moved step");
            }
            free(m.buf);
        }
        printf("strip planes h=%d y0=%d: %s\n", h, y0, fails ? "FAIL" : "ok (full, turns, pans, spectrum, move)");
        free(l.buf); free(spec);
        for (int i = 0; i < nsplanes; i++) { splanes[i].w = 0; splanes[i].on = splanes[i].valid = false; }
    }
    return fails;
}

static double ms(struct timespec a, struct timespec b) { return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }

int main(int argc, char **argv) {
    wf_shadow = calloc(PANEL_W * PANEL_H, 4);
    for (int i = 0; i < 2; i++) {
        primary.bufs[i].pitch = PANEL_W * 4 + 64;      /* padded pitch, as a dumb buffer may have */
        primary.bufs[i].map = calloc(PANEL_H, primary.bufs[i].pitch);
    }
    uint32_t *ps = calloc(LAND_W * LAND_H, 4);
    int fails = 0, hs[] = {1, 7, 300, 450}, y0s[] = {0, 33, 150};

    for (unsigned k = 0; k < sizeof hs / sizeof hs[0]; k++) {
        int h = hs[k], y0 = y0s[k % 3];
        view_t p = { .h = h, .portrait = true, .buf = calloc(LAND_W * h, 4) };
        view_t l = { .h = h, .portrait = false, .buf = calloc(LAND_W * h, 4) };
        srand(k + 1);
        for (int a = 0; a < h; a++) p.rid[a] = l.rid[a] = a;
        p.center = l.center = 5000;
        full(&p); full(&l);
        fails += check(&p, &l, y0, ps, NULL, "full");
        for (int step = 0; step < 60 && !fails; step++) {
            int op = rand() % 4, n = 1 + rand() % (h < 4 ? 1 : 3), px = 1 + rand() % 200;
            if (op == 0 || op == 3) { if (n >= h) n = 1; new_rows(&p, n, next_rid); new_rows(&l, n, next_rid); next_rid += n; }
            else if (op == 1) { pan(&p, px); pan(&l, px); }
            else { pan(&p, -px); pan(&l, -px); }
                    fails += check(&p, &l, y0, ps, NULL, "step");
        }
        printf("h=%d y0=%d: %s\n", h, y0, fails ? "FAIL" : "ok (full, ring turns, pans both ways)");
        free(p.buf); free(l.buf);
    }
    if (!fails) fails += sp_tests();
    printf("%s\n", fails ? "RESULT FAIL" : "RESULT OK");

    if (!fails && argc > 1 && !strcmp(argv[1], "bench")) {
        int h = 300, n = 300;
        uint32_t *src = malloc((size_t) LAND_W * h * 4);
        struct timespec a, b;
        for (int i = 0; i < LAND_W * h; i++) src[i] = rand();
        nsplanes = 0;           /* the primary-plane path */
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < n; k++) { drm_wf_present_ring(src, 0, h, k % h); drm_commit_stale(); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double land = ms(a, b) / n;
        nsplanes = 2;           /* strip planes: a whole picture, a one-row turn */
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < n; k++) { drm_wf_present_ring(src, 0, h, k % h); sp_commit(); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double spf = ms(a, b) / n;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < n; k++) { drm_wf_scroll(src, 0, h, (h - 1 - k % h), 1); sp_commit(); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("1024x300 per present: primary plane (shadow + buffer + stale buffer) %.2f ms; strip plane whole picture %.2f ms, one-row turn %.3f ms\n",
               land, spf, ms(a, b) / n);
    }
    return fails;
}
