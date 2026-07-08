# lcdmirror — mirror the device screen to a browser

`lcdmirror` puts the device's physical LCD in a browser window: the device
streams changed screen regions out and the browser sends click/drag back, driving
the **same** LVGL UI the physical panel drives. It is a VNC-style mirror of the
real screen — not a second, independently-rendered UI (that is the web SPA's job).
Look at and poke the device's actual launcher from a laptop.

It rides the platform's existing WebRTC transport (a reliable+ordered DataChannel
on the same peer connection that already carries storage/log/cli) and a
compression scheme — 2D-friendly RLE with a literal fallback — matched to the flat,
solid-fill launcher theme.

## What it needs

Hard dependencies, one-way: `spangap-lcd` (the screen it mirrors) and
`spangap-web` (the WebRTC DataChannel↔ITS router). It stages on a screen board —
`hw-tdeck` pulls it in the way it pulls in `spangap-lcd` — so a screenless /
headless build never includes it. Drop it from a screen build with
`--without lcdmirror` for a panel-only image.

## Using it

Open the web UI, click the **Screen** app in the Dock. A window shows the live
device screen; click and drag in it to drive the device. One client at a time
(the WebRTC signalling already enforces a single session; a second tab is BUSY,
or takes over with `?force=1`). While a client is connected the panel is held
awake (no inactivity blank) so the mirror never goes dark; it reverts to the
normal timeout when the client leaves.

Turn remote viewing off from **Settings → Screen Mirror → Allow remote viewing**
(or `set s.lcdmirror.enabled=0`); a connected client is dropped immediately.

## How it works (brief)

- The `lcdmirror` task opens an ITS packet server port. The browser opens a
  DataChannel labelled `lcdmirror:1`; the webrtc router bridges it to the port.
- Every LVGL flush strip is captured (pre-byte-swap, so it is little-endian for
  the canvas) into a full-screen framebuffer, growing a damage bounding-box. At
  a capped ~5 Hz all damage since the last send coalesces into ONE RLE packet —
  the whole screen while scrolling, a tiny rect when idle. On connect the device
  forces a full-screen repaint, so the first coalesced frame is the keyframe.
- The browser RLE-decodes each rectangle, converts RGB565→RGBA and blits it onto
  a native-resolution `<canvas>`; the window sizes itself to the panel and stays
  proportional when resized.
- Pointer and keyboard events are mapped and sent back (6/5-byte packets),
  injected into the same LVGL input paths the local touch/trackball/keyboard
  use — click the mirror once to give it keyboard focus. The titlebar camera
  saves a `<hostname>_screenshot_<timestamp>.png`.
- Under back-pressure the mirror simply coalesces harder (bigger, rarer
  frames) and never blocks the panel; only a client whose link never drains is
  dropped, and the browser reconnects with backoff to a fresh keyframe.

Wire format and internals: [INTERNALS.md](INTERNALS.md).

## Storage variables

| key | kind | default | meaning |
|-----|------|---------|---------|
| `s.lcdmirror.enabled` | persistent | `1` | accept remote clients at all |
| `lcdmirror.clients`   | ephemeral  | `0` | live connected-client count (0/1), read-only |

It starts automatically when staged — there is no init call to make.
