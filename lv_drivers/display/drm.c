/**
 * @file drm.c
 *
 * DRM/KMS output on two planes:
 * - the overlay plane (ARGB) shows the GUI: LVGL renders landscape 800x480
 *   areas, they are rotated 90 ccw into a cached portrait shadow, and on the
 *   last area of a frame the dirty rectangles are copied to the back buffer;
 * - the primary plane (XRGB) under it shows what the GUI leaves transparent:
 *   the waterfall, drawn there directly (drm_wf_present), not by LVGL.
 * Both are double buffered and shown together with one atomic commit at
 * vblank (drm_commit, from the main loop): no tearing.
 *
 * A back buffer misses the previous frame too (it went to the other buffer),
 * so the rectangles of the previous frame are copied again: only dirty areas
 * are moved, never the whole screen.
 *
 * Without a usable overlay plane the GUI goes on the primary plane alone and
 * drm_planes() is false: the waterfall stays an LVGL image.
 *
 * A landscape panel (BOARD_FB_ROTATE_90=0) has no rotation: GUI areas are copied
 * as they are, and the strips, which the views keep as landscape rows (see
 * drm_wf_present in drm.h), are copied as rows. There each strip gets an overlay plane
 * of its own between the primary and the GUI when the CRTC has them (splane_t): a
 * whole picture is one copy, a waterfall row two rows and a SRC_Y move.
 */

/*********************
 *      INCLUDES
 *********************/
#include "drm.h"
#if USE_DRM

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef BOARD_DISP_W
#include "src/board.h"
#endif

/*********************
 *      DEFINES
 *********************/
#define LAND_W      BOARD_DISP_W    /* LVGL, landscape */
#define LAND_H      BOARD_DISP_H
#if BOARD_FB_ROTATE_90
#define PANEL_W     LAND_H          /* panel, portrait */
#define PANEL_H     LAND_W
#else
#define PANEL_W     LAND_W
#define PANEL_H     LAND_H
#endif
#define MAX_RECTS   128             /* per frame, more: whole screen (the screensaver's rain: two a column) */
#define FLIP_TIMEOUT_MS 100

#define err(msg, ...) fprintf(stderr, "drm: " msg "\n", ##__VA_ARGS__)

/**********************
 *      TYPEDEFS
 **********************/
struct drm_buffer {
    uint32_t    handle;
    uint32_t    pitch;
    uint32_t    size;
    uint32_t    fb_id;
    uint8_t     *map;
};

typedef struct {
    lv_area_t   rects[MAX_RECTS];   /* portrait coordinates */
    uint32_t    cnt;
    bool        full;
} rects_t;

/* A plane with two buffers: back is the one not on screen */
typedef struct {
    uint32_t            id;
    struct drm_buffer   bufs[2];
    int                 back;
    bool                ready;      /* back buffer filled, to be shown */
    /* property ids */
    uint32_t            p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;
} plane_t;

/**********************
 *  STATIC VARIABLES
 **********************/
static int                  fd = -1;
static uint32_t             conn_id, crtc_id;
static int                  crtc_idx;
static drmModeModeInfo      mode;
static plane_t              primary, overlay;
static plane_t              *gui;           /* the plane LVGL draws to */
static bool                 planes;         /* the GUI on the overlay, the waterfall on the primary */
static bool                 crtc_set;
static volatile bool        flip_pending;
static uint32_t             *shadow;        /* portrait copy of the GUI, cached RAM */
static uint32_t             *wf_shadow;     /* portrait copy of the primary plane (screen dump) */
static rects_t              frame[2];       /* this frame, previous frame */
static int                  cur;
static bool                 portrait_mode;  /* LVGL renders 480x800: turned 180, not 90 */

/**********************
 *   STATIC FUNCTIONS
 **********************/

static bool find_output(void) {
    drmModeRes *res = drmModeGetResources(fd);

    if (!res) {
        return false;
    }

    bool ok = false;

    for (int i = 0; i < res->count_connectors && !ok; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            int m = 0;

            for (int j = 0; j < conn->count_modes; j++) {
                if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
                    m = j;
                    break;
                }
            }
            mode = conn->modes[m];

            /* CRTC: the one already driving the connector, else the first possible one */
            crtc_id = 0;
            for (int j = 0; j < conn->count_encoders && !crtc_id; j++) {
                drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[j]);

                if (!enc) {
                    continue;
                }
                if (enc->encoder_id == conn->encoder_id && enc->crtc_id) {
                    crtc_id = enc->crtc_id;
                }
                for (int k = 0; k < res->count_crtcs && !crtc_id; k++) {
                    if (enc->possible_crtcs & (1 << k)) {
                        crtc_id = res->crtcs[k];
                    }
                }
                drmModeFreeEncoder(enc);
            }
            for (int k = 0; k < res->count_crtcs; k++) {
                if (res->crtcs[k] == crtc_id) {
                    crtc_idx = k;
                }
            }
            conn_id = conn->connector_id;
            ok = crtc_id != 0;
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return ok;
}

