# lcdmirror internals

Maintainer reference for the browser screen-mirror. Operator guide: [README.md](README.md).

## 1. What it adds

- **The `-lcd` mirror primitives** (in `spangap-lcd`, transport-agnostic —
  `lcd.h` / `lcd_lvgl.cpp`), the only new seam outside this straddle:
  - `lcdMirrorAttach(sink)` — `flushCb` calls `sink(area, px)` **before** the
    in-place `lv_draw_sw_rgb565_swap`, so `px` is little-endian RGB565 (what the
    browser canvas wants). One null-check per flush when detached.
  - `lcdMirrorInjectPointer(x, y, state)` — pushes a sample onto a small queue
    drained by a dedicated always-present EVENT-mode pointer indev, forcing a read
    on the lcd task via `lcdRun`. Works on any board (no trackball needed).
  - `lcdMirrorInjectKey(key)` — one keystroke (press+release) into a keypad indev
    joined to `lcdInputGroup()`, so a browser client types wherever the board's
    own keyboard would. Input bursts coalesce into at most ONE pending `lcdRun`
    (`mirrorSchedule`), because lcdRun rides the lcd task's shallow ITS aux inbox
    — the same inbox storage delivers CHANGED on — and flooding it backs up
    storage's subscriber delivery.
  - `lcdMirrorKeepAwake(on)` — wakes the panel and suspends the inactivity blank
    timer while a client is connected (`s_mirrorHold` gates `armBlankTimer`).
- **This straddle**: the `lcdmirror` FreeRTOS task (ITS packet server,
  framebuffer + damage box, RLE encoder, keyframe/backpressure state machine,
  pointer/key inject) and the browser `<canvas>` app.

## 2. Framebuffer + damage box, drained at a capped rate

The core is a full-screen RGB565 framebuffer plus a damage bounding-box, drained
into one coalesced packet per tick — **not** a per-strip stream.

- **lcd task** (`sinkCb`, inside `flushCb`): `memcpy` each flush strip's rows into
  the framebuffer at its position and expand the damage box (min/max of every
  dirty rect since the last send), under a tiny `portMUX` spinlock. No ITS, no
  compression — it must not stall the render path, which holds the SPI bus lock
  shared with LoRa on SPI2.
- **`lcdmirror` task**: every `FRAME_MS` (200 ms — a 5 Hz screen-send cap suited
  to this SPI panel over a reliable channel; input drains separately at ~50 Hz,
  `POLL_MS`) it snapshots+clears the damage box, gathers that region from the
  framebuffer, and sends it as ONE RLE packet (banded by rows only if it exceeds
  `MAXBODY`). Sole owner of `s_client`.

The framebuffer (`s_fb`, `fbW*fbH*2` ≈ 150 KB), a same-size gather buffer, and the
encode buffer are allocated in PSRAM on first connect and **kept for the process
lifetime** — never freed on disconnect, so a flush mid-copy can never race a free.
The framebuffer is read during encode without a lock: a torn read costs a few
stale pixels for one frame, healed on the next (the standard VNC trade to keep the
render path lock-free). The damage box is the only locked state.

`lv_display_get_horizontal_resolution` / `_vertical_resolution` give the panel
size (the public `lcd.h` does not expose `lcdScreenW/H`), matching the coordinate
space of `flushCb` areas.

## 3. Keyframe

`onConnect` allocates the framebuffer, attaches the sink, sets `s_wantKeyframe`,
and calls `lv_obj_invalidate(lv_screen_active())` (on the lcd task): LVGL
re-flushes every strip, the sink fills the framebuffer and grows the damage box to
the whole screen, and the next tick sends it — usually as ONE packet (a flat theme
RLEs well under `MAXBODY`), banded only if oversized. The first packet after
connect carries `FLAG_KEYFRAME` so the browser clears its overlay.

Every connection-identity change (first connect, DTLS/SCTP reconnect, `?force=1`
takeover, DC-drop resync) produces a fresh ITS handle → a fresh `onConnect` →
keyframe. There is no separate "refresh" opcode.

## 4. Wire format

Little-endian throughout. Device→browser, one packet per dirty rectangle:

