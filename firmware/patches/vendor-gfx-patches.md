# Vendor library patches — must be re-applied if `vendor/` is re-cloned

The build depends on two small hand-edits to the **GFX_Library_for_Arduino**
library bundled under `vendor/` (which is gitignored, so these edits are NOT
otherwise tracked). If `vendor/` is deleted/re-cloned, or a file gets reverted
(e.g. by OneDrive sync), re-apply both edits below or the firmware won't build
or will show the boot vertical offset.

Library root:
`vendor/waveshare-demo/examples/Arduino-v3.3.2/libraries/GFX_Library_for_Arduino/`

---

## Patch 1 — DISABLE the bounce buffer (stops the image drifting under WiFi)

File: `src/databus/Arduino_ESP32RGBPanel.cpp` (in the `getFrameBuffer()` panel
config, ~line 75).

Change the bounce-buffer size to `0`:

```c
.bounce_buffer_size_px = 0,    // was: 40 * w
```

Why: with a bounce buffer, its refill ISR gets starved by WiFi bursts and the
RGB pixel feed slips → the image drifts up permanently (top wraps to bottom),
recurring every few minutes. Disabling it makes the RGB DMA read the PSRAM
framebuffer directly (no refill ISR to starve). Octal PSRAM bandwidth far
exceeds the scan need at 12 MHz pclk, so direct read is stable. **This is the
real fix for the vertical-offset bug.** (2026-06-13.)

Note: the nearby `.bb_invalidate_cache = true` flag (an earlier patch) is now
moot since there's no bounce buffer, but it's harmless — leave it as-is.

---

## Patch 2 — expose the panel handle (REQUIRED to compile)

File: `src/databus/Arduino_ESP32RGBPanel.h`, in the `public:` section right
after the `getFrameBuffer(...)` declaration.

Add this getter:

```cpp
  // Panel handle, exposed so the app can call esp_lcd_rgb_panel_restart() to
  // resync the scan-out DMA to the frame start (fixes the boot vertical offset).
  // Valid only after begin().
  esp_lcd_panel_handle_t getPanelHandle() { return _panel_handle; }
```

Why: `firmware/src/app/main.cpp` calls `rgbpanel->getPanelHandle()` to do a
one-shot `esp_lcd_rgb_panel_restart()` ~800 ms after boot, which realigns the
image (fixes "picture shifted up, top wraps to bottom"). Without this getter the
app **will not compile**.

---

## How to verify the patches are present

```sh
grep -n "bb_invalidate_cache = true" \
  vendor/.../GFX_Library_for_Arduino/src/databus/Arduino_ESP32RGBPanel.cpp
grep -n "getPanelHandle" \
  vendor/.../GFX_Library_for_Arduino/src/databus/Arduino_ESP32RGBPanel.h
```

Both should return a line. Then build: `pio run -e app` from `firmware/`.