static uint32_t prop_id(drmModeObjectProperties *props, const char *name) {
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        uint32_t           id = 0;

        if (p && strcmp(p->name, name) == 0) {
            id = p->prop_id;
        }
        drmModeFreeProperty(p);
        if (id) {
            return id;
        }
    }
    return 0;
}

static bool plane_taken(uint32_t id, uint32_t skip);
#if !BOARD_FB_ROTATE_90
static void splanes_init(void);
static void splanes_off(void);
#endif

/* The first plane of this type on our CRTC that takes the format */
static bool find_plane(plane_t *pl, uint64_t type, uint32_t fourcc, uint32_t skip) {
    drmModePlaneRes *res = drmModeGetPlaneResources(fd);
    bool            ok = false;

    if (!res) {
        return false;
    }
    for (uint32_t i = 0; i < res->count_planes && !ok; i++) {
        drmModePlane            *p = drmModeGetPlane(fd, res->planes[i]);
        drmModeObjectProperties *props;
        bool                    fmt = false;

        if (!p) {
            continue;
        }
        for (uint32_t f = 0; f < p->count_formats; f++) {
            fmt |= p->formats[f] == fourcc;
        }
        props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props && fmt && !plane_taken(p->plane_id, skip) && (p->possible_crtcs & (1 << crtc_idx))) {
            uint32_t t = prop_id(props, "type");

            for (uint32_t j = 0; j < props->count_props; j++) {
                if (props->props[j] == t && props->prop_values[j] == type) {
                    pl->id = p->plane_id;
                    pl->p_fb = prop_id(props, "FB_ID");
                    pl->p_crtc = prop_id(props, "CRTC_ID");
                    pl->p_sx = prop_id(props, "SRC_X");
                    pl->p_sy = prop_id(props, "SRC_Y");
                    pl->p_sw = prop_id(props, "SRC_W");
                    pl->p_sh = prop_id(props, "SRC_H");
                    pl->p_cx = prop_id(props, "CRTC_X");
                    pl->p_cy = prop_id(props, "CRTC_Y");
                    pl->p_cw = prop_id(props, "CRTC_W");
                    pl->p_ch = prop_id(props, "CRTC_H");
                    ok = pl->p_fb && pl->p_crtc && pl->p_ch;
                }
            }
        }
        drmModeFreeObjectProperties(props);
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(res);
    return ok;
}

static bool alloc_buf_h(struct drm_buffer *buf, uint32_t fourcc, uint32_t h) {
    struct drm_mode_create_dumb creq = { .width = PANEL_W, .height = h, .bpp = 32 };
    struct drm_mode_map_dumb    mreq = { 0 };
    uint32_t                    handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        err("create dumb: %s", strerror(errno));
        return false;
    }
    buf->handle = creq.handle;
    buf->pitch = creq.pitch;
    buf->size = creq.size;

    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        err("map dumb: %s", strerror(errno));
        return false;
    }
    buf->map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        buf->map = NULL;
        err("mmap: %s", strerror(errno));
        return false;
    }
    memset(buf->map, 0, creq.size);

    /* The byte order of lv_color32_t (B, G, R, A) */
    handles[0] = buf->handle;
    pitches[0] = buf->pitch;
    if (drmModeAddFB2(fd, PANEL_W, h, fourcc, handles, pitches, offsets, &buf->fb_id, 0)) {
        err("add fb: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool alloc_buf(struct drm_buffer *buf, uint32_t fourcc) {
    return alloc_buf_h(buf, fourcc, PANEL_H);
}

static bool alloc_plane(plane_t *pl, uint32_t fourcc) {
    return alloc_buf(&pl->bufs[0], fourcc) && alloc_buf(&pl->bufs[1], fourcc);
}

static void free_plane(plane_t *pl) {
    for (int i = 0; i < 2; i++) {
        if (pl->bufs[i].map) {
            munmap(pl->bufs[i].map, pl->bufs[i].size);
            pl->bufs[i].map = NULL;
        }
    }
}

static bool open_card(const char *path) {
    uint64_t has_dumb = 0;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    /* drmSetMaster: fails if another process owns the display */
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) == 0 && has_dumb &&
        drmSetMaster(fd) == 0 && find_output())
    {
        if (mode.hdisplay == PANEL_W && mode.vdisplay == PANEL_H) {
            return true;
        }
        err("%s: mode %ux%u, %ux%u expected", path, mode.hdisplay, mode.vdisplay, PANEL_W, PANEL_H);
    }
    close(fd);
    fd = -1;
    return false;
}