```
header (10 B): u16 x, u16 y, u16 w, u16 h, u8 format, u8 flags
  format: 0 = RLE16, 1 = RAW16 (uncompressed RGB565 LE)
  flags:  bit0 (0x01) = KEYFRAME_START
body: RLE16 stream or raw RGB565, w*h pixels row-major
```

**RLE16** — PackBits over 16-bit pixels, threshold-3:
- control `c` in `[0,127]`: literal run of `c+1` pixels (`(c+1)*2` body bytes).
- control `c` in `[128,255]`: repeat run of `(c-128)+2` pixels (one 2-byte pixel).

A run of 2 stays literal (cheaper); AA edges degrade to literals + ~1/128
overhead instead of exploding. The encoder falls back to RAW16 whenever the RLE
body would be ≥ raw (`encodeCap` overflow returns 0 → raw). Estimates assume the
flat, solid-fill launcher theme; a photographic wallpaper pushes toward raw — do
not "improve" the theme into a bandwidth cliff.

Browser→device input packets, dispatched by the leading type byte:
- **pointer** (6 B): `u8 type(=1), u8 pressed, u16 x, u16 y` in panel coordinates
  (the browser maps canvas→panel through the object-fit letterbox).
- **key** (5 B): `u8 type(=2), u32 key` — an LVGL key code (Unicode codepoint for
  printables, `LV_KEY_*` for specials, `| LCD_KEY_CTRL` for a control combo). One
  packet is one keystroke; the device injects a press+release into a keypad indev
  joined to `lcdInputGroup()`, so it types wherever the board's own keyboard would.

Message framing is free: the ITS port is packet-mode (one `itsSend` = one SCTP
DATA message), so the packet boundary is the frame boundary — no length prefixes.

## 5. Backpressure — a coalescing gradient

Reliable+ordered has no lossy escape hatch, so we can never skip a region's
pixels — but we *can* coalesce and rate-limit before anything is sent. Each tick
checks `itsSpacesAvailable`: if the send window has less than a frame's room
(`CONGEST_MIN`), the tick **skips** and the damage box keeps accumulating. So
under sustained motion (a scroll) the mirror degrades smoothly — larger,
less-frequent *coalesced* frames — instead of flooding per-strip packets or
stalling. `itsSend` is non-blocking (`timeout 0`), so the task never blocks the
panel; a send that fills the window mid-frame re-marks the region for the next
tick.

Dropping the DC is the **last resort only**: a client so wedged that the window
never drains for `STUCK_MS` gets `itsDisconnect`ed (an SCTP stream reset), and the
browser reconnects with exponential backoff (250 ms → 5 s) and a fresh keyframe.
This is what avoids the keyframe-amplified reconnect loop — on a link too slow for
even a coalesced frame, we send *nothing* and wait for drain, rather than
resending the heaviest payload forever.

## 6. Transport wiring

The browser opens `pc.createDataChannel('lcdmirror:1', { ordered: true })` on the
shared session (reliable+ordered — no `maxRetransmits`/`maxPacketLifeTime`). The
webrtc router parses the label as `<taskName>:<port>` and calls
`itsConnect("lcdmirror", 1, …)` — so the task **must** be named `lcdmirror` (the
launcher task is `lcd`; do not collide). Port `LCDMIRROR_PORT = 1`,
`maxHandles = 1` (single session, also enforced by the signalling WS). The router
pins this DC to priority 64 — below the control channels (storage/log/cli, 256) —
so bulk pixel data can never delay the storage heartbeat on the shared SCTP
association. A freshly-connected client is held idle for `ACTIVATE_MS` (1.5 s)
before capture attaches: a busy device can flap a new session a few times before
it sticks, and piling a keyframe onto that fragile window makes it worse.

## 7. Pitfalls

- **Tap before the swap.** Capturing after `lv_draw_sw_rgb565_swap` yields
  big-endian pixels → mangled colours. The sink runs pre-swap for this reason.
- **Never hold `px`.** It is LVGL's draw buffer, reused after `flushCb` returns;
  the sink copies into the ring and returns at once.
- **Task name is load-bearing.** It is the ITS server identity the router dials.
- **Don't block the lcd task.** All compression/ITS runs on the `lcdmirror`
  task; the sink is memcpy-only.
- **Single owner for `s_client`.** Only the `lcdmirror` task reads/writes it; the
  sink never touches it (it only feeds the ring, drained regardless of client).
