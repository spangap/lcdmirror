/**
 * lcdmirror — the browser screen-mirror device task.
 *
 * The `lcdmirror` FreeRTOS task opens an ITS packet server port (the WebRTC
 * DataChannel↔ITS router bridges a browser DataChannel labelled
 * "lcdmirror:<port>" to it), taps -lcd's framebuffer while a client is connected
 * (lcdMirrorAttach), drains a ring of captured dirty rectangles, RLE-encodes each
 * and sends it, streams a full keyframe on every new connection, injects the
 * remote pointer back into the LVGL UI (lcdMirrorInjectPointer), holds the panel
 * awake while watched (lcdMirrorKeepAwake), and applies the backpressure→
 * drop-the-DC policy. Compression and ITS never run on the lcd task — the capture
 * sink only memcpys a strip into the ring.
 *
 * Wire protocol (little-endian throughout):
 *
 *   Device → browser, one packet per dirty rectangle:
 *     header (10 bytes): u16 x, u16 y, u16 w, u16 h, u8 format, u8 flags
 *       format 0 = RLE16, 1 = RAW16 (uncompressed RGB565 little-endian)
 *       flags  bit0 (0x01) = KEYFRAME_START (first rect of a full repaint)
 *     body: RLE16 stream or raw RGB565, w*h pixels row-major.
 *
 *   RLE16 opcode stream over 16-bit pixels (PackBits-style):
 *     control c in [0,127]   : literal run of (c+1) pixels; (c+1)*2 body bytes.
 *     control c in [128,255] : repeat run of (c-128)+2 pixels; one 2-byte pixel.
 *
 *   Browser → device, one packet per pointer sample (6 bytes):
 *     u8 type (1=pointer), u8 pressed (0/1), u16 x, u16 y   (panel coords)
 */
#pragma once

#include "service.h"
#include <cstdint>
#include <cstddef>

/** The ITS packet port the browser DataChannel "lcdmirror:<port>" routes to. */
#define LCDMIRROR_PORT        1

/* Wire-protocol constants (shared with the browser decoder in lcdmirror.ts). */
#define LCDMIRROR_HDR_BYTES   10
#define LCDMIRROR_FMT_RLE16   0
#define LCDMIRROR_FMT_RAW16   1
#define LCDMIRROR_FLAG_KEYFRAME 0x01
#define LCDMIRROR_INPUT_POINTER 1
#define LCDMIRROR_INPUT_KEY     2

/** Boot service: spawns the `lcdmirror` task. */
class LcdmirrorService : public Service {
public:
    void onInit() override;
};