static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *data) {
    flip_pending = false;
}

static void wait_flip(void) {
    drmEventContext ctx = { .version = 2, .page_flip_handler = page_flip_handler };
    struct pollfd   pfd = { .fd = fd, .events = POLLIN };

    while (flip_pending) {
        if (poll(&pfd, 1, FLIP_TIMEOUT_MS) <= 0) {
            err("page flip timeout");
            flip_pending = false;       /* never hang the GUI on a lost event */
            break;
        }
        drmHandleEvent(fd, &ctx);
    }
}

#if !BOARD_FB_ROTATE_90
/* Landscape area -> shadow at the same coordinates (no rotation) */
static void copy_area(const lv_area_t *a, const uint32_t *src) {
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);

    for (int32_t y = 0; y < h; y++) {
        memcpy(shadow + (a->y1 + y) * PANEL_W + a->x1, src + y * w, (size_t) w * 4);
    }
}
#endif

#if BOARD_FB_ROTATE_90
/* Landscape area (w x h pixels, packed) -> portrait shadow, 90 ccw:
 * landscape (x, y) goes to portrait column y, row LAND_W - 1 - x */
static void rotate_area(const lv_area_t *a, const uint32_t *src) {
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);
    int32_t x = 0, y = 0;

#ifdef __ARM_NEON
    /* 4x4 blocks: transpose in registers, store 4 portrait rows of 4 pixels */
    for (y = 0; y + 4 <= h; y += 4) {
        for (x = 0; x + 4 <= w; x += 4) {
            const uint32_t *s = src + y * w + x;
            uint32_t *d = shadow + (LAND_W - 1 - (a->x1 + x)) * PANEL_W + a->y1 + y;

            uint32x4x2_t t0 = vtrnq_u32(vld1q_u32(s), vld1q_u32(s + w));
            uint32x4x2_t t1 = vtrnq_u32(vld1q_u32(s + 2 * w), vld1q_u32(s + 3 * w));

            vst1q_u32(d, vcombine_u32(vget_low_u32(t0.val[0]), vget_low_u32(t1.val[0])));
            vst1q_u32(d - PANEL_W, vcombine_u32(vget_low_u32(t0.val[1]), vget_low_u32(t1.val[1])));
            vst1q_u32(d - 2 * PANEL_W, vcombine_u32(vget_high_u32(t0.val[0]), vget_high_u32(t1.val[0])));
            vst1q_u32(d - 3 * PANEL_W, vcombine_u32(vget_high_u32(t0.val[1]), vget_high_u32(t1.val[1])));
        }
        /* right edge of this band */
        for (int32_t yy = y; yy < y + 4; yy++) {
            for (int32_t xx = x; xx < w; xx++) {
                shadow[(LAND_W - 1 - (a->x1 + xx)) * PANEL_W + a->y1 + yy] = src[yy * w + xx];
            }
        }
    }
#endif

    /* bottom rows (all rows without NEON) */
    for (; y < h; y++) {
        for (x = 0; x < w; x++) {
            shadow[(LAND_W - 1 - (a->x1 + x)) * PANEL_W + a->y1 + y] = src[y * w + x];
        }
    }
}
#endif /* BOARD_FB_ROTATE_90 */

static void rects_add(rects_t *r, const lv_area_t *portrait) {
    if (r->cnt < MAX_RECTS) {
        r->rects[r->cnt++] = *portrait;
    } else {
        r->full = true;
    }
}

static void copy_rect(struct drm_buffer *buf, const lv_area_t *p) {
    size_t n = lv_area_get_width(p) * 4;

    for (int32_t y = p->y1; y <= p->y2; y++) {
        memcpy(buf->map + y * buf->pitch + p->x1 * 4, shadow + y * PANEL_W + p->x1, n);
    }
}

static void copy_rects(struct drm_buffer *buf, const rects_t *r) {
    static const lv_area_t all = { 0, 0, PANEL_W - 1, PANEL_H - 1 };

    if (r->full) {
        copy_rect(buf, &all);
    } else {
        for (uint32_t i = 0; i < r->cnt; i++) {
            copy_rect(buf, &r->rects[i]);
        }
    }
}

static void add_prop(drmModeAtomicReq *req, uint32_t obj, uint32_t prop, uint64_t val) {
    if (prop) {
        drmModeAtomicAddProperty(req, obj, prop, val);
    }
}

