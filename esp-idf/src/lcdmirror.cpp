/**
 * lcdmirror.cpp — the browser screen-mirror device task (see lcdmirror.h).
 *
 * Design: a full-screen framebuffer + damage box, drained at a capped rate into
 * one coalesced packet per tick. The capture sink (lcd task) blits each LVGL
 * flush strip into the framebuffer and grows a damage bounding-box — no per-strip
 * queue, no per-strip packet. The lcdmirror task, ~30 times a second, snapshots
 * the damage box, encodes that region from the framebuffer as ONE RLE packet
 * (banded only if it exceeds the size cap) and sends it.
 *
 * This is the backpressure gradient: if the send window is below a frame's worth,
 * the tick skips and the damage box keeps accumulating, so under sustained motion
 * you get larger, less-frequent *coalesced* frames — a smooth degradation, never
 * a per-strip flood or a stall. Dropping the DC survives only as the last resort
 * for a client so wedged the window never drains (STUCK_MS).
 *
 * Threading. The sink runs on the lcd task inside flushCb: it memcpys strip rows
 * into the framebuffer and expands the damage box under a tiny spinlock — no ITS,
 * no compression, so it never stalls the render path (which holds the SPI bus
 * lock shared with LoRa). Everything else runs on the lcdmirror task, sole owner
 * of s_client. The framebuffer is read during encode without a lock: a torn read
 * costs at most a few stale pixels for one frame, corrected on the next — the
 * standard VNC-style trade to keep the render path lock-free. lcdMirror* are safe
 * from any task.
 */
#include "lcdmirror.h"

#include "its.h"
#include "log.h"
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "lcd.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "lcdmirror";

/* Send-rate cap for the SCREEN only. 5 Hz suits this SPI panel over a reliable
 * channel: coalescing merges each 200 ms of damage into one well-compressed frame,
 * so a scroll costs a handful of full frames instead of flooding the link (which
 * dropped at 30 Hz). Input is drained on a separate, fast cadence (POLL_MS) so
 * pointer/keys stay responsive and never back up the device's receive buffer —
 * capping the send rate must not throttle the receive path. */
#define FRAME_MS         200           /* ~5 fps screen-send cap */
#define POLL_MS          20            /* input/connect drain cadence (~50 Hz) */
/* Defer all capture/encode until a new session has survived this long. A busy
 * device (an on-device Nomad page fetch+render) can be slow to answer the
 * browser's 1 s storage heartbeat, so a fresh session may flap a few times
 * before it sticks. Piling a full-screen keyframe onto that fragile just-
 * connected window only makes it worse, so the mirror stays idle until the link
 * proves stable — during a flap it does nothing. */
#define ACTIVATE_MS      1500

/* ITS packet port sizing. MAXBODY caps one coalesced-frame packet — a typical
 * full screen RLEs well under it, so the whole screen is usually ONE packet;
 * larger frames band by rows. fromCap holds a couple of frames in flight so a
 * transient stall rides the buffer. toCap carries only tiny pointer packets. */
#define MAXBODY          (48 * 1024)
#define ITS_TO_CAP       4096          /* input window (6 B pointer / 5 B key packets) */
#define ITS_FROM_CAP     (128 * 1024)
#define ITS_MAX_MSG      (MAXBODY + LCDMIRROR_HDR_BYTES)
#define CONGEST_MIN      MAXBODY       /* need a frame's room free to send this tick */
#define STUCK_MS         8000          /* dirty + congested this long → drop the DC */

/* Full-screen framebuffer (little-endian RGB565), allocated on first connect and
 * kept for the process lifetime (so a flush mid-copy can never race a free). */
static uint16_t*    s_fb     = nullptr;
static uint8_t*     s_gather = nullptr;   /* contiguous region pixels for encode */
static uint8_t*     s_enc    = nullptr;   /* header + encoded body */
static int          s_fbW = 0, s_fbH = 0;

/* Damage bounding-box (inclusive; x2 < x1 means empty). Written by the sink (lcd
 * task) and the drain (lcdmirror task) under s_dmgMux. */
static portMUX_TYPE s_dmgMux = portMUX_INITIALIZER_UNLOCKED;
static int          s_dx1 = 0, s_dy1 = 0, s_dx2 = -1, s_dy2 = -1;
static bool         s_dirty = false;
static TickType_t   s_dirtySince = 0;

