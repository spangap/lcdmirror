<!-- LcdMirrorWindow — the "Screen" Dock app: a live mirror of the device LCD.
     A <canvas> at the panel's native resolution shows RLE dirty-rectangles
     streamed over the "lcdmirror:1" DataChannel (reliable+ordered); pointer
     events are mapped to panel coordinates and sent back, so clicking/dragging
     here drives the same LVGL UI the physical panel drives.

     The backing store is native (320x240) and CSS-scaled to fit, aspect
     preserved. On DataChannel close (the device drops it under sustained
     backpressure) we reopen with exponential backoff; the device sends a fresh
     keyframe on every (re)connect, so the canvas heals itself. -->
<template>
  <FloatingWindow
    ref="win"
    id="lcdmirror"
    :title="title"
    :visible="visible"
    :focus-token="focusToken"
    :default-geom="defaultGeom"
    :min-size="{ w: 12, h: 10 }"
    :aspect="aspect"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <button class="cam" title="Save screenshot (PNG)" @click="screenshot">
        <!-- camera glyph, white strokes -->
        <svg viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="1.7"
             stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 8.5 h3 l1.4 -2 h5.2 l1.4 2 h3 a1 1 0 0 1 1 1 v8 a1 1 0 0 1 -1 1 H3 a1 1 0 0 1 -1 -1 v-8 a1 1 0 0 1 1 -1 Z" />
          <circle cx="12" cy="13" r="3.2" />
        </svg>
      </button>
    </template>
    <div class="mirror">
      <canvas
        ref="cv"
        class="screen"
        width="320"
        height="240"
        tabindex="0"
        @pointerdown="onDown"
        @pointermove="onMove"
        @pointerup="onUp"
        @pointercancel="onUp"
        @pointerleave="onLeave"
        @keydown="onKey"
        @contextmenu.prevent
      />
      <div v-if="!live" class="overlay">{{ statusText }}</div>
    </div>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, computed, watch, onBeforeUnmount } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { getSession } from 'spangap-browser/lib/webrtc-session'
import { useDeviceStore } from 'spangap-browser/stores/device'
import {
  parseHeader, decodeBody, rgb565ToRgba, mapKeyEvent,
  HDR_BYTES, FLAG_KEYFRAME, INPUT_POINTER, INPUT_KEY,
} from '../modules/lcdmirror'

const device = useDeviceStore()

const props = defineProps<{ visible: boolean; title: string; focusToken?: number }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 30, y: 12, w: 34, h: 62 }

const win = ref<InstanceType<typeof FloatingWindow> | null>(null)
const cv = ref<HTMLCanvasElement | null>(null)
const live = ref(false)                 /* a frame has landed since (re)connect */
const statusText = ref('Connecting…')

/* Learned panel size (canvas backing store); the window opens at this native
 * size and stays proportional to it on resize. */
const panelW = ref(320)
const panelH = ref(240)
const aspect = computed(() => panelW.value / panelH.value)
let fitted = false                      /* one native-size fit per window open */
let fitTimer: ReturnType<typeof setTimeout> | null = null

let dc: RTCDataChannel | null = null
let unregister: (() => void) | null = null
let reopenTimer: ReturnType<typeof setTimeout> | null = null
let backoff = 250
let down = false
let lastMoveSent = 0

function ctx(): CanvasRenderingContext2D | null {
  return cv.value ? cv.value.getContext('2d') : null
}

/* Grow the backing store to fit a rect, preserving already-painted pixels. The
 * first keyframe's strips settle it at the panel's true size. */
function ensureSize(w: number, h: number) {
  const c = cv.value
  if (!c || (w <= c.width && h <= c.height)) return
  const g = ctx()
  const old = g ? g.getImageData(0, 0, c.width, c.height) : null
  c.width = Math.max(w, c.width)
  c.height = Math.max(h, c.height)
  if (old) ctx()?.putImageData(old, 0, 0)
}