/* Full-screen plane on our CRTC (first commit only) */
static void add_plane_setup(drmModeAtomicReq *req, plane_t *pl) {
    add_prop(req, pl->id, pl->p_crtc, crtc_id);
    add_prop(req, pl->id, pl->p_sx, 0);
    add_prop(req, pl->id, pl->p_sy, 0);
    add_prop(req, pl->id, pl->p_sw, (uint64_t) PANEL_W << 16);
    add_prop(req, pl->id, pl->p_sh, (uint64_t) PANEL_H << 16);
    add_prop(req, pl->id, pl->p_cx, 0);
    add_prop(req, pl->id, pl->p_cy, 0);
    add_prop(req, pl->id, pl->p_cw, PANEL_W);
    add_prop(req, pl->id, pl->p_ch, PANEL_H);
}

/* The first frame: modeset with the primary plane (synchronous), then the overlay on */
static int first_commit(void) {
    int ret = drmModeSetCrtc(fd, crtc_id, primary.bufs[primary.back].fb_id, 0, 0, &conn_id, 1, &mode);

    if (ret || !planes) {
        return ret;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();

    add_plane_setup(req, &overlay);
    add_prop(req, overlay.id, overlay.p_fb, overlay.bufs[overlay.back].fb_id);
    ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

bool drm_init(void) {
    const char *path = getenv("DRM_CARD");

    if (path ? !open_card(path) : !open_card("/dev/dri/card0") && !open_card("/dev/dri/card1")) {
        err("no usable device, fbdev is used");
        return false;
    }

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    planes = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 &&
             find_plane(&primary, DRM_PLANE_TYPE_PRIMARY, DRM_FORMAT_XRGB8888, 0) &&
             find_plane(&overlay, DRM_PLANE_TYPE_OVERLAY, DRM_FORMAT_ARGB8888, primary.id);

    shadow = calloc(PANEL_W * PANEL_H, 4);
    wf_shadow = planes ? calloc(PANEL_W * PANEL_H, 4) : NULL;

    if (!shadow || (planes && !wf_shadow) || !alloc_plane(&primary, DRM_FORMAT_XRGB8888) ||
        (planes && !alloc_plane(&overlay, DRM_FORMAT_ARGB8888)))
    {
        err("buffers not allocated, fbdev is used");
        free_plane(&primary);
        free_plane(&overlay);
        free(shadow);
        free(wf_shadow);
        shadow = wf_shadow = NULL;
        close(fd);              /* also frees the dumb buffers and their fbs */
        fd = -1;
        return false;
    }
    gui = planes ? &overlay : &primary;
#if !BOARD_FB_ROTATE_90
    if (planes) {
        splanes_init();
    }
#endif

    /* Both buffers are empty: the first two frames copy the whole screen */
    frame[0].full = true;
    frame[1].full = true;

    fprintf(stderr, "drm: %ux%u, connector %u, crtc %u, %s\n", mode.hdisplay, mode.vdisplay, conn_id, crtc_id,
            planes ? "GUI on the overlay plane, waterfall on the primary" : "one plane");
    return true;
}

bool drm_planes(void) {
    return planes;
}

void drm_set_portrait(bool on) {
    portrait_mode = on;
    frame[0].full = true;       /* both buffers get the whole GUI again */
    frame[1].full = true;
#if !BOARD_FB_ROTATE_90
    if (on) {
        splanes_off();          /* no strips in portrait */
    }
#endif
}

void drm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p) {
    if (portrait_mode) {
        /* Turned 180: the panel's native top is the side away from the knobs */
        if (area->x1 >= 0 && area->y1 >= 0 && area->x2 < PANEL_W && area->y2 < PANEL_H) {
            lv_area_t      p = { PANEL_W - 1 - area->x2, PANEL_H - 1 - area->y2,
                                 PANEL_W - 1 - area->x1, PANEL_H - 1 - area->y1 };
            const uint32_t *src = (const uint32_t *) color_p;

            for (int32_t y = area->y1; y <= area->y2; y++) {
                uint32_t *d = shadow + (PANEL_H - 1 - y) * PANEL_W + PANEL_W - 1 - area->x1;

                for (int32_t x = area->x1; x <= area->x2; x++) {
                    *d-- = *src++;
                }
            }
            rects_add(&frame[cur], &p);
        }
    } else if (area->x1 >= 0 && area->y1 >= 0 && area->x2 < LAND_W && area->y2 < LAND_H) {
#if BOARD_FB_ROTATE_90
        lv_area_t p = { area->y1, LAND_W - 1 - area->x2, area->y2, LAND_W - 1 - area->x1 };

        rotate_area(area, (const uint32_t *) color_p);
        rects_add(&frame[cur], &p);
#else
        copy_area(area, (const uint32_t *) color_p);
        rects_add(&frame[cur], area);
#endif
    }

    if (lv_disp_flush_is_last(drv)) {
        wait_flip();                    /* the back buffer was on screen until this flip */
        copy_rects(&gui->bufs[gui->back], &frame[cur ^ 1]);
        copy_rects(&gui->bufs[gui->back], &frame[cur]);
        gui->ready = true;
    }
    lv_disp_flush_ready(drv);
}

void drm_wf_present(const uint32_t *src, int32_t x, int32_t w) {
    drm_wf_present_ring(src, x, w, 0);
}

/* The strips presented (waterfall, spectrum) and the buffers that miss their latest
 * columns: those get them from wf_shadow just before they are shown (drm_commit),
 * unless the strip was presented again meanwhile. A range for all of them, copied
 * at the next present, also copied again the strips that present was redrawing. */
#define MAX_STRIPS 4
static struct {
    int32_t x, w;
    bool    stale[2];
} strips[MAX_STRIPS];
static int nstrips;

/* A strip of wf_shadow into buf: panel columns x .. x + w - 1 (portrait panel),
 * panel rows (landscape) */
static void copy_cols(struct drm_buffer *buf, int32_t x, int32_t w) {
#if BOARD_FB_ROTATE_90
    for (int32_t y = 0; y < PANEL_H; y++) {
        memcpy(buf->map + y * buf->pitch + x * 4, wf_shadow + y * PANEL_W + x, w * 4);
    }
#else
    for (int32_t y = x; y < x + w; y++) {
        memcpy(buf->map + y * buf->pitch, wf_shadow + y * PANEL_W, PANEL_W * 4);
    }
#endif
}

#if !BOARD_FB_ROTATE_90
/* Landscape: each strip on an overlay plane of its own, opaque, between the primary and
 * the GUI (when the CRTC has them free: splanes_init). Two buffers of 2 x PANEL_H rows
 * each. A whole picture goes into the buffer not on screen, rows 0 .. w - 1 in screen
 * order: one copy, no tearing. A ring turn writes only the new rows, each twice (rows c
 * and c + w of the buffer), and the plane's SRC_Y moves to the newest: the plane shows
 * rows win .. win + w - 1, and buffer row c (and c + w) holds src row (c + base) % w.
 * Before, every turn copied the whole strip three times (wf_shadow, then each buffer). */
#define MAX_SPLANES 2
typedef struct {
    plane_t     pl;         /* back: the buffer shown at the next commit; ready: to commit */
    uint32_t    p_zpos;
    int32_t     x, w;       /* panel rows x .. x + w - 1; w 0: free */
    int32_t     base, win;
    int         shown;      /* the buffer on screen */
    bool        valid;      /* holds the strip's picture: a ring turn may move win */
    bool        both;       /* rows w .. 2w - 1 hold it too */
    bool        on;         /* enabled (or to be, at the next commit) */
    bool        setup;      /* the next commit places it (position, size, zpos) */
} splane_t;
static splane_t splanes[MAX_SPLANES];
static int      nsplanes;
static bool     splanes_bad;    /* a commit placing them failed: strips on the primary plane again */

/* n rows of src (PANEL_W pixels each) into buf from row y */
static void copy_rows(struct drm_buffer *buf, int32_t y, const uint32_t *src, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        memcpy(buf->map + (size_t) (y + i) * buf->pitch, src + (size_t) i * PANEL_W, PANEL_W * 4);
    }
}