/* Session (lcdmirror task). */
static int          s_client = -1;
static bool         s_wantKeyframe = false;
static bool         s_active   = false;   /* capture attached (session proven stable) */
static TickType_t   s_activateAt = 0;     /* when to activate a freshly-connected client */

/* ---- damage box ---- */

static inline void dmgReset(void) { s_dx1 = 0; s_dy1 = 0; s_dx2 = -1; s_dy2 = -1; s_dirty = false; }

static void dmgAdd(int x1, int y1, int x2, int y2) {
    taskENTER_CRITICAL(&s_dmgMux);
    bool was = s_dirty;
    if (s_dx2 < s_dx1) { s_dx1 = x1; s_dy1 = y1; s_dx2 = x2; s_dy2 = y2; }
    else {
        if (x1 < s_dx1) s_dx1 = x1;
        if (y1 < s_dy1) s_dy1 = y1;
        if (x2 > s_dx2) s_dx2 = x2;
        if (y2 > s_dy2) s_dy2 = y2;
    }
    s_dirty = true;
    if (!was) s_dirtySince = xTaskGetTickCount();
    taskEXIT_CRITICAL(&s_dmgMux);
}

/* ---- capture sink (LCD TASK, inside flushCb, pre-swap little-endian RGB565) ---- */

static void sinkCb(const lv_area_t* area, const uint8_t* px) {
    if (!s_fb) return;
    int aw = area->x2 - area->x1 + 1;              /* strip stride */
    int x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= s_fbW) x2 = s_fbW - 1;
    if (y2 >= s_fbH) y2 = s_fbH - 1;
    if (x2 < x1 || y2 < y1) return;
    const uint16_t* src = (const uint16_t*)px;
    size_t rowBytes = (size_t)(x2 - x1 + 1) * 2;
    for (int r = y1; r <= y2; r++)
        memcpy(&s_fb[(size_t)r * s_fbW + x1],
               &src[(size_t)(r - area->y1) * aw + (x1 - area->x1)], rowBytes);
    dmgAdd(x1, y1, x2, y2);
}

/* ---- RLE (PackBits over 16-bit pixels) ---- */

static size_t rleEncode(const uint16_t* src, int n, uint8_t* out, size_t outCap) {
    size_t o = 0;
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && src[i + run] == src[i] && run < 129) run++;
        if (run >= 3) {
            if (o + 3 > outCap) return 0;
            out[o++] = (uint8_t)(128 + (run - 2));
            uint16_t p = src[i];
            out[o++] = (uint8_t)(p & 0xff);
            out[o++] = (uint8_t)(p >> 8);
            i += run;
        } else {
            int litStart = i, lit = 0;
            while (i < n && lit < 128) {
                int r = 1;
                while (i + r < n && src[i + r] == src[i] && r < 3) r++;
                if (r >= 3) break;
                i++; lit++;
            }
            if (o + 1 + (size_t)2 * lit > outCap) return 0;
            out[o++] = (uint8_t)(lit - 1);
            for (int k = 0; k < lit; k++) {
                uint16_t p = src[litStart + k];
                out[o++] = (uint8_t)(p & 0xff);
                out[o++] = (uint8_t)(p >> 8);
            }
        }
    }
    return o;
}

static void putHdr(uint8_t* h, int x, int y, int w, int hgt, uint8_t fmt, uint8_t flags) {
    h[0] = (uint8_t)(x & 0xff);   h[1] = (uint8_t)((x >> 8) & 0xff);
    h[2] = (uint8_t)(y & 0xff);   h[3] = (uint8_t)((y >> 8) & 0xff);
    h[4] = (uint8_t)(w & 0xff);   h[5] = (uint8_t)((w >> 8) & 0xff);
    h[6] = (uint8_t)(hgt & 0xff); h[7] = (uint8_t)((hgt >> 8) & 0xff);
    h[8] = fmt; h[9] = flags;
}

/* Encode one contiguous band (already gathered at `g`, w*h pixels) and send it as
 * one packet. Returns false if the send window is full. */
static bool sendPacket(const uint16_t* g, int x, int y, int w, int h, bool keyframe) {
    int n = w * h;
    size_t raw = (size_t)n * 2;
    uint8_t fmt = LCDMIRROR_FMT_RLE16;
    size_t body = rleEncode(g, n, s_enc + LCDMIRROR_HDR_BYTES, MAXBODY);
    if (body == 0 || body >= raw) {                 /* incompressible / too big for RLE */
        fmt = LCDMIRROR_FMT_RAW16;
        body = raw;
        memcpy(s_enc + LCDMIRROR_HDR_BYTES, g, raw);
    }
    putHdr(s_enc, x, y, w, h, fmt, keyframe ? LCDMIRROR_FLAG_KEYFRAME : 0);
    return itsSend(s_client, s_enc, LCDMIRROR_HDR_BYTES + body, 0) != 0;
}