function onPacket(data: ArrayBuffer) {
  if (data.byteLength < HDR_BYTES) return
  const h = parseHeader(new DataView(data))
  const n = h.w * h.h
  if (n <= 0) return
  const px = decodeBody(new Uint8Array(data, HDR_BYTES), n, h.format)
  const rgba = rgb565ToRgba(px, n)
  ensureSize(h.x + h.w, h.y + h.h)
  const c = cv.value
  if (c) { panelW.value = c.width; panelH.value = c.height }   /* aspect tracks live */
  ctx()?.putImageData(new ImageData(rgba, h.w, h.h), h.x, h.y)
  if (h.flags & FLAG_KEYFRAME) { live.value = true; statusText.value = '' }
  else if (!live.value) live.value = true

  /* Size the window to the panel's NATIVE size once the canvas stops growing —
   * i.e. after the whole keyframe has landed (each strip resets the debounce), so
   * we use the full panel size, not the first strip. Respects a user-saved size
   * (fitBodyPx no-ops then). */
  if (!fitted) scheduleFit()
}

function scheduleFit() {
  if (fitTimer) clearTimeout(fitTimer)
  fitTimer = setTimeout(() => {
    fitTimer = null
    const c = cv.value
    if (!c) return
    fitted = true
    win.value?.fitBodyPx(c.width, c.height)
  }, 250)
}

/* ---- DataChannel ---- */

function build(pc: RTCPeerConnection) {
  if (dc) { try { dc.onclose = null; dc.close() } catch { /* */ } }
  try {
    dc = pc.createDataChannel('lcdmirror:1', { ordered: true })
  } catch (e) {
    console.error('[lcdmirror] createDataChannel failed:', e)
    dc = null
    return
  }
  dc.binaryType = 'arraybuffer'
  dc.onopen = () => { backoff = 250; statusText.value = 'Waiting for screen…' }
  dc.onmessage = (ev) => { if (ev.data instanceof ArrayBuffer) onPacket(ev.data) }
  dc.onclose = () => { dc = null; live.value = false; scheduleReopen() }
  dc.onerror = () => { /* onclose follows */ }
}

function openDC() {
  const s = getSession()
  if (s.pc && s.state === 'connected') build(s.pc)
}

function scheduleReopen() {
  if (!props.visible) return
  statusText.value = 'Reconnecting…'
  if (reopenTimer) clearTimeout(reopenTimer)
  reopenTimer = setTimeout(() => { openDC(); backoff = Math.min(backoff * 2, 5000) }, backoff)
}

function start() {
  if (unregister) return
  live.value = false
  statusText.value = 'Connecting…'
  unregister = getSession().registerChannel(build)   /* built now + on each PC reconnect */
  getSession().connect()
}

function stop() {
  if (reopenTimer) { clearTimeout(reopenTimer); reopenTimer = null }
  if (fitTimer) { clearTimeout(fitTimer); fitTimer = null }
  if (unregister) { unregister(); unregister = null }
  if (dc) { try { dc.onclose = null; dc.close() } catch { /* */ } dc = null }
  live.value = false
  fitted = false
}

watch(() => props.visible, (v) => { if (v) start(); else stop() }, { immediate: true })
onBeforeUnmount(stop)

/* ---- pointer → panel coordinates → device ---- */

function sendPtr(x: number, y: number, pressed: boolean) {
  if (!dc || dc.readyState !== 'open') return
  const b = new Uint8Array(6)
  b[0] = INPUT_POINTER
  b[1] = pressed ? 1 : 0
  b[2] = x & 0xff; b[3] = (x >> 8) & 0xff
  b[4] = y & 0xff; b[5] = (y >> 8) & 0xff
  try { dc.send(b) } catch { /* drop */ }
}