/* The plane of strip x, w: its own, else the one it overlaps (it moved or changed
 * height), else a free one; NULL: none, the strip goes on the primary plane */
static splane_t * splane_for(int32_t x, int32_t w) {
    splane_t *s = NULL;

    if (splanes_bad) {
        return NULL;
    }
    for (int i = 0; i < nsplanes; i++) {
        if (splanes[i].w && splanes[i].x == x && splanes[i].w == w) {
            return &splanes[i];
        }
    }
    for (int i = 0; i < nsplanes; i++) {
        splane_t *o = &splanes[i];

        if (o->w && o->x < x + w && x < o->x + o->w) {
            if (!s) {
                s = o;
            } else {
                o->w = 0;           /* a second one under the new strip: off */
                o->on = false;
                o->pl.ready = true;
            }
        }
    }
    for (int i = 0; i < nsplanes && !s; i++) {
        if (!splanes[i].w) {
            s = &splanes[i];
        }
    }
    if (s && (s->x != x || s->w != w)) {
        s->x = x;
        s->w = w;
        s->valid = false;
        s->setup = true;
    }
    return s;
}

static void splane_full(splane_t *s, const uint32_t *src, int32_t head) {
    struct drm_buffer *buf;

    wait_flip();
    s->pl.back = s->shown ^ 1;
    buf = &s->pl.bufs[s->pl.back];
    copy_rows(buf, 0, src + (size_t) head * PANEL_W, s->w - head);
    copy_rows(buf, s->w - head, src, head);
    s->base = head;
    s->win = 0;
    s->valid = true;
    s->both = false;
    if (!s->on) {
        s->on = true;
        s->setup = true;
    }
    s->pl.ready = true;
}