/* Gather the damage region from the framebuffer and send it — as ONE packet when
 * it fits MAXBODY, else banded by rows. Returns false if a send stalled. */
static bool sendRegion(int x1, int y1, int x2, int y2, bool keyframe) {
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w <= 0 || h <= 0) return true;
    for (int r = 0; r < h; r++)
        memcpy(s_gather + (size_t)r * w * 2, &s_fb[(size_t)(y1 + r) * s_fbW + x1], (size_t)w * 2);
    const uint16_t* g = (const uint16_t*)s_gather;

    if ((size_t)w * h * 2 <= MAXBODY)               /* whole region fits one packet */
        return sendPacket(g, x1, y1, w, h, keyframe);

    int bandRows = MAXBODY / (w * 2);
    if (bandRows < 1) bandRows = 1;
    for (int r = 0; r < h; r += bandRows) {
        int bh = (r + bandRows <= h) ? bandRows : (h - r);
        if (!sendPacket(g + (size_t)r * w, x1, y1 + r, w, bh, keyframe && r == 0))
            return false;
    }
    return true;
}

/* ---- session ---- */

static bool ensureFb(void) {
    if (s_fb) return true;
    lv_display_t* d = lv_display_get_default();
    if (!d) return false;
    int w = lv_display_get_horizontal_resolution(d);
    int h = lv_display_get_vertical_resolution(d);
    if (w <= 0 || h <= 0) return false;
    s_fbW = w; s_fbH = h;
    s_fb     = (uint16_t*)gp_alloc((size_t)w * h * 2);
    s_gather = (uint8_t*)gp_alloc((size_t)w * h * 2);
    s_enc    = (uint8_t*)gp_alloc(MAXBODY + LCDMIRROR_HDR_BYTES);
    if (!s_fb || !s_gather || !s_enc) { err("framebuffer alloc failed\n"); return false; }
    return true;
}

/* Begin capturing for a client whose session has proven stable (ACTIVATE_MS). */
static void activate(void) {
    s_active = true;
    s_wantKeyframe = true;
    taskENTER_CRITICAL(&s_dmgMux); dmgReset(); taskEXIT_CRITICAL(&s_dmgMux);
    lcdMirrorAttach(sinkCb);            /* start filling the framebuffer */
    lcdMirrorKeepAwake(true);
    lcdRun([](void*) { lv_obj_invalidate(lv_screen_active()); });  /* populate fb → full damage */
    storageSet("lcdmirror.clients", "1");
    info("client active (handle %d)\n", s_client);
}

static void clientGone(void) {
    bool wasActive = s_active;
    lcdMirrorAttach(nullptr);
    lcdMirrorKeepAwake(false);
    s_client = -1;
    s_active = false;
    s_wantKeyframe = false;
    taskENTER_CRITICAL(&s_dmgMux); dmgReset(); taskEXIT_CRITICAL(&s_dmgMux);
    if (wasActive) storageSet("lcdmirror.clients", "0");   /* skip churn on flaps that never activated */
}

static void dropClient(const char* why) {
    if (s_client >= 0) {
        warn("dropping client: %s\n", why);
        itsDisconnect(s_client);        /* stream-reset → browser reconnects + keyframe */
    }
    clientGone();
}

/* ---- ITS server callbacks (lcdmirror task, via itsPoll) ---- */

static int onConnect(int handle, const void* /*data*/, size_t /*len*/) {
    if (storageGetInt("s.lcdmirror.enabled", 1) == 0) return -1;
    if (s_client >= 0) return -1;
    if (!ensureFb()) return -1;
    /* Accept, but don't attach/capture yet — wait ACTIVATE_MS to see the session
     * survive (see the task loop). During a flap the mirror stays fully idle. */
    s_client = handle;
    s_active = false;
    s_activateAt = xTaskGetTickCount() + pdMS_TO_TICKS(ACTIVATE_MS);
    info("client connected (handle %d) — settling\n", handle);
    return 0;
}

static void onDisconnect(int /*ref*/) {
    info("client disconnected\n");
    clientGone();
}