function toPanel(ev: PointerEvent): { x: number; y: number } {
  const c = cv.value!
  const r = c.getBoundingClientRect()
  /* Account for object-fit: contain — the drawn image is centred and scaled to
   * fit, so map through the letterbox offset/scale, not the raw element rect. */
  const scale = Math.min(r.width / c.width, r.height / c.height) || 1
  const offX = (r.width - c.width * scale) / 2
  const offY = (r.height - c.height * scale) / 2
  const x = Math.round((ev.clientX - r.left - offX) / scale)
  const y = Math.round((ev.clientY - r.top - offY) / scale)
  return {
    x: Math.max(0, Math.min(c.width - 1, x)),
    y: Math.max(0, Math.min(c.height - 1, y)),
  }
}

function onDown(ev: PointerEvent) {
  down = true
  cv.value?.focus()                    /* take keyboard focus so typing reaches the device */
  try { (ev.target as HTMLElement).setPointerCapture(ev.pointerId) } catch { /* */ }
  const p = toPanel(ev); sendPtr(p.x, p.y, true)
}
function onMove(ev: PointerEvent) {
  if (!down) return
  /* Throttle drag moves to ~60 Hz — a high-frequency mouse/trackpad fires far
   * more pointermove events than the device needs, and unthrottled they flood
   * the input channel. Press/release (onDown/onUp) are never throttled. */
  const now = performance.now()
  if (now - lastMoveSent < 16) return
  lastMoveSent = now
  const p = toPanel(ev); sendPtr(p.x, p.y, true)
}
function onUp(ev: PointerEvent) {
  if (!down) return
  down = false
  const p = toPanel(ev); sendPtr(p.x, p.y, false)
}
function onLeave(ev: PointerEvent) {
  if (!down) return
  down = false
  const p = toPanel(ev); sendPtr(p.x, p.y, false)
}

/* ---- keyboard → device ---- */

function sendKey(code: number) {
  if (!dc || dc.readyState !== 'open') return
  const b = new Uint8Array(5)
  b[0] = INPUT_KEY
  b[1] = code & 0xff
  b[2] = (code >>> 8) & 0xff
  b[3] = (code >>> 16) & 0xff
  b[4] = (code >>> 24) & 0xff
  try { dc.send(b) } catch { /* drop */ }
}

function onKey(ev: KeyboardEvent) {
  const code = mapKeyEvent(ev)
  if (code === null) return
  ev.preventDefault()                  /* keep browser shortcuts out while typing to the device */
  sendKey(code)
}

/* ---- screenshot → downloads ---- */

function pad(n: number, w = 2) { return String(n).padStart(w, '0') }

function screenshot() {
  const c = cv.value
  if (!c) return
  const host = (() => {
    const h = device.get('s.net.hostname')
    return (typeof h === 'string' && h.trim()) ? h.trim() : (location.hostname || 'device')
  })()
  const d = new Date()
  const ts = `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
  c.toBlob((blob) => {
    if (!blob) return
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `${host}_screenshot_${ts}.png`
    document.body.appendChild(a)
    a.click()
    a.remove()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
  }, 'image/png')
}
</script>

<style scoped>
.mirror {
  width: 100%;
  height: 100%;
  background: #000;
  position: relative;
  overflow: hidden;
}
.screen {
  /* Fill the (aspect-locked) window; object-fit keeps the panel's aspect and
   * scales the 320x240 backing store up to the window size. */
  width: 100%;
  height: 100%;
  object-fit: contain;
  touch-action: none;            /* we own drag; don't scroll/zoom the page */
  image-rendering: auto;
  display: block;
  outline: none;                 /* focusable (for typing) but no focus ring */
}
.cam {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 22px;
  height: 22px;
  padding: 0;
  border: 0;
  background: transparent;
  color: #fff;
  cursor: pointer;
  opacity: 0.75;
}
.cam:hover { opacity: 1; }
.cam svg { width: 18px; height: 18px; display: block; }
.overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: rgba(255, 255, 255, 0.7);
  font-size: 13px;
  pointer-events: none;
  background: rgba(0, 0, 0, 0.35);
}
</style>