/* Rows head .. head + n - 1 (mod w) of src are new, the others as presented before */
static void splane_turn(splane_t *s, const uint32_t *src, int32_t head, int32_t n) {
    int32_t             w = s->w;
    struct drm_buffer   *buf;

    wait_flip();
    buf = &s->pl.bufs[s->pl.back];
    if (!s->both) {
        /* The second copy: off screen, win is 0 since the whole picture */
        copy_rows(buf, w, src + (size_t) s->base * PANEL_W, w - s->base);
        copy_rows(buf, 2 * w - s->base, src, s->base);
        s->both = true;
    }
    /* On screen already, the copy that replaces one of the oldest rows (the bottom one)
     * shows the new row there until the commit: n rows, one frame at most */
    for (int32_t i = 0; i < n; i++) {
        int32_t r = (head + i) % w;
        int32_t c = (r - s->base + w) % w;

        copy_rows(buf, c, src + (size_t) r * PANEL_W, 1);
        copy_rows(buf, c + w, src + (size_t) r * PANEL_W, 1);
    }
    s->win = (head - s->base + w) % w;
    s->pl.ready = true;
}

void drm_wf_scroll(const uint32_t *src, int32_t x, int32_t w, int32_t head, int32_t n) {
    splane_t *s;

    if (!planes || portrait_mode || x < 0 || w <= 0 || x + w > LAND_H) {
        return;
    }
    s = splane_for(x, w);
    if (!s) {
        drm_wf_present_ring(src, x, w, head);
    } else if (!s->valid || n <= 0 || n >= w) {
        splane_full(s, src, head);
    } else {
        splane_turn(s, src, head, n);
    }
}
#endif

void drm_wf_present_ring(const uint32_t *src, int32_t x, int32_t w, int32_t head) {
    int i, b;

    /* Portrait: no waterfall on the screen, the primary plane stays black */
    if (!planes || portrait_mode || x < 0 || w <= 0 || x + w > LAND_H) {
        return;
    }
#if !BOARD_FB_ROTATE_90
    splane_t *s = splane_for(x, w);

    if (s) {
        splane_full(s, src, head);
        return;
    }
#endif
#if BOARD_FB_ROTATE_90
    for (int32_t y = 0; y < PANEL_H; y++) {
        const uint32_t *s = src + y * w;
        uint32_t       *d = wf_shadow + y * PANEL_W + x;

        memcpy(d, s + head, (w - head) * 4);
        memcpy(d + w - head, s, head * 4);
    }
#else
    /* The ring is turned on the way: rows head .. w - 1 of src first */
    memcpy(wf_shadow + x * PANEL_W, src + head * PANEL_W, (size_t) (w - head) * PANEL_W * 4);
    memcpy(wf_shadow + (x + w - head) * PANEL_W, src, (size_t) head * PANEL_W * 4);
#endif
    for (i = 0; i < nstrips && strips[i].x != x; i++) {}
    if (i == nstrips) {
        if (nstrips == MAX_STRIPS) {
            i = 0;              /* not expected: the oldest is forgotten */
        } else {
            nstrips++;
        }
        strips[i].x = x;
    }
    strips[i].w = w;
    wait_flip();
    b = primary.back;
    copy_cols(&primary.bufs[b], x, w);
    strips[i].stale[b] = false;
    strips[i].stale[b ^ 1] = true;
    primary.ready = true;
}

uint32_t drm_frame_ms(void) {
    return mode.vrefresh ? (1000 + mode.vrefresh - 1) / mode.vrefresh : 17;
}

static bool plane_taken(uint32_t id, uint32_t skip) {
#if !BOARD_FB_ROTATE_90
    for (int i = 0; i < nsplanes; i++) {
        if (splanes[i].pl.id == id) {
            return true;
        }
    }
#endif
    return id == skip;
}

#if !BOARD_FB_ROTATE_90
static uint32_t gui_zpos;       /* the GUI's zpos property: over the strip planes */
static bool     gui_zpos_set;