static void onRecv(int handle, size_t /*bytesAvail*/) {
    uint8_t buf[16];
    size_t nb;
    while ((nb = itsRecv(handle, buf, sizeof(buf), 0)) > 0) {
        if (nb >= 6 && buf[0] == LCDMIRROR_INPUT_POINTER) {
            int16_t x = (int16_t)(buf[2] | (buf[3] << 8));
            int16_t y = (int16_t)(buf[4] | (buf[5] << 8));
            lcdMirrorInjectPointer(x, y, buf[1] ? LCD_PTR_PRESSED : LCD_PTR_RELEASED);
        } else if (nb >= 5 && buf[0] == LCDMIRROR_INPUT_KEY) {
            uint32_t key = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
                           ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
            lcdMirrorInjectKey(key);
        }
    }
}

/* ---- task ---- */

static void lcdmirrorTask(void*) {
    if (!itsServerInit()) { err("itsServerInit failed\n"); vTaskDelete(nullptr); return; }
    if (!itsServerPortOpen(LCDMIRROR_PORT, ITS_PACKET, 1,
                           ITS_TO_CAP, ITS_FROM_CAP, 0, ITS_MAX_MSG)) {
        err("port open failed\n");
        vTaskDelete(nullptr);
        return;
    }
    itsServerOnConnect(LCDMIRROR_PORT, onConnect);
    itsServerOnRecv(LCDMIRROR_PORT, onRecv);
    itsServerOnDisconnect(LCDMIRROR_PORT, onDisconnect);

    storageSubscribeChanges("s.lcdmirror.enabled", ON_CHANGE {
        if (s_client >= 0 && val && val[0] == '0') dropClient("disabled");
    });

    info("up: ITS port %d\n", LCDMIRROR_PORT);

    TickType_t lastSend = xTaskGetTickCount();
    for (;;) {
        /* Drain input/connect/disconnect promptly — itsPoll wakes the moment a
         * packet arrives, or after POLL_MS. This is NOT rate-capped; only the
         * screen send below is. */
        itsPoll(pdMS_TO_TICKS(POLL_MS));
        while (itsPoll(0)) {}
        if (s_client < 0) { lastSend = xTaskGetTickCount(); continue; }

        /* Hold off capture until the session has survived ACTIVATE_MS. Until then
         * the mirror is idle (no sink attached, nothing sent), so a flapping
         * connection never gets a keyframe piled onto it. */
        if (!s_active) {
            if ((int32_t)(xTaskGetTickCount() - s_activateAt) < 0) continue;
            activate();
        }

        /* Rate-cap the screen send to ~FRAME_MS. */
        if (xTaskGetTickCount() - lastSend < pdMS_TO_TICKS(FRAME_MS)) continue;
        lastSend = xTaskGetTickCount();

        int x1, y1, x2, y2;
        bool dirty;
        taskENTER_CRITICAL(&s_dmgMux);
        dirty = s_dirty; x1 = s_dx1; y1 = s_dy1; x2 = s_dx2; y2 = s_dy2;
        taskEXIT_CRITICAL(&s_dmgMux);
        if (!dirty) continue;

        /* Congested: leave the damage to accumulate (coalesce) and wait; only a
         * client that never drains for STUCK_MS gets dropped. */
        if (itsSpacesAvailable(s_client) < CONGEST_MIN) {
            if (xTaskGetTickCount() - s_dirtySince > pdMS_TO_TICKS(STUCK_MS)) dropClient("stuck");
            continue;
        }

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= s_fbW) x2 = s_fbW - 1;
        if (y2 >= s_fbH) y2 = s_fbH - 1;
        bool kf = s_wantKeyframe;

        /* Clear the box before encoding: changes arriving mid-encode re-mark it
         * (and are read from the framebuffer with the latest pixels), so nothing
         * is lost — at worst a region is resent next tick. */
        taskENTER_CRITICAL(&s_dmgMux); dmgReset(); taskEXIT_CRITICAL(&s_dmgMux);

        if (sendRegion(x1, y1, x2, y2, kf)) s_wantKeyframe = false;
        else dmgAdd(x1, y1, x2, y2);        /* window filled mid-send → resend next tick */
    }
}

/* ---- boot ---- */

void LcdmirrorService::onInit() {
    storageBegin();
    storageDefault("s.lcdmirror.enabled", 1);
    storageEnd();
    storageSet("lcdmirror.clients", "0");

    /* Core 1, prio 1, 8 KB PSRAM stack. */
    spawnTask(lcdmirrorTask, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
}
