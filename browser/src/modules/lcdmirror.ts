/**
 * lcdmirror — browser-side screen mirror (SPA side).
 *
 * A floating window (the "Screen" Dock app) whose <canvas> shows the device's
 * live LCD, streamed as RLE dirty-rectangles over a WebRTC DataChannel
 * ("lcdmirror:1", reliable+ordered). Click/drag in the canvas is mapped to panel
 * coordinates and sent back, driving the same LVGL UI the physical panel drives.
 *
 * This module owns the Dock + window-mount registration and the visibility refs;
 * the DataChannel lifecycle and pixel decode live in the LcdMirrorWindow component
 * (which owns the canvas). The wire format is defined in the firmware lcdmirror.h.
 */
import { ref } from 'vue'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
/* Import cycle with the panel (it pulls the wire-format constants from here) —
 * benign: both sides only touch the other's bindings from inside functions,
 * never at module-eval time. */
import LcdMirrorWindow from '../panels/LcdMirrorWindow.vue'

/* Window visibility + focus nonce — <StraddleWindows/> binds these to the
 * window through the mount registered below. */
export const lcdMirrorVisible = ref(false)
export const lcdMirrorFocus = ref(0)

/** Dock action: show + raise the mirror window. */
export function showLcdMirror() {
  lcdMirrorVisible.value = true
  lcdMirrorFocus.value++
}

export function registerLcdMirror() {
  registerApp({
    id: 'lcdmirror',
    label: 'Screen',
    icon: 'lcdmirror',
    placement: 8,
    open: showLcdMirror,
    isOpen: () => lcdMirrorVisible.value,
  })
  registerWindowMount({
    id: 'lcdmirror',
    title: 'LCD mirror',
    component: LcdMirrorWindow,
    visible: lcdMirrorVisible,
    focusToken: lcdMirrorFocus,
  })
}

/* ---- Wire format (mirrors firmware lcdmirror.h) ---- */

export const HDR_BYTES = 10
export const FMT_RLE16 = 0
export const FMT_RAW16 = 1
export const FLAG_KEYFRAME = 0x01
export const INPUT_POINTER = 1
export const INPUT_KEY = 2

/* LVGL key codes (lv_group). Printable characters go through as their Unicode
 * codepoint; these are the special keys. LCD_KEY_CTRL is spangap-lcd's modifier
 * bit meaning "Ctrl + this letter" (decoded to a control byte by the terminal). */
const LV_KEY = {
  UP: 17, DOWN: 18, RIGHT: 19, LEFT: 20, ESC: 27, DEL: 127,
  BACKSPACE: 8, ENTER: 10, NEXT: 9, PREV: 11, HOME: 2, END: 3,
}
const LCD_KEY_CTRL = 0x40000000

/** Map a browser KeyboardEvent to an LVGL key code, or null if it should be
 *  ignored (a bare modifier, an unmapped key). */
export function mapKeyEvent(ev: KeyboardEvent): number | null {
  switch (ev.key) {
    case 'Enter': return LV_KEY.ENTER
    case 'Backspace': return LV_KEY.BACKSPACE
    case 'Escape': return LV_KEY.ESC
    case 'Delete': return LV_KEY.DEL
    case 'ArrowUp': return LV_KEY.UP
    case 'ArrowDown': return LV_KEY.DOWN
    case 'ArrowLeft': return LV_KEY.LEFT
    case 'ArrowRight': return LV_KEY.RIGHT
    case 'Tab': return ev.shiftKey ? LV_KEY.PREV : LV_KEY.NEXT
    case 'Home': return LV_KEY.HOME
    case 'End': return LV_KEY.END
  }
  if (ev.key.length === 1) {
    const cp = ev.key.codePointAt(0)!
    if (ev.ctrlKey) {
      const lower = ev.key.toLowerCase()
      if (lower >= 'a' && lower <= 'z') return LCD_KEY_CTRL | lower.codePointAt(0)!
    }
    return cp
  }
  return null
}

export interface RectHeader {
  x: number; y: number; w: number; h: number
  format: number; flags: number
}

export function parseHeader(v: DataView): RectHeader {
  return {
    x: v.getUint16(0, true),
    y: v.getUint16(2, true),
    w: v.getUint16(4, true),
    h: v.getUint16(6, true),
    format: v.getUint8(8),
    flags: v.getUint8(9),
  }
}

/** Decode `n` RGB565 pixels from a body: RLE16 (PackBits over 16-bit pixels) or
 *  raw. Returns a Uint16Array of length n (little-endian RGB565). */
export function decodeBody(body: Uint8Array, n: number, format: number): Uint16Array {
  const out = new Uint16Array(n)
  if (format === FMT_RAW16) {
    for (let k = 0, i = 0; k < n; k++, i += 2) out[k] = body[i] | (body[i + 1] << 8)
    return out
  }
  let o = 0, i = 0
  while (o < n && i < body.length) {
    const c = body[i++]
    if (c < 128) {
      const cnt = c + 1
      for (let k = 0; k < cnt && o < n; k++, i += 2) out[o++] = body[i] | (body[i + 1] << 8)
    } else {
      const cnt = (c - 128) + 2
      const px = body[i] | (body[i + 1] << 8)
      i += 2
      for (let k = 0; k < cnt && o < n; k++) out[o++] = px
    }
  }
  return out
}

/** Expand `n` little-endian RGB565 pixels into an RGBA byte buffer (opaque). */
export function rgb565ToRgba(px: Uint16Array, n: number): Uint8ClampedArray {
  const rgba = new Uint8ClampedArray(n * 4)
  for (let k = 0, j = 0; k < n; k++, j += 4) {
    const p = px[k]
    const r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f
    rgba[j] = (r5 << 3) | (r5 >> 2)
    rgba[j + 1] = (g6 << 2) | (g6 >> 4)
    rgba[j + 2] = (b5 << 3) | (b5 >> 2)
    rgba[j + 3] = 255
  }
  return rgba
}