static uint32_t plane_prop(uint32_t id, const char *name) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
    uint32_t                p = props ? prop_id(props, name) : 0;

    drmModeFreeObjectProperties(props);
    return p;
}

/* The overlay planes left on our CRTC for the strips, with their buffers; none and the strips
 * go on the primary plane as before */
static void splanes_init(void) {
    gui_zpos = plane_prop(overlay.id, "zpos");
    while (gui_zpos && nsplanes < MAX_SPLANES) {
        splane_t *s = &splanes[nsplanes];

        if (!find_plane(&s->pl, DRM_PLANE_TYPE_OVERLAY, DRM_FORMAT_XRGB8888, overlay.id) ||
            !(s->p_zpos = plane_prop(s->pl.id, "zpos")) ||
            !alloc_buf_h(&s->pl.bufs[0], DRM_FORMAT_XRGB8888, 2 * PANEL_H) ||
            !alloc_buf_h(&s->pl.bufs[1], DRM_FORMAT_XRGB8888, 2 * PANEL_H))
        {
            free_plane(&s->pl);
            memset(s, 0, sizeof(*s));
            break;
        }
        nsplanes++;
    }
    if (nsplanes) {
        fprintf(stderr, "drm: %d strip planes (waterfall, spectrum) under the GUI\n", nsplanes);
    }
}

/* The strips' planes off at the next commit; each back the next time its strip is presented */
static void splanes_off(void) {
    for (int i = 0; i < nsplanes; i++) {
        if (splanes[i].w) {
            splanes[i].w = 0;
            splanes[i].on = false;
            splanes[i].valid = false;
            splanes[i].pl.ready = true;
        }
    }
}

static bool splanes_ready(void) {
    for (int i = 0; i < nsplanes; i++) {
        if (splanes[i].pl.ready) {
            return true;
        }
    }
    return false;
}

/* Into the commit: the planes that changed. true: one of them is placed (a commit that can fail on it) */
static bool splanes_add(drmModeAtomicReq *req) {
    bool placed = false;

    for (int i = 0; i < nsplanes; i++) {
        splane_t *s = &splanes[i];
        plane_t  *p = &s->pl;

        if (!p->ready) {
            continue;
        }
        if (!s->on) {
            add_prop(req, p->id, p->p_fb, 0);
            add_prop(req, p->id, p->p_crtc, 0);
            continue;
        }
        if (s->setup) {
            add_prop(req, p->id, p->p_crtc, crtc_id);
            add_prop(req, p->id, p->p_sx, 0);
            add_prop(req, p->id, p->p_sw, (uint64_t) PANEL_W << 16);
            add_prop(req, p->id, p->p_sh, (uint64_t) s->w << 16);
            add_prop(req, p->id, p->p_cx, 0);
            add_prop(req, p->id, p->p_cy, s->x);
            add_prop(req, p->id, p->p_cw, PANEL_W);
            add_prop(req, p->id, p->p_ch, s->w);
            add_prop(req, p->id, s->p_zpos, 1 + i);
            placed = true;
        }
        add_prop(req, p->id, p->p_fb, p->bufs[p->back].fb_id);
        add_prop(req, p->id, p->p_sy, (uint64_t) s->win << 16);
    }
    if (placed && !gui_zpos_set) {
        add_prop(req, overlay.id, gui_zpos, MAX_SPLANES + 1);
    }
    return placed;
}

static void splanes_done(bool placed) {
    for (int i = 0; i < nsplanes; i++) {
        splane_t *s = &splanes[i];

        if (s->pl.ready) {
            s->pl.ready = false;
            s->shown = s->pl.back;
            s->setup = false;
        }
    }
    gui_zpos_set |= placed;
}

/* Placing them was refused: off at the next commit, and the strips on the primary plane from now on */
static void splanes_fail(void) {
    err("strip planes refused, strips on the primary plane");
    splanes_bad = true;
    for (int i = 0; i < nsplanes; i++) {
        splanes[i].w = 0;
        splanes[i].on = false;
        splanes[i].setup = false;
        splanes[i].pl.ready = true;
    }
}
#endif

/* Row y of what shows under the GUI: a strip plane's, else the primary plane's (wf_shadow) */
static const uint32_t * under_row(int32_t y) {
#if !BOARD_FB_ROTATE_90
    for (int i = 0; i < nsplanes; i++) {
        splane_t *s = &splanes[i];

        if (s->on && s->valid && y >= s->x && y < s->x + s->w) {
            struct drm_buffer *b = &s->pl.bufs[s->pl.back];

            return (const uint32_t *) (b->map + (size_t) (s->win + y - s->x) * b->pitch);
        }
    }
#endif
    return wf_shadow + y * PANEL_W;
}

void drm_commit(void) {
    static bool failed;
    int         ret;

#if !BOARD_FB_ROTATE_90
    bool placed = false;

    if (fd < 0 || !(gui->ready || (planes && primary.ready) || splanes_ready())) {
        return;
    }
#else
    if (fd < 0 || !(gui->ready || (planes && primary.ready))) {
        return;
    }
#endif
    wait_flip();
    if (planes && primary.ready) {
        for (int i = 0; i < nstrips; i++) {
            if (strips[i].stale[primary.back]) {
                copy_cols(&primary.bufs[primary.back], strips[i].x, strips[i].w);
                strips[i].stale[primary.back] = false;
            }
        }
    }

    if (!crtc_set) {
        ret = first_commit();
        crtc_set = ret == 0;
    } else if (planes) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();

        if (overlay.ready) {
            add_prop(req, overlay.id, overlay.p_fb, overlay.bufs[overlay.back].fb_id);
        }
        if (primary.ready) {
            add_prop(req, primary.id, primary.p_fb, primary.bufs[primary.back].fb_id);
        }
#if !BOARD_FB_ROTATE_90
        placed = splanes_add(req);
#endif
        /* shown at the next vblank, the event clears flip_pending */
        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
        drmModeAtomicFree(req);
        flip_pending = ret == 0;
#if !BOARD_FB_ROTATE_90
        if (ret == 0) {
            splanes_done(placed);
        } else if (placed && !splanes_bad) {
            splanes_fail();     /* the GUI and the primary plane go again at the next commit, without them */
            return;
        }
#endif
    } else {
        ret = drmModePageFlip(fd, crtc_id, primary.bufs[primary.back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
        flip_pending = ret == 0;
    }

    if (ret) {
        if (!failed) {
            err("%s: %s", crtc_set ? "commit" : "set crtc", strerror(errno));
        }
        failed = true;
        /* Not shown, buffers not swapped: both get the whole GUI once commits work again */
        frame[0].full = true;
        frame[1].full = true;
        return;
    }
    failed = false;

    if (gui->ready) {
        gui->ready = false;
        gui->back ^= 1;
        /* this frame becomes the previous one */
        cur ^= 1;
        frame[cur].cnt = 0;
        frame[cur].full = false;
    }
    if (planes && primary.ready) {
        primary.ready = false;
        primary.back ^= 1;
    }
}

bool drm_dump(const char *path) {
    FILE *f;
    bool  ok;

    if (!shadow || !(f = fopen(path, "wb"))) {
        return false;
    }
    if (planes) {
        /* What the panel shows: the GUI over the waterfall */
        static uint32_t row[PANEL_W];

        ok = true;
        for (int32_t y = 0; y < PANEL_H && ok; y++) {
            drm_blend_under(row, shadow + y * PANEL_W, under_row(y), PANEL_W);
            ok = fwrite(row, 4, PANEL_W, f) == PANEL_W;
        }
    } else {
        ok = fwrite(shadow, 4, PANEL_W * PANEL_H, f) == PANEL_W * PANEL_H;
    }
    return fclose(f) == 0 && ok;
}

void drm_blend_under(uint32_t *dst, const uint32_t *top, const uint32_t *bottom, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        uint32_t t = top[i], b = bottom[i];
        uint32_t a = t >> 24;

        if (a == 255) {
            dst[i] = t;
        } else if (a == 0) {
            dst[i] = b | 0xff000000;
        } else {
            uint32_t r = (((t >> 16) & 0xff) * a + ((b >> 16) & 0xff) * (255 - a) + 127) / 255;
            uint32_t g = (((t >> 8) & 0xff) * a + ((b >> 8) & 0xff) * (255 - a) + 127) / 255;
            uint32_t bl = ((t & 0xff) * a + (b & 0xff) * (255 - a) + 127) / 255;

            dst[i] = 0xff000000 | (r << 16) | (g << 8) | bl;
        }
    }
}

const uint32_t * drm_wf_pixels(void) {
    return planes ? wf_shadow : NULL;
}

void drm_compose_landscape(uint32_t *px) {
    if (!planes) {
        return;
    }
    for (int32_t y = 0; y < LAND_H; y++) {
        uint32_t *row = px + y * LAND_W;
        uint32_t under[LAND_W];

#if BOARD_FB_ROTATE_90
        for (int32_t x = 0; x < LAND_W; x++) {
            under[x] = wf_shadow[(LAND_W - 1 - x) * PANEL_W + y];
        }
#else
        memcpy(under, under_row(y), (size_t) LAND_W * 4);
#endif
        drm_blend_under(row, row, under, LAND_W);
    }
}

#endif
